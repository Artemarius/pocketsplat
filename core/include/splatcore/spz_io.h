#pragma once

#include <filesystem>

#include "splatcore/result.h"
#include "splatcore/splat_cloud.h"

namespace splatcore {

// Reads an SPZ file (nianticlabs/spz, any supported version) into a SplatCloud in RUB coordinates
// (SPZ's native system — no conversion is applied). SH degrees 0-3 are accepted; degree 4 files
// are rejected. The antialiased flag is preserved.
Result<SplatCloud> read_spz(const std::filesystem::path& path);

// Writes a SplatCloud in RUB coordinates as SPZ. The cloud's SH degree must be 0 or 1 (hard
// constraint: higher degrees are truncated by cleaning before export, never written).
Status write_spz(const SplatCloud& cloud, const std::filesystem::path& path);

}  // namespace splatcore
