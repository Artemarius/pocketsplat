#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "splatcore/cleaning.h"
#include "splatcore/result.h"

namespace splatcore {

// What a scene file claims to contain, read from its header alone (no splat data is touched, so
// probing a multi-gigabyte capture is instant). Dispatches on the extension like load_scene_pack:
// ".pspk" reads the 64-byte pack header, ".spz" reads the container header (legacy gzip v1-3 via
// a partial inflate, NGSP v4 raw), anything else is parsed as a PLY text header.
struct SceneProbe {
    std::string kind;         // "pspk" | "spz" | "ply"
    std::uint64_t count = 0;  // splat count from the header
    int sh_degree = 0;        // 0-3 (sources may exceed our storable 0-1; import truncates)
    std::uint64_t file_bytes = 0;
};
Result<SceneProbe> probe_scene(const std::filesystem::path& path);

// Stages reported to ImportOptions::progress, in pipeline order (kSor only with deep_clean).
enum class ImportStage { kRead, kClean, kSor, kPack, kWrite };

struct ImportOptions {
    int sh_target = 1;                 // truncate SH above this degree (0 or 1)
    bool deep_clean = false;           // run SOR floater removal (seconds to minutes at 1M+)
    std::size_t max_splats = 1000000;  // prune_budget bound; 0 disables the budget prune
    // Hard guard on the probed input count, checked before anything is allocated: a mobile host
    // cannot hold the float expansion of a 10M+ capture (~240 B/splat at SH 3).
    std::uint64_t refuse_above = 8000000;
    // Called from the importing thread as each stage begins; return false to cancel the import at
    // the next stage boundary (the ops themselves are not interruptible). Null = never cancel.
    std::function<bool(ImportStage)> progress;
};

struct ImportReport {
    std::uint32_t count = 0;  // splats in the written pack
    int sh_degree = 0;        // pack SH degree (0 or 1)
    std::uint64_t input_count = 0;
    std::vector<CleanOpStats> ops;  // the same records `splatconv clean` reports
};

// One-call import for renderer hosts: probe + guards, read (.ply assumed RDF -> RUB, matching
// load_scene_pack), the docs/cleaning.md pipeline (drop_invalid, prune_opacity, prune_scale,
// clamp_needle_scales, SOR only with deep_clean, truncate_sh, prune_budget), Morton reorder,
// pack, and an atomic write of dst (tmp file next to it + rename). Cancellation fails with
// exactly "import cancelled"; on any failure dst is left untouched.
Result<ImportReport> import_scene(const std::filesystem::path& src, const std::filesystem::path& dst,
                                  const ImportOptions& options);

}  // namespace splatcore
