#include "splatcore/coverage.h"

#include <gtest/gtest.h>

#include <cmath>

namespace splatcore {
namespace {

GpuPack make_pack(const std::vector<float>& positions, const std::vector<float>& alphas) {
    GpuPack pack;
    pack.header.count = static_cast<std::uint32_t>(alphas.size());
    pack.positions = positions;
    for (float a : alphas) pack.color.push_back(static_cast<std::uint32_t>(std::lround(a * 255.0f)) << 24);
    return pack;
}

CaptureCamera make_camera(const float pos[3], const float right[3], const float down[3], const float fwd[3]) {
    CaptureCamera c;
    for (int k = 0; k < 3; ++k) {
        c.position[k] = pos[k];
        c.right[k] = right[k];
        c.down[k] = down[k];
        c.forward[k] = fwd[k];
    }
    c.fx = c.fy = 100.0f;
    c.width = c.height = 64;
    return c;
}

TEST(Coverage, EveryCellCentreMapsBackToItsCell) {
    for (std::uint32_t cell = 0; cell < kCoverageGrid * kCoverageGrid; ++cell) {
        float c[3];
        coverage_cell_center(cell, c);
        EXPECT_NEAR(c[0] * c[0] + c[1] * c[1] + c[2] * c[2], 1.0f, 1e-5f);
        EXPECT_EQ(coverage_cell(c), cell) << "cell " << cell;
    }
}

TEST(Coverage, OppositePolesFallInDifferentCells) {
    const float up[3] = {0.0f, 0.0f, 1.0f};
    const float down[3] = {0.0f, 0.0f, -1.0f};
    EXPECT_NE(coverage_cell(up), coverage_cell(down));
    // Scale does not matter: the cell depends on the direction only.
    const float far_up[3] = {0.0f, 0.0f, 250.0f};
    EXPECT_EQ(coverage_cell(up), coverage_cell(far_up));
}

TEST(Coverage, MatchIsOneAtAnObservedCentreAndZeroOppositeOrEmpty) {
    const float cos_full = std::cos(35.0f * 3.14159265f / 180.0f);
    const float cos_zero = std::cos(90.0f * 3.14159265f / 180.0f);
    for (std::uint32_t cell = 0; cell < kCoverageGrid * kCoverageGrid; ++cell) {
        float c[3];
        coverage_cell_center(cell, c);
        const float opposite[3] = {-c[0], -c[1], -c[2]};
        EXPECT_FLOAT_EQ(coverage_match(1u << cell, c, cos_full, cos_zero), 1.0f);
        EXPECT_FLOAT_EQ(coverage_match(1u << cell, opposite, cos_full, cos_zero), 0.0f);
        EXPECT_FLOAT_EQ(coverage_match(0u, c, cos_full, cos_zero), 0.0f);
    }
    // In between: monotone in the angle to the observed centre.
    const float pole[3] = {0.0f, 0.0f, 1.0f};
    const std::uint32_t mask = 1u << coverage_cell(pole);
    float previous = 2.0f;
    for (int deg = 0; deg <= 180; deg += 10) {
        const float a = static_cast<float>(deg) * 3.14159265f / 180.0f;
        const float dir[3] = {std::sin(a), 0.0f, std::cos(a)};
        const float m = coverage_match(mask, dir, cos_full, cos_zero);
        EXPECT_LE(m, previous + 1e-6f) << deg << " deg";
        previous = m;
    }
}

TEST(Coverage, ParsesCamerasJsonIntoRub) {
    const std::string json = R"([
        {"id": 0, "img_name": "a\"b", "width": 640, "height": 480,
         "position": [1.0, 2.0, 3.0],
         "rotation": [[1, 0, 0], [0, 1, 0], [0, 0, 1]], "fy": 500.5, "fx": 400}
    ])";
    auto cams = parse_cameras_json(json);
    ASSERT_TRUE(cams.ok()) << cams.error();
    ASSERT_EQ(cams.value().size(), 1u);
    const CaptureCamera& c = cams.value()[0];
    EXPECT_FLOAT_EQ(c.position[0], 1.0f);
    EXPECT_FLOAT_EQ(c.position[1], -2.0f);
    EXPECT_FLOAT_EQ(c.position[2], -3.0f);
    EXPECT_FLOAT_EQ(c.forward[2], -1.0f);  // RDF +z forward -> RUB -z
    EXPECT_FLOAT_EQ(c.down[1], -1.0f);
    EXPECT_FLOAT_EQ(c.right[0], 1.0f);
    EXPECT_FLOAT_EQ(c.fx, 400.0f);
    EXPECT_FLOAT_EQ(c.fy, 500.5f);
    EXPECT_EQ(c.width, 640u);
    EXPECT_EQ(c.height, 480u);
}

TEST(Coverage, RejectsMalformedCameras) {
    EXPECT_FALSE(parse_cameras_json("[{\"fx\": 1").ok());
    EXPECT_FALSE(parse_cameras_json("{}").ok());
    EXPECT_FALSE(parse_cameras_json("[{\"fx\": 1, \"fy\": 1, \"width\": 4, \"height\": 4}]").ok());
    EXPECT_FALSE(
        parse_cameras_json("[{\"fx\": -1, \"fy\": 1, \"width\": 4, \"height\": 4, \"position\": [0,0,0], "
                           "\"rotation\": [[1,0,0],[0,1,0],[0,0,1]]}]")
            .ok());
    EXPECT_TRUE(parse_cameras_json("[]").ok());
}

TEST(Coverage, OcclusionAndFrustumDecideWhoObservedWhat) {
    // Two opaque splats on the z axis, a camera on each side looking at them, one splat outside
    // every frustum, one behind camera A only (and outside B's image).
    const GpuPack pack =
        make_pack({0, 0, 8, /**/ 0, 0, 5, /**/ 100, 0, 0, /**/ 0, 0, 12}, {1.0f, 1.0f, 1.0f, 1.0f});
    const float pa[3] = {0, 0, 10}, ra[3] = {1, 0, 0}, da[3] = {0, -1, 0}, fa[3] = {0, 0, -1};
    const float pb[3] = {0, 0, -10}, rb[3] = {-1, 0, 0}, db[3] = {0, -1, 0}, fb[3] = {0, 0, 1};
    const std::vector<CaptureCamera> cams = {make_camera(pa, ra, da, fa), make_camera(pb, rb, db, fb)};
    const float to_a[3] = {0, 0, 1}, to_b[3] = {0, 0, -1};
    for (int threads : {1, 2, 4}) {
        CoverageOptions options;
        options.threads = threads;
        const std::vector<std::uint32_t> mask = compute_coverage(pack, cams, options);
        ASSERT_EQ(mask.size(), 4u);
        EXPECT_EQ(mask[0], 1u << coverage_cell(to_a)) << threads;  // front for A; hidden from B
        EXPECT_EQ(mask[1], 1u << coverage_cell(to_b)) << threads;  // front for B; hidden from A
        EXPECT_EQ(mask[2], 0u) << threads;                         // outside both images
        // Behind A; B sees it straight ahead but behind the two opaque splats.
        EXPECT_EQ(mask[3], 0u) << threads;
    }
    // A transparent occluder does not hide: make the splat nearer A transparent.
    const GpuPack faint = make_pack({0, 0, 8, /**/ 0, 0, 5}, {0.1f, 1.0f});
    const std::vector<std::uint32_t> m2 = compute_coverage(faint, cams);
    EXPECT_EQ(m2[1], (1u << coverage_cell(to_a)) | (1u << coverage_cell(to_b)));
}

TEST(Coverage, SidecarRoundTripAndBinding) {
    const GpuPack pack = make_pack({0, 0, 1, 1, 2, 3}, {1.0f, 0.5f});
    CoverageHeader header;
    header.count = 2;
    header.positions_hash = coverage_positions_hash(pack);
    header.camera_count = 7;
    const std::vector<std::uint32_t> masks = {0x1u, 0x1000000u};
    std::vector<std::uint8_t> bytes = serialize_coverage(header, masks);
    ASSERT_EQ(bytes.size(), 32u + 8u);
    auto parsed = parse_coverage(bytes, pack);
    ASSERT_TRUE(parsed.ok()) << parsed.error();
    EXPECT_EQ(parsed.value(), masks);

    GpuPack moved = pack;
    moved.positions[0] = 0.5f;  // same count, different scene
    EXPECT_FALSE(parse_coverage(bytes, moved).ok());
    const GpuPack shorter = make_pack({0, 0, 1}, {1.0f});
    EXPECT_FALSE(parse_coverage(bytes, shorter).ok());
    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 1);
    EXPECT_FALSE(parse_coverage(truncated, pack).ok());
    bytes[0] ^= 0xFF;
    EXPECT_FALSE(parse_coverage(bytes, pack).ok());
}

}  // namespace
}  // namespace splatcore
