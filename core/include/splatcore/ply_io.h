#pragma once

#include <filesystem>
#include <iosfwd>

#include "splatcore/result.h"
#include "splatcore/splat_cloud.h"

namespace splatcore {

// Reads an INRIA-layout binary little-endian 3DGS PLY with SH degree 0-3. Rejects ASCII and
// big-endian files, unknown or non-float vertex properties, f_rest counts other than
// {0, 9, 24, 45}, and truncated data. nx/ny/nz are accepted and ignored. Quaternions are
// normalized on read (zero-norm ones are left for the drop_invalid cleaning op).
Result<SplatCloud> read_ply(std::istream& in);
Result<SplatCloud> read_ply(const std::filesystem::path& path);

// Parses and validates only the PLY header (same acceptance rules as read_ply) and returns the
// vertex count and SH degree without reading any splat data, so probing a multi-gigabyte capture
// is instant.
struct PlyInfo {
    std::size_t count = 0;
    int sh_degree = 0;
};
Result<PlyInfo> probe_ply(std::istream& in);
Result<PlyInfo> probe_ply(const std::filesystem::path& path);

// Writes binary little-endian PLY at the cloud's SH degree, which must be 0 or 1 (hard
// constraint: higher degrees are truncated by cleaning before export, never written). Normals
// are not written.
Status write_ply(const SplatCloud& cloud, std::ostream& out);
Status write_ply(const SplatCloud& cloud, const std::filesystem::path& path);

}  // namespace splatcore
