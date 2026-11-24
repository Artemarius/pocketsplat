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
    EXPECT_EQ(pack.scale_rot[2], 0x00000000u);     // snorm8(0) | snorm8(0)<<8 | index 0 (no palette)
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

// ---- Pack v3: SH1 palette (docs/sh1-palette-research.md §2.1) ----

// A 3-entry palette over make_cloud(n, 1): entry e = 0.1 * (e + 1) * (1, -1, 0.5, ...) and
// labels i % 3, except that entry 2 is never used when `skip_last` is set.
Sh1Palette make_palette(std::size_t n, bool skip_last) {
    Sh1Palette palette;
    palette.centroids.resize(3 * kSh1Dims);
    for (std::size_t e = 0; e < 3; ++e) {
        for (std::size_t c = 0; c < kSh1Dims; ++c) {
            const float sign = c % 2 == 0 ? 1.0f : -1.0f;
            palette.centroids[kSh1Dims * e + c] =
                0.1f * static_cast<float>(e + 1) * sign / static_cast<float>(c + 1);
        }
    }
    if (skip_last) palette.centroids[kSh1Dims * 2] = 9.0f;  // unused: must not set sh1_scale
    palette.labels.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        palette.labels[i] = static_cast<std::uint16_t>(skip_last ? i % 2 : i % 3);
    return palette;
}

TEST(PackPalette, KnownValueEncoding) {
    SplatCloud cloud = make_cloud(2, 1);
    Sh1Palette palette;
    palette.centroids = {0.5f, -0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,    // entry 0
                         0.0f, 0.0f,   0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -0.5f};  // entry 1
    palette.labels = {1, 0};
    Result<GpuPack> result = build_pack(cloud, &palette);
    ASSERT_TRUE(result.ok()) << result.error();
    const GpuPack& pack = result.value();

    EXPECT_TRUE(pack.has_palette());
    EXPECT_EQ(pack.header.palette_count(), 2u);
    EXPECT_EQ(pack.header.flags(), 0u);
    EXPECT_FLOAT_EQ(pack.header.sh1_scale, 0.5f);
    // Word 2 = snorm8(y) | snorm8(z)<<8 | index<<16; make_cloud's rotation is (1,0,0,0).
    EXPECT_EQ(pack.scale_rot[2], 0x00010000u);
    EXPECT_EQ(pack.scale_rot[5], 0x00000000u);
    // Entries, as KnownValueEncodingDegree1: 0x7F, 0xC0; entry 1 c2.b = -127 = 0x81.
    ASSERT_EQ(pack.sh1.size(), 6u);
    EXPECT_EQ(pack.sh1[0], 0x0000C07Fu);
    EXPECT_EQ(pack.sh1[1], 0x00000000u);
    EXPECT_EQ(pack.sh1[2], 0x00000000u);
    EXPECT_EQ(pack.sh1[3], 0x00000000u);
    EXPECT_EQ(pack.sh1[4], 0x00000000u);
    EXPECT_EQ(pack.sh1[5], 0x00000081u);

    // Header word 28 = palette_count | flags<<24; the palette is 12 B per entry, not per splat.
    const std::vector<std::uint8_t> bytes = pack.serialize();
    std::uint32_t version = 0, word28 = 0;
    std::memcpy(&version, bytes.data() + 4, 4);
    std::memcpy(&word28, bytes.data() + 28, 4);
    EXPECT_EQ(version, 3u);
    EXPECT_EQ(word28, 2u);
    EXPECT_EQ(bytes.size(), 64u + 2u * 28u + 2u * 12u + 7u * 4u);
}

TEST(PackPalette, DecodesLikeTheDirectPackOfTheSnappedCloud) {
    const std::size_t n = 600;
    for (bool skip_last : {false, true}) {
        const SplatCloud cloud = make_cloud(n, 1);
        const Sh1Palette palette = make_palette(n, skip_last);
        const GpuPack indexed = build_pack(cloud, &palette).value();
        SplatCloud snapped = cloud;
        ASSERT_TRUE(apply_sh1_palette(snapped, palette).ok());
        const GpuPack direct = build_pack(snapped).value();

        EXPECT_EQ(indexed.header.sh1_scale, direct.header.sh1_scale) << "skip_last " << skip_last;
        EXPECT_EQ(indexed.positions, direct.positions);
        EXPECT_EQ(indexed.color, direct.color);
        EXPECT_EQ(indexed.chunks, direct.chunks);
        EXPECT_EQ(indexed.header.source_hash, build_pack(cloud).value().header.source_hash);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t w2 = indexed.scale_rot[3 * i + 2];
            EXPECT_EQ(indexed.scale_rot[3 * i], direct.scale_rot[3 * i]);
            EXPECT_EQ(indexed.scale_rot[3 * i + 1], direct.scale_rot[3 * i + 1]);
            EXPECT_EQ(w2 & 0xFFFFu, direct.scale_rot[3 * i + 2]);
            const std::uint32_t entry = w2 >> 16;
            ASSERT_EQ(entry, palette.labels[i]);
            for (std::size_t w = 0; w < 3; ++w) EXPECT_EQ(indexed.sh1[3 * entry + w], direct.sh1[3 * i + w]);
        }
    }
}

TEST(PackPalette, RoundTripsAndParseRejectsBadPalettes) {
    const std::size_t n = 300;
    const SplatCloud cloud = make_cloud(n, 1);
    const Sh1Palette palette = make_palette(n, false);
    const GpuPack built = build_pack(cloud, &palette).value();
    const std::vector<std::uint8_t> bytes = built.serialize();
    Result<GpuPack> parsed = parse_pack(bytes);
    ASSERT_TRUE(parsed.ok()) << parsed.error();
    EXPECT_EQ(parsed.value().header.palette_count(), 3u);
    EXPECT_EQ(parsed.value().sh1, built.sh1);
    EXPECT_EQ(parsed.value().scale_rot, built.scale_rot);
    EXPECT_EQ(parsed.value().serialize(), bytes);

    const auto set_word = [](std::vector<std::uint8_t> b, std::size_t offset, std::uint32_t v) {
        std::memcpy(b.data() + offset, &v, 4);
        return b;
    };
    // An index at or past palette_count (splat 7, word 2 of scale_rot).
    const std::size_t word2 = 64 + 3 * n * 4 + (3 * 7 + 2) * 4;
    EXPECT_FALSE(parse_pack(set_word(bytes, word2, 3u << 16)).ok());
    EXPECT_TRUE(parse_pack(set_word(bytes, word2, 2u << 16)).ok());
    // Unknown flags, a palette at degree 0, a palette over 65,536 entries, a v2 pack.
    EXPECT_FALSE(parse_pack_header(set_word(bytes, 28, 3u | (1u << 24))).ok());
    EXPECT_FALSE(parse_pack_header(set_word(bytes, 16, 0u)).ok());
    EXPECT_FALSE(parse_pack_header(set_word(bytes, 28, kMaxPaletteSize + 1)).ok());
    EXPECT_FALSE(parse_pack_header(set_word(bytes, 4, 2u)).ok());  // a v2 pack has no palette
    // A palette count that does not match the stored entries fails the size check.
    EXPECT_FALSE(parse_pack(set_word(bytes, 28, 4u)).ok());
}

TEST(Pack, ReadsVersion2AsADirectPackAndRejectsVersion1) {
    const GpuPack built = build_pack(make_cloud(40, 1)).value();
    std::vector<std::uint8_t> bytes = built.serialize();
    const std::uint32_t v2 = 2, v1 = 1, nonzero = 5;
    std::memcpy(bytes.data() + 4, &v2, 4);
    Result<GpuPack> parsed = parse_pack(bytes);
    ASSERT_TRUE(parsed.ok()) << parsed.error();
    EXPECT_EQ(parsed.value().header.version, 2u);
    EXPECT_FALSE(parsed.value().has_palette());
    EXPECT_EQ(parsed.value().sh1, built.sh1);
    std::vector<std::uint8_t> reserved_set = bytes;
    std::memcpy(reserved_set.data() + 28, &nonzero, 4);
    EXPECT_FALSE(parse_pack_header(reserved_set).ok());
    std::memcpy(bytes.data() + 4, &v1, 4);
    EXPECT_FALSE(parse_pack_header(bytes).ok());
}

TEST(PackPalette, BuildRejectsMismatchedPalettes) {
    const SplatCloud cloud = make_cloud(10, 1);
    Sh1Palette palette = make_palette(10, false);
    EXPECT_FALSE(build_pack(make_cloud(10, 0), &palette).ok());  // degree 0 cloud
    Sh1Palette short_labels = palette;
    short_labels.labels.pop_back();
    EXPECT_FALSE(build_pack(cloud, &short_labels).ok());
    Sh1Palette bad_label = palette;
    bad_label.labels[4] = 3;
    EXPECT_FALSE(build_pack(cloud, &bad_label).ok());
    Sh1Palette empty;
    empty.labels.assign(10, 0);
    EXPECT_FALSE(build_pack(cloud, &empty).ok());
}

TEST(PackPalette, SubsetKeepsThePaletteWholeAndTheIndices) {
    const std::size_t n = 1000;
    SplatCloud cloud = make_cloud(n, 1);
    for (std::size_t i = 0; i < n; i += 3) {  // every third splat is the most important
        for (std::size_t c = 0; c < 3; ++c) cloud.log_scales[3 * i + c] = -1.0f;
    }
    const Sh1Palette palette = make_palette(n, false);
    const GpuPack pack = build_pack(cloud, &palette).value();
    std::vector<std::uint32_t> kept;
    const GpuPack sub = subset_pack(pack, 334, &kept);
    ASSERT_EQ(sub.header.count, 334u);
    EXPECT_EQ(sub.header.palette_count(), 3u);
    EXPECT_EQ(sub.sh1, pack.sh1);
    for (std::size_t k = 0; k < kept.size(); ++k) {
        EXPECT_EQ(sub.scale_rot[3 * k + 2], pack.scale_rot[3 * kept[k] + 2]);
        EXPECT_EQ(sub.scale_rot[3 * k + 2] >> 16, palette.labels[kept[k]]);
    }
    EXPECT_TRUE(parse_pack(sub.serialize()).ok());
}

// ADR-0018: segment plans at the edges.
constexpr std::uint64_t kAdreno750Range = 134217728;   // 128 MB
constexpr std::uint64_t kAdreno830Range = 2147483647;  // 2 GB - 1
constexpr std::uint64_t kSpecMinWorkgroups = 65535;

TEST(PackSegments, Adreno750SplitsAtAWholeNumberOfGranules) {
    // 128 MB / 12 B = 11,184,810 splats -> 170 granules of 65,536 = 11,141,120.
    Result<SegmentPlan> one = plan_segments(11141120, kAdreno750Range, kSpecMinWorkgroups);
    ASSERT_TRUE(one.ok()) << one.error();
    EXPECT_EQ(one.value().segment_splats, 11141120u);
    EXPECT_EQ(one.value().segment_splats % kSegmentGranule, 0u);
    EXPECT_EQ(one.value().segment_count, 1u);
    Result<SegmentPlan> two = plan_segments(11141121, kAdreno750Range, kSpecMinWorkgroups);
    ASSERT_TRUE(two.ok());
    EXPECT_EQ(two.value().segment_count, 2u);
    Result<SegmentPlan> single = plan_segments(1, kAdreno750Range, kSpecMinWorkgroups);
    ASSERT_TRUE(single.ok());
    EXPECT_EQ(single.value().segment_count, 1u);
    // bicycle x4 (17.93 M): two segments on the 750.
    EXPECT_EQ(plan_segments(17932420, kAdreno750Range, kSpecMinWorkgroups).value().segment_count, 2u);
}

TEST(PackSegments, TheDispatchLimitCapsWideBindings) {
    // The 830's 2 GB binding holds 178 M splats, but 65,535 workgroups of 256 splats are 16.78 M:
    // 255 granules = 16,711,680.
    Result<SegmentPlan> plan = plan_segments(17932420, kAdreno830Range, kSpecMinWorkgroups);
    ASSERT_TRUE(plan.ok());
    EXPECT_EQ(plan.value().segment_splats, 16711680u);
    EXPECT_EQ(plan.value().segment_count, 2u);
    // A GPU reporting a larger dispatch limit keeps one segment.
    EXPECT_EQ(plan_segments(17932420, kAdreno830Range, 0x7fffffffu).value().segment_count, 1u);
}

TEST(PackSegments, ForcedSizesAndRejections) {
    Result<SegmentPlan> forced =
        plan_segments(300000, kAdreno750Range, kSpecMinWorkgroups, 12, kSegmentGranule);
    ASSERT_TRUE(forced.ok());
    EXPECT_EQ(forced.value().segment_splats, kSegmentGranule);
    EXPECT_EQ(forced.value().segment_count, 5u);  // 4 full + 37,856
    EXPECT_FALSE(plan_segments(300000, kAdreno750Range, kSpecMinWorkgroups, 12, kSegmentGranule + 256).ok());
    EXPECT_FALSE(
        plan_segments(300000, kAdreno750Range, kSpecMinWorkgroups, 12, 171ull * kSegmentGranule).ok());
    EXPECT_FALSE(plan_segments(0, kAdreno750Range, kSpecMinWorkgroups).ok());
    EXPECT_FALSE(plan_segments(kMaxGpuSplats + 1, kAdreno830Range, 0x7fffffffu).ok());
    EXPECT_FALSE(
        plan_segments(1000, kSegmentGranule * 12 - 1, kSpecMinWorkgroups).ok());  // under one granule
    EXPECT_TRUE(plan_segments(1000, kSegmentGranule * 12, kSpecMinWorkgroups).ok());
}

TEST(PackSegments, MemoryBound) {
    PackHeader header;
    header.sh_degree = 0;
    EXPECT_EQ(gpu_bytes_per_splat(header, false), 29u);
    header.sh_degree = 1;
    EXPECT_EQ(gpu_bytes_per_splat(header, false), 41u);  // direct SH1
    EXPECT_EQ(gpu_bytes_per_splat(header, true), 45u);
    header.palette_flags = 4096;
    EXPECT_EQ(gpu_bytes_per_splat(header, false), 29u);  // a palette is shared
    EXPECT_EQ(max_splats_for_memory(1000, 100, 30), 30u);
    EXPECT_EQ(max_splats_for_memory(100, 100, 30), 0u);
    EXPECT_EQ(max_splats_for_memory(1ull << 50, 0, 1), kMaxGpuSplats);
}

}  // namespace
}  // namespace splatcore
