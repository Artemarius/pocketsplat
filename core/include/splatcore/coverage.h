#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "splatcore/pack.h"
#include "splatcore/result.h"

namespace splatcore {

// Coverage mask (docs/coverage-mask-research.md, ADR-0013): per splat, the set of directions the
// capture cameras observed it from, as bits of a kCoverageGrid x kCoverageGrid octahedral grid
// over the sphere of directions splat -> camera (25 cells of ~40 deg, one uint32 per splat). At
// render time a splat seen from a direction far from every observed cell is drawn more
// transparent, by a strength the viewer controls (0 = unchanged rendering).

inline constexpr int kCoverageGrid = 5;
inline constexpr std::uint32_t kCoverageMagic = 0x56435350;  // "PSCV" as little-endian bytes
inline constexpr std::uint32_t kCoverageVersion = 1;

// One capture camera in the renderer's RUB world: position and camera-to-world axes (right, down,
// forward - the camera looks along +forward), focal lengths in pixels, image size.
struct CaptureCamera {
    float position[3];
    float right[3];
    float down[3];
    float forward[3];
    float fx = 0.0f, fy = 0.0f;
    std::uint32_t width = 0, height = 0;
};

// Parses a 3DGS `cameras.json` (the training cameras the reference 3DGS trainer writes next to
// its point cloud: an array of {position, rotation (camera-to-world rows), fx, fy, width,
// height}) in the PLY's RDF world and converts each camera to RUB like the splats.
Result<std::vector<CaptureCamera>> parse_cameras_json(const std::string& text);

// Octahedral cell of a direction (any non-zero length): fold the lower hemisphere onto the
// octahedron's square, then a kCoverageGrid x kCoverageGrid grid over it; index iy * G + ix.
std::uint32_t coverage_cell(const float dir[3]);
// Unit direction at the centre of `cell` (the inverse map at the cell's centre).
void coverage_cell_center(std::uint32_t cell, float out[3]);

struct CoverageOptions {
    int depth_downscale = 8;        // occlusion buffer at 1/8 of each camera's resolution
    float depth_tolerance = 0.03f;  // observed when depth <= nearest opaque depth x (1 + this)
    float opaque_alpha = 0.5f;      // splats at least this opaque occlude
    int threads = 0;                // 0 = hardware concurrency (capped at 16)
};

// Mask per splat of `pack` (pack order). A splat counts as observed by a camera when its centre
// projects inside the image, in front of the camera, and not behind the camera's coarse depth
// buffer (the nearest opaque splat centre per pixel). Deterministic for any thread count.
std::vector<std::uint32_t> compute_coverage(const GpuPack& pack, const std::vector<CaptureCamera>& cameras,
                                            const CoverageOptions& options = {});

// How well a direction (unit, splat -> eye) matches a mask: the best over observed cells c of
// smoothstep(cos_zero, cos_full, dot(dir, centre(c))) - 1 within the full angle of an observed
// cell centre, 0 beyond the zero angle or for an empty mask. The CPU reference of the preprocess
// shader's coverage_match (render/shaders/splat_preprocess.comp).
float coverage_match(std::uint32_t mask, const float dir[3], float cos_full, float cos_zero);

// Fingerprint that binds a coverage file to its pack: FNV-1a 64 over the packed positions.
std::uint64_t coverage_positions_hash(const GpuPack& pack);

// 32-byte sidecar header followed by `count` uint32 masks (docs/formats.md, coverage sidecar).
struct CoverageHeader {
    std::uint32_t magic = kCoverageMagic;
    std::uint32_t version = kCoverageVersion;
    std::uint32_t count = 0;
    std::uint32_t grid = kCoverageGrid;
    std::uint64_t positions_hash = 0;
    std::uint32_t camera_count = 0;
    std::uint32_t source = 0;  // CaptureSource (capture.h, ADR-0015); was reserved = 0
};
static_assert(sizeof(CoverageHeader) == 32, "CoverageHeader is a 32-byte on-disk structure");

std::vector<std::uint8_t> serialize_coverage(const CoverageHeader& header,
                                             const std::vector<std::uint32_t>& masks);
// Parses a sidecar and checks it belongs to `pack` (count and positions hash); returns the masks.
Result<std::vector<std::uint32_t>> parse_coverage(const std::vector<std::uint8_t>& bytes,
                                                  const GpuPack& pack);

}  // namespace splatcore
