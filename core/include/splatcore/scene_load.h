#pragma once

#include <filesystem>

#include "splatcore/pack.h"
#include "splatcore/result.h"

namespace splatcore {

// Loads a scene file into a GPU pack, dispatching on the extension: .pspk parses directly;
// .ply (RDF assumed) and .spz go through splatcore IO with SH truncation to sh_target (0 or 1,
// applied only when the input degree exceeds it) and a Morton reorder before packing. This is
// the one load path shared by every renderer host (desktop GLFW, Android).
Result<GpuPack> load_scene_pack(const std::filesystem::path& path, int sh_target);

// Default-camera framing box of the packed positions (per-axis 5th-95th percentile): captures
// often carry background splats hundreds of units out, so the raw AABB over-frames the content.
struct SceneFraming {
    float target[3];  // box center
    float diagonal;   // box diagonal, >= 1e-3
};
SceneFraming robust_framing(const GpuPack& pack);

}  // namespace splatcore
