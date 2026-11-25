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
//
// `options.max_sh_degree` keeps only the SH bands up to that degree while reading (coefficient-
// major layout: exactly what truncate_sh leaves), so a degree 3 capture read for a degree 1
// target never holds its 45-float tail: 92 instead of 236 B per splat in memory. With
// `options.mark_discarded_nonfinite`, a row whose discarded coefficients are not all finite
// gets a NaN opacity, so drop_invalid removes it just as it would with those coefficients kept
// (import_scene's pipeline stays byte-identical); without it they are simply discarded, like
// truncate_sh does.
struct PlyReadOptions {
    int max_sh_degree = 3;
    bool mark_discarded_nonfinite = false;
};
Result<SplatCloud> read_ply(std::istream& in, const PlyReadOptions& options = {});
Result<SplatCloud> read_ply(const std::filesystem::path& path, const PlyReadOptions& options = {});

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
