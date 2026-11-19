#include "splatcore/palette.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

namespace splatcore {
namespace {

// A degree-1 cloud whose SH1 vectors are given (9 floats per splat); other attributes neutral.
SplatCloud cloud_with_sh1(const std::vector<float>& sh1) {
    SplatCloud cloud;
    cloud.resize(sh1.size() / kSh1Dims, 1);
    cloud.sh = sh1;
    for (std::size_t i = 0; i < cloud.count; ++i) cloud.rotations[4 * i] = 1.0f;
    return cloud;
}

std::vector<float> random_vectors(std::size_t n, std::uint32_t seed, float spread) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-spread, spread);
    std::vector<float> v(n * kSh1Dims);
    for (float& x : v) x = u(rng);
    return v;
}

// `distinct` random SH1 vectors, each repeated `copies` times, interleaved.
std::vector<float> repeated_values(std::size_t distinct, std::size_t copies, std::uint32_t seed) {
    const std::vector<float> values = random_vectors(distinct, seed, 0.5f);
    std::vector<float> out;
    for (std::size_t c = 0; c < copies; ++c) {
        for (std::size_t v = 0; v < distinct; ++v) {
            out.insert(out.end(), values.begin() + static_cast<std::ptrdiff_t>(v * kSh1Dims),
                       values.begin() + static_cast<std::ptrdiff_t>((v + 1) * kSh1Dims));
        }
    }
    return out;
}

TEST(Palette, AssignmentMatchesBruteForce) {
    const std::vector<float> points = random_vectors(3000, 1, 1.0f);
    for (std::uint32_t k : {1u, 7u, 512u, 1000u}) {  // below, at and across the 512-centroid tile
        const std::vector<float> centroids = random_vectors(k, 2 + k, 1.0f);
        std::vector<std::uint16_t> labels(3000);
        std::vector<float> d2(3000);
        assign_nearest(points.data(), 3000, centroids.data(), k, labels.data(), d2.data(), 3);
        for (std::size_t i = 0; i < 3000; ++i) {
            double best = 1e30;
            std::uint32_t best_j = 0;
            for (std::uint32_t j = 0; j < k; ++j) {
                double s = 0.0;
                for (int d = 0; d < kSh1Dims; ++d) {
                    const double diff = double(points[i * kSh1Dims + d]) - centroids[j * kSh1Dims + d];
                    s += diff * diff;
                }
                if (s < best) best = s, best_j = j;
            }
            ASSERT_EQ(labels[i], best_j) << "k " << k << " point " << i;
            EXPECT_NEAR(d2[i], best, 1e-5);
        }
    }
}

TEST(Palette, RecoversClustersOfIdenticalVectorsExactly) {
    // 40 distinct vectors x 50 copies, k = 40. Random initial centroids can repeat a value; the
    // empty clusters that leaves are refilled by splitting the largest cluster, which holds two or
    // more values while any value lacks its own centroid - so every seed recovers all 40.
    const std::vector<float> sh1 = repeated_values(40, 50, 7);
    const SplatCloud cloud = cloud_with_sh1(sh1);
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        PaletteParams params;
        params.k = 40;
        params.seed = seed;
        params.threads = 2;
        const Result<Sh1Palette> palette = train_sh1_palette(cloud, params);
        ASSERT_TRUE(palette) << palette.error();
        const Sh1Palette& p = palette.value();
        EXPECT_EQ(p.size(), 40u);
        EXPECT_EQ(p.stats.mse, 0.0) << "seed " << seed;
        EXPECT_EQ(p.stats.dead, 0u);
        EXPECT_NEAR(p.stats.used_entropy, std::log2(40.0), 1e-9);
        for (std::size_t i = 0; i < cloud.count; ++i) {
            EXPECT_EQ(p.labels[i], p.labels[i % 40]);  // copies of a value share an entry
        }
        const std::set<std::uint16_t> distinct(p.labels.begin(), p.labels.end());
        EXPECT_EQ(distinct.size(), 40u);
    }
}

TEST(Palette, DeterministicForSeedAndThreadCount) {
    const SplatCloud cloud = cloud_with_sh1(random_vectors(20000, 3, 0.4f));
    PaletteParams params;
    params.k = 64;
    params.epochs = 10;
    params.seed = 42;
    params.threads = 4;
    const Result<Sh1Palette> a = train_sh1_palette(cloud, params);
    const Result<Sh1Palette> b = train_sh1_palette(cloud, params);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a.value().centroids, b.value().centroids);
    EXPECT_EQ(a.value().labels, b.value().labels);
    EXPECT_EQ(a.value().stats.mse, b.value().stats.mse);
    params.seed = 43;
    const Result<Sh1Palette> c = train_sh1_palette(cloud, params);
    ASSERT_TRUE(c);
    EXPECT_NE(a.value().centroids, c.value().centroids);
    // Training lowers the error against the initial centroids (one epoch of Lloyd cannot raise it).
    params.seed = 42;
    params.epochs = 1;
    const Result<Sh1Palette> one = train_sh1_palette(cloud, params);
    ASSERT_TRUE(one);
    EXPECT_LT(a.value().stats.mse, one.value().stats.mse);
}

TEST(Palette, MoreEntriesThanDistinctValuesLeavesTheRestDead) {
    const SplatCloud cloud = cloud_with_sh1(repeated_values(10, 30, 11));
    PaletteParams params;
    params.k = 25;
    const Result<Sh1Palette> palette = train_sh1_palette(cloud, params);
    ASSERT_TRUE(palette) << palette.error();
    EXPECT_EQ(palette.value().size(), 25u);
    EXPECT_EQ(palette.value().stats.mse, 0.0);
    EXPECT_EQ(palette.value().stats.dead, 15u);
}

TEST(Palette, FewerSplatsThanEntriesClampsThePalette) {
    const SplatCloud cloud = cloud_with_sh1(random_vectors(5, 5, 1.0f));
    PaletteParams params;
    params.k = 16;
    const Result<Sh1Palette> palette = train_sh1_palette(cloud, params);
    ASSERT_TRUE(palette) << palette.error();
    EXPECT_EQ(palette.value().size(), 5u);
    EXPECT_EQ(palette.value().stats.mse, 0.0);
    EXPECT_EQ(palette.value().stats.dead, 0u);
}

TEST(Palette, AllZeroSh1UsesOneEntry) {
    const SplatCloud cloud = cloud_with_sh1(std::vector<float>(1000 * kSh1Dims, 0.0f));
    PaletteParams params;
    params.k = 16;
    const Result<Sh1Palette> palette = train_sh1_palette(cloud, params);
    ASSERT_TRUE(palette) << palette.error();
    const Sh1Palette& p = palette.value();
    EXPECT_EQ(p.stats.mse, 0.0);
    EXPECT_EQ(p.stats.dead, 15u);
    EXPECT_EQ(p.stats.used_entropy, 0.0);
    for (float v : p.centroids) EXPECT_TRUE(std::isfinite(v));
}

TEST(Palette, TrainingCapStillLabelsEverySplat) {
    const SplatCloud cloud = cloud_with_sh1(repeated_values(8, 500, 13));
    PaletteParams params;
    params.k = 8;
    params.train_cap = 400;
    const Result<Sh1Palette> palette = train_sh1_palette(cloud, params);
    ASSERT_TRUE(palette) << palette.error();
    EXPECT_EQ(palette.value().labels.size(), cloud.count);
    EXPECT_EQ(palette.value().stats.mse, 0.0);
}

TEST(Palette, RejectsBadInputs) {
    SplatCloud sh0;
    sh0.resize(10, 0);
    EXPECT_FALSE(train_sh1_palette(sh0, PaletteParams{}));
    SplatCloud sh3;
    sh3.resize(10, 3);
    EXPECT_FALSE(train_sh1_palette(sh3, PaletteParams{}));
    SplatCloud empty;
    empty.resize(0, 1);
    EXPECT_FALSE(train_sh1_palette(empty, PaletteParams{}));
    const SplatCloud cloud = cloud_with_sh1(random_vectors(10, 1, 1.0f));
    PaletteParams params;
    params.k = 0;
    EXPECT_FALSE(train_sh1_palette(cloud, params));
    params.k = kMaxPaletteSize + 1;
    EXPECT_FALSE(train_sh1_palette(cloud, params));
}

TEST(Palette, ApplySnapsEverySplatToItsEntry) {
    SplatCloud cloud = cloud_with_sh1(random_vectors(500, 21, 0.3f));
    PaletteParams params;
    params.k = 4;
    const Result<Sh1Palette> palette = train_sh1_palette(cloud, params);
    ASSERT_TRUE(palette);
    ASSERT_TRUE(apply_sh1_palette(cloud, palette.value()));
    const Sh1Palette& p = palette.value();
    for (std::size_t i = 0; i < cloud.count; ++i) {
        for (int d = 0; d < kSh1Dims; ++d) {
            EXPECT_EQ(cloud.sh[i * kSh1Dims + d], p.centroids[p.labels[i] * kSh1Dims + d]);
        }
    }
    SplatCloud other = cloud_with_sh1(random_vectors(10, 1, 1.0f));
    EXPECT_FALSE(apply_sh1_palette(other, p));
}

}  // namespace
}  // namespace splatcore
