#include "splatcore/palette.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>  // SSE2: part of the x86-64 baseline, no compiler flag needed
#define SPLATCORE_PALETTE_SSE2 1
#endif

namespace splatcore {
namespace {

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

SoaCentroids to_soa(const float* aos, std::uint32_t k) {
    SoaCentroids soa;
    soa.k = k;
    soa.stride = (k + 7u) & ~7u;
    soa.c.assign(static_cast<std::size_t>(kSh1Dims) * soa.stride, 0.0f);
    soa.half_norm.assign(soa.stride, std::numeric_limits<float>::infinity());
    for (std::uint32_t j = 0; j < k; ++j) {
        float norm = 0.0f;
        for (int d = 0; d < kSh1Dims; ++d) {
            const float v = aos[static_cast<std::size_t>(kSh1Dims) * j + d];
            soa.c[static_cast<std::size_t>(d) * soa.stride + j] = v;
            norm += v * v;
        }
        soa.half_norm[j] = 0.5f * norm;
    }
    return soa;
}

void exact_sq_dist(const float* x, const float* aos, std::uint32_t label, float* out) {
    const float* c = aos + static_cast<std::size_t>(kSh1Dims) * label;
    float d2 = 0.0f;
    for (int d = 0; d < kSh1Dims; ++d) d2 += (x[d] - c[d]) * (x[d] - c[d]);
    *out = d2;
}

#if defined(SPLATCORE_PALETTE_SSE2)

// SSE2 kernel: two vectors of four centroids per step, each lane keeps its best score and index
// (strictly smaller wins, so a lane keeps its lowest index on ties); the eight lanes are merged at
// the end with the same rule. The dot product is summed as a tree (the order the portable kernel
// uses too) so the add latency does not serialize the loop. Explicit because MSVC's
// auto-vectorized loop ran at ~6.5 cycles per pair on the dev laptop.
struct Lanes {
    __m128 best = _mm_set1_ps(std::numeric_limits<float>::infinity());
    __m128i best_idx = _mm_setzero_si128();
};

inline void keep_better(Lanes& lanes, __m128 score, __m128i idx) {
    const __m128 better = _mm_cmplt_ps(score, lanes.best);
    lanes.best = _mm_or_ps(_mm_and_ps(better, score), _mm_andnot_ps(better, lanes.best));
    const __m128i take = _mm_castps_si128(better);
    lanes.best_idx = _mm_or_si128(_mm_and_si128(take, idx), _mm_andnot_si128(take, lanes.best_idx));
}

void assign_range(const float* points, std::size_t begin, std::size_t end, const SoaCentroids& soa,
                  const float* aos, std::uint16_t* labels, float* sq_dist) {
    const std::size_t s = soa.stride;
    const float* c = soa.c.data();
    const float* hn = soa.half_norm.data();
    for (std::size_t i = begin; i < end; ++i) {
        const float* x = points + static_cast<std::size_t>(kSh1Dims) * i;
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
            keep_better(lo, score4(j), idx);
            keep_better(hi, score4(j + 4), _mm_add_epi32(idx, four));
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
        const auto label = static_cast<std::uint32_t>(lane_idx[pick]);
        labels[i - begin] = static_cast<std::uint16_t>(label);
        if (sq_dist != nullptr) exact_sq_dist(x, aos, label, sq_dist + (i - begin));
    }
}

#else

// Portable kernel (arm64 for now; a NEON version comes with the phone measurements of Phase C,
// where it can be tested on the device). Same scores in the same order of operations.
void assign_range(const float* points, std::size_t begin, std::size_t end, const SoaCentroids& soa,
                  const float* aos, std::uint16_t* labels, float* sq_dist) {
    const std::size_t s = soa.stride;
    const float* c = soa.c.data();
    for (std::size_t i = begin; i < end; ++i) {
        const float* x = points + static_cast<std::size_t>(kSh1Dims) * i;
        float best = std::numeric_limits<float>::infinity();
        std::uint32_t best_j = 0;
        for (std::uint32_t j = 0; j < soa.k; ++j) {
            const auto t = [&](int d) { return x[d] * c[static_cast<std::size_t>(d) * s + j]; };
            const float dot = ((t(0) + t(1)) + (t(2) + t(3))) + ((t(4) + t(5)) + (t(6) + t(7))) + t(8);
            const float score = soa.half_norm[j] - dot;
            if (score < best) best = score, best_j = j;
        }
        labels[i - begin] = static_cast<std::uint16_t>(best_j);
        if (sq_dist != nullptr) exact_sq_dist(x, aos, best_j, sq_dist + (i - begin));
    }
}

#endif

std::uint32_t resolve_threads(std::uint32_t requested, std::size_t n) {
    std::size_t t = requested != 0 ? requested : std::max(1u, std::thread::hardware_concurrency());
    t = std::min<std::size_t>(t, std::max<std::size_t>(1, n));
    return static_cast<std::uint32_t>(t);
}

// Runs fn(thread, begin, end) over `threads` contiguous, equal ranges of [0, n).
template <typename Fn>
void parallel_ranges(std::size_t n, std::uint32_t threads, Fn&& fn) {
    const std::size_t per = (n + threads - 1) / threads;
    if (threads == 1) {
        fn(0u, std::size_t{0}, n);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
        const std::size_t begin = std::min(n, per * t);
        const std::size_t end = std::min(n, begin + per);
        pool.emplace_back([&fn, t, begin, end] { fn(t, begin, end); });
    }
    for (std::thread& th : pool) th.join();
}

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

}  // namespace

void assign_nearest(const float* points, std::size_t n, const float* centroids, std::uint32_t k,
                    std::uint16_t* labels, float* sq_dist, std::uint32_t threads) {
    if (n == 0 || k == 0) return;
    const SoaCentroids soa = to_soa(centroids, k);
    parallel_ranges(n, resolve_threads(threads, n), [&](std::uint32_t, std::size_t begin, std::size_t end) {
        assign_range(points, begin, end, soa, centroids, labels + begin,
                     sq_dist != nullptr ? sq_dist + begin : nullptr);
    });
}

Result<Sh1Palette> train_sh1_palette(const SplatCloud& cloud, const PaletteParams& params) {
    using R = Result<Sh1Palette>;
    if (cloud.sh_degree != 1) {
        return R::failure("SH1 palette needs a cloud at SH degree 1, got degree " +
                          std::to_string(cloud.sh_degree));
    }
    if (!cloud.valid_sizes()) return R::failure("SplatCloud has inconsistent array sizes");
    if (cloud.count == 0) return R::failure("SH1 palette needs at least one splat");
    if (params.k == 0 || params.k > kMaxPaletteSize) {
        return R::failure("palette size must be 1.." + std::to_string(kMaxPaletteSize) + ", got " +
                          std::to_string(params.k));
    }
    if (cloud.count > std::numeric_limits<std::uint32_t>::max()) {
        return R::failure("splat count exceeds the palette's 32-bit index range");
    }
    const auto start = std::chrono::steady_clock::now();
    std::mt19937_64 rng(params.seed);
    const std::size_t n = cloud.count;

    // Training set: every splat, or a seeded sample of train_cap splats kept in pack order.
    std::vector<float> sample;
    const float* train = cloud.sh.data();
    std::size_t m = n;
    if (params.train_cap != 0 && params.train_cap < n) {
        std::vector<std::uint32_t> idx = sample_distinct(rng, n, params.train_cap);
        std::sort(idx.begin(), idx.end());
        m = idx.size();
        sample.resize(static_cast<std::size_t>(kSh1Dims) * m);
        for (std::size_t s = 0; s < m; ++s) {
            std::copy_n(cloud.sh.data() + static_cast<std::size_t>(kSh1Dims) * idx[s], kSh1Dims,
                        sample.data() + static_cast<std::size_t>(kSh1Dims) * s);
        }
        train = sample.data();
    }
    const auto k = static_cast<std::uint32_t>(std::min<std::size_t>(params.k, m));
    const std::uint32_t threads = resolve_threads(params.threads, m);

    Sh1Palette palette;
    palette.centroids.resize(static_cast<std::size_t>(kSh1Dims) * k);
    {
        const std::vector<std::uint32_t> init = sample_distinct(rng, m, k);
        for (std::uint32_t j = 0; j < k; ++j) {
            std::copy_n(train + static_cast<std::size_t>(kSh1Dims) * init[j], kSh1Dims,
                        palette.centroids.data() + static_cast<std::size_t>(kSh1Dims) * j);
        }
    }

    // Split offset for empty clusters: tiny against the data's spread, never zero.
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kSh1Dims) * m; ++i)
        sum_sq += double(train[i]) * train[i];
    const float split_eps =
        std::max(1e-7f, 1e-4f * static_cast<float>(std::sqrt(sum_sq / static_cast<double>(m))));

    // Lloyd: assign every training point, then move each centroid to the mean of its points.
    // Per-thread double sums over fixed ranges, reduced in thread order: deterministic for a
    // fixed thread count.
    std::vector<std::uint16_t> labels(m, 0);
    std::vector<std::vector<double>> sums(threads,
                                          std::vector<double>(static_cast<std::size_t>(kSh1Dims) * k));
    std::vector<std::vector<std::uint32_t>> counts(threads, std::vector<std::uint32_t>(k));
    std::vector<std::size_t> changed(threads);
    std::vector<double> total(static_cast<std::size_t>(kSh1Dims) * k);
    std::vector<std::uint64_t> total_count(k);
    bool split_last = false;
    std::uint32_t epochs_run = 0;
    for (std::uint32_t epoch = 0; epoch < params.epochs; ++epoch) {
        const SoaCentroids soa = to_soa(palette.centroids.data(), k);
        parallel_ranges(m, threads, [&](std::uint32_t t, std::size_t begin, std::size_t end) {
            std::vector<double>& sum = sums[t];
            std::vector<std::uint32_t>& cnt = counts[t];
            std::fill(sum.begin(), sum.end(), 0.0);
            std::fill(cnt.begin(), cnt.end(), 0u);
            std::size_t moved = 0;
            constexpr std::size_t kBatch = 1024;
            std::uint16_t fresh[kBatch];
            for (std::size_t b = begin; b < end; b += kBatch) {
                const std::size_t e = std::min(end, b + kBatch);
                assign_range(train, b, e, soa, palette.centroids.data(), fresh, nullptr);
                for (std::size_t i = b; i < e; ++i) {
                    const std::uint16_t label = fresh[i - b];
                    moved += label != labels[i] ? 1 : 0;
                    labels[i] = label;
                    ++cnt[label];
                    const float* x = train + static_cast<std::size_t>(kSh1Dims) * i;
                    double* s = sum.data() + static_cast<std::size_t>(kSh1Dims) * label;
                    for (int d = 0; d < kSh1Dims; ++d) s[d] += x[d];
                }
            }
            changed[t] = moved;
        });
        std::size_t moved = 0;
        for (std::size_t c : changed) moved += c;
        if (epoch > 0 && moved == 0 && !split_last) break;  // converged: the means would not move

        std::fill(total.begin(), total.end(), 0.0);
        std::fill(total_count.begin(), total_count.end(), 0u);
        for (std::uint32_t t = 0; t < threads; ++t) {
            for (std::size_t i = 0; i < total.size(); ++i) total[i] += sums[t][i];
            for (std::uint32_t j = 0; j < k; ++j) total_count[j] += counts[t][j];
        }
        for (std::uint32_t j = 0; j < k; ++j) {
            if (total_count[j] == 0) continue;
            for (int d = 0; d < kSh1Dims; ++d) {
                palette.centroids[static_cast<std::size_t>(kSh1Dims) * j + d] = static_cast<float>(
                    total[static_cast<std::size_t>(kSh1Dims) * j + d] / static_cast<double>(total_count[j]));
            }
        }
        // Empty clusters: split the most populated cluster into two copies offset by +-eps along
        // an alternating-sign direction, so the next assignment divides its points. Not after the
        // last update: the palette returned is always the means of its clusters.
        // Donors come from a max-heap on (count, lowest index first).
        split_last = false;
        using Donor = std::pair<std::uint64_t, std::uint32_t>;
        const auto later = [](const Donor& a, const Donor& b) {
            return a.first != b.first ? a.first < b.first : a.second > b.second;
        };
        std::priority_queue<Donor, std::vector<Donor>, decltype(later)> donors(later);
        if (epoch + 1 < params.epochs &&
            std::find(total_count.begin(), total_count.end(), 0u) != total_count.end()) {
            for (std::uint32_t j = 0; j < k; ++j) {
                if (total_count[j] >= 2) donors.emplace(total_count[j], j);
            }
        }
        for (std::uint32_t j = 0; j < k && !donors.empty(); ++j) {
            if (total_count[j] != 0) continue;
            const std::uint32_t donor = donors.top().second;
            donors.pop();
            float* cd = palette.centroids.data() + static_cast<std::size_t>(kSh1Dims) * donor;
            float* cj = palette.centroids.data() + static_cast<std::size_t>(kSh1Dims) * j;
            for (int d = 0; d < kSh1Dims; ++d) {
                const float offset = (d % 2 == 0) ? split_eps : -split_eps;
                cj[d] = cd[d] + offset;
                cd[d] -= offset;
            }
            total_count[j] = total_count[donor] / 2;
            total_count[donor] -= total_count[j];
            if (total_count[donor] >= 2) donors.emplace(total_count[donor], donor);
            if (total_count[j] >= 2) donors.emplace(total_count[j], j);
            split_last = true;
        }
        epochs_run = epoch + 1;
    }

    // Final assignment of every splat against the trained palette.
    palette.labels.resize(n);
    std::vector<float> sq_dist(n);
    assign_nearest(cloud.sh.data(), n, palette.centroids.data(), k, palette.labels.data(), sq_dist.data(),
                   params.threads);
    double err = 0.0;
    std::vector<std::uint64_t> used(k, 0);
    for (std::size_t i = 0; i < n; ++i) {
        err += sq_dist[i];
        ++used[palette.labels[i]];
    }
    PaletteStats& stats = palette.stats;
    stats.mse = err / static_cast<double>(n);
    stats.epochs_run = epochs_run;
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
