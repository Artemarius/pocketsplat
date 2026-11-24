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

// GPU segments (ADR-0018). A storage buffer is bound whole and capped by the device's
// maxStorageBufferRange (Adreno 750: 128 MB), so the renderer splits every per-splat array into
// segments of `segment_splats` splats, each in its own buffers with one preprocess dispatch.
// Segments hold whole 256-chunk blocks (kSegmentGranule splats) so the chunk cull, which walks
// the chunks 256 at a time, starts each segment's visible-chunk list at a fixed offset.
inline constexpr std::uint32_t kSegmentGranule = kPackChunkSize * kPackChunkSize;  // 65,536 splats
inline constexpr std::uint64_t kMaxGpuSplats = 0x7fffffffu;  // splat indices and counts are uint32

struct SegmentPlan {
    std::uint32_t segment_splats = 0;  // splats per segment (the last may hold fewer)
    std::uint32_t segment_count = 0;
};

// The largest multiple of kSegmentGranule whose widest per-splat array (`bytes_per_splat`) fits
// one binding of `max_binding_bytes` and whose chunks fit one dispatch of `max_workgroups_x`
// workgroups (one 256-splat chunk each), and the segments `count` splats need. `forced` (a
// multiple of kSegmentGranule, at most that size) replaces it - testing only. Fails for an empty
// scene, more than kMaxGpuSplats splats, a device too small for one granule, or a bad `forced`.
Result<SegmentPlan> plan_segments(std::uint64_t count, std::uint64_t max_binding_bytes,
                                  std::uint64_t max_workgroups_x, std::uint32_t bytes_per_splat = 12,
                                  std::uint64_t forced = 0);

// Resident GPU bytes per splat of a pack's per-splat data: pos + scale_rot + color (28 B), direct
// SH1 (12 B; a palette is shared and counted separately), the optional coverage mask (4 B), and
// one byte for the per-chunk bounds and list (32 B per 256 splats, rounded up).
std::uint64_t gpu_bytes_per_splat(const PackHeader& header, bool coverage);

// The most splats whose per-splat data (`bytes_per_splat`) fits `budget_bytes` after
// `fixed_bytes` (buffers sized independently of the scene), capped by kMaxGpuSplats; 0 when the
// fixed part alone exceeds the budget.
std::uint64_t max_splats_for_memory(std::uint64_t budget_bytes, std::uint64_t fixed_bytes,
                                    std::uint64_t bytes_per_splat);

}  // namespace splatcore
