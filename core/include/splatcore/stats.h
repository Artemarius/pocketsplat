#pragma once

#include <array>
#include <cstdint>

#include "splatcore/splat_cloud.h"

namespace splatcore {

inline constexpr int kHistogramBins = 10;

struct Histogram {
    float min_value = 0.0f;  // lower edge of bin 0
    float max_value = 0.0f;  // upper edge of the last bin
    std::array<std::uint64_t, kHistogramBins> bins{};
};

struct CloudStats {
    std::size_t count = 0;
    int sh_degree = 0;
    std::size_t nan_count = 0;  // splats with any non-finite field; excluded from bbox/histograms
    std::array<float, 3> bbox_min{};
    std::array<float, 3> bbox_max{};
    Histogram opacity;  // over alpha = sigmoid(opacity logit), fixed range [0, 1]
    Histogram scale;    // over exp(max log-scale component) per splat, observed range
};

CloudStats compute_stats(const SplatCloud& cloud);

// True when every field of splat i is finite. Shared by stats and the drop_invalid cleaning op.
bool splat_is_finite(const SplatCloud& cloud, std::size_t i);

}  // namespace splatcore
