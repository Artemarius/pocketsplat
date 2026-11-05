#pragma once

#include <cstdint>
#include <vector>

#include "splatcore/result.h"
#include "splatcore/splat_cloud.h"

namespace splatcore {

inline constexpr std::uint32_t kPackMagic = 0x4B505350;  // "PSPK" as little-endian bytes
inline constexpr std::uint32_t kPackVersion = 2;         // v2: chunk AABBs are conservative cull bounds
inline constexpr std::uint32_t kPackChunkSize = 256;
inline constexpr std::uint32_t kPackCoordsRub = 1;  // the only coordinate tag we ever write
inline constexpr int kChunkFloats = 7;              // aabb_min xyz, aabb_max xyz, importance_max

// 64-byte pack header (docs/formats.md §3). Field order is part of the on-disk cache format —
// bump kPackVersion on any change.
struct PackHeader {
    std::uint32_t magic = kPackMagic;
    std::uint32_t version = kPackVersion;
    std::uint32_t count = 0;
    std::uint32_t chunk_size = kPackChunkSize;
    std::uint32_t sh_degree = 0;  // 0 or 1
    std::uint32_t coords = kPackCoordsRub;
    float sh1_scale = 0.0f;  // max-abs SH1 coefficient (1.0 if all zero); 0 at degree 0
    std::uint32_t reserved = 0;
    float aabb_min[3] = {0.0f, 0.0f, 0.0f};  // raw min/max of the packed positions
    float aabb_max[3] = {0.0f, 0.0f, 0.0f};
    std::uint64_t source_hash = 0;  // FNV-1a 64 over the source SplatCloud arrays
};
static_assert(sizeof(PackHeader) == 64, "PackHeader is a 64-byte on-disk structure");

// GPU-ready SoA arrays, std430-friendly (flat scalars, no vec3). Bit layout in docs/formats.md §3.
struct GpuPack {
    PackHeader header;
    std::vector<float> positions;          // 3 * count: x y z, float32, RUB
    std::vector<std::uint32_t> scale_rot;  // 3 * count: 3 x fp16 log-scale, 4 x snorm8 quat, pad
    std::vector<std::uint32_t> color;      // count: RGBA8 unorm (rgb = display color, a = alpha)
    std::vector<std::uint32_t> sh1;        // 3 * count at degree 1 (9 x snorm8 + 3 B pad); empty at 0
    std::vector<float> chunks;             // kChunkFloats * chunk_count; AABBs are cull bounds
                                           // (center +/- 3 * max scale), not center bounds (v2)

    std::size_t chunk_count() const { return (header.count + kPackChunkSize - 1) / kPackChunkSize; }
    // Header followed by positions, scale_rot, color, sh1, chunks, each tightly packed.
    std::vector<std::uint8_t> serialize() const;
};

// Packs a cleaned, Morton-ordered SplatCloud (SH degree 0-1, RUB) into the GPU layout.
Result<GpuPack> build_pack(const SplatCloud& cloud);

// Validates and returns the header of a serialized pack (first 64 bytes).
Result<PackHeader> parse_pack_header(const std::vector<std::uint8_t>& bytes);

// Full deserialization of a serialized pack (the inverse of GpuPack::serialize). Rejects data
// whose size does not match the header exactly.
Result<GpuPack> parse_pack(const std::vector<std::uint8_t>& bytes);

// The `max_count` most important splats of `pack` - the same choice as prune_budget: importance
// = alpha x (sx sy + sy sz + sz sx), ties filled in pack order - kept in pack order, so chunks
// stay spatially compact; the chunk cull bounds and the AABB are rebuilt as build_pack does. For
// opening a scene larger than the GPU can draw (the Adreno 750: 4,194,304 splats). `kept`, if
// given, receives each kept splat's index in `pack`, to select the same entries of per-splat
// sidecars. A pack within the limit is returned unchanged (kept = 0..count-1).
GpuPack subset_pack(const GpuPack& pack, std::size_t max_count, std::vector<std::uint32_t>* kept = nullptr);

}  // namespace splatcore
