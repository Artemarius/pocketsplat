#include "splatcore/stats.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace splatcore {

bool splat_is_finite(const SplatCloud& cloud, std::size_t i) {
    for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(cloud.positions[3 * i + c]) || !std::isfinite(cloud.log_scales[3 * i + c]) ||
            !std::isfinite(cloud.dc[3 * i + c])) {
            return false;
        }
    }
    for (int c = 0; c < 4; ++c) {
        if (!std::isfinite(cloud.rotations[4 * i + c])) return false;
    }
    if (!std::isfinite(cloud.opacities[i])) return false;
    const std::size_t rest = 3 * static_cast<std::size_t>(sh_rest_coeffs(cloud.sh_degree));
    for (std::size_t c = 0; c < rest; ++c) {
        if (!std::isfinite(cloud.sh[rest * i + c])) return false;
    }
    return true;
}

namespace {

int bin_index(float value, float min_value, float max_value) {
    if (max_value <= min_value) return 0;
    const float t = (value - min_value) / (max_value - min_value);
    return std::clamp(static_cast<int>(t * kHistogramBins), 0, kHistogramBins - 1);
}

}  // namespace

CloudStats compute_stats(const SplatCloud& cloud) {
    CloudStats stats;
    stats.count = cloud.count;
    stats.sh_degree = cloud.sh_degree;
    stats.opacity.min_value = 0.0f;
    stats.opacity.max_value = 1.0f;

    std::vector<float> max_scales;
    max_scales.reserve(cloud.count);
    std::vector<float> alphas;
    alphas.reserve(cloud.count);

    float bbox_min[3], bbox_max[3];
    std::fill_n(bbox_min, 3, std::numeric_limits<float>::max());
    std::fill_n(bbox_max, 3, std::numeric_limits<float>::lowest());
    bool any_finite = false;

    for (std::size_t i = 0; i < cloud.count; ++i) {
        if (!splat_is_finite(cloud, i)) {
            ++stats.nan_count;
            continue;
        }
        any_finite = true;
        for (int c = 0; c < 3; ++c) {
            bbox_min[c] = std::min(bbox_min[c], cloud.positions[3 * i + c]);
            bbox_max[c] = std::max(bbox_max[c], cloud.positions[3 * i + c]);
        }
        alphas.push_back(1.0f / (1.0f + std::exp(-cloud.opacities[i])));
        const float max_log =
            std::max({cloud.log_scales[3 * i], cloud.log_scales[3 * i + 1], cloud.log_scales[3 * i + 2]});
        max_scales.push_back(std::exp(max_log));
    }

    if (any_finite) {
        for (int c = 0; c < 3; ++c) {
            stats.bbox_min[c] = bbox_min[c];
            stats.bbox_max[c] = bbox_max[c];
        }
    }
    for (float a : alphas) ++stats.opacity.bins[static_cast<std::size_t>(bin_index(a, 0.0f, 1.0f))];
    if (!max_scales.empty()) {
        const auto [lo, hi] = std::minmax_element(max_scales.begin(), max_scales.end());
        stats.scale.min_value = *lo;
        stats.scale.max_value = *hi;
        for (float s : max_scales) {
            ++stats.scale.bins[static_cast<std::size_t>(bin_index(s, *lo, *hi))];
        }
    }
    return stats;
}

}  // namespace splatcore
