#include "splatcore/spz_io.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace splatcore {
namespace {

namespace fs = std::filesystem;

float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

// Synthetic cloud with values inside SPZ's representable ranges (log scales in [-10, 5.9],
// f_dc within +-3.3, SH coefficients within +-1).
SplatCloud make_cloud(std::size_t n, int degree) {
    SplatCloud cloud;
    cloud.resize(n, degree);
    const int rest = sh_rest_coeffs(degree);
    for (std::size_t i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i);
        for (int c = 0; c < 3; ++c) {
            cloud.positions[3 * i + c] = 0.25f * fi - 2.0f + 0.5f * static_cast<float>(c);
            cloud.log_scales[3 * i + c] = -4.0f + 0.125f * static_cast<float>(c) + 0.03125f * fi;
            // Stay well inside SPZ's representable color range (|f_dc| <= (1-0.5)/0.15 = 3.33).
            cloud.dc[3 * i + c] = -1.0f + 0.04f * fi + 0.25f * static_cast<float>(c);
        }
        cloud.opacities[i] = -2.0f + 0.25f * fi;
        const float angle = 0.15f * fi;
        cloud.rotations[4 * i + 0] = std::cos(angle);
        cloud.rotations[4 * i + 1] = std::sin(angle);
        cloud.rotations[4 * i + 2] = 0.0f;
        cloud.rotations[4 * i + 3] = 0.0f;
        for (int k = 0; k < 3 * rest; ++k) {
            cloud.sh[3 * static_cast<std::size_t>(rest) * i + static_cast<std::size_t>(k)] =
                0.4f * std::sin(0.37f * static_cast<float>(k) + fi);
        }
    }
    return cloud;
}

fs::path temp_spz(const std::string& tag) {
    return fs::temp_directory_path() / ("pocketsplat_test_" + tag + ".spz");
}

void expect_round_trip(const SplatCloud& cloud, const SplatCloud& back) {
    ASSERT_EQ(back.count, cloud.count);
    ASSERT_EQ(back.sh_degree, cloud.sh_degree);
    EXPECT_EQ(back.antialiased, cloud.antialiased);
    // Tolerances follow the SPZ quantization: 24-bit fixed-point positions, 1/16-step log scales,
    // smallest-three quaternions, 8-bit alpha (compared in sigmoid space), 8-bit color, SH at the
    // library's default reduced bit depth.
    for (std::size_t i = 0; i < cloud.count; ++i) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(back.positions[3 * i + c], cloud.positions[3 * i + c], 0.01f);
            EXPECT_NEAR(back.log_scales[3 * i + c], cloud.log_scales[3 * i + c], 0.04f);
            EXPECT_NEAR(back.dc[3 * i + c], cloud.dc[3 * i + c], 0.03f);
        }
        // q and -q encode the same rotation and SPZ's smallest-three packing canonicalizes the
        // sign, so align the signs before comparing.
        float dot = 0.0f;
        for (int c = 0; c < 4; ++c) dot += back.rotations[4 * i + c] * cloud.rotations[4 * i + c];
        const float sign = dot < 0.0f ? -1.0f : 1.0f;
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(sign * back.rotations[4 * i + c], cloud.rotations[4 * i + c], 0.01f);
        }
        EXPECT_NEAR(sigmoid(back.opacities[i]), sigmoid(cloud.opacities[i]), 0.005f);
    }
    ASSERT_EQ(back.sh.size(), cloud.sh.size());
    for (std::size_t k = 0; k < cloud.sh.size(); ++k) {
        EXPECT_NEAR(back.sh[k], cloud.sh[k], 0.06f) << "sh index " << k;
    }
}

TEST(SpzIo, RoundTripDegree0) {
    const SplatCloud cloud = make_cloud(64, 0);
    const fs::path path = temp_spz("deg0");
    ASSERT_TRUE(write_spz(cloud, path).ok());
    Result<SplatCloud> back = read_spz(path);
    fs::remove(path);
    ASSERT_TRUE(back.ok()) << back.error();
    expect_round_trip(cloud, back.value());
}

TEST(SpzIo, RoundTripDegree1) {
    SplatCloud cloud = make_cloud(48, 1);
    cloud.antialiased = true;
    const fs::path path = temp_spz("deg1");
    ASSERT_TRUE(write_spz(cloud, path).ok());
    Result<SplatCloud> back = read_spz(path);
    fs::remove(path);
    ASSERT_TRUE(back.ok()) << back.error();
    expect_round_trip(cloud, back.value());
}

TEST(SpzIo, WriterRejectsDegree2) {
    const SplatCloud cloud = make_cloud(4, 2);
    Status status = write_spz(cloud, temp_spz("deg2"));
    ASSERT_FALSE(status.ok());
    EXPECT_NE(status.error().find("degree"), std::string::npos);
}

TEST(SpzIo, ReadRejectsMissingFile) {
    Result<SplatCloud> result = read_spz(temp_spz("missing_nonexistent"));
    ASSERT_FALSE(result.ok());
}

TEST(SpzIo, ReadsRealNianticFileIfPresent) {
    // Local test data (gitignored); skip cleanly on machines without it.
    fs::path path;
    for (const char* candidate : {"data/raw/racoonfamily.spz", "../data/raw/racoonfamily.spz",
                                  "../../data/raw/racoonfamily.spz", "../../../data/raw/racoonfamily.spz"}) {
        if (fs::exists(candidate)) {
            path = candidate;
            break;
        }
    }
    if (path.empty()) GTEST_SKIP() << "racoonfamily.spz not available";
    Result<SplatCloud> result = read_spz(path);
    ASSERT_TRUE(result.ok()) << result.error();
    EXPECT_EQ(result.value().count, 932560u);
    EXPECT_EQ(result.value().sh_degree, 3);
    EXPECT_TRUE(result.value().antialiased);  // trained with Mip-Splatting per the source notes
}

}  // namespace
}  // namespace splatcore
