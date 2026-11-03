#pragma once

#include "splatcore/result.h"
#include "splatcore/splat_cloud.h"

namespace splatcore {

// The two coordinate systems PocketSplat deals with (see docs/formats.md). Both are right-handed
// and differ only by sign flips of the y and z axes, so conversion is involutive:
//   kRdf — X right, Y down, Z front: COLMAP/INRIA PLY convention.
//   kRub — X right, Y up, Z back: SPZ default and the renderer's system.
enum class CoordSystem { kRdf, kRub };

// In-place conversion. Flips position y/z, quaternion y/z components (conjugation by the 180-deg
// rotation about x that maps one frame to the other), and applies per-coefficient sign flips to
// the SH bands derived from each basis function's parity in y and z. No-op when from == to.
void convert_coordinates(SplatCloud& cloud, CoordSystem from, CoordSystem to);

}  // namespace splatcore
