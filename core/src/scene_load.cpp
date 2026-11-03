#include "splatcore/scene_load.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

#include "splatcore/cleaning.h"
#include "splatcore/coords.h"
#include "splatcore/ply_io.h"
#include "splatcore/spz_io.h"

namespace splatcore {

Result<GpuPack> load_scene_pack(const std::filesystem::path& path, int sh_target) {
    using R = Result<GpuPack>;
    if (sh_target != 0 && sh_target != 1) {
        return R::failure("SH target must be 0 or 1, got " + std::to_string(sh_target));
    }
    const std::string ext = path.extension().string();

    if (ext == ".pspk") {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return R::failure("cannot open '" + path.string() + "'");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) return R::failure("read failed for '" + path.string() + "'");
        return parse_pack(bytes);
    }

    Result<SplatCloud> cloud = ext == ".spz" ? read_spz(path) : read_ply(path);
    if (!cloud) return R::failure(cloud.error());
    if (ext != ".spz") {
        // PLY interchange files carry the training convention (RDF); the runtime is RUB.
        convert_coordinates(cloud.value(), CoordSystem::kRdf, CoordSystem::kRub);
    }
    if (cloud.value().sh_degree > sh_target) {
        truncate_sh(cloud.value(), sh_target);
    }
    reorder_morton(cloud.value(), robust_aabb(cloud.value()));
    return build_pack(cloud.value());
}

SceneFraming robust_framing(const GpuPack& pack) {
    SceneFraming framing{{0.0f, 0.0f, 0.0f}, 1e-3f};
    const std::size_t n = pack.header.count;
    if (n == 0) return framing;
    float lo[3] = {}, hi[3] = {};
    std::vector<float> axis(n);
    for (int c = 0; c < 3; ++c) {
        for (std::size_t i = 0; i < n; ++i) axis[i] = pack.positions[3 * i + static_cast<std::size_t>(c)];
        const std::size_t p_lo = n / 20, p_hi = n - 1 - n / 20;
        std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(p_lo), axis.end());
        lo[c] = axis[p_lo];
        std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(p_hi), axis.end());
        hi[c] = axis[p_hi];
        framing.target[c] = 0.5f * (lo[c] + hi[c]);
    }
    const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    framing.diagonal = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
    return framing;
}

}  // namespace splatcore
