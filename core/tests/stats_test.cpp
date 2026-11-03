#include "splatcore/stats.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace splatcore {
namespace {

TEST(Stats, CountsNaNSplatsAndExcludesThem) {
    SplatCloud cloud;
    cloud.resize(3, 0);
    for (std::size_t i = 0; i < 3; ++i) {
        cloud.positions[3 * i + 0] = static_cast<float>(i);  // x: 0, 1, 2
        cloud.rotations[4 * i] = 1.0f;
    }
    cloud.positions[3 * 2 + 1] = std::numeric_limits<float>::quiet_NaN();  // splat 2 is invalid

    const CloudStats stats = compute_stats(cloud);
    EXPECT_EQ(stats.count, 3u);
    EXPECT_EQ(stats.nan_count, 1u);
    EXPECT_FLOAT_EQ(stats.bbox_min[0], 0.0f);
    EXPECT_FLOAT_EQ(stats.bbox_max[0], 1.0f);  // NaN splat's x=2 excluded
}

TEST(Stats, OpacityHistogramIsFixedRange) {
    SplatCloud cloud;
    cloud.resize(2, 0);
    cloud.rotations[0] = cloud.rotations[4] = 1.0f;
    cloud.opacities[0] = 0.0f;   // sigmoid -> 0.5, bin 5
    cloud.opacities[1] = 10.0f;  // sigmoid -> ~1.0, bin 9

    const CloudStats stats = compute_stats(cloud);
    EXPECT_FLOAT_EQ(stats.opacity.min_value, 0.0f);
    EXPECT_FLOAT_EQ(stats.opacity.max_value, 1.0f);
    EXPECT_EQ(stats.opacity.bins[5], 1u);
    EXPECT_EQ(stats.opacity.bins[9], 1u);
}

TEST(Stats, ScaleHistogramUsesObservedRange) {
    SplatCloud cloud;
    cloud.resize(2, 0);
    cloud.rotations[0] = cloud.rotations[4] = 1.0f;
    // Splat 0: all log-scales 0 -> max scale exp(0) = 1. Splat 1: max log-scale 1 -> exp(1).
    cloud.log_scales[3 * 1 + 2] = 1.0f;

    const CloudStats stats = compute_stats(cloud);
    EXPECT_FLOAT_EQ(stats.scale.min_value, 1.0f);
    EXPECT_FLOAT_EQ(stats.scale.max_value, std::exp(1.0f));
    EXPECT_EQ(stats.scale.bins[0], 1u);
    EXPECT_EQ(stats.scale.bins[kHistogramBins - 1], 1u);
}

TEST(Stats, EmptyCloud) {
    const CloudStats stats = compute_stats(SplatCloud{});
    EXPECT_EQ(stats.count, 0u);
    EXPECT_EQ(stats.nan_count, 0u);
}

}  // namespace
}  // namespace splatcore
