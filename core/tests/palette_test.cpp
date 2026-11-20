#include "splatcore/palette.h"

#include <gtest/gtest.h>

#include <algorithm>
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

// What a SIMD kernel can get wrong that the brute-force test does not pin: exact ties between
// copies of a centroid in different lanes, vectors and steps (lowest index wins), the padded tail
// of a stride that is not a multiple of 8, and labels that depend on the thread count (the
// assignment hands ranges out dynamically).
TEST(Palette, AssignmentTiesPickTheLowestIndexAtAnyThreadCount) {
    const std::uint32_t k = 21;  // stride 24: three padding centroids
    std::vector<float> centroids = random_vectors(k, 5, 1.0f);
    const auto copy = [&](std::uint32_t from, std::uint32_t to) {
        std::copy_n(centroids.begin() + from * kSh1Dims, kSh1Dims, centroids.begin() + to * kSh1Dims);
    };
    copy(13, 2);                 // 2 and 13: other vector, other step
    copy(13, 6);                 // 6: same step, second vector
    copy(19, 9);                 // 9 and 19: the 19 sits in the last, padded step
    copy(20, 17);                // 17 and 20 likewise
    const std::size_t n = 1237;  // not a multiple of any range size
    std::vector<float> points(n * kSh1Dims);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t at = static_cast<std::uint32_t>(i % k);  // points on the centroids
        std::copy_n(centroids.begin() + at * kSh1Dims, kSh1Dims,
                    points.begin() + static_cast<std::ptrdiff_t>(i * kSh1Dims));
    }
    const std::uint16_t lowest[k] = {0, 1, 2, 3, 4, 5, 2, 7, 8, 9, 10, 11, 12, 2, 14, 15, 16, 17, 18, 9, 17};
    std::vector<std::uint16_t> reference;
    for (std::uint32_t threads : {1u, 2u, 5u, 16u}) {
        std::vector<std::uint16_t> labels(n);
        assign_nearest(points.data(), n, centroids.data(), k, labels.data(), nullptr, threads);
        for (std::size_t i = 0; i < n; ++i)
            ASSERT_EQ(labels[i], lowest[i % k]) << "point " << i << " threads " << threads;
        if (reference.empty()) reference = labels;
        EXPECT_EQ(labels, reference);
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

// Tight Gaussian blobs around `centers` random centres, `per` points each, interleaved.
std::vector<float> blobs(std::size_t centers, std::size_t per, float sigma, std::uint32_t seed) {
    const std::vector<float> c = random_vectors(centers, seed, 1.0f);
    std::mt19937 rng(seed + 1);
    std::normal_distribution<float> noise(0.0f, sigma);
    std::vector<float> out;
    out.reserve(centers * per * kSh1Dims);
    for (std::size_t p = 0; p < per; ++p) {
        for (std::size_t j = 0; j < centers; ++j) {
            for (int d = 0; d < kSh1Dims; ++d) out.push_back(c[j * kSh1Dims + d] + noise(rng));
        }
    }
    return out;
}

TEST(Palette, TrainerNames) {
    EXPECT_STREQ(palette_trainer_name(PaletteTrainer::kLloyd), "lloyd");
    EXPECT_STREQ(palette_trainer_name(PaletteTrainer::kHamerly), "hamerly");
    EXPECT_STREQ(palette_trainer_name(PaletteTrainer::kEMeans), "emeans");
}

TEST(Palette, HamerlyEqualsLloydBitForBit) {
    // Random data (many near-ties, every centroid moves), repeated values (empty clusters, splits,
    // early convergence) and blobs: same labels, centroids and epochs, fewer distance evaluations.
    struct Case {
        std::vector<float> sh1;
        std::uint32_t k;
        std::uint32_t epochs;
    };
    const Case cases[] = {{random_vectors(20000, 3, 0.4f), 64, 12},
                          {random_vectors(5000, 4, 1.0f), 300, 8},
                          {repeated_values(40, 50, 7), 40, 25},
                          {blobs(50, 200, 0.02f, 5), 64, 25}};
    for (const Case& c : cases) {
        const SplatCloud cloud = cloud_with_sh1(c.sh1);
        for (std::uint32_t threads : {1u, 3u}) {
            PaletteParams params;
            params.k = c.k;
            params.epochs = c.epochs;
            params.seed = 9;
            params.threads = threads;
            const Result<Sh1Palette> lloyd = train_sh1_palette(cloud, params);
            params.trainer = PaletteTrainer::kHamerly;
            const Result<Sh1Palette> hamerly = train_sh1_palette(cloud, params);
            ASSERT_TRUE(lloyd && hamerly);
            EXPECT_EQ(hamerly.value().centroids, lloyd.value().centroids) << "k " << c.k;
            EXPECT_EQ(hamerly.value().labels, lloyd.value().labels);
            EXPECT_EQ(hamerly.value().stats.epochs_run, lloyd.value().stats.epochs_run);
            EXPECT_EQ(hamerly.value().stats.mse, lloyd.value().stats.mse);
            if (lloyd.value().stats.epochs_run > 2) {
                EXPECT_LT(hamerly.value().stats.pairs, lloyd.value().stats.pairs) << "k " << c.k;
            }
        }
    }
}

TEST(Palette, EMeansScheduleFollowsTheScalingLaws) {
    // Reference values from the formulas of the paper's Table 2 ("knee / 8" batch law), evaluated
    // independently (Python, double precision).
    const EMeansSchedule a = emeans_schedule(1000000, 4096, 3);  // raw batch 6019 -> 8192 by ratio
    EXPECT_EQ(a.batch, 8192u);
    EXPECT_NEAR(a.alpha0, 0.1056353430, 1e-9);
    const EMeansSchedule b = emeans_schedule(314457, 4096, 3);  // raw 2175 -> 2048
    EXPECT_EQ(b.batch, 2048u);
    EXPECT_NEAR(b.alpha0, 0.0510566934, 1e-9);
    const EMeansSchedule c = emeans_schedule(4049567, 16384, 3);  // raw 24678 -> 32768
    EXPECT_EQ(c.batch, 32768u);
    EXPECT_NEAR(c.alpha0, 0.1036052536, 1e-9);
    const EMeansSchedule tiny = emeans_schedule(3, 2, 25);  // the law asks for more than n
    EXPECT_LE(tiny.batch, 3u);
    EXPECT_GE(tiny.batch, 1u);
    EXPECT_LE(tiny.alpha0, 1.0);
    const EMeansSchedule big = emeans_schedule(100000000, 64, 25);
    EXPECT_EQ(big.alpha0, 1.0);  // clamped
    EXPECT_EQ(big.batch & (big.batch - 1), 0u);
}

TEST(Palette, EMeansReachesTheLloydLossInFewerPasses) {
    // The paper's headline at small scale and d = 9: three e-means epochs against 25 Lloyd epochs
    // from the same initial centroids. Uniform data (no cluster structure, like most SH1): within
    // 0.5 % of the Lloyd-25 loss (measured: 0.1-0.8 % below it) for an eighth of the distance work.
    // Tight blobs, k = number of blobs: Lloyd keeps the local minimum its start gives it (two
    // centroids in one blob, two blobs under one centroid); e-means does no worse, with no entry dead.
    struct Case {
        std::vector<float> sh1;
        std::uint32_t k;
    };
    const Case cases[] = {{random_vectors(20000, 3, 0.4f), 64},
                          {random_vectors(50000, 5, 0.4f), 256},
                          {blobs(32, 400, 0.01f, 17), 32}};
    for (const Case& c : cases) {
        const SplatCloud cloud = cloud_with_sh1(c.sh1);
        for (std::uint64_t seed = 1; seed <= 3; ++seed) {
            PaletteParams params;
            params.k = c.k;
            params.seed = seed;
            params.epochs = 25;
            const Result<Sh1Palette> lloyd = train_sh1_palette(cloud, params);
            params.trainer = PaletteTrainer::kEMeans;
            params.epochs = 3;
            const Result<Sh1Palette> emeans = train_sh1_palette(cloud, params);
            ASSERT_TRUE(lloyd && emeans);
            const PaletteStats& s = emeans.value().stats;
            EXPECT_LT(s.mse, 1.005 * lloyd.value().stats.mse) << "k " << c.k << " seed " << seed;
            EXPECT_EQ(s.dead, 0u) << "k " << c.k << " seed " << seed;
            EXPECT_EQ(s.epochs_run, 3u);
            EXPECT_GT(s.batch, 0u);
            EXPECT_GT(s.alpha0, 0.0);
            EXPECT_EQ(s.updates, 3u * ((cloud.count + s.batch - 1) / s.batch));
            EXPECT_EQ(s.pairs, 3u * cloud.count * c.k);
            if (lloyd.value().stats.epochs_run == 25) EXPECT_LT(8 * s.pairs, lloyd.value().stats.pairs);
        }
    }
}

TEST(Palette, EMeansIsDeterministicAndIndependentOfThreads) {
    const SplatCloud cloud = cloud_with_sh1(random_vectors(20000, 3, 0.4f));
    PaletteParams params;
    params.k = 64;
    params.epochs = 3;
    params.seed = 42;
    params.trainer = PaletteTrainer::kEMeans;
    params.threads = 1;
    const Result<Sh1Palette> a = train_sh1_palette(cloud, params);
    params.threads = 4;
    const Result<Sh1Palette> b = train_sh1_palette(cloud, params);
    const Result<Sh1Palette> c = train_sh1_palette(cloud, params);
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(a.value().centroids, b.value().centroids);  // batch sums run in batch order
    EXPECT_EQ(b.value().centroids, c.value().centroids);
    EXPECT_EQ(a.value().labels, b.value().labels);
    params.seed = 43;
    const Result<Sh1Palette> d = train_sh1_palette(cloud, params);
    ASSERT_TRUE(d);
    EXPECT_NE(a.value().centroids, d.value().centroids);
}

TEST(Palette, EMeansOverridesAndRevival) {
    // A small batch forces many updates; with 40 distinct values and k = 40 a random start leaves
    // some values without a centroid, which revival fixes.
    const SplatCloud cloud = cloud_with_sh1(repeated_values(40, 50, 7));
    PaletteParams params;
    params.k = 40;
    params.epochs = 10;
    params.trainer = PaletteTrainer::kEMeans;
    params.batch = 100;
    params.alpha0 = 0.5;
    const Result<Sh1Palette> palette = train_sh1_palette(cloud, params);
    ASSERT_TRUE(palette) << palette.error();
    const PaletteStats& s = palette.value().stats;
    EXPECT_EQ(s.batch, 100u);
    EXPECT_EQ(s.alpha0, 0.5);
    EXPECT_EQ(s.updates, 10u * 20u);
    EXPECT_GT(s.revived, 0u);
    EXPECT_LE(s.dead, 2u);
}

TEST(Palette, EveryTrainerHandlesDegenerateInputs) {
    for (PaletteTrainer trainer :
         {PaletteTrainer::kLloyd, PaletteTrainer::kHamerly, PaletteTrainer::kEMeans}) {
        SCOPED_TRACE(palette_trainer_name(trainer));
        PaletteParams params;
        params.trainer = trainer;
        params.k = 16;
        params.epochs = 5;
        {  // all-zero SH1: one entry carries everything, every centroid stays finite
            const SplatCloud cloud = cloud_with_sh1(std::vector<float>(1000 * kSh1Dims, 0.0f));
            const Result<Sh1Palette> p = train_sh1_palette(cloud, params);
            ASSERT_TRUE(p) << p.error();
            EXPECT_EQ(p.value().stats.mse, 0.0);
            EXPECT_EQ(p.value().stats.dead, 15u);
            for (float v : p.value().centroids) EXPECT_TRUE(std::isfinite(v));
        }
        {  // fewer splats than entries
            const SplatCloud cloud = cloud_with_sh1(random_vectors(5, 5, 1.0f));
            const Result<Sh1Palette> p = train_sh1_palette(cloud, params);
            ASSERT_TRUE(p) << p.error();
            EXPECT_EQ(p.value().size(), 5u);
            for (float v : p.value().centroids) EXPECT_TRUE(std::isfinite(v));
        }
        {  // one entry: the mean
            const SplatCloud cloud = cloud_with_sh1(random_vectors(300, 8, 1.0f));
            params.k = 1;
            const Result<Sh1Palette> p = train_sh1_palette(cloud, params);
            ASSERT_TRUE(p) << p.error();
            for (int d = 0; d < kSh1Dims; ++d) {
                double mean = 0.0;
                for (std::size_t i = 0; i < cloud.count; ++i) mean += cloud.sh[i * kSh1Dims + d];
                EXPECT_NEAR(p.value().centroids[d], mean / static_cast<double>(cloud.count), 1e-5);
            }
        }
        {  // training cap: every splat still gets a label
            const SplatCloud cloud = cloud_with_sh1(repeated_values(8, 500, 13));
            params.k = 8;
            params.train_cap = 400;
            const Result<Sh1Palette> p = train_sh1_palette(cloud, params);
            ASSERT_TRUE(p) << p.error();
            EXPECT_EQ(p.value().labels.size(), cloud.count);
        }
    }
}

}  // namespace
}  // namespace splatcore
