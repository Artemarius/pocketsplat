#include "splatcore/spz_io.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "load-spz.h"

namespace splatcore {

Result<SplatCloud> read_spz(const std::filesystem::path& path) {
    using R = Result<SplatCloud>;
    spz::UnpackOptions options;
    options.to = spz::CoordinateSystem::RUB;  // SPZ native; explicit no-op conversion
    const spz::GaussianCloud gaussians = spz::loadSpz(path.string(), options);
    if (gaussians.numPoints <= 0) {
        // The library returns an empty cloud (and logs) on any failure; it cannot distinguish a
        // genuinely empty file, which we treat as an error too.
        return R::failure("cannot read SPZ '" + path.string() + "' (missing, corrupt, or empty)");
    }
    if (gaussians.shDegree < 0 || gaussians.shDegree > 3) {
        return R::failure("SPZ SH degree " + std::to_string(gaussians.shDegree) +
                          " is outside the supported range 0-3");
    }

    const std::size_t n = static_cast<std::size_t>(gaussians.numPoints);
    SplatCloud cloud;
    cloud.resize(n, gaussians.shDegree);
    cloud.antialiased = gaussians.antialiased;
    if (gaussians.positions.size() != 3 * n || gaussians.scales.size() != 3 * n ||
        gaussians.rotations.size() != 4 * n || gaussians.alphas.size() != n ||
        gaussians.colors.size() != 3 * n || gaussians.sh.size() != cloud.sh.size()) {
        return R::failure("SPZ '" + path.string() + "' has inconsistent array sizes");
    }
    // Same conventions on both sides (log scales, logit alphas, f_dc colors, coefficient-major SH
    // with channel fastest); only the quaternion order differs: spz (x,y,z,w) -> ours (w,x,y,z).
    std::memcpy(cloud.positions.data(), gaussians.positions.data(), 3 * n * sizeof(float));
    std::memcpy(cloud.log_scales.data(), gaussians.scales.data(), 3 * n * sizeof(float));
    std::memcpy(cloud.opacities.data(), gaussians.alphas.data(), n * sizeof(float));
    // spz's invSigmoid returns +-inf for alphas that saturate the 8-bit encoding (byte 0 or 255).
    // Those are legitimate fully-transparent/fully-opaque splats, not garbage - clamp to a finite
    // logit far beyond the format's resolution (sigmoid(15) differs from 1 by ~3e-7).
    for (float& opacity : cloud.opacities) opacity = std::clamp(opacity, -15.0f, 15.0f);
    std::memcpy(cloud.dc.data(), gaussians.colors.data(), 3 * n * sizeof(float));
    std::memcpy(cloud.sh.data(), gaussians.sh.data(), cloud.sh.size() * sizeof(float));
    for (std::size_t i = 0; i < n; ++i) {
        cloud.rotations[4 * i + 0] = gaussians.rotations[4 * i + 3];
        cloud.rotations[4 * i + 1] = gaussians.rotations[4 * i + 0];
        cloud.rotations[4 * i + 2] = gaussians.rotations[4 * i + 1];
        cloud.rotations[4 * i + 3] = gaussians.rotations[4 * i + 2];
    }
    return R::success(std::move(cloud));
}

Status write_spz(const SplatCloud& cloud, const std::filesystem::path& path) {
    if (cloud.sh_degree != 0 && cloud.sh_degree != 1) {
        return Status::failure("SPZ export supports SH degree 0 or 1 only, got degree " +
                               std::to_string(cloud.sh_degree) + " (truncate first)");
    }
    if (!cloud.valid_sizes()) return Status::failure("SplatCloud has inconsistent array sizes");

    spz::GaussianCloud gaussians;
    gaussians.numPoints = static_cast<int32_t>(cloud.count);
    gaussians.shDegree = cloud.sh_degree;
    gaussians.antialiased = cloud.antialiased;
    gaussians.positions = cloud.positions;
    gaussians.scales = cloud.log_scales;
    gaussians.alphas = cloud.opacities;
    gaussians.colors = cloud.dc;
    gaussians.sh = cloud.sh;
    gaussians.rotations.resize(4 * cloud.count);
    for (std::size_t i = 0; i < cloud.count; ++i) {
        gaussians.rotations[4 * i + 0] = cloud.rotations[4 * i + 1];
        gaussians.rotations[4 * i + 1] = cloud.rotations[4 * i + 2];
        gaussians.rotations[4 * i + 2] = cloud.rotations[4 * i + 3];
        gaussians.rotations[4 * i + 3] = cloud.rotations[4 * i + 0];
    }

    spz::PackOptions options;
    options.from = spz::CoordinateSystem::RUB;  // our clouds are RUB at export time; no conversion
    if (!spz::saveSpz(gaussians, options, path.string())) {
        return Status::failure("failed to write SPZ '" + path.string() + "'");
    }
    return Status::success();
}

}  // namespace splatcore
