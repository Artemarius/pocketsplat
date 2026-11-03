#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <istream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "splatcore/ply_io.h"

namespace splatcore {
namespace {

static_assert(std::endian::native == std::endian::little,
              "splatcore assumes a little-endian host; binary PLY/pack IO would need byte swaps");

struct PlyHeader {
    std::size_t count = 0;
    std::vector<std::string> properties;  // vertex property names in file order, all float32
};

// Splits a header line into whitespace-separated tokens, tolerating trailing '\r'.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        if (!token.empty() && token.back() == '\r') token.pop_back();
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

Result<PlyHeader> parse_header(std::istream& in) {
    using R = Result<PlyHeader>;
    std::string line;
    if (!std::getline(in, line) || tokenize(line) != std::vector<std::string>{"ply"}) {
        return R::failure("not a PLY file: missing 'ply' magic");
    }

    PlyHeader header;
    bool format_seen = false;
    bool vertex_seen = false;
    bool header_ended = false;
    while (std::getline(in, line)) {
        const std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty()) continue;
        const std::string& keyword = tokens[0];
        if (keyword == "comment" || keyword == "obj_info") continue;
        if (keyword == "end_header") {
            header_ended = true;
            break;
        }
        if (keyword == "format") {
            if (tokens.size() != 3) return R::failure("malformed 'format' line");
            if (tokens[1] == "ascii")
                return R::failure("ASCII PLY is not supported (binary little-endian only)");
            if (tokens[1] == "binary_big_endian") return R::failure("big-endian PLY is not supported");
            if (tokens[1] != "binary_little_endian" || tokens[2] != "1.0") {
                return R::failure("unsupported PLY format '" + tokens[1] + " " + tokens[2] + "'");
            }
            format_seen = true;
        } else if (keyword == "element") {
            if (tokens.size() != 3) return R::failure("malformed 'element' line");
            if (tokens[1] != "vertex") return R::failure("unsupported element '" + tokens[1] + "'");
            if (vertex_seen) return R::failure("duplicate 'element vertex'");
            vertex_seen = true;
            const std::string& text = tokens[2];
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), header.count);
            if (ec != std::errc{} || ptr != text.data() + text.size()) {
                return R::failure("invalid vertex count '" + text + "'");
            }
        } else if (keyword == "property") {
            if (!vertex_seen) return R::failure("property before 'element vertex'");
            if (tokens.size() >= 2 && tokens[1] == "list")
                return R::failure("list properties are not supported");
            if (tokens.size() != 3) return R::failure("malformed 'property' line");
            if (tokens[1] != "float" && tokens[1] != "float32") {
                return R::failure("property '" + tokens[2] + "' has type '" + tokens[1] +
                                  "', only float is supported");
            }
            header.properties.push_back(tokens[2]);
        } else {
            return R::failure("unknown header keyword '" + keyword + "'");
        }
    }
    if (!header_ended) return R::failure("unexpected end of file inside header");
    if (!format_seen) return R::failure("missing 'format' line");
    if (!vertex_seen) return R::failure("missing 'element vertex'");
    if (header.properties.empty()) return R::failure("vertex element has no properties");
    return R::success(std::move(header));
}

// Column layout of the vertex properties we consume, resolved by name (file order is irrelevant).
struct ColumnMap {
    std::size_t pos[3];
    std::size_t dc[3];
    std::size_t opacity;
    std::size_t scale[3];
    std::size_t rot[4];
    std::vector<std::size_t> rest;  // rest[k] = column of f_rest_k
    int sh_degree = 0;
};

Result<ColumnMap> resolve_columns(const PlyHeader& header) {
    using R = Result<ColumnMap>;
    std::unordered_map<std::string, std::size_t> column_of;
    for (std::size_t i = 0; i < header.properties.size(); ++i) {
        if (!column_of.emplace(header.properties[i], i).second) {
            return R::failure("duplicate property '" + header.properties[i] + "'");
        }
    }

    ColumnMap map;
    std::size_t rest_count = 0;
    for (const auto& [name, column] : column_of) {
        if (name.rfind("f_rest_", 0) == 0) {
            (void)column;
            ++rest_count;
        }
    }
    if (rest_count != 0 && rest_count != 9 && rest_count != 24 && rest_count != 45) {
        return R::failure("unsupported f_rest count " + std::to_string(rest_count) +
                          " (expected 0, 9, 24 or 45 for SH degree 0-3)");
    }
    map.sh_degree = rest_count == 0 ? 0 : (rest_count == 9 ? 1 : (rest_count == 24 ? 2 : 3));

    auto lookup = [&](const std::string& name, std::size_t& out) -> bool {
        auto it = column_of.find(name);
        if (it == column_of.end()) return false;
        out = it->second;
        column_of.erase(it);
        return true;
    };
    const char* axes = "xyz";
    for (int i = 0; i < 3; ++i) {
        if (!lookup(std::string(1, axes[i]), map.pos[i])) return R::failure("missing position property");
        if (!lookup("f_dc_" + std::to_string(i), map.dc[i])) return R::failure("missing f_dc property");
        if (!lookup("scale_" + std::to_string(i), map.scale[i])) return R::failure("missing scale property");
    }
    if (!lookup("opacity", map.opacity)) return R::failure("missing opacity property");
    for (int i = 0; i < 4; ++i) {
        if (!lookup("rot_" + std::to_string(i), map.rot[i])) return R::failure("missing rot property");
    }
    map.rest.resize(rest_count);
    for (std::size_t k = 0; k < rest_count; ++k) {
        if (!lookup("f_rest_" + std::to_string(k), map.rest[k])) {
            return R::failure("f_rest indices are not contiguous (missing f_rest_" + std::to_string(k) + ")");
        }
    }
    // Normals are present in INRIA output (always zero); ignore them, reject anything else.
    std::size_t ignored;
    lookup("nx", ignored);
    lookup("ny", ignored);
    lookup("nz", ignored);
    if (!column_of.empty()) {
        return R::failure("unknown vertex property '" + column_of.begin()->first + "'");
    }
    return R::success(std::move(map));
}

}  // namespace

Result<SplatCloud> read_ply(std::istream& in) {
    using R = Result<SplatCloud>;
    Result<PlyHeader> header = parse_header(in);
    if (!header) return R::failure(header.error());
    Result<ColumnMap> columns = resolve_columns(header.value());
    if (!columns) return R::failure(columns.error());
    const ColumnMap& map = columns.value();
    const std::size_t count = header.value().count;
    const std::size_t ncols = header.value().properties.size();
    const int rest_per_channel = sh_rest_coeffs(map.sh_degree);

    SplatCloud cloud;
    cloud.resize(count, map.sh_degree);

    constexpr std::size_t kChunkRows = 65536;
    std::vector<float> buffer(std::min(kChunkRows, count == 0 ? std::size_t{1} : count) * ncols);
    for (std::size_t start = 0; start < count; start += kChunkRows) {
        const std::size_t rows = std::min(kChunkRows, count - start);
        const std::streamsize bytes = static_cast<std::streamsize>(rows * ncols * sizeof(float));
        in.read(reinterpret_cast<char*>(buffer.data()), bytes);
        if (in.gcount() != bytes) {
            return R::failure("truncated PLY data: expected " + std::to_string(count) + " vertices");
        }
        for (std::size_t r = 0; r < rows; ++r) {
            const float* row = buffer.data() + r * ncols;
            const std::size_t i = start + r;
            for (int c = 0; c < 3; ++c) {
                cloud.positions[3 * i + c] = row[map.pos[c]];
                cloud.log_scales[3 * i + c] = row[map.scale[c]];
                cloud.dc[3 * i + c] = row[map.dc[c]];
            }
            cloud.opacities[i] = row[map.opacity];
            for (int c = 0; c < 4; ++c) cloud.rotations[4 * i + c] = row[map.rot[c]];
            // Planar per channel in the file (all R coefficients, then G, then B) to
            // coefficient-major in memory (c0.rgb, c1.rgb, ...).
            float* dst = cloud.sh.data() + 3 * static_cast<std::size_t>(rest_per_channel) * i;
            for (int ch = 0; ch < 3; ++ch) {
                for (int k = 0; k < rest_per_channel; ++k) {
                    dst[k * 3 + ch] = row[map.rest[static_cast<std::size_t>(ch) * rest_per_channel + k]];
                }
            }
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        float* q = cloud.rotations.data() + 4 * i;
        const float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (std::isfinite(norm) && norm > 1e-12f) {
            for (int c = 0; c < 4; ++c) q[c] /= norm;
        }
    }
    return R::success(std::move(cloud));
}

Result<SplatCloud> read_ply(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<SplatCloud>::failure("cannot open '" + path.string() + "'");
    return read_ply(file);
}

Result<PlyInfo> probe_ply(std::istream& in) {
    using R = Result<PlyInfo>;
    Result<PlyHeader> header = parse_header(in);
    if (!header) return R::failure(header.error());
    Result<ColumnMap> columns = resolve_columns(header.value());
    if (!columns) return R::failure(columns.error());
    return R::success(PlyInfo{header.value().count, columns.value().sh_degree});
}

Result<PlyInfo> probe_ply(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<PlyInfo>::failure("cannot open '" + path.string() + "'");
    return probe_ply(file);
}

}  // namespace splatcore
