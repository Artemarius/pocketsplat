#include "splatcore/cleaning.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>

#include "splatcore/stats.h"

namespace splatcore {
namespace {

// Removes all splats with keep[i] == 0, preserving the order of survivors. Returns removed count.
std::size_t compact(SplatCloud& cloud, const std::vector<char>& keep) {
    const int rest3 = 3 * sh_rest_coeffs(cloud.sh_degree);
    std::size_t out = 0;
    for (std::size_t i = 0; i < cloud.count; ++i) {
        if (!keep[i]) continue;
        if (out != i) {
            for (int c = 0; c < 3; ++c) {
                cloud.positions[3 * out + c] = cloud.positions[3 * i + c];
                cloud.log_scales[3 * out + c] = cloud.log_scales[3 * i + c];
                cloud.dc[3 * out + c] = cloud.dc[3 * i + c];
            }
            for (int c = 0; c < 4; ++c) cloud.rotations[4 * out + c] = cloud.rotations[4 * i + c];
            cloud.opacities[out] = cloud.opacities[i];
            for (int c = 0; c < rest3; ++c) {
                cloud.sh[static_cast<std::size_t>(rest3) * out + static_cast<std::size_t>(c)] =
                    cloud.sh[static_cast<std::size_t>(rest3) * i + static_cast<std::size_t>(c)];
            }
        }
        ++out;
    }
    const std::size_t removed = cloud.count - out;
    cloud.resize(out, cloud.sh_degree);
    return removed;
}

float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

// Runs f(begin, end) over [0, n) split across hardware threads. Used for the read-only kNN pass.
template <typename F>
void parallel_for(std::size_t n, F&& f) {
    const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const std::size_t chunks = std::min(hw, std::max<std::size_t>(1, n / 4096));
    if (chunks <= 1) {
        f(std::size_t{0}, n);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(chunks);
    const std::size_t per = (n + chunks - 1) / chunks;
    for (std::size_t c = 0; c < chunks; ++c) {
        const std::size_t begin = c * per;
        const std::size_t end = std::min(n, begin + per);
        if (begin >= end) break;
        threads.emplace_back([&f, begin, end] { f(begin, end); });
    }
    for (auto& t : threads) t.join();
}

// x -> every third bit of a 30-bit code (standard Morton "part1by2" bit spreading).
std::uint32_t part1by2(std::uint32_t x) {
    x &= 0x3ff;
    x = (x | (x << 16)) & 0x030000ff;
    x = (x | (x << 8)) & 0x0300f00f;
    x = (x | (x << 4)) & 0x030c30c3;
    x = (x | (x << 2)) & 0x09249249;
    return x;
}

}  // namespace

float Aabb::diagonal() const {
    const float dx = max[0] - min[0], dy = max[1] - min[1], dz = max[2] - min[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Aabb robust_aabb(const SplatCloud& cloud) {
    Aabb box;
    std::vector<float> axis;
    axis.reserve(cloud.count);
    for (int c = 0; c < 3; ++c) {
        axis.clear();
        for (std::size_t i = 0; i < cloud.count; ++i) {
            const float v = cloud.positions[3 * i + static_cast<std::size_t>(c)];
            if (std::isfinite(v)) axis.push_back(v);
        }
        if (axis.empty()) {
            box.min[c] = box.max[c] = 0.0f;
            continue;
        }
        std::size_t lo = 0, hi = axis.size() - 1;
        if (axis.size() >= 100) {
            lo = axis.size() / 100;
            hi = axis.size() - 1 - axis.size() / 100;
        }
        std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(lo), axis.end());
        box.min[c] = axis[lo];
        std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(hi), axis.end());
        box.max[c] = axis[hi];
    }
    return box;
}

float importance(const SplatCloud& cloud, std::size_t i) {
    const float alpha = sigmoid(cloud.opacities[i]);
    const float sx = std::exp(cloud.log_scales[3 * i]);
    const float sy = std::exp(cloud.log_scales[3 * i + 1]);
    const float sz = std::exp(cloud.log_scales[3 * i + 2]);
    return alpha * (sx * sy + sy * sz + sz * sx);
}

CleanOpStats drop_invalid(SplatCloud& cloud) {
    CleanOpStats stats{"drop_invalid", cloud.count, 0, ""};
    std::vector<char> keep(cloud.count, 1);
    for (std::size_t i = 0; i < cloud.count; ++i) {
        if (!splat_is_finite(cloud, i)) {
            keep[i] = 0;
            continue;
        }
        const float* q = cloud.rotations.data() + 4 * i;
        const float norm2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (norm2 < 1e-24f) keep[i] = 0;
        for (int c = 0; c < 3; ++c) {
            if (!(std::exp(cloud.log_scales[3 * i + static_cast<std::size_t>(c)]) > 0.0f)) keep[i] = 0;
        }
    }
    stats.removed = compact(cloud, keep);
    return stats;
}

CleanOpStats prune_opacity(SplatCloud& cloud, float min_alpha) {
    CleanOpStats stats{"prune_opacity", cloud.count, 0, "min_alpha=" + std::to_string(min_alpha)};
    std::vector<char> keep(cloud.count, 1);
    for (std::size_t i = 0; i < cloud.count; ++i) {
        if (sigmoid(cloud.opacities[i]) < min_alpha) keep[i] = 0;
    }
    stats.removed = compact(cloud, keep);
    return stats;
}

CleanOpStats prune_scale(SplatCloud& cloud, const Aabb& scene, float max_diag_fraction,
                         float min_diag_fraction) {
    const float diag = scene.diagonal();
    CleanOpStats stats{"prune_scale", cloud.count, 0,
                       "max_diag_fraction=" + std::to_string(max_diag_fraction) + " min_diag_fraction=" +
                           std::to_string(min_diag_fraction) + " diag=" + std::to_string(diag)};
    const float max_extent = max_diag_fraction * diag;
    const float min_extent = min_diag_fraction * diag;
    std::vector<char> keep(cloud.count, 1);
    for (std::size_t i = 0; i < cloud.count; ++i) {
        const float sx = std::exp(cloud.log_scales[3 * i]);
        const float sy = std::exp(cloud.log_scales[3 * i + 1]);
        const float sz = std::exp(cloud.log_scales[3 * i + 2]);
        if (std::max({sx, sy, sz}) > max_extent || std::min({sx, sy, sz}) < min_extent) keep[i] = 0;
    }
    stats.removed = compact(cloud, keep);
    return stats;
}

CleanOpStats clamp_needle_scales(SplatCloud& cloud, const Aabb& scene, float len_cap_fraction,
                                 float aspect_cap) {
    const float len_cap = len_cap_fraction * scene.diagonal();
    std::size_t clamped = 0;
    for (std::size_t i = 0; i < cloud.count; ++i) {
        float s[3];
        for (int c = 0; c < 3; ++c) s[c] = std::exp(cloud.log_scales[3 * i + static_cast<std::size_t>(c)]);
        int max_c = 0;
        for (int c = 1; c < 3; ++c) {
            if (s[c] > s[max_c]) max_c = c;
        }
        const float mid = std::max(s[(max_c + 1) % 3], s[(max_c + 2) % 3]);
        const float cap = std::max(len_cap, aspect_cap * mid);
        if (s[max_c] > cap) {
            cloud.log_scales[3 * i + static_cast<std::size_t>(max_c)] = std::log(cap);
            ++clamped;
        }
    }
    return CleanOpStats{"clamp_needle_scales", cloud.count, 0,
                        "len_cap_fraction=" + std::to_string(len_cap_fraction) + " aspect_cap=" +
                            std::to_string(aspect_cap) + " clamped=" + std::to_string(clamped)};
}

CleanOpStats remove_floaters_sor(SplatCloud& cloud, int k_neighbors, float stddev_multiplier) {
    CleanOpStats stats{"remove_floaters_sor", cloud.count, 0,
                       "k=" + std::to_string(k_neighbors) + " m=" + std::to_string(stddev_multiplier)};
    const std::size_t n = cloud.count;
    if (n < static_cast<std::size_t>(k_neighbors) + 1) return stats;

    // The grid spans the FULL extent: clamping far-away points (sky shells, background) into
    // boundary cells would pile thousands of points into single cells and make the scan quadratic.
    Aabb box{{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
              std::numeric_limits<float>::max()},
             {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
              std::numeric_limits<float>::lowest()}};
    for (std::size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = cloud.positions[3 * i + static_cast<std::size_t>(c)];
            box.min[c] = std::min(box.min[c], v);
            box.max[c] = std::max(box.max[c], v);
        }
    }

    // Initial cell size ~2x the average spacing assuming the bulk (robust box) fills its volume.
    // Real captures are surface-dominated, which makes this a big over-estimate (hundreds of
    // points per cell on a ground plane), so measure the occupancy and shrink until occupied
    // cells hold a comfortable count for kNN gathering.
    const Aabb robust = robust_aabb(cloud);
    float cell =
        std::max(2.0f * std::cbrt(std::max((robust.max[0] - robust.min[0]), 1e-9f) *
                                  std::max((robust.max[1] - robust.min[1]), 1e-9f) *
                                  std::max((robust.max[2] - robust.min[2]), 1e-9f) / static_cast<float>(n)),
                 1e-9f);
    // Up to 2^20 cells per axis, packed into an exact 60-bit key (no hash collisions).
    // static: std::clamp odr-uses it (by const&) inside the grid_dim lambda below, and a
    // non-static constexpr local would need capturing there (clang enforces this, MSVC did not).
    static constexpr std::int64_t kMaxCells = std::int64_t{1} << 20;
    std::int64_t nx = 1, ny = 1, nz = 1;
    const auto cell_of = [&](std::size_t i) -> std::uint64_t {
        const float* p = cloud.positions.data() + 3 * i;
        const auto ix =
            std::clamp(static_cast<std::int64_t>((p[0] - box.min[0]) / cell), std::int64_t{0}, nx - 1);
        const auto iy =
            std::clamp(static_cast<std::int64_t>((p[1] - box.min[1]) / cell), std::int64_t{0}, ny - 1);
        const auto iz =
            std::clamp(static_cast<std::int64_t>((p[2] - box.min[2]) / cell), std::int64_t{0}, nz - 1);
        return (static_cast<std::uint64_t>(ix) << 40) | (static_cast<std::uint64_t>(iy) << 20) |
               static_cast<std::uint64_t>(iz);
    };

    // CSR layout over occupied cells, rebuilt at most 3 times while adapting the cell size.
    std::unordered_map<std::uint64_t, std::uint32_t> slot_of;
    std::vector<std::uint32_t> cell_slot(n);
    for (int attempt = 0;; ++attempt) {
        slot_of.clear();
        const auto grid_dim = [cell](float lo, float hi) {
            return std::clamp(static_cast<std::int64_t>((hi - lo) / cell) + 1, std::int64_t{1}, kMaxCells);
        };
        nx = grid_dim(box.min[0], box.max[0]);
        ny = grid_dim(box.min[1], box.max[1]);
        nz = grid_dim(box.min[2], box.max[2]);
        for (std::size_t i = 0; i < n; ++i) {
            const auto result = slot_of.emplace(cell_of(i), static_cast<std::uint32_t>(slot_of.size()));
            cell_slot[i] = result.first->second;
        }
        const float occupancy = static_cast<float>(n) / static_cast<float>(slot_of.size());
        if (occupancy <= 24.0f || attempt >= 2) break;
        cell *= std::cbrt(12.0f / occupancy);
    }
    std::vector<std::uint32_t> counts(slot_of.size() + 1, 0);
    for (std::size_t i = 0; i < n; ++i) ++counts[cell_slot[i] + 1];
    for (std::size_t s = 1; s < counts.size(); ++s) counts[s] += counts[s - 1];
    std::vector<std::uint32_t> cell_points(n);
    {
        std::vector<std::uint32_t> cursor(counts.begin(), counts.end() - 1);
        for (std::size_t i = 0; i < n; ++i)
            cell_points[cursor[cell_slot[i]]++] = static_cast<std::uint32_t>(i);
    }

    const std::size_t k = static_cast<std::size_t>(k_neighbors);
    std::vector<float> mean_dist(n, 0.0f);
    parallel_for(n, [&](std::size_t begin, std::size_t end) {
        std::vector<float> best;  // squared distances of current candidates
        for (std::size_t i = begin; i < end; ++i) {
            const float* p = cloud.positions.data() + 3 * i;
            const auto cx =
                std::clamp(static_cast<std::int64_t>((p[0] - box.min[0]) / cell), std::int64_t{0}, nx - 1);
            const auto cy =
                std::clamp(static_cast<std::int64_t>((p[1] - box.min[1]) / cell), std::int64_t{0}, ny - 1);
            const auto cz =
                std::clamp(static_cast<std::int64_t>((p[2] - box.min[2]) / cell), std::int64_t{0}, nz - 1);
            best.clear();
            // Expand the neighbourhood ring by ring until we have k candidates (or hit the ring
            // cap; extremely isolated points then use the neighbours they do have, which are far
            // anyway). The candidate cap bounds the cost per point even for degenerate scenes.
            constexpr std::size_t kMaxCandidates = 4096;
            bool full = false;
            for (std::int64_t ring = 1; ring <= 4 && !full; ++ring) {
                best.clear();
                for (std::int64_t ix = std::max<std::int64_t>(0, cx - ring);
                     ix <= std::min(nx - 1, cx + ring) && !full; ++ix) {
                    for (std::int64_t iy = std::max<std::int64_t>(0, cy - ring);
                         iy <= std::min(ny - 1, cy + ring) && !full; ++iy) {
                        for (std::int64_t iz = std::max<std::int64_t>(0, cz - ring);
                             iz <= std::min(nz - 1, cz + ring) && !full; ++iz) {
                            const auto it = slot_of.find((static_cast<std::uint64_t>(ix) << 40) |
                                                         (static_cast<std::uint64_t>(iy) << 20) |
                                                         static_cast<std::uint64_t>(iz));
                            if (it == slot_of.end()) continue;
                            for (std::uint32_t s = counts[it->second]; s < counts[it->second + 1]; ++s) {
                                const std::uint32_t j = cell_points[s];
                                if (j == i) continue;
                                const float* q = cloud.positions.data() + 3 * j;
                                const float ddx = p[0] - q[0], ddy = p[1] - q[1], ddz = p[2] - q[2];
                                best.push_back(ddx * ddx + ddy * ddy + ddz * ddz);
                                if (best.size() >= kMaxCandidates) {
                                    full = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (best.size() >= k) break;
            }
            const std::size_t use = std::min(k, best.size());
            if (use == 0) {
                mean_dist[i] = std::numeric_limits<float>::max();
                continue;
            }
            std::nth_element(best.begin(), best.begin() + static_cast<std::ptrdiff_t>(use - 1), best.end());
            float sum = 0.0f;
            for (std::size_t s = 0; s < use; ++s) sum += std::sqrt(best[s]);
            mean_dist[i] = sum / static_cast<float>(use);
        }
    });

    double mean = 0.0;
    for (float d : mean_dist) mean += d;
    mean /= static_cast<double>(n);
    double var = 0.0;
    for (float d : mean_dist) var += (d - mean) * (d - mean);
    const double stddev = std::sqrt(var / static_cast<double>(n));
    const float threshold = static_cast<float>(mean + stddev_multiplier * stddev);

    std::vector<char> keep(n, 1);
    for (std::size_t i = 0; i < n; ++i) {
        if (mean_dist[i] > threshold) keep[i] = 0;
    }
    stats.removed = compact(cloud, keep);
    return stats;
}

CleanOpStats truncate_sh(SplatCloud& cloud, int target_degree) {
    CleanOpStats stats{"truncate_sh", cloud.count, 0,
                       "degree " + std::to_string(cloud.sh_degree) + " -> " + std::to_string(target_degree)};
    if (target_degree >= cloud.sh_degree) {
        stats.params += " (no-op)";
        return stats;
    }
    // Coefficient-major layout: the first sh_rest_coeffs(target) coefficients of each splat are
    // exactly the bands <= target_degree, so truncation is a contiguous per-splat prefix copy.
    const std::size_t old_block = 3 * static_cast<std::size_t>(sh_rest_coeffs(cloud.sh_degree));
    const std::size_t new_block = 3 * static_cast<std::size_t>(sh_rest_coeffs(target_degree));
    for (std::size_t i = 0; i < cloud.count; ++i) {
        for (std::size_t c = 0; c < new_block; ++c) {
            cloud.sh[new_block * i + c] = cloud.sh[old_block * i + c];
        }
    }
    cloud.sh_degree = target_degree;
    cloud.sh.resize(new_block * cloud.count);
    return stats;
}

CleanOpStats prune_budget(SplatCloud& cloud, std::size_t max_splats) {
    CleanOpStats stats{"prune_budget", cloud.count, 0, "max_splats=" + std::to_string(max_splats)};
    if (cloud.count <= max_splats) {
        stats.params += " (within budget)";
        return stats;
    }
    std::vector<float> imp(cloud.count);
    for (std::size_t i = 0; i < cloud.count; ++i) imp[i] = importance(cloud, i);
    std::vector<float> sorted = imp;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(max_splats - 1),
                     sorted.end(), std::greater<float>());
    const float threshold = sorted[max_splats - 1];

    std::vector<char> keep(cloud.count, 0);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < cloud.count && kept < max_splats; ++i) {  // strictly-above first
        if (imp[i] > threshold) {
            keep[i] = 1;
            ++kept;
        }
    }
    for (std::size_t i = 0; i < cloud.count && kept < max_splats; ++i) {  // fill ties in file order
        if (!keep[i] && imp[i] == threshold) {
            keep[i] = 1;
            ++kept;
        }
    }
    stats.removed = compact(cloud, keep);
    return stats;
}

CleanOpStats reorder_morton(SplatCloud& cloud, const Aabb& scene) {
    CleanOpStats stats{"reorder_morton", cloud.count, 0, "bits=10/axis"};
    const std::size_t n = cloud.count;
    if (n < 2) return stats;

    std::vector<std::uint32_t> code(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t q[3];
        for (int c = 0; c < 3; ++c) {
            const float extent = std::max(scene.max[c] - scene.min[c], 1e-9f);
            const float t = (cloud.positions[3 * i + static_cast<std::size_t>(c)] - scene.min[c]) / extent;
            q[c] = static_cast<std::uint32_t>(std::clamp(t, 0.0f, 1.0f) * 1023.0f);
        }
        code[i] = part1by2(q[0]) | (part1by2(q[1]) << 1) | (part1by2(q[2]) << 2);
    }
    std::vector<std::uint32_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = static_cast<std::uint32_t>(i);
    std::stable_sort(order.begin(), order.end(),
                     [&code](std::uint32_t a, std::uint32_t b) { return code[a] < code[b]; });

    SplatCloud out;
    out.resize(n, cloud.sh_degree);
    out.antialiased = cloud.antialiased;
    const std::size_t rest3 = 3 * static_cast<std::size_t>(sh_rest_coeffs(cloud.sh_degree));
    for (std::size_t dst = 0; dst < n; ++dst) {
        const std::size_t src = order[dst];
        for (int c = 0; c < 3; ++c) {
            out.positions[3 * dst + static_cast<std::size_t>(c)] =
                cloud.positions[3 * src + static_cast<std::size_t>(c)];
            out.log_scales[3 * dst + static_cast<std::size_t>(c)] =
                cloud.log_scales[3 * src + static_cast<std::size_t>(c)];
            out.dc[3 * dst + static_cast<std::size_t>(c)] = cloud.dc[3 * src + static_cast<std::size_t>(c)];
        }
        for (int c = 0; c < 4; ++c) {
            out.rotations[4 * dst + static_cast<std::size_t>(c)] =
                cloud.rotations[4 * src + static_cast<std::size_t>(c)];
        }
        out.opacities[dst] = cloud.opacities[src];
        for (std::size_t c = 0; c < rest3; ++c) out.sh[rest3 * dst + c] = cloud.sh[rest3 * src + c];
    }
    cloud = std::move(out);
    return stats;
}

}  // namespace splatcore
