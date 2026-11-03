#include "splatcore/cleaning.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace splatcore {
namespace {

// n splats at distinct positions, unit quaternions, moderate values everywhere.
SplatCloud make_cloud(std::size_t n, int degree = 0) {
    SplatCloud cloud;
    cloud.resize(n, degree);
    const int rest = sh_rest_coeffs(degree);
    for (std::size_t i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i);
        cloud.positions[3 * i] = fi * 0.01f;
        cloud.positions[3 * i + 1] = 0.5f;
        cloud.positions[3 * i + 2] = -0.25f;
        for (int c = 0; c < 3; ++c) {
            cloud.log_scales[3 * i + c] = -5.0f;
            cloud.dc[3 * i + c] = 0.5f;
        }
        cloud.opacities[i] = 2.0f;
        cloud.rotations[4 * i] = 1.0f;
        for (int k = 0; k < 3 * rest; ++k) {
            cloud.sh[3 * static_cast<std::size_t>(rest) * i + static_cast<std::size_t>(k)] =
                0.1f * static_cast<float>(k + 1);
        }
    }
    return cloud;
}

TEST(Cleaning, DropInvalidRemovesNanAndZeroQuat) {
    SplatCloud cloud = make_cloud(4);
    cloud.positions[3 * 1] = std::numeric_limits<float>::quiet_NaN();
    cloud.rotations[4 * 2] = 0.0f;  // zero-norm quaternion
    const CleanOpStats stats = drop_invalid(cloud);
    EXPECT_EQ(stats.input_count, 4u);
    EXPECT_EQ(stats.removed, 2u);
    EXPECT_EQ(cloud.count, 2u);
    EXPECT_FLOAT_EQ(cloud.positions[0], 0.0f);   // splat 0 kept
    EXPECT_FLOAT_EQ(cloud.positions[3], 0.03f);  // splat 3 kept, shifted down
}

TEST(Cleaning, PruneOpacity) {
    SplatCloud cloud = make_cloud(3);
    cloud.opacities[1] = -5.0f;  // sigmoid ~ 0.0067 < 0.02
    const CleanOpStats stats = prune_opacity(cloud, 0.02f);
    EXPECT_EQ(stats.removed, 1u);
    EXPECT_EQ(cloud.count, 2u);
}

TEST(Cleaning, PruneScaleRemovesHugeAndTiny) {
    SplatCloud cloud = make_cloud(100);  // positions span ~1 unit in x
    const Aabb scene = robust_aabb(cloud);
    ASSERT_GT(scene.diagonal(), 0.5f);
    cloud.log_scales[3 * 5] = std::log(scene.diagonal());              // huge: > 5% of diagonal
    cloud.log_scales[3 * 7 + 1] = std::log(scene.diagonal()) - 20.0f;  // tiny: < 1e-6 diag
    const CleanOpStats stats = prune_scale(cloud, scene, 0.05f, 1e-6f);
    EXPECT_EQ(stats.removed, 2u);
    EXPECT_EQ(cloud.count, 98u);
}

TEST(Cleaning, PruneScaleDefaultsKeepFlatDisks) {
    // ADR-0007: a trained surface disk (two ordinary axes, one ~1e-8 of the diagonal) is content.
    SplatCloud cloud = make_cloud(100);
    const Aabb scene = robust_aabb(cloud);
    cloud.log_scales[3 * 7 + 1] = std::log(scene.diagonal()) - 20.0f;
    const CleanOpStats stats = prune_scale(cloud, scene);
    EXPECT_EQ(stats.removed, 0u);
    EXPECT_EQ(cloud.count, 100u);
}

TEST(Cleaning, ClampNeedleScalesShortensOnlyNeedles) {
    SplatCloud cloud = make_cloud(100);
    const Aabb scene = robust_aabb(cloud);
    const float diag = scene.diagonal();
    ASSERT_GT(diag, 0.5f);
    // A needle: one axis at 10% of the diagonal, the other two tiny.
    cloud.log_scales[3 * 3 + 1] = std::log(0.1f * diag);
    // A disk: two comparable large axes - the aspect term must leave it untouched.
    cloud.log_scales[3 * 4] = std::log(0.04f * diag);
    cloud.log_scales[3 * 4 + 1] = std::log(0.04f * diag);
    const float disk_before = cloud.log_scales[3 * 4];
    // A short elongated splat: over the default isotropic scale but under both caps - untouched.
    cloud.log_scales[3 * 5 + 2] = std::log(0.008f * diag);
    const float short_needle_before = cloud.log_scales[3 * 5 + 2];

    const CleanOpStats stats = clamp_needle_scales(cloud, scene, 0.01f, 6.0f);
    EXPECT_EQ(stats.removed, 0u);
    EXPECT_EQ(cloud.count, 100u);
    EXPECT_NE(stats.params.find("clamped=1"), std::string::npos);
    // The needle's major axis lands on max(0.01 * diag, 6 * mid); mid = exp(-5) here.
    const float expected = std::max(0.01f * diag, 6.0f * std::exp(-5.0f));
    EXPECT_NEAR(std::exp(cloud.log_scales[3 * 3 + 1]), expected, 1e-5f);
    EXPECT_EQ(cloud.log_scales[3 * 4], disk_before);
    EXPECT_EQ(cloud.log_scales[3 * 5 + 2], short_needle_before);
}

TEST(Cleaning, SorRemovesIsolatedFloater) {
    // 60-splat dense cluster plus one splat far away.
    SplatCloud cloud = make_cloud(61);
    cloud.positions[3 * 60] = 100.0f;
    cloud.positions[3 * 60 + 1] = 100.0f;
    cloud.positions[3 * 60 + 2] = 100.0f;
    const CleanOpStats stats = remove_floaters_sor(cloud, 16, 2.0f);
    EXPECT_EQ(stats.removed, 1u);
    EXPECT_EQ(cloud.count, 60u);
    for (std::size_t i = 0; i < cloud.count; ++i) {
        EXPECT_LT(cloud.positions[3 * i], 1.0f);  // the far splat is the one that is gone
    }
}

TEST(Cleaning, TruncateShDegree3To1) {
    SplatCloud cloud = make_cloud(3, 3);
    const std::vector<float> before = cloud.sh;
    const CleanOpStats stats = truncate_sh(cloud, 1);
    EXPECT_EQ(stats.removed, 0u);
    EXPECT_EQ(cloud.sh_degree, 1);
    ASSERT_EQ(cloud.sh.size(), 3u * 3u * 3u);  // 3 splats x 3 coeffs x 3 channels
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t c = 0; c < 9; ++c) {
            EXPECT_FLOAT_EQ(cloud.sh[9 * i + c], before[45 * i + c]);  // band-1 prefix preserved
        }
    }
    EXPECT_TRUE(cloud.valid_sizes());
}

TEST(Cleaning, TruncateShNoOpAtOrBelowTarget) {
    SplatCloud cloud = make_cloud(2, 1);
    const std::vector<float> before = cloud.sh;
    truncate_sh(cloud, 1);
    EXPECT_EQ(cloud.sh_degree, 1);
    EXPECT_EQ(cloud.sh, before);
}

TEST(Cleaning, PruneBudgetKeepsMostImportant) {
    SplatCloud cloud = make_cloud(5);
    // Give splats 1 and 3 much larger scales -> much higher importance.
    for (int c = 0; c < 3; ++c) {
        cloud.log_scales[3 * 1 + c] = -1.0f;
        cloud.log_scales[3 * 3 + c] = -1.0f;
    }
    const CleanOpStats stats = prune_budget(cloud, 2);
    EXPECT_EQ(stats.removed, 3u);
    ASSERT_EQ(cloud.count, 2u);
    EXPECT_FLOAT_EQ(cloud.positions[0], 0.01f);  // former splat 1, order preserved
    EXPECT_FLOAT_EQ(cloud.positions[3], 0.03f);  // former splat 3
}

TEST(Cleaning, PruneBudgetNoOpWithinBudget) {
    SplatCloud cloud = make_cloud(5);
    const CleanOpStats stats = prune_budget(cloud, 10);
    EXPECT_EQ(stats.removed, 0u);
    EXPECT_EQ(cloud.count, 5u);
}

TEST(Cleaning, ReorderMortonKeepsFieldsTogetherAndIsDeterministic) {
    SplatCloud cloud = make_cloud(64, 1);
    // Scatter positions and tag each splat's dc with its original position.
    for (std::size_t i = 0; i < cloud.count; ++i) {
        cloud.positions[3 * i] = static_cast<float>((i * 37) % 64);
        cloud.positions[3 * i + 1] = static_cast<float>((i * 17) % 64);
        cloud.positions[3 * i + 2] = static_cast<float>((i * 53) % 64);
        for (int c = 0; c < 3; ++c) cloud.dc[3 * i + c] = cloud.positions[3 * i + c];
    }
    const Aabb scene = robust_aabb(cloud);
    reorder_morton(cloud, scene);
    for (std::size_t i = 0; i < cloud.count; ++i) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(cloud.dc[3 * i + c], cloud.positions[3 * i + c]);
        }
    }
    // Deterministic: a second reorder over the same box changes nothing.
    const std::vector<float> once = cloud.positions;
    reorder_morton(cloud, scene);
    EXPECT_EQ(cloud.positions, once);
}

TEST(Cleaning, RobustAabbIgnoresOutliers) {
    SplatCloud cloud = make_cloud(1000);
    cloud.positions[3 * 999] = 1e9f;  // single far outlier must not blow up the box
    const Aabb box = robust_aabb(cloud);
    EXPECT_LT(box.max[0], 100.0f);
}

}  // namespace
}  // namespace splatcore
