#include "splatcore/scene_import.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#include "splatcore/coords.h"
#include "splatcore/pack.h"
#include "splatcore/ply_io.h"
#include "splatcore/spz_io.h"

namespace splatcore {
namespace {

namespace fs = std::filesystem;

// First 16 bytes of both SPZ containers (nianticlabs/spz): the legacy gzip format (versions 1-3)
// carries this at the start of the *decompressed* stream, NGSP v4 stores it raw at the file start
// (its full header is 32 bytes, but the leading fields line up). Only probed fields are declared.
struct SpzHeadFields {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t num_points = 0;
    std::uint8_t sh_degree = 0;
    std::uint8_t fractional_bits = 0;
    std::uint8_t flags = 0;
    std::uint8_t tail = 0;  // legacy: reserved; NGSP: numStreams
};
static_assert(sizeof(SpzHeadFields) == 16, "SPZ probe reads a 16-byte header prefix");

constexpr std::uint32_t kSpzMagic = 0x5053474e;  // "NGSP"

Result<SpzHeadFields> probe_spz(const fs::path& path) {
    using R = Result<SpzHeadFields>;
    std::ifstream file(path, std::ios::binary);
    if (!file) return R::failure("cannot open '" + path.string() + "'");
    // The first 16 decompressed bytes of a gzip stream come from far fewer than 4 KB of input.
    std::vector<unsigned char> head(4096);
    file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    const std::size_t got = static_cast<std::size_t>(file.gcount());
    if (got < sizeof(SpzHeadFields)) return R::failure("'" + path.string() + "' is too small to be SPZ");

    SpzHeadFields fields;
    if (head[0] == 0x1f && head[1] == 0x8b) {  // gzip magic: legacy container, inflate the header
        z_stream stream{};
        stream.next_in = head.data();
        stream.avail_in = static_cast<uInt>(got);
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
            return R::failure("zlib init failed probing '" + path.string() + "'");
        }
        stream.next_out = reinterpret_cast<Bytef*>(&fields);
        stream.avail_out = static_cast<uInt>(sizeof(fields));
        const int res = inflate(&stream, Z_NO_FLUSH);
        const bool filled =
            (res == Z_OK || res == Z_STREAM_END || res == Z_BUF_ERROR) && stream.avail_out == 0;
        inflateEnd(&stream);
        if (!filled) return R::failure("'" + path.string() + "' is not a valid gzip SPZ");
    } else {
        std::memcpy(&fields, head.data(), sizeof(fields));
    }
    if (fields.magic != kSpzMagic) return R::failure("'" + path.string() + "' has no SPZ magic");
    if (fields.version < 1 || fields.version > 4) {
        return R::failure("SPZ version " + std::to_string(fields.version) + " is not supported");
    }
    if (fields.sh_degree > 3) {
        return R::failure("SPZ SH degree " + std::to_string(fields.sh_degree) +
                          " is outside the supported range 0-3");
    }
    return R::success(fields);
}

}  // namespace

Result<SceneProbe> probe_scene(const fs::path& path) {
    using R = Result<SceneProbe>;
    SceneProbe probe;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    probe.file_bytes = ec ? 0 : static_cast<std::uint64_t>(size);

    const std::string ext = path.extension().string();
    if (ext == ".pspk") {
        std::ifstream file(path, std::ios::binary);
        if (!file) return R::failure("cannot open '" + path.string() + "'");
        std::vector<std::uint8_t> bytes(sizeof(PackHeader));
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(file.gcount()) != bytes.size()) {
            return R::failure("'" + path.string() + "' is too small to be a pack");
        }
        Result<PackHeader> header = parse_pack_header(bytes);
        if (!header) return R::failure(header.error());
        probe.kind = "pspk";
        probe.count = header.value().count;
        probe.sh_degree = static_cast<int>(header.value().sh_degree);
        return R::success(std::move(probe));
    }
    if (ext == ".spz") {
        Result<SpzHeadFields> fields = probe_spz(path);
        if (!fields) return R::failure(fields.error());
        probe.kind = "spz";
        probe.count = fields.value().num_points;
        probe.sh_degree = static_cast<int>(fields.value().sh_degree);
        return R::success(std::move(probe));
    }
    // Everything else is treated as PLY, matching load_scene_pack (the magic check rejects junk).
    Result<PlyInfo> info = probe_ply(path);
    if (!info) return R::failure(info.error());
    probe.kind = "ply";
    probe.count = info.value().count;
    probe.sh_degree = info.value().sh_degree;
    return R::success(std::move(probe));
}

Result<ImportReport> import_scene(const fs::path& src, const fs::path& dst, const ImportOptions& options) {
    using R = Result<ImportReport>;
    if (options.sh_target != 0 && options.sh_target != 1) {
        return R::failure("SH target must be 0 or 1, got " + std::to_string(options.sh_target));
    }
    Result<SceneProbe> probe = probe_scene(src);
    if (!probe) return R::failure(probe.error());
    if (probe.value().kind == "pspk") {
        return R::failure("'" + src.string() + "' is already a pack - open it directly");
    }
    if (options.refuse_above > 0 && probe.value().count > options.refuse_above) {
        return R::failure("scene has " + std::to_string(probe.value().count) +
                          " splats, above the import limit of " + std::to_string(options.refuse_above) +
                          " - convert it with splatconv on a desktop first");
    }

    auto step = [&](ImportStage stage) { return !options.progress || options.progress(stage); };
    const auto cancelled = [] { return R::failure("import cancelled"); };

    if (!step(ImportStage::kRead)) return cancelled();
    Result<SplatCloud> read = probe.value().kind == "spz" ? read_spz(src) : read_ply(src);
    if (!read) return R::failure(read.error());
    SplatCloud& cloud = read.value();
    if (probe.value().kind != "spz") {
        // PLY interchange files carry the training convention (RDF); the runtime is RUB.
        convert_coordinates(cloud, CoordSystem::kRdf, CoordSystem::kRub);
    }

    ImportReport report;
    report.input_count = cloud.count;

    if (!step(ImportStage::kClean)) return cancelled();
    report.ops.push_back(drop_invalid(cloud));
    report.ops.push_back(prune_opacity(cloud));
    report.ops.push_back(prune_scale(cloud, robust_aabb(cloud)));
    report.ops.push_back(clamp_needle_scales(cloud, robust_aabb(cloud)));
    if (options.deep_clean) {
        if (!step(ImportStage::kSor)) return cancelled();
        report.ops.push_back(remove_floaters_sor(cloud));
    }
    report.ops.push_back(truncate_sh(cloud, options.sh_target));
    if (options.max_splats > 0) report.ops.push_back(prune_budget(cloud, options.max_splats));
    if (cloud.count == 0) return R::failure("no splats survived cleaning");

    if (!step(ImportStage::kPack)) return cancelled();
    report.ops.push_back(reorder_morton(cloud, robust_aabb(cloud)));
    Result<GpuPack> pack = build_pack(cloud);
    if (!pack) return R::failure(pack.error());
    const std::vector<std::uint8_t> bytes = pack.value().serialize();
    report.count = pack.value().header.count;
    report.sh_degree = static_cast<int>(pack.value().header.sh_degree);

    if (!step(ImportStage::kWrite)) return cancelled();
    const fs::path tmp = fs::path(dst.string() + ".tmp");  // same directory, so rename is atomic
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.flush()) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return R::failure("cannot write '" + tmp.string() + "'");
        }
    }
    std::error_code ec;
    fs::remove(dst, ec);  // Windows rename does not overwrite; POSIX does - make both identical
    fs::rename(tmp, dst, ec);
    if (ec) {
        std::error_code cleanup;
        fs::remove(tmp, cleanup);
        return R::failure("cannot move pack into place at '" + dst.string() + "': " + ec.message());
    }
    return R::success(std::move(report));
}

}  // namespace splatcore
