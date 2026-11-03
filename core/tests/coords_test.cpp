#include "splatcore/coords.h"

#include <gtest/gtest.h>

namespace splatcore {
namespace {

SplatCloud one_splat_cloud(int degree) {
    SplatCloud cloud;
    cloud.resize(1, degree);
    cloud.positions = {1.0f, 2.0f, 3.0f};
    cloud.rotations = {0.5f, 0.5f, 0.5f, 0.5f};  // (w, x, y, z)
    cloud.log_scales = {-1.0f, -2.0f, -3.0f};
    cloud.opacities = {0.7f};
    cloud.dc = {0.1f, 0.2f, 0.3f};
    for (std::size_t k = 0; k < cloud.sh.size(); ++k) cloud.sh[k] = 1.0f + static_cast<float>(k);
    return cloud;
}

TEST(Coords, NoOpWhenSameSystem) {
    SplatCloud cloud = one_splat_cloud(1);
    const SplatCloud original = cloud;
    convert_coordinates(cloud, CoordSystem::kRub, CoordSystem::kRub);
    EXPECT_EQ(cloud.positions, original.positions);
    EXPECT_EQ(cloud.sh, original.sh);
}

TEST(Coords, RoundTripIsIdentity) {
    SplatCloud cloud = one_splat_cloud(3);
    const SplatCloud original = cloud;
    convert_coordinates(cloud, CoordSystem::kRdf, CoordSystem::kRub);
    convert_coordinates(cloud, CoordSystem::kRub, CoordSystem::kRdf);
    EXPECT_EQ(cloud.positions, original.positions);
    EXPECT_EQ(cloud.rotations, original.rotations);
    EXPECT_EQ(cloud.sh, original.sh);
    EXPECT_EQ(cloud.log_scales, original.log_scales);
    EXPECT_EQ(cloud.dc, original.dc);
}

TEST(Coords, PositionAndQuaternionFlipYZ) {
    SplatCloud cloud = one_splat_cloud(0);
    convert_coordinates(cloud, CoordSystem::kRdf, CoordSystem::kRub);
    EXPECT_FLOAT_EQ(cloud.positions[0], 1.0f);
    EXPECT_FLOAT_EQ(cloud.positions[1], -2.0f);
    EXPECT_FLOAT_EQ(cloud.positions[2], -3.0f);
    EXPECT_FLOAT_EQ(cloud.rotations[0], 0.5f);    // w
    EXPECT_FLOAT_EQ(cloud.rotations[1], 0.5f);    // x
    EXPECT_FLOAT_EQ(cloud.rotations[2], -0.5f);   // y
    EXPECT_FLOAT_EQ(cloud.rotations[3], -0.5f);   // z
    EXPECT_FLOAT_EQ(cloud.log_scales[1], -2.0f);  // scales unchanged
    EXPECT_FLOAT_EQ(cloud.dc[0], 0.1f);           // colors unchanged
}

TEST(Coords, ShSignTable) {
    // Expected sign per f_rest coefficient under y,z negation, from the basis parity:
    // band 1 (y,z,x), band 2 (xy,yz,3z2-r2,xz,x2-y2), band 3 (m=-3..3).
    const float expected_sign[15] = {-1, -1, +1, -1, +1, +1, -1, +1, -1, +1, -1, -1, +1, -1, +1};
    SplatCloud cloud = one_splat_cloud(3);
    const SplatCloud original = cloud;
    convert_coordinates(cloud, CoordSystem::kRdf, CoordSystem::kRub);
    for (int k = 0; k < 15; ++k) {
        for (int ch = 0; ch < 3; ++ch) {
            EXPECT_FLOAT_EQ(cloud.sh[static_cast<std::size_t>(3 * k + ch)],
                            expected_sign[k] * original.sh[static_cast<std::size_t>(3 * k + ch)])
                << "coefficient " << k << " channel " << ch;
        }
    }
}

}  // namespace
}  // namespace splatcore
