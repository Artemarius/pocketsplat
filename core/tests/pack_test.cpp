#include "splatcore/pack.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

#include "splatcore/cleaning.h"

namespace splatcore {
namespace {

SplatCloud make_cloud(std::size_t n, int degree) {
    SplatCloud cloud;
    cloud.resize(n, degree);
    const int rest = sh_rest_coeffs(degree);
    for (std::size_t i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i);
        for (int c = 0; c < 3; ++c) {
            cloud.positions[3 * i + c] = fi + 0.25f * static_cast<float>(c);
            cloud.log_scales[3 * i + c] = -3.0f + 0.125f * static_cast<float>(c);
            cloud.dc[3 * i + c] = 0.5f - 0.01f * fi;
        }
        cloud.opacities[i] = 1.0f;
        cloud.rotations[4 * i] = 1.0f;
        for (int k = 0; k < 3 * rest; ++k) {
            cloud.sh[3 * static_cast<std::size_t>(rest) * i + static_cast<std::size_t>(k)] =
                0.3f - 0.02f * static_cast<float>(k);
        }
    }
    return cloud;
}

TEST(Pack, HeaderLayoutIsByteExact) {
    const SplatCloud cloud = make_cloud(1, 0);
    Result<GpuPack> pack = build_pack(cloud);
    ASSERT_TRUE(pack.ok()) << pack.error();
    const std::vector<std::uint8_t> bytes = pack.value().serialize();
    ASSERT_GE(bytes.size(), 64u);
    EXPECT_EQ(bytes[0], 'P');
    EXPECT_EQ(bytes[1], 'S');
    EXPECT_EQ(bytes[2], 'P');
    EXPECT_EQ(bytes[3], 'K');
    std::uint32_t version, count, chunk_size, sh_degree, coords;
    std::memcpy(&version, bytes.data() + 4, 4);
    std::memcpy(&count, bytes.data() + 8, 4);
    std::memcpy(&chunk_size, bytes.data() + 12, 4);
    std::memcpy(&sh_degree, bytes.data() + 16, 4);
    std::memcpy(&coords, bytes.data() + 20, 4);
    EXPECT_EQ(version, kPackVersion);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(chunk_size, 256u);
    EXPECT_EQ(sh_degree, 0u);
    EXPECT_EQ(coords, kPackCoordsRub);
}

TEST(Pack, KnownValueEncodingDegree1) {
    SplatCloud cloud;
    cloud.resize(1, 1);
    cloud.positions = {1.5f, -2.25f, 3.0f};
    cloud.log_scales = {1.0f, -2.0f, 0.5f};          // halves 0x3C00, 0xC000, 0x3800
    cloud.rotations = {-1.0f, 0.0f, 0.0f, 0.0f};     // canonicalized to (1,0,0,0)
    cloud.opacities = {0.0f};                        // sigmoid -> 0.5 -> 128
    cloud.dc = {0.5f / kShC0, 0.0f, -0.5f / kShC0};  // display 1.0, 0.5, 0.0 -> 255, 128, 0
    cloud.sh = {0.5f, -0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    Result<GpuPack> result = build_pack(cloud);
    ASSERT_TRUE(result.ok()) << result.error();
    const GpuPack& pack = result.value();

    EXPECT_EQ(pack.positions, (std::vector<float>{1.5f, -2.25f, 3.0f}));
    EXPECT_EQ(pack.scale_rot[0], 0xC0003C00u);     // h(1.0) | h(-2.0)<<16
    EXPECT_EQ(pack.scale_rot[1], 0x007F3800u);     // h(0.5) | snorm8(1)<<16 | snorm8(0)<<24
    EXPECT_EQ(pack.scale_rot[2], 0x00000000u);     // snorm8(0) | snorm8(0)<<8 | pad
    EXPECT_EQ(pack.color[0], 0x800080FFu);         // r=255 g=128 b=0 a=128
    EXPECT_FLOAT_EQ(pack.header.sh1_scale, 0.5f);  // max-abs coefficient
    // Quantized SH: 0.5/0.5*127 = 127 = 0x7F; -0.25/0.5*127 = -63.5 -> -64 -> 0xC0; rest 0.
    EXPECT_EQ(pack.sh1[0], 0x0000C07Fu);
    EXPECT_EQ(pack.sh1[1], 0x00000000u);
    EXPECT_EQ(pack.sh1[2], 0x00000000u);

    // Single chunk: cull AABB = position +/- 3 * exp(max log-scale), plus the importance.
    const float r = 3.0f * std::exp(1.0f);
    ASSERT_EQ(pack.chunks.size(), 7u);
    EXPECT_FLOAT_EQ(pack.chunks[0], 1.5f - r);
    EXPECT_FLOAT_EQ(pack.chunks[3], 1.5f + r);
    EXPECT_FLOAT_EQ(pack.chunks[6], importance(cloud, 0));

    // 40 B/splat at SH 1: 12 pos + 12 scale_rot + 4 color + 12 sh1.
    EXPECT_EQ(pack.serialize().size(), 64u + 40u + 7u * 4u);
}

TEST(Pack, Degree0HasNoSh1AndIs28Bytes) {
    const SplatCloud cloud = make_cloud(3, 0);
    Result<GpuPack> pack = build_pack(cloud);
    ASSERT_TRUE(pack.ok()) << pack.error();
    EXPECT_TRUE(pack.value().sh1.empty());
    EXPECT_FLOAT_EQ(pack.value().header.sh1_scale, 0.0f);
    EXPECT_EQ(pack.value().serialize().size(), 64u + 3u * 28u + 7u * 4u);
}

TEST(Pack, ChunkBoundsAreConservative) {
    const std::size_t n = 300;  // 256 + 44
    const SplatCloud cloud = make_cloud(n, 0);
    Result<GpuPack> result = build_pack(cloud);
    ASSERT_TRUE(result.ok()) << result.error();
    const GpuPack& pack = result.value();
    ASSERT_EQ(pack.chunk_count(), 2u);
    ASSERT_EQ(pack.chunks.size(), 14u);
    // Positions are increasing in i and every splat shares the same log-scales, so the cull AABBs
    // are the center bounds expanded by 3 * exp(max log-scale) = the 3-sigma splat extent.
    const float r = 3.0f * std::exp(-2.75f);       // make_cloud: max log-scale is -3.0 + 0.125 * 2
    EXPECT_FLOAT_EQ(pack.chunks[0], 0.0f - r);     // chunk 0 min x = splat 0
    EXPECT_FLOAT_EQ(pack.chunks[3], 255.0f + r);   // chunk 0 max x = splat 255
    EXPECT_FLOAT_EQ(pack.chunks[7], 256.0f - r);   // chunk 1 min x
    EXPECT_FLOAT_EQ(pack.chunks[10], 299.0f + r);  // chunk 1 max x
    float importance_max = 0.0f;
    for (std::size_t i = 0; i < 256; ++i) importance_max = std::max(importance_max, importance(cloud, i));
    EXPECT_FLOAT_EQ(pack.chunks[6], importance_max);
}

TEST(Pack, DeterministicSerialization) {
    const SplatCloud cloud = make_cloud(500, 1);
    Result<GpuPack> a = build_pack(cloud);
    Result<GpuPack> b = build_pack(cloud);
    ASSERT_TRUE(a.ok() && b.ok());
    EXPECT_EQ(a.value().serialize(), b.value().serialize());
    EXPECT_NE(a.value().header.source_hash, 0u);
}

TEST(Pack, RejectsDegree2) {
    const SplatCloud cloud = make_cloud(2, 2);
    Result<GpuPack> pack = build_pack(cloud);
    ASSERT_FALSE(pack.ok());
    EXPECT_NE(pack.error().find("degree"), std::string::npos);
}

TEST(Pack, EmptyCloudSerializesToHeaderOnly) {
    Result<GpuPack> pack = build_pack(SplatCloud{});
    ASSERT_TRUE(pack.ok()) << pack.error();
    EXPECT_EQ(pack.value().serialize().size(), 64u);
    EXPECT_EQ(pack.value().chunk_count(), 0u);
}

TEST(Pack, ParsePackRoundTripsSerialize) {
    const SplatCloud cloud = make_cloud(300, 1);
    Result<GpuPack> built = build_pack(cloud);
    ASSERT_TRUE(built.ok());
    const std::vector<std::uint8_t> bytes = built.value().serialize();
    Result<GpuPack> parsed = parse_pack(bytes);
    ASSERT_TRUE(parsed.ok()) << parsed.error();
    EXPECT_EQ(parsed.value().positions, built.value().positions);
    EXPECT_EQ(parsed.value().scale_rot, built.value().scale_rot);
    EXPECT_EQ(parsed.value().color, built.value().color);
    EXPECT_EQ(parsed.value().sh1, built.value().sh1);
    EXPECT_EQ(parsed.value().chunks, built.value().chunks);
    EXPECT_EQ(parsed.value().serialize(), bytes);

    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 4);
    EXPECT_FALSE(parse_pack(truncated).ok());
}

TEST(Pack, ParseHeaderRoundTripAndValidation) {
    const SplatCloud cloud = make_cloud(7, 1);
    Result<GpuPack> pack = build_pack(cloud);
    ASSERT_TRUE(pack.ok());
    std::vector<std::uint8_t> bytes = pack.value().serialize();
    Result<PackHeader> header = parse_pack_header(bytes);
    ASSERT_TRUE(header.ok()) << header.error();
    EXPECT_EQ(header.value().count, 7u);
    EXPECT_EQ(header.value().sh_degree, 1u);
    EXPECT_EQ(header.value().source_hash, pack.value().header.source_hash);

    bytes[0] = 'X';
    EXPECT_FALSE(parse_pack_header(bytes).ok());
    EXPECT_FALSE(parse_pack_header(std::vector<std::uint8_t>(10)).ok());
}

TEST(PackSubset, KeepsTheMostImportantInPackOrderAndRebuildsChunks) {
    SplatCloud cloud;
    cloud.resize(1000, 0);
    for (std::size_t i = 0; i < 1000; ++i) {
        cloud.positions[3 * i] = static_cast<float>(i);  // along x, in order
        for (int k = 0; k < 3; ++k) cloud.log_scales[3 * i + static_cast<std::size_t>(k)] = std::log(0.01f);
        cloud.rotations[4 * i] = 1.0f;
        // Every third splat is ten times larger: those 334 are the most important.
        if (i % 3 == 0) {
            for (int k = 0; k < 3; ++k)
                cloud.log_scales[3 * i + static_cast<std::size_t>(k)] = std::log(0.1f);
        }
        cloud.opacities[i] = 2.0f;
    }
    const GpuPack pack = build_pack(cloud).value();
    std::vector<std::uint32_t> kept;
    const GpuPack sub = subset_pack(pack, 334, &kept);
    ASSERT_EQ(sub.header.count, 334u);
    ASSERT_EQ(kept.size(), 334u);
    for (std::size_t k = 0; k < kept.size(); ++k) {
        EXPECT_EQ(kept[k] % 3, 0u);
        if (k > 0) EXPECT_GT(kept[k], kept[k - 1]);  // pack order
        EXPECT_EQ(sub.positions[3 * k], pack.positions[3 * kept[k]]);
        EXPECT_EQ(sub.color[k], pack.color[kept[k]]);
    }
    EXPECT_EQ(sub.chunk_count(), 2u);
    // Chunk bounds cover every kept splat's 3-sigma extent.
    for (std::size_t k = 0; k < sub.header.count; ++k) {
        const float* chunk = sub.chunks.data() + kChunkFloats * (k / kPackChunkSize);
        EXPECT_LE(chunk[0], sub.positions[3 * k] - 0.29f);
        EXPECT_GE(chunk[3], sub.positions[3 * k] + 0.29f);
    }
    // Within the limit: unchanged.
    const GpuPack same = subset_pack(pack, 5000, &kept);
    EXPECT_EQ(same.header.count, 1000u);
    EXPECT_EQ(kept.size(), 1000u);
}

}  // namespace
}  // namespace splatcore
