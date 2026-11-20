#pragma once

#include <cstdint>
#include <vector>

#include "splatcore/palette.h"
#include "splatcore/result.h"
#include "splatcore/splat_cloud.h"

namespace splatcore {

inline constexpr std::uint32_t kPackMagic = 0x4B505350;  // "PSPK" as little-endian bytes
inline constexpr std::uint32_t kPackVersion = 3;         // v3: optional SH1 palette (v2: cull-bound AABBs)
inline constexpr std::uint32_t kPackMinVersion = 2;  // oldest version parse_pack reads (as v3, no palette)
inline constexpr std::uint32_t kPackChunkSize = 256;
inline constexpr std::uint32_t kPackCoordsRub = 1;  // the only coordinate tag we ever write
inline constexpr int kChunkFloats = 7;              // aabb_min xyz, aabb_max xyz, importance_max
inline constexpr std::uint32_t kPackPaletteCountMask = 0x00FFFFFFu;  // header word 28, low 24 bits

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
    // Low 24 bits: palette_count, the number of SH1 palette entries in `sh1` (0 = direct SH1, one
    // entry per splat); high 8 bits: flags, none defined yet (readers reject packs that set any).
    std::uint32_t palette_flags = 0;
    float aabb_min[3] = {0.0f, 0.0f, 0.0f};  // raw min/max of the packed positions
    float aabb_max[3] = {0.0f, 0.0f, 0.0f};
    std::uint64_t source_hash = 0;  // FNV-1a 64 over the source SplatCloud arrays

    std::uint32_t palette_count() const { return palette_flags & kPackPaletteCountMask; }
    std::uint32_t flags() const { return palette_flags >> 24; }
};
static_assert(sizeof(PackHeader) == 64, "PackHeader is a 64-byte on-disk structure");

// GPU-ready SoA arrays, std430-friendly (flat scalars, no vec3). Bit layout in docs/formats.md §3.
struct GpuPack {
    PackHeader header;
    std::vector<float> positions;          // 3 * count: x y z, float32, RUB
    std::vector<std::uint32_t> scale_rot;  // 3 * count: 3 x fp16 log-scale, 4 x snorm8 quat, pad
    std::vector<std::uint32_t> color;      // count: RGBA8 unorm (rgb = display color, a = alpha)
    std::vector<std::uint32_t> sh1;        // 3 per entry (9 x snorm8 + 3 B pad): one entry per splat at
                                           // degree 1, or palette_count entries indexed by scale_rot
                                           // word 2's high 16 bits (palette pack, v3); empty at 0
    std::vector<float> chunks;             // kChunkFloats * chunk_count; AABBs are cull bounds
                                           // (center +/- 3 * max scale), not center bounds (v2)

    std::size_t chunk_count() const { return (header.count + kPackChunkSize - 1) / kPackChunkSize; }
    bool has_palette() const { return header.palette_count() != 0; }
    // Header followed by positions, scale_rot, color, sh1, chunks, each tightly packed.
    std::vector<std::uint8_t> serialize() const;
};

// Packs a cleaned, Morton-ordered SplatCloud (SH degree 0-1, RUB) into the GPU layout. With a
// palette (trained on this cloud, degree 1), `sh1` holds the palette entries instead of per-splat
// SH1 and each splat's entry index goes into scale_rot word 2's high 16 bits; sh1_scale is the
// max-abs coefficient over the entries in use, so every splat decodes to exactly the SH1 a direct
// pack of the cloud after apply_sh1_palette would hold. The cloud's own SH1 values are ignored.
Result<GpuPack> build_pack(const SplatCloud& cloud, const Sh1Palette* palette = nullptr);

// Validates and returns the header of a serialized pack (first 64 bytes).
Result<PackHeader> parse_pack_header(const std::vector<std::uint8_t>& bytes);

// Full deserialization of a serialized pack (the inverse of GpuPack::serialize). Rejects data
// whose size does not match the header exactly and palette packs with an index outside the
// palette (the shader would read past the sh1 buffer).
Result<GpuPack> parse_pack(const std::vector<std::uint8_t>& bytes);

// The `max_count` most important splats of `pack` - the same choice as prune_budget: importance
// = alpha x (sx sy + sy sz + sz sx), ties filled in pack order - kept in pack order, so chunks
// stay spatially compact; the chunk cull bounds and the AABB are rebuilt as build_pack does. For
// opening a scene larger than the GPU can draw (the Adreno 750: 11,184,810 splats). `kept`, if
// given, receives each kept splat's index in `pack`, to select the same entries of per-splat
// sidecars. A palette is kept whole (indices stay valid). A pack within the limit is returned
// unchanged (kept = 0..count-1).
GpuPack subset_pack(const GpuPack& pack, std::size_t max_count, std::vector<std::uint32_t>* kept = nullptr);

}  // namespace splatcore
