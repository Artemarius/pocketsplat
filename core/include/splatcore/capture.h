#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "splatcore/coverage.h"
#include "splatcore/pack.h"
#include "splatcore/result.h"

namespace splatcore {

// Capture cameras for the coverage fade (ADR-0013, ADR-0015): readers for the camera files that
// training pipelines write next to their splats, and, for scenes without any, a set of virtual
// cameras inferred from the splats and a scene type the user picks. Either way the result feeds
// compute_coverage and the same `.pscov` sidecar.

// One input file: its name (only the extension and the words "cameras"/"images" are used) and bytes.
struct CaptureFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

// Reads capture cameras from any supported set of files, in the PLY's RDF world converted to RUB:
//  * a 3DGS `cameras.json` (array of {position, rotation, fx, fy, width, height});
//  * a nerfstudio / instant-ngp / Brush `transforms.json` (object with "frames" of camera-to-world
//    4x4 `transform_matrix` in the OpenGL camera convention; fl_x/fl_y or camera_angle_x, w/h);
//  * a COLMAP sparse model: `cameras` and `images`, both `.bin` or both `.txt` (world-to-camera
//    quaternion + translation, OpenCV camera axes; any camera model, distortion ignored).
// The format is sniffed from the content; COLMAP needs both files.
Result<std::vector<CaptureCamera>> parse_capture_cameras(const std::vector<CaptureFile>& files);

Result<std::vector<CaptureCamera>> parse_transforms_json(const std::string& text);
Result<std::vector<CaptureCamera>> parse_colmap(const std::vector<std::uint8_t>& cameras_file,
                                                const std::vector<std::uint8_t>& images_file);

// Share of the pack's opaque splats (alpha >= 0.5) whose centre lies in front of and inside the
// image of at least one camera. Samples at most 200 K splats. The 13 INRIA scenes' own cameras
// score 0.999-1.000; truck's cameras in a wrong frame (a normalized copy of the scene without
// its dataparser_transforms.json) scored 0.765, other scenes' cameras score near 0.
float capture_fit(const GpuPack& pack, const std::vector<CaptureCamera>& cameras);
inline constexpr float kMinCaptureFit = 0.9f;  // camera files below this are rejected

// Scene types for virtual cameras (ADR-0015). The values are stored in the sidecar header
// (CoverageHeader::source).
enum class CaptureSource : std::uint32_t {
    kCameras = 0,  // real capture cameras from a file
    kRoom = 1,     // inferred: a space walked through (rooms, halls): seen from the free space inside
    kObject = 2,   // inferred: an object orbited by the capture (a statue, a table, a car)
};

struct VirtualCaptureOptions {
    // Room: omnidirectional cameras (6 cube faces) at `room_positions` free points within
    // `room_radius` x the home view's full distance of the focus, with at least `room_clearance`
    // voxels of free space around them (farthest-point sampled, starting at the home eye).
    int room_positions = 24;
    float room_radius = 0.5f;
    int room_clearance = 2;
    float room_enclosure = 0.85f;  // share of 26 axis/diagonal rays that must hit geometry
    // Object: a ring of cameras looking at the focus at these fractions of the full distance and
    // elevations above the fitted ground plane, `object_azimuths` around; positions inside
    // geometry are skipped.
    std::vector<float> object_radii = {0.7f, 1.0f, 1.4f};  // sweep, ADR-0015
    std::vector<float> object_elevations_deg = {5.0f, 20.0f, 35.0f, 50.0f};
    int object_azimuths = 16;
    float object_fov_deg = 65.0f;  // horizontal, 4:3 image
    int grid = 128;                // occupancy grid resolution
    float grid_half = 2.0f;        // grid: cube around the focus, half size = this x full distance
};

// Virtual capture cameras for a scene without camera files, from the splats alone: the home view
// (focus, ground plane, distance) and an occupancy grid of the opaque splats' footprints.
std::vector<CaptureCamera> infer_capture_cameras(const GpuPack& pack, CaptureSource kind,
                                                 const VirtualCaptureOptions& options = {});

// A finished sidecar: the serialized `.pscov` bytes and what went into them.
struct CoverageSidecar {
    std::vector<std::uint8_t> bytes;
    CaptureSource source = CaptureSource::kCameras;
    std::size_t cameras = 0;          // real or virtual
    float fit = 0.0f;                 // capture_fit of those cameras
    std::string interpretation;       // which world of the camera files fit the scene
    std::vector<CaptureCamera> used;  // the cameras the masks came from
};

// The whole path from camera files (source kCameras) or a scene type (kRoom / kObject, `files`
// ignored) to a sidecar for `pack`: parse or infer the cameras, reject camera files that see
// too little of the scene (capture_fit < kMinCaptureFit: another scene or frame), compute the
// masks and serialize them with the source in the header. Used by splatconv and the app.
// A transforms.json is tried as given, with its `applied_transform` undone (nerfstudio's
// re-oriented COLMAP frame), and with a `dataparser_transforms.json` among the files applied
// (splatfacto's normalized frame); the interpretation whose cameras see most of the scene wins.
Result<CoverageSidecar> make_coverage_sidecar(const GpuPack& pack, CaptureSource source,
                                              const std::vector<CaptureFile>& files,
                                              const VirtualCaptureOptions& options = {});

}  // namespace splatcore
