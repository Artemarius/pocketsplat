#include "splatcore/palette.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>  // SSE2: part of the x86-64 baseline, no compiler flag needed
#define SPLATCORE_PALETTE_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>  // Advanced SIMD: part of the AArch64 baseline, no compiler flag needed
#define SPLATCORE_PALETTE_NEON 1
#endif

// No fused multiply-adds anywhere in this file: clang contracts a * b + c by default on arm64
// (MSVC on x86-64 does not), which rounds differently and made the S30 phone palettes differ from
// the desktop ones. With contraction off, the same seed and inputs give the same palette on both.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace splatcore {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// Centroids in structure-of-arrays form for the assignment kernel: dimension d of centroid j at
// c[d * stride + j], and half the squared norm. argmin_j |x - c_j|^2 = argmin_j (|c_j|^2 / 2 - x.c_j),
// so a pair costs nine multiply-adds. Rows are padded to a multiple of 8 with zero centroids whose
// half norm is +inf, so padding never wins.
struct SoaCentroids {
    std::uint32_t k = 0;
    std::uint32_t stride = 0;
    std::vector<float> c;
    std::vector<float> half_norm;
};

// Fills `soa` from k centroids in array-of-structures form, reusing its storage.
void fill_soa(SoaCentroids& soa, const float* aos, std::uint32_t k) {
    soa.k = k;
    soa.stride = (k + 7u) & ~7u;
    soa.c.assign(static_cast<std::size_t>(kSh1Dims) * soa.stride, 0.0f);
    soa.half_norm.assign(soa.stride, kInf);
    for (std::uint32_t j = 0; j < k; ++j) {
        float norm = 0.0f;
        for (int d = 0; d < kSh1Dims; ++d) {
            const float v = aos[static_cast<std::size_t>(kSh1Dims) * j + d];
            soa.c[static_cast<std::size_t>(d) * soa.stride + j] = v;
            norm += v * v;
        }
        soa.half_norm[j] = 0.5f * norm;
    }
}

float exact_sq_dist(const float* x, const float* aos, std::uint32_t label) {
    const float* c = aos + static_cast<std::size_t>(kSh1Dims) * label;
    float d2 = 0.0f;
    for (int d = 0; d < kSh1Dims; ++d) d2 += (x[d] - c[d]) * (x[d] - c[d]);
    return d2;
}

float sq_norm(const float* x) {
    float n = 0.0f;
    for (int d = 0; d < kSh1Dims; ++d) n += x[d] * x[d];
    return n;
}

#if defined(SPLATCORE_PALETTE_SSE2)

// SSE2 kernel: two vectors of four centroids per step, each lane keeps its best score and index
// (strictly smaller wins, so a lane keeps its lowest index on ties); the eight lanes are merged at
// the end with the same rule. With kTop2 each lane also keeps its second-best score (Hamerly's
// lower bound); the best is chosen by the same comparisons either way. The dot product is summed
// as a tree (the order the portable kernel uses too) so the add latency does not serialize the
// loop. Explicit because MSVC's auto-vectorized loop ran at ~6.5 cycles per pair on the dev laptop.
struct Lanes {
    __m128 best = _mm_set1_ps(kInf);
    __m128 second = _mm_set1_ps(kInf);
    __m128i best_idx = _mm_setzero_si128();
};

inline __m128 select_ps(__m128 mask, __m128 a, __m128 b) {
    return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
}

template <bool kTop2>
inline void keep_better(Lanes& lanes, __m128 score, __m128i idx) {
    const __m128 better = _mm_cmplt_ps(score, lanes.best);
    if constexpr (kTop2) lanes.second = select_ps(better, lanes.best, _mm_min_ps(lanes.second, score));
    lanes.best = select_ps(better, score, lanes.best);
    const __m128i take = _mm_castps_si128(better);
    lanes.best_idx = _mm_or_si128(_mm_and_si128(take, idx), _mm_andnot_si128(take, lanes.best_idx));
}

// Nearest centroid of x (lowest index on ties); with kTop2, also the best and second-best scores.
template <bool kTop2>
inline std::uint32_t scan(const float* x, const SoaCentroids& soa, float* best_score, float* second_score) {
    const std::size_t s = soa.stride;
    const float* c = soa.c.data();
    const float* hn = soa.half_norm.data();
    const __m128 x0 = _mm_set1_ps(x[0]), x1 = _mm_set1_ps(x[1]), x2 = _mm_set1_ps(x[2]);
    const __m128 x3 = _mm_set1_ps(x[3]), x4 = _mm_set1_ps(x[4]), x5 = _mm_set1_ps(x[5]);
    const __m128 x6 = _mm_set1_ps(x[6]), x7 = _mm_set1_ps(x[7]), x8 = _mm_set1_ps(x[8]);
    const auto score4 = [&](std::size_t j) {
        const __m128 a =
            _mm_add_ps(_mm_mul_ps(x0, _mm_loadu_ps(c + j)), _mm_mul_ps(x1, _mm_loadu_ps(c + s + j)));
        const __m128 b = _mm_add_ps(_mm_mul_ps(x2, _mm_loadu_ps(c + 2 * s + j)),
                                    _mm_mul_ps(x3, _mm_loadu_ps(c + 3 * s + j)));
        const __m128 e = _mm_add_ps(_mm_mul_ps(x4, _mm_loadu_ps(c + 4 * s + j)),
                                    _mm_mul_ps(x5, _mm_loadu_ps(c + 5 * s + j)));
        const __m128 f = _mm_add_ps(_mm_mul_ps(x6, _mm_loadu_ps(c + 6 * s + j)),
                                    _mm_mul_ps(x7, _mm_loadu_ps(c + 7 * s + j)));
        const __m128 dot = _mm_add_ps(_mm_add_ps(_mm_add_ps(a, b), _mm_add_ps(e, f)),
                                      _mm_mul_ps(x8, _mm_loadu_ps(c + 8 * s + j)));
        return _mm_sub_ps(_mm_loadu_ps(hn + j), dot);
    };
    Lanes lo, hi;
    __m128i idx = _mm_set_epi32(3, 2, 1, 0);
    const __m128i four = _mm_set1_epi32(4), eight = _mm_set1_epi32(8);
    for (std::size_t j = 0; j < s; j += 8) {  // stride is a multiple of 8
        keep_better<kTop2>(lo, score4(j), idx);
        keep_better<kTop2>(hi, score4(j + 4), _mm_add_epi32(idx, four));
        idx = _mm_add_epi32(idx, eight);
    }
    alignas(16) float lane_best[8];
    alignas(16) std::int32_t lane_idx[8];
    _mm_store_ps(lane_best, lo.best);
    _mm_store_ps(lane_best + 4, hi.best);
    _mm_store_si128(reinterpret_cast<__m128i*>(lane_idx), lo.best_idx);
    _mm_store_si128(reinterpret_cast<__m128i*>(lane_idx + 4), hi.best_idx);
    int pick = 0;
    for (int l = 1; l < 8; ++l) {
        if (lane_best[l] < lane_best[pick] ||
            (lane_best[l] == lane_best[pick] && lane_idx[l] < lane_idx[pick])) {
            pick = l;
        }
    }
    if constexpr (kTop2) {
        alignas(16) float lane_second[8];
        _mm_store_ps(lane_second, lo.second);
        _mm_store_ps(lane_second + 4, hi.second);
        float second = kInf;
        for (int l = 0; l < 8; ++l) {
            second = std::min(second, lane_second[l]);
            if (l != pick) second = std::min(second, lane_best[l]);
        }
        *best_score = lane_best[pick];
        *second_score = second;
    }
    return static_cast<std::uint32_t>(lane_idx[pick]);
}

#elif defined(SPLATCORE_PALETTE_NEON)

// NEON kernel: the SSE2 kernel's structure lane for lane (two vectors of four centroids per step,
// strictly-smaller-wins lanes, the same eight-lane merge, the same summation tree). Separate
// multiplies and adds, not vfmaq: fused multiply-adds round once instead of twice, so the scores,
// and the labels on near-ties, would differ from the x86-64 kernel. The S30 phone runs used the
// portable loop, which the compiler may contract into FMAs; this kernel does not leave it the
// choice.
struct Lanes {
    float32x4_t best = vdupq_n_f32(kInf);
    float32x4_t second = vdupq_n_f32(kInf);
    uint32x4_t best_idx = vdupq_n_u32(0);
};

template <bool kTop2>
inline void keep_better(Lanes& lanes, float32x4_t score, uint32x4_t idx) {
    const uint32x4_t better = vcltq_f32(score, lanes.best);
    if constexpr (kTop2) lanes.second = vbslq_f32(better, lanes.best, vminq_f32(lanes.second, score));
    lanes.best = vbslq_f32(better, score, lanes.best);
    lanes.best_idx = vbslq_u32(better, idx, lanes.best_idx);
}

template <bool kTop2>
inline std::uint32_t scan(const float* x, const SoaCentroids& soa, float* best_score, float* second_score) {
    const std::size_t s = soa.stride;
    const float* c = soa.c.data();
    const float* hn = soa.half_norm.data();
    const float32x4_t x0 = vdupq_n_f32(x[0]), x1 = vdupq_n_f32(x[1]), x2 = vdupq_n_f32(x[2]);
    const float32x4_t x3 = vdupq_n_f32(x[3]), x4 = vdupq_n_f32(x[4]), x5 = vdupq_n_f32(x[5]);
    const float32x4_t x6 = vdupq_n_f32(x[6]), x7 = vdupq_n_f32(x[7]), x8 = vdupq_n_f32(x[8]);
    const auto score4 = [&](std::size_t j) {
        const float32x4_t a = vaddq_f32(vmulq_f32(x0, vld1q_f32(c + j)), vmulq_f32(x1, vld1q_f32(c + s + j)));
        const float32x4_t b =
            vaddq_f32(vmulq_f32(x2, vld1q_f32(c + 2 * s + j)), vmulq_f32(x3, vld1q_f32(c + 3 * s + j)));
        const float32x4_t e =
            vaddq_f32(vmulq_f32(x4, vld1q_f32(c + 4 * s + j)), vmulq_f32(x5, vld1q_f32(c + 5 * s + j)));
        const float32x4_t f =
            vaddq_f32(vmulq_f32(x6, vld1q_f32(c + 6 * s + j)), vmulq_f32(x7, vld1q_f32(c + 7 * s + j)));
        const float32x4_t dot =
            vaddq_f32(vaddq_f32(vaddq_f32(a, b), vaddq_f32(e, f)), vmulq_f32(x8, vld1q_f32(c + 8 * s + j)));
        return vsubq_f32(vld1q_f32(hn + j), dot);
    };
    Lanes lo, hi;
    static constexpr std::uint32_t kIota[4] = {0, 1, 2, 3};
    uint32x4_t idx = vld1q_u32(kIota);
    const uint32x4_t four = vdupq_n_u32(4), eight = vdupq_n_u32(8);
    for (std::size_t j = 0; j < s; j += 8) {  // stride is a multiple of 8
        keep_better<kTop2>(lo, score4(j), idx);
        keep_better<kTop2>(hi, score4(j + 4), vaddq_u32(idx, four));
        idx = vaddq_u32(idx, eight);
    }
    float lane_best[8];
    std::uint32_t lane_idx[8];
    vst1q_f32(lane_best, lo.best);
    vst1q_f32(lane_best + 4, hi.best);
    vst1q_u32(lane_idx, lo.best_idx);
    vst1q_u32(lane_idx + 4, hi.best_idx);
    int pick = 0;
    for (int l = 1; l < 8; ++l) {
        if (lane_best[l] < lane_best[pick] ||
            (lane_best[l] == lane_best[pick] && lane_idx[l] < lane_idx[pick])) {
            pick = l;
        }
    }
    if constexpr (kTop2) {
        float lane_second[8];
        vst1q_f32(lane_second, lo.second);
        vst1q_f32(lane_second + 4, hi.second);
        float second = kInf;
        for (int l = 0; l < 8; ++l) {
            second = std::min(second, lane_second[l]);
            if (l != pick) second = std::min(second, lane_best[l]);
        }
        *best_score = lane_best[pick];
        *second_score = second;
    }
    return lane_idx[pick];
}

#else

// Portable kernel (targets without SSE2 or NEON). Same scores in the same order of operations,
// unless the compiler contracts them into fused multiply-adds.
template <bool kTop2>
inline std::uint32_t scan(const float* x, const SoaCentroids& soa, float* best_score, float* second_score) {
    const std::size_t s = soa.stride;
    const float* c = soa.c.data();
    float best = kInf, second = kInf;
    std::uint32_t best_j = 0;
    for (std::uint32_t j = 0; j < soa.k; ++j) {
        const auto t = [&](int d) { return x[d] * c[static_cast<std::size_t>(d) * s + j]; };
        const float dot = ((t(0) + t(1)) + (t(2) + t(3))) + ((t(4) + t(5)) + (t(6) + t(7))) + t(8);
        const float score = soa.half_norm[j] - dot;
        if (score < best) {
            if constexpr (kTop2) second = best;
            best = score, best_j = j;
        } else if constexpr (kTop2) {
            second = std::min(second, score);
        }
    }
    if constexpr (kTop2) *best_score = best, *second_score = second;
    return best_j;
}

#endif

void assign_range(const float* points, std::size_t begin, std::size_t end, const SoaCentroids& soa,
                  const float* aos, std::uint16_t* labels, float* sq_dist) {
    for (std::size_t i = begin; i < end; ++i) {
        const float* x = points + static_cast<std::size_t>(kSh1Dims) * i;
        const std::uint32_t label = scan<false>(x, soa, nullptr, nullptr);
        labels[i - begin] = static_cast<std::uint16_t>(label);
        if (sq_dist != nullptr) sq_dist[i - begin] = exact_sq_dist(x, aos, label);
    }
}

std::uint32_t resolve_threads(std::uint32_t requested, std::size_t n) {
    std::size_t t = requested != 0 ? requested : std::max(1u, std::thread::hardware_concurrency());
    t = std::min<std::size_t>(t, std::max<std::size_t>(1, n));
    return static_cast<std::uint32_t>(t);
}

// A fixed set of threads for the parallel phases of training: e-means runs one parallel
// assignment per mini-batch, Hamerly several per epoch, so threads are started once.
// ranges(n, fn) runs fn(thread, begin, end) over `threads` contiguous, equal ranges of [0, n);
// the calling thread takes range 0. The split depends on n and the thread count only, so
// per-thread partial sums reduce in a fixed order.
// dynamic(n, fn) runs fn(begin, end) over small ranges handed out from a shared counter, so a fast
// core takes more of them than a slow one (phones: one big, several mid, two little cores; with
// equal ranges every e-means batch waited for the little cores). Only for work whose result does
// not depend on which thread runs a range: assignment into per-point outputs.
class WorkerPool {
public:
    explicit WorkerPool(std::uint32_t threads) : m_threads(std::max(1u, threads)) {
        for (std::uint32_t t = 1; t < m_threads; ++t) m_workers.emplace_back([this, t] { loop(t); });
    }
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            ++m_generation;
        }
        m_start.notify_all();
        for (std::thread& w : m_workers) w.join();
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    std::uint32_t threads() const { return m_threads; }

    template <typename Fn>
    void ranges(std::size_t n, Fn&& fn) {
        const std::size_t per = (n + m_threads - 1) / m_threads;
        const std::function<void(std::uint32_t)> job = [&fn, n, per](std::uint32_t t) {
            const std::size_t begin = std::min(n, per * t);
            const std::size_t end = std::min(n, begin + per);
            fn(t, begin, end);
        };
        run(job);
    }

    template <typename Fn>
    void dynamic(std::size_t n, Fn&& fn) {
        // About 16 ranges per thread, at least 32 points each: at k = 4,096 a range is ~130 K
        // pairs, 0.1-0.2 ms on a phone core, against one atomic increment.
        const std::size_t grain = std::max<std::size_t>(32, n / (16 * static_cast<std::size_t>(m_threads)));
        std::atomic<std::size_t> next{0};
        const std::function<void(std::uint32_t)> job = [&fn, &next, n, grain](std::uint32_t) {
            for (;;) {
                const std::size_t begin = next.fetch_add(grain, std::memory_order_relaxed);
                if (begin >= n) return;
                fn(begin, std::min(n, begin + grain));
            }
        };
        run(job);
    }

private:
    void run(const std::function<void(std::uint32_t)>& job) {
        if (m_threads == 1) {
            job(0);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_job = &job;
            m_pending = m_threads - 1;
            ++m_generation;
        }
        m_start.notify_all();
        job(0);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this] { return m_pending == 0; });
        m_job = nullptr;
    }

    void loop(std::uint32_t t) {
        std::uint64_t seen = 0;
        for (;;) {
            const std::function<void(std::uint32_t)>* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_start.wait(lock, [&] { return m_generation != seen; });
                seen = m_generation;
                if (m_stop) return;
                job = m_job;
            }
            (*job)(t);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (--m_pending == 0) m_done.notify_one();
            }
        }
    }

    std::uint32_t m_threads;
    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_start;
    std::condition_variable m_done;
    const std::function<void(std::uint32_t)>* m_job = nullptr;
    std::uint32_t m_pending = 0;
    std::uint64_t m_generation = 0;
    bool m_stop = false;
};

// Uniform integer in [0, range) from a 64-bit Mersenne Twister, by rejection: the standard
// distributions are implementation-defined, this is the same on every platform.
std::uint64_t bounded(std::mt19937_64& rng, std::uint64_t range) {
    const std::uint64_t limit =
        std::numeric_limits<std::uint64_t>::max() - std::numeric_limits<std::uint64_t>::max() % range;
    std::uint64_t r;
    do {
        r = rng();
    } while (r >= limit);
    return r % range;
}

// Uniform double in (0, 1), the same on every platform.
double open_unit(std::mt19937_64& rng) {
    return (static_cast<double>(rng() >> 11) + 0.5) * 0x1.0p-53;
}

// `count` distinct indices of [0, n) (Floyd's algorithm), in the order they were drawn.
std::vector<std::uint32_t> sample_distinct(std::mt19937_64& rng, std::size_t n, std::size_t count) {
    std::vector<std::uint32_t> picked;
    picked.reserve(count);
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(count * 2);
    for (std::size_t j = n - count; j < n; ++j) {
        auto t = static_cast<std::uint32_t>(bounded(rng, j + 1));
        if (!seen.insert(t).second) {
            t = static_cast<std::uint32_t>(j);
            seen.insert(t);
        }
        picked.push_back(t);
    }
    return picked;
}

// What every trainer works on: m training vectors, k centroids (initialised to distinct random
// training vectors, as the paper and Lloyd both do), the pool, the seeded generator.
struct Training {
    const float* train = nullptr;
    std::size_t m = 0;
    std::uint32_t k = 0;
    std::uint32_t epochs = 0;
    float split_eps = 0.0f;  // additive split offset, tiny against the data's spread, never zero
    WorkerPool* pool = nullptr;
    std::mt19937_64* rng = nullptr;
    std::vector<float>* centroids = nullptr;  // kSh1Dims * k
    PaletteStats* stats = nullptr;

    const float* point(std::size_t i) const { return train + static_cast<std::size_t>(kSh1Dims) * i; }
    float* centroid(std::uint32_t j) const {
        return centroids->data() + static_cast<std::size_t>(kSh1Dims) * j;
    }
};

// The update step Lloyd and Hamerly share: per-thread double sums over the pool's fixed ranges,
// reduced in thread order (deterministic for a fixed thread count), then the cluster means, then
// empty clusters refilled by splitting the most populated one.
class LloydUpdate {
public:
    LloydUpdate(std::uint32_t threads, std::uint32_t k)
        : m_sums(threads, std::vector<double>(static_cast<std::size_t>(kSh1Dims) * k)),
          m_counts(threads, std::vector<std::uint32_t>(k)),
          m_total(static_cast<std::size_t>(kSh1Dims) * k),
          m_total_count(k) {}

    void clear(std::uint32_t t) {
        std::fill(m_sums[t].begin(), m_sums[t].end(), 0.0);
        std::fill(m_counts[t].begin(), m_counts[t].end(), 0u);
    }
    void add(std::uint32_t t, std::uint16_t label, const float* x) {
        ++m_counts[t][label];
        double* s = m_sums[t].data() + static_cast<std::size_t>(kSh1Dims) * label;
        for (int d = 0; d < kSh1Dims; ++d) s[d] += x[d];
    }

    // Moves every populated centroid to its mean; with allow_split, splits donors into the empty
    // clusters. Returns whether any split happened.
    bool apply(const Training& tr, bool allow_split) {
        const std::uint32_t k = tr.k;
        std::fill(m_total.begin(), m_total.end(), 0.0);
        std::fill(m_total_count.begin(), m_total_count.end(), 0u);
        for (std::size_t t = 0; t < m_sums.size(); ++t) {
            for (std::size_t i = 0; i < m_total.size(); ++i) m_total[i] += m_sums[t][i];
            for (std::uint32_t j = 0; j < k; ++j) m_total_count[j] += m_counts[t][j];
        }
        for (std::uint32_t j = 0; j < k; ++j) {
            if (m_total_count[j] == 0) continue;
            float* c = tr.centroid(j);
            for (int d = 0; d < kSh1Dims; ++d) {
                c[d] = static_cast<float>(m_total[static_cast<std::size_t>(kSh1Dims) * j + d] /
                                          static_cast<double>(m_total_count[j]));
            }
        }
        // Empty clusters: split the most populated cluster into two copies offset by +-eps along
        // an alternating-sign direction, so the next assignment divides its points. Not after the
        // last update: the palette returned is always the means of its clusters.
        // Donors come from a max-heap on (count, lowest index first).
        using Donor = std::pair<std::uint64_t, std::uint32_t>;
        const auto later = [](const Donor& a, const Donor& b) {
            return a.first != b.first ? a.first < b.first : a.second > b.second;
        };
        std::priority_queue<Donor, std::vector<Donor>, decltype(later)> donors(later);
        if (allow_split && std::find(m_total_count.begin(), m_total_count.end(), 0u) != m_total_count.end()) {
            for (std::uint32_t j = 0; j < k; ++j) {
                if (m_total_count[j] >= 2) donors.emplace(m_total_count[j], j);
            }
        }
        bool split = false;
        for (std::uint32_t j = 0; j < k && !donors.empty(); ++j) {
            if (m_total_count[j] != 0) continue;
            const std::uint32_t donor = donors.top().second;
            donors.pop();
            float* cd = tr.centroid(donor);
            float* cj = tr.centroid(j);
            for (int d = 0; d < kSh1Dims; ++d) {
                const float offset = (d % 2 == 0) ? tr.split_eps : -tr.split_eps;
                cj[d] = cd[d] + offset;
                cd[d] -= offset;
            }
            m_total_count[j] = m_total_count[donor] / 2;
            m_total_count[donor] -= m_total_count[j];
            if (m_total_count[donor] >= 2) donors.emplace(m_total_count[donor], donor);
            if (m_total_count[j] >= 2) donors.emplace(m_total_count[j], j);
            split = true;
        }
        return split;
    }

private:
    std::vector<std::vector<double>> m_sums;
    std::vector<std::vector<std::uint32_t>> m_counts;
    std::vector<double> m_total;
    std::vector<std::uint64_t> m_total_count;
};

// Lloyd: assign every training point, then move each centroid to the mean of its points.
void train_lloyd(const Training& tr) {
    WorkerPool& pool = *tr.pool;
    LloydUpdate update(pool.threads(), tr.k);
    std::vector<std::uint16_t> labels(tr.m, 0);
    std::vector<std::size_t> changed(pool.threads());
    SoaCentroids soa;
    bool split_last = false;
    for (std::uint32_t epoch = 0; epoch < tr.epochs; ++epoch) {
        fill_soa(soa, tr.centroids->data(), tr.k);
        pool.ranges(tr.m, [&](std::uint32_t t, std::size_t begin, std::size_t end) {
            update.clear(t);
            std::size_t moved = 0;
            constexpr std::size_t kBatch = 1024;
            std::uint16_t fresh[kBatch];
            for (std::size_t b = begin; b < end; b += kBatch) {
                const std::size_t e = std::min(end, b + kBatch);
                assign_range(tr.train, b, e, soa, tr.centroids->data(), fresh, nullptr);
                for (std::size_t i = b; i < e; ++i) {
                    const std::uint16_t label = fresh[i - b];
                    moved += label != labels[i] ? 1 : 0;
                    labels[i] = label;
                    update.add(t, label, tr.point(i));
                }
            }
            changed[t] = moved;
        });
        tr.stats->pairs += static_cast<std::uint64_t>(tr.m) * tr.k;
        std::size_t moved = 0;
        for (std::size_t c : changed) moved += c;
        if (epoch > 0 && moved == 0 && !split_last) break;  // converged: the means would not move
        split_last = update.apply(tr, epoch + 1 < tr.epochs);
        tr.stats->epochs_run = epoch + 1;
    }
}

// Hamerly's bounds (SDM 2010) on the Lloyd iteration. Per point: an upper bound u on the distance
// to its centroid and a lower bound l on the distance to every other one; per centroid: s, the
// distance to its nearest other centroid, so s - u bounds the other distances too (triangle
// inequality). A point keeps its label without a scan when u^2 + slack < max(s - u, l)^2: the
// slack covers the float error of the kernel's scores (|x|^2 + 2 score, cancellation included), so
// a skipped point is one the kernel would have given the same label. Rescans use the same kernel
// (top-2 variant, same comparisons for the best), and the update step is LloydUpdate over the same
// ranges in the same order: labels and centroids equal kLloyd's bit for bit.
void train_hamerly(const Training& tr) {
    constexpr float kSlack = 1e-5f;        // relative to |x|^2 + max |c|^2; >5x the kernel's error bound
    constexpr float kGrow = 1.0f + 1e-6f;  // rounds upper bounds and drifts up
    constexpr float kShrink = 1.0f - 1e-6f;
    WorkerPool& pool = *tr.pool;
    const std::uint32_t threads = pool.threads();
    const std::uint32_t k = tr.k;
    LloydUpdate update(threads, k);
    std::vector<std::uint16_t> labels(tr.m, 0);
    std::vector<float> upper(tr.m), lower(tr.m);
    std::vector<float> s_bound(k), drift(k), previous(tr.centroids->size());
    std::vector<std::size_t> changed(threads);
    std::vector<std::uint64_t> work(threads);
    SoaCentroids soa;
    bool split_last = false;
    for (std::uint32_t epoch = 0; epoch < tr.epochs; ++epoch) {
        fill_soa(soa, tr.centroids->data(), k);
        float c_max = 0.0f;  // max |c|^2
        for (std::uint32_t j = 0; j < k; ++j) c_max = std::max(c_max, 2.0f * soa.half_norm[j]);
        const auto rescan = [&](std::size_t i, const float* x, float x_norm) {
            float best = 0.0f, second = 0.0f;
            const std::uint32_t label = scan<true>(x, soa, &best, &second);
            labels[i] = static_cast<std::uint16_t>(label);
            upper[i] = std::sqrt(exact_sq_dist(x, tr.centroids->data(), label)) * kGrow;
            const float l2 = x_norm + 2.0f * second - kSlack * (x_norm + c_max);
            lower[i] = std::sqrt(std::max(0.0f, l2));
            return label;
        };
        if (epoch == 0) {
            pool.ranges(tr.m, [&](std::uint32_t, std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) rescan(i, tr.point(i), sq_norm(tr.point(i)));
            });
            tr.stats->pairs += static_cast<std::uint64_t>(tr.m) * k;
        } else {
            // s: a lower bound on each centroid's distance to its nearest other centroid, from the
            // second-best score of the centroid against all of them (the best is itself, or an
            // identical copy, which makes the bound 0 as it should be).
            pool.ranges(k, [&](std::uint32_t, std::size_t begin, std::size_t end) {
                for (std::size_t j = begin; j < end; ++j) {
                    const float* c = tr.centroid(static_cast<std::uint32_t>(j));
                    float best = 0.0f, second = 0.0f;
                    scan<true>(c, soa, &best, &second);
                    const float c_norm = 2.0f * soa.half_norm[j];
                    const float d2 = c_norm + 2.0f * second - kSlack * (c_norm + c_max);
                    s_bound[j] = std::sqrt(std::max(0.0f, d2)) * kShrink;
                }
            });
            tr.stats->pairs += static_cast<std::uint64_t>(k) * k;
            pool.ranges(tr.m, [&](std::uint32_t t, std::size_t begin, std::size_t end) {
                std::size_t moved = 0;
                std::uint64_t pairs = 0;
                for (std::size_t i = begin; i < end; ++i) {
                    const std::uint16_t a = labels[i];
                    const float* x = tr.point(i);
                    const float x_norm = sq_norm(x);
                    const float slack = kSlack * (x_norm + c_max);
                    const auto separated = [&](float u) {
                        const float m = std::max(s_bound[a] - u, lower[i]);
                        return m > u && u * u + slack < m * m;
                    };
                    if (separated(upper[i])) continue;
                    upper[i] = std::sqrt(exact_sq_dist(x, tr.centroids->data(), a)) * kGrow;
                    ++pairs;
                    if (separated(upper[i])) continue;
                    moved += rescan(i, x, x_norm) != a ? 1 : 0;
                    pairs += k;
                }
                changed[t] = moved;
                work[t] = pairs;
            });
            for (std::uint64_t w : work) tr.stats->pairs += w;
        }
        // Sums over the same ranges, in the same order, as train_lloyd.
        pool.ranges(tr.m, [&](std::uint32_t t, std::size_t begin, std::size_t end) {
            update.clear(t);
            for (std::size_t i = begin; i < end; ++i) update.add(t, labels[i], tr.point(i));
        });
        std::size_t moved = 0;
        for (std::size_t c : changed) moved += c;
        if (epoch > 0 && moved == 0 && !split_last) break;
        std::copy(tr.centroids->begin(), tr.centroids->end(), previous.begin());
        split_last = update.apply(tr, epoch + 1 < tr.epochs);
        tr.stats->epochs_run = epoch + 1;
        if (epoch + 1 == tr.epochs) break;

        // Bounds follow the centroids: u grows by its centroid's drift, l shrinks by the largest
        // drift of any other centroid.
        float max1 = 0.0f, max2 = 0.0f;
        std::uint32_t arg1 = 0;
        for (std::uint32_t j = 0; j < k; ++j) {
            double d2 = 0.0;
            for (int d = 0; d < kSh1Dims; ++d) {
                const double diff =
                    double(tr.centroid(j)[d]) - previous[static_cast<std::size_t>(kSh1Dims) * j + d];
                d2 += diff * diff;
            }
            drift[j] = static_cast<float>(std::sqrt(d2)) * kGrow;
            if (drift[j] > max1) {
                max2 = max1, max1 = drift[j], arg1 = j;
            } else {
                max2 = std::max(max2, drift[j]);
            }
        }
        pool.ranges(tr.m, [&](std::uint32_t, std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const std::uint16_t a = labels[i];
                upper[i] = (upper[i] + drift[a]) * kGrow;
                lower[i] = (lower[i] - (a == arg1 ? max2 : max1)) * kShrink;
            }
        });
    }
}

// e-means (arXiv 2611.09501 v1, Algorithm 1). Per cluster an EMA count N_j and sum S_j; per
// mini-batch: assign against c_j = S_j / N_j, then N_j <- g N_j + a n_j, S_j <- g S_j + a s_j with
// a = 1 - g from the cosine schedule (Eq. 5); every 5 updates, dead-cluster revival (§3.1, App. H);
// at the end, each centroid is the mean of its last-epoch assignments (§3.2). Batches come from a
// fresh shuffle per epoch; the last partial batch of an epoch is kept (counted in M). N and S are
// doubles (the paper: fp32), so untouched clusters can decay for thousands of steps.
void train_emeans(const Training& tr, std::uint32_t batch, double alpha0) {
    constexpr double kFloor = 0.1;         // cosine floor epsilon (Eq. 5)
    constexpr double kInitCount = 1e-10;   // N_j at initialisation
    constexpr double kDeadFraction = 0.1;  // revival threshold: tau = 0.1 x mean N
    constexpr std::uint32_t kRevivalPeriod = 5;
    constexpr float kDelta = 0x1.0p-7f;  // split perturbation: the bf16 machine epsilon (App. H)
    WorkerPool& pool = *tr.pool;
    std::mt19937_64& rng = *tr.rng;
    const std::uint32_t k = tr.k;
    const std::size_t m = tr.m;
    const std::size_t per_epoch = (m + batch - 1) / batch;
    const double total_updates = static_cast<double>(per_epoch) * tr.epochs;

    std::vector<double> count(k, kInitCount), sum(static_cast<std::size_t>(kSh1Dims) * k);
    for (std::uint32_t j = 0; j < k; ++j) {
        for (int d = 0; d < kSh1Dims; ++d)
            sum[static_cast<std::size_t>(kSh1Dims) * j + d] = kInitCount * tr.centroid(j)[d];
    }
    std::vector<double> last_sum(static_cast<std::size_t>(kSh1Dims) * k, 0.0);
    std::vector<std::uint64_t> last_count(k, 0);
    std::vector<std::uint32_t> order(m);
    std::iota(order.begin(), order.end(), 0u);
    std::vector<float> points(static_cast<std::size_t>(kSh1Dims) * batch);
    std::vector<std::uint16_t> labels(batch);
    std::vector<std::uint32_t> touched_at(k, 0);  // update number + 1 that last touched cluster j
    std::vector<std::uint32_t> touched;
    std::vector<std::uint32_t> dead, donors;
    std::vector<std::pair<double, std::uint32_t>> keys;
    SoaCentroids soa;
    std::uint32_t t = 0;

    const auto set_centroid = [&](std::uint32_t j) {
        float* c = tr.centroid(j);
        for (int d = 0; d < kSh1Dims; ++d)
            c[d] = static_cast<float>(sum[static_cast<std::size_t>(kSh1Dims) * j + d] / count[j]);
    };
    // Revival: clusters with N_j < tau, in index order, each take half of a donor drawn without
    // replacement among the live clusters with probability proportional to N (Efraimidis-Spirakis
    // keys: the largest log(u) / N_j win). The donor's centroid is split multiplicatively with
    // alternating signs (Eq. 8); an all-zero donor is split by an additive offset instead.
    const auto revive = [&]() {
        double total = 0.0;
        for (double n : count) total += n;
        const double tau = kDeadFraction * total / k;
        dead.clear();
        keys.clear();
        for (std::uint32_t j = 0; j < k; ++j) {
            if (count[j] < tau) {
                dead.push_back(j);
            } else if (count[j] > 0.0) {
                keys.emplace_back(std::log(open_unit(rng)) / count[j], j);
            }
        }
        if (dead.empty() || keys.empty()) return;
        const std::size_t splits = std::min(dead.size(), keys.size());
        std::partial_sort(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(splits), keys.end(),
                          [](const auto& a, const auto& b) {
                              return a.first != b.first ? a.first > b.first : a.second < b.second;
                          });
        for (std::size_t s = 0; s < splits; ++s) {
            const std::uint32_t j = dead[s];
            const std::uint32_t donor = keys[s].second;
            float* cd = tr.centroid(donor);
            float* cj = tr.centroid(j);
            bool distinct = false;
            for (int d = 0; d < kSh1Dims; ++d) {
                const float sign = (d % 2 == 0) ? 1.0f : -1.0f;
                cj[d] = cd[d] * (1.0f + sign * kDelta);
                cd[d] = cd[d] * (1.0f - sign * kDelta);
                distinct = distinct || cj[d] != cd[d];
            }
            if (!distinct) cj[0] += tr.split_eps, cd[0] -= tr.split_eps;
            const double half = 0.5 * count[donor];
            for (const std::uint32_t c : {j, donor}) {
                count[c] = half;
                for (int d = 0; d < kSh1Dims; ++d)
                    sum[static_cast<std::size_t>(kSh1Dims) * c + d] = half * tr.centroid(c)[d];
            }
        }
        tr.stats->revived += static_cast<std::uint32_t>(splits);
    };

    for (std::uint32_t epoch = 0; epoch < tr.epochs; ++epoch) {
        for (std::size_t i = m; i > 1; --i) {  // Fisher-Yates
            std::swap(order[i - 1], order[bounded(rng, i)]);
        }
        const bool last_epoch = epoch + 1 == tr.epochs;
        for (std::size_t b0 = 0; b0 < m; b0 += batch) {
            const std::size_t bn = std::min<std::size_t>(batch, m - b0);
            for (std::size_t p = 0; p < bn; ++p)
                std::copy_n(tr.point(order[b0 + p]), kSh1Dims,
                            points.data() + static_cast<std::size_t>(kSh1Dims) * p);
            fill_soa(soa, tr.centroids->data(), k);
            pool.dynamic(bn, [&](std::size_t begin, std::size_t end) {
                assign_range(points.data(), begin, end, soa, tr.centroids->data(), labels.data() + begin,
                             nullptr);
            });
            tr.stats->pairs += static_cast<std::uint64_t>(bn) * k;

            const double alpha =
                alpha0 *
                (kFloor + (1.0 - kFloor) * 0.5 * (1.0 + std::cos(std::numbers::pi * t / total_updates)));
            const double gamma = 1.0 - alpha;
            for (double& n : count) n *= gamma;
            for (double& s : sum) s *= gamma;
            touched.clear();
            for (std::size_t p = 0; p < bn; ++p) {  // in batch order: independent of the thread count
                const std::uint16_t a = labels[p];
                const float* x = points.data() + static_cast<std::size_t>(kSh1Dims) * p;
                count[a] += alpha;
                double* s = sum.data() + static_cast<std::size_t>(kSh1Dims) * a;
                for (int d = 0; d < kSh1Dims; ++d) s[d] += alpha * x[d];
                if (last_epoch) {
                    ++last_count[a];
                    double* ls = last_sum.data() + static_cast<std::size_t>(kSh1Dims) * a;
                    for (int d = 0; d < kSh1Dims; ++d) ls[d] += x[d];
                }
                if (touched_at[a] != t + 1) touched_at[a] = t + 1, touched.push_back(a);
            }
            for (const std::uint32_t j : touched) set_centroid(j);
            ++t;
            if (t % kRevivalPeriod == 0) revive();
        }
    }
    // Last-epoch mean; a cluster with no last-epoch points keeps its EMA centroid.
    for (std::uint32_t j = 0; j < k; ++j) {
        if (last_count[j] == 0) continue;
        float* c = tr.centroid(j);
        for (int d = 0; d < kSh1Dims; ++d) {
            c[d] = static_cast<float>(last_sum[static_cast<std::size_t>(kSh1Dims) * j + d] /
                                      static_cast<double>(last_count[j]));
        }
    }
    tr.stats->updates = t;
    tr.stats->epochs_run = tr.epochs;
}

double emeans_step(double n, double k, double batch, double epochs) {
    // Eq. 6 with Table 2's constants.
    return std::min(1.0, 0.824 * std::pow(k, -0.46) * std::pow(batch, 0.90) * std::pow(n, -0.45) *
                             std::pow(epochs, -0.11));
}

}  // namespace

const char* palette_trainer_name(PaletteTrainer trainer) {
    switch (trainer) {
        case PaletteTrainer::kLloyd:
            return "lloyd";
        case PaletteTrainer::kHamerly:
            return "hamerly";
        case PaletteTrainer::kEMeans:
            return "emeans";
    }
    return "?";
}

EMeansSchedule emeans_schedule(std::size_t n, std::uint32_t k, std::uint32_t epochs) {
    EMeansSchedule schedule;
    if (n == 0 || k == 0) return schedule;
    const double nd = static_cast<double>(n), kd = k, td = std::max(1u, epochs);
    // Eq. 7, "knee / 8" row of Table 2.
    const double raw = 0.0044 * std::pow(kd, 0.13) * std::pow(nd, 0.88) * std::pow(td, 0.81);
    std::uint64_t batch = 1;
    if (raw > 1.0) {
        const auto lo = static_cast<std::uint64_t>(1) << static_cast<int>(std::floor(std::log2(raw)));
        batch = raw / static_cast<double>(lo) < static_cast<double>(2 * lo) / raw ? lo : 2 * lo;
    }
    batch = std::clamp<std::uint64_t>(batch, 1, n);
    schedule.batch =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(batch, std::numeric_limits<std::uint32_t>::max()));
    schedule.alpha0 = emeans_step(nd, kd, schedule.batch, td);
    return schedule;
}

void assign_nearest(const float* points, std::size_t n, const float* centroids, std::uint32_t k,
                    std::uint16_t* labels, float* sq_dist, std::uint32_t threads) {
    if (n == 0 || k == 0) return;
    SoaCentroids soa;
    fill_soa(soa, centroids, k);
    WorkerPool pool(resolve_threads(threads, n));
    pool.dynamic(n, [&](std::size_t begin, std::size_t end) {
        assign_range(points, begin, end, soa, centroids, labels + begin,
                     sq_dist != nullptr ? sq_dist + begin : nullptr);
    });
}

Result<Sh1Palette> train_sh1_palette(const SplatCloud& cloud, const PaletteParams& params) {
    if (cloud.sh_degree != 1) {
        return Result<Sh1Palette>::failure("SH1 palette needs a cloud at SH degree 1, got degree " +
                                           std::to_string(cloud.sh_degree));
    }
    if (!cloud.valid_sizes()) return Result<Sh1Palette>::failure("SplatCloud has inconsistent array sizes");
    return train_sh1_palette(cloud.sh.data(), cloud.count, params);
}

Result<Sh1Palette> train_sh1_palette(const float* sh1, std::size_t count, const PaletteParams& params) {
    using R = Result<Sh1Palette>;
    if (count == 0 || sh1 == nullptr) return R::failure("SH1 palette needs at least one splat");
    if (params.k == 0 || params.k > kMaxPaletteSize) {
        return R::failure("palette size must be 1.." + std::to_string(kMaxPaletteSize) + ", got " +
                          std::to_string(params.k));
    }
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        return R::failure("splat count exceeds the palette's 32-bit index range");
    }
    const auto start = std::chrono::steady_clock::now();
    std::mt19937_64 rng(params.seed);
    const std::size_t n = count;

    // Training set: every splat, or a seeded sample of train_cap splats kept in pack order.
    std::vector<float> sample;
    const float* train = sh1;
    std::size_t m = n;
    if (params.train_cap != 0 && params.train_cap < n) {
        std::vector<std::uint32_t> idx = sample_distinct(rng, n, params.train_cap);
        std::sort(idx.begin(), idx.end());
        m = idx.size();
        sample.resize(static_cast<std::size_t>(kSh1Dims) * m);
        for (std::size_t s = 0; s < m; ++s) {
            std::copy_n(sh1 + static_cast<std::size_t>(kSh1Dims) * idx[s], kSh1Dims,
                        sample.data() + static_cast<std::size_t>(kSh1Dims) * s);
        }
        train = sample.data();
    }
    const auto k = static_cast<std::uint32_t>(std::min<std::size_t>(params.k, m));

    Sh1Palette palette;
    palette.centroids.resize(static_cast<std::size_t>(kSh1Dims) * k);
    {
        const std::vector<std::uint32_t> init = sample_distinct(rng, m, k);
        for (std::uint32_t j = 0; j < k; ++j) {
            std::copy_n(train + static_cast<std::size_t>(kSh1Dims) * init[j], kSh1Dims,
                        palette.centroids.data() + static_cast<std::size_t>(kSh1Dims) * j);
        }
    }
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kSh1Dims) * m; ++i)
        sum_sq += double(train[i]) * train[i];

    WorkerPool pool(resolve_threads(params.threads, m));
    Training tr;
    tr.train = train;
    tr.m = m;
    tr.k = k;
    tr.epochs = params.epochs;
    tr.split_eps = std::max(1e-7f, 1e-4f * static_cast<float>(std::sqrt(sum_sq / static_cast<double>(m))));
    tr.pool = &pool;
    tr.rng = &rng;
    tr.centroids = &palette.centroids;
    tr.stats = &palette.stats;
    switch (params.trainer) {
        case PaletteTrainer::kLloyd:
            train_lloyd(tr);
            break;
        case PaletteTrainer::kHamerly:
            train_hamerly(tr);
            break;
        case PaletteTrainer::kEMeans: {
            if (params.epochs == 0) break;
            const EMeansSchedule law = emeans_schedule(m, k, params.epochs);
            const std::uint32_t batch =
                params.batch != 0 ? static_cast<std::uint32_t>(std::min<std::size_t>(params.batch, m))
                                  : law.batch;
            const double alpha0 =
                params.alpha0 > 0.0
                    ? std::min(1.0, params.alpha0)
                    : (params.batch != 0 ? emeans_step(double(m), k, batch, params.epochs) : law.alpha0);
            palette.stats.batch = batch;
            palette.stats.alpha0 = alpha0;
            train_emeans(tr, batch, alpha0);
            break;
        }
    }
    palette.stats.train_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    // Final assignment of every splat against the trained palette.
    palette.labels.resize(n);
    std::vector<float> sq_dist(n);
    {
        SoaCentroids soa;
        fill_soa(soa, palette.centroids.data(), k);
        WorkerPool final_pool(resolve_threads(params.threads, n));
        final_pool.dynamic(n, [&](std::size_t begin, std::size_t end) {
            assign_range(sh1, begin, end, soa, palette.centroids.data(), palette.labels.data() + begin,
                         sq_dist.data() + begin);
        });
    }
    double err = 0.0;
    std::vector<std::uint64_t> used(k, 0);
    for (std::size_t i = 0; i < n; ++i) {
        err += sq_dist[i];
        ++used[palette.labels[i]];
    }
    PaletteStats& stats = palette.stats;
    stats.mse = err / static_cast<double>(n);
    for (std::uint64_t u : used) {
        if (u == 0) {
            ++stats.dead;
            continue;
        }
        const double p = static_cast<double>(u) / static_cast<double>(n);
        stats.used_entropy -= p * std::log2(p);
    }
    stats.wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return R::success(std::move(palette));
}

Status apply_sh1_palette(SplatCloud& cloud, const Sh1Palette& palette) {
    if (cloud.sh_degree != 1 || !cloud.valid_sizes())
        return Status::failure("cloud is not a valid SH degree 1 cloud");
    if (palette.labels.size() != cloud.count) {
        return Status::failure("palette labels (" + std::to_string(palette.labels.size()) +
                               ") do not match the splat count (" + std::to_string(cloud.count) + ")");
    }
    const std::uint32_t k = palette.size();
    for (std::size_t i = 0; i < cloud.count; ++i) {
        const std::uint16_t label = palette.labels[i];
        if (label >= k) return Status::failure("palette label out of range");
        std::copy_n(palette.centroids.data() + static_cast<std::size_t>(kSh1Dims) * label, kSh1Dims,
                    cloud.sh.data() + static_cast<std::size_t>(kSh1Dims) * i);
    }
    return Status::success();
}

}  // namespace splatcore
