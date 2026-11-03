#include "splatcore/ply_io.h"

#include <gtest/gtest.h>

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace splatcore {
namespace {

// Builds raw INRIA-layout PLY bytes: x y z [nx ny nz] f_dc f_rest(planar) opacity scale rot.
struct PlyBuilder {
    std::size_t count = 0;
    int degree = 0;
    bool with_normals = true;

    int rest_per_channel() const { return sh_rest_coeffs(degree); }
    std::size_t row_floats() const {
        return 3 + (with_normals ? 3 : 0) + 3 + 3 * static_cast<std::size_t>(rest_per_channel()) + 1 + 3 + 4;
    }

    std::string header() const {
        std::string h = "ply\nformat binary_little_endian 1.0\n";
        h += "element vertex " + std::to_string(count) + "\n";
        for (const char* p : {"x", "y", "z"}) h += std::string("property float ") + p + "\n";
        if (with_normals) {
            for (const char* p : {"nx", "ny", "nz"}) h += std::string("property float ") + p + "\n";
        }
        for (int i = 0; i < 3; ++i) h += "property float f_dc_" + std::to_string(i) + "\n";
        for (int k = 0; k < 3 * rest_per_channel(); ++k) {
            h += "property float f_rest_" + std::to_string(k) + "\n";
        }
        h += "property float opacity\n";
        for (int i = 0; i < 3; ++i) h += "property float scale_" + std::to_string(i) + "\n";
        for (int i = 0; i < 4; ++i) h += "property float rot_" + std::to_string(i) + "\n";
        h += "end_header\n";
        return h;
    }

    // Deterministic per-splat values; f_rest coefficient k of channel ch is 100*ch + k.
    std::vector<float> row(std::size_t i) const {
        std::vector<float> r;
        const float fi = static_cast<float>(i);
        for (int c = 0; c < 3; ++c) r.push_back(fi + 0.1f * static_cast<float>(c));  // x y z
        if (with_normals) {
            for (int c = 0; c < 3; ++c) r.push_back(0.0f);
        }
        for (int c = 0; c < 3; ++c) r.push_back(0.5f * static_cast<float>(c) - fi);  // f_dc
        for (int ch = 0; ch < 3; ++ch) {
            for (int k = 0; k < rest_per_channel(); ++k) {
                r.push_back(static_cast<float>(100 * ch + k));
            }
        }
        r.push_back(0.25f * fi);                                                        // opacity
        for (int c = 0; c < 3; ++c) r.push_back(-1.0f - 0.5f * static_cast<float>(c));  // scale
        r.push_back(1.0f);                                                              // rot (w,x,y,z)
        r.push_back(0.0f);
        r.push_back(0.0f);
        r.push_back(0.0f);
        EXPECT_EQ(r.size(), row_floats());
        return r;
    }

    std::string bytes() const {
        std::string out = header();
        for (std::size_t i = 0; i < count; ++i) {
            const std::vector<float> r = row(i);
            out.append(reinterpret_cast<const char*>(r.data()), r.size() * sizeof(float));
        }
        return out;
    }
};

Result<SplatCloud> read_from_string(const std::string& bytes) {
    std::istringstream in(bytes, std::ios::binary);
    return read_ply(in);
}

SplatCloud make_cloud(std::size_t n, int degree) {
    SplatCloud cloud;
    cloud.resize(n, degree);
    const int rest = sh_rest_coeffs(degree);
    for (std::size_t i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i);
        for (int c = 0; c < 3; ++c) {
            cloud.positions[3 * i + c] = fi * 2.0f + static_cast<float>(c);
            cloud.log_scales[3 * i + c] = -2.0f + 0.25f * static_cast<float>(c);
            cloud.dc[3 * i + c] = 0.125f * fi - static_cast<float>(c);
        }
        cloud.opacities[i] = 3.0f - fi;
        // Exactly normalized quaternion so the reader's renormalization is bit-preserving.
        cloud.rotations[4 * i + 0] = (i % 2 == 0) ? 1.0f : 0.0f;
        cloud.rotations[4 * i + 1] = (i % 2 == 0) ? 0.0f : 1.0f;
        cloud.rotations[4 * i + 2] = 0.0f;
        cloud.rotations[4 * i + 3] = 0.0f;
        for (int k = 0; k < 3 * rest; ++k) {
            cloud.sh[3 * static_cast<std::size_t>(rest) * i + static_cast<std::size_t>(k)] =
                0.01f * static_cast<float>(k) + fi;
        }
    }
    return cloud;
}

void expect_clouds_equal(const SplatCloud& a, const SplatCloud& b) {
    ASSERT_EQ(a.count, b.count);
    ASSERT_EQ(a.sh_degree, b.sh_degree);
    EXPECT_EQ(a.positions, b.positions);
    EXPECT_EQ(a.log_scales, b.log_scales);
    EXPECT_EQ(a.opacities, b.opacities);
    EXPECT_EQ(a.dc, b.dc);
    EXPECT_EQ(a.sh, b.sh);
    ASSERT_EQ(a.rotations.size(), b.rotations.size());
    for (std::size_t i = 0; i < a.rotations.size(); ++i) {
        EXPECT_NEAR(a.rotations[i], b.rotations[i], 1e-6f) << "rotation component " << i;
    }
}

TEST(PlyRoundTrip, Degree0) {
    const SplatCloud cloud = make_cloud(17, 0);
    std::ostringstream out(std::ios::binary);
    ASSERT_TRUE(write_ply(cloud, out).ok());
    Result<SplatCloud> back = read_from_string(out.str());
    ASSERT_TRUE(back.ok()) << back.error();
    expect_clouds_equal(cloud, back.value());
}

TEST(PlyRoundTrip, Degree1) {
    const SplatCloud cloud = make_cloud(9, 1);
    std::ostringstream out(std::ios::binary);
    ASSERT_TRUE(write_ply(cloud, out).ok());
    Result<SplatCloud> back = read_from_string(out.str());
    ASSERT_TRUE(back.ok()) << back.error();
    expect_clouds_equal(cloud, back.value());
}

TEST(PlyRoundTrip, EmptyCloud) {
    const SplatCloud cloud = make_cloud(0, 0);
    std::ostringstream out(std::ios::binary);
    ASSERT_TRUE(write_ply(cloud, out).ok());
    Result<SplatCloud> back = read_from_string(out.str());
    ASSERT_TRUE(back.ok()) << back.error();
    EXPECT_EQ(back.value().count, 0u);
}

TEST(PlyReader, ReadsDegree2WithNormals) {
    PlyBuilder builder{.count = 3, .degree = 2, .with_normals = true};
    Result<SplatCloud> result = read_from_string(builder.bytes());
    ASSERT_TRUE(result.ok()) << result.error();
    const SplatCloud& cloud = result.value();
    EXPECT_EQ(cloud.count, 3u);
    EXPECT_EQ(cloud.sh_degree, 2);
    EXPECT_FLOAT_EQ(cloud.positions[3], 1.0f);
    EXPECT_FLOAT_EQ(cloud.opacities[2], 0.5f);
    // Planar file layout (100*ch + k) must land coefficient-major: sh[k*3 + ch].
    const int rest = sh_rest_coeffs(2);
    ASSERT_EQ(rest, 8);
    for (std::size_t i = 0; i < cloud.count; ++i) {
        const float* sh = cloud.sh.data() + 3 * static_cast<std::size_t>(rest) * i;
        for (int ch = 0; ch < 3; ++ch) {
            for (int k = 0; k < rest; ++k) {
                EXPECT_FLOAT_EQ(sh[k * 3 + ch], static_cast<float>(100 * ch + k));
            }
        }
    }
}

TEST(PlyReader, ReadsDegree3) {
    PlyBuilder builder{.count = 2, .degree = 3, .with_normals = true};
    Result<SplatCloud> result = read_from_string(builder.bytes());
    ASSERT_TRUE(result.ok()) << result.error();
    EXPECT_EQ(result.value().sh_degree, 3);
    const int rest = sh_rest_coeffs(3);
    ASSERT_EQ(rest, 15);
    const float* sh = result.value().sh.data();
    EXPECT_FLOAT_EQ(sh[14 * 3 + 2], 214.0f);  // last coefficient, blue channel
}

TEST(PlyReader, NormalizesQuaternions) {
    PlyBuilder builder{.count = 1, .degree = 0, .with_normals = false};
    std::string bytes = builder.header();
    std::vector<float> row = builder.row(0);
    row[row.size() - 4] = 2.0f;  // rot_0 = w
    bytes.append(reinterpret_cast<const char*>(row.data()), row.size() * sizeof(float));
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_TRUE(result.ok()) << result.error();
    EXPECT_FLOAT_EQ(result.value().rotations[0], 1.0f);
    EXPECT_FLOAT_EQ(result.value().rotations[1], 0.0f);
}

TEST(PlyReader, LeavesZeroNormQuaternion) {
    PlyBuilder builder{.count = 1, .degree = 0, .with_normals = false};
    std::string bytes = builder.header();
    std::vector<float> row = builder.row(0);
    row[row.size() - 4] = 0.0f;  // zero-norm quaternion, left for drop_invalid
    bytes.append(reinterpret_cast<const char*>(row.data()), row.size() * sizeof(float));
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_TRUE(result.ok()) << result.error();
    EXPECT_FLOAT_EQ(result.value().rotations[0], 0.0f);
}

TEST(PlyReader, RejectsBadMagic) {
    std::string bytes = PlyBuilder{.count = 1, .degree = 0}.bytes();
    bytes[0] = 'q';
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("magic"), std::string::npos);
}

TEST(PlyReader, RejectsAscii) {
    std::string bytes = PlyBuilder{.count = 1, .degree = 0}.bytes();
    const std::string from = "binary_little_endian";
    bytes.replace(bytes.find(from), from.size(), "ascii");
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("ASCII"), std::string::npos);
}

TEST(PlyReader, RejectsBigEndian) {
    std::string bytes = PlyBuilder{.count = 1, .degree = 0}.bytes();
    const std::string from = "binary_little_endian";
    bytes.replace(bytes.find(from), from.size(), "binary_big_endian");
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("big-endian"), std::string::npos);
}

TEST(PlyReader, RejectsUnsupportedFRestCount) {
    // Degree-1 header with f_rest_9 added on top: 10 rest properties is not a valid degree.
    PlyBuilder builder{.count = 0, .degree = 1};
    std::string bytes = builder.bytes();
    const std::string anchor = "property float opacity\n";
    bytes.insert(bytes.find(anchor), "property float f_rest_9\n");
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("f_rest count"), std::string::npos);
}

TEST(PlyReader, RejectsNonFloatProperty) {
    std::string bytes = PlyBuilder{.count = 0, .degree = 0}.bytes();
    const std::string from = "property float x\n";
    bytes.replace(bytes.find(from), from.size(), "property double x\n");
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("double"), std::string::npos);
}

TEST(PlyReader, RejectsMissingProperty) {
    std::string bytes = PlyBuilder{.count = 0, .degree = 0}.bytes();
    const std::string from = "property float opacity\n";
    bytes.replace(bytes.find(from), from.size(), "");
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("opacity"), std::string::npos);
}

TEST(PlyReader, RejectsUnknownProperty) {
    std::string bytes = PlyBuilder{.count = 0, .degree = 0}.bytes();
    const std::string anchor = "property float opacity\n";
    bytes.insert(bytes.find(anchor), "property float mystery\n");
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("mystery"), std::string::npos);
}

TEST(PlyReader, RejectsTruncatedData) {
    std::string bytes = PlyBuilder{.count = 4, .degree = 0}.bytes();
    bytes.resize(bytes.size() - 10);
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("truncated"), std::string::npos);
}

TEST(PlyReader, RejectsHeaderWithoutEnd) {
    const std::string bytes = "ply\nformat binary_little_endian 1.0\nelement vertex 1\nproperty float x\n";
    Result<SplatCloud> result = read_from_string(bytes);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().find("header"), std::string::npos);
}

TEST(PlyWriter, RejectsDegree2) {
    const SplatCloud cloud = make_cloud(2, 2);
    std::ostringstream out(std::ios::binary);
    Status status = write_ply(cloud, out);
    ASSERT_FALSE(status.ok());
    EXPECT_NE(status.error().find("degree"), std::string::npos);
}

TEST(PlyWriter, RejectsInconsistentSizes) {
    SplatCloud cloud = make_cloud(2, 0);
    cloud.positions.pop_back();
    std::ostringstream out(std::ios::binary);
    EXPECT_FALSE(write_ply(cloud, out).ok());
}

}  // namespace
}  // namespace splatcore
