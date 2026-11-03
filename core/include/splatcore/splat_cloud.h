#pragma once

#include <cstddef>
#include <vector>

namespace splatcore {

// Y_0^0 = sqrt(1 / (4*pi)); display color c = 0.5 + kShC0 * f_dc, clamped to [0, 1].
inline constexpr float kShC0 = 0.28209479177387814f;

// Number of f_rest coefficients per channel at SH degree d: (d+1)^2 - 1.
constexpr int sh_rest_coeffs(int sh_degree) {
    return (sh_degree + 1) * (sh_degree + 1) - 1;
}

// SoA storage for a splat scene in trainer space (PLY conventions, see docs/formats.md):
// log-scales, quaternions (w, x, y, z) normalized on read, opacity as logit, dc as raw f_dc
// SH coefficients. sh holds the f_rest coefficients re-ordered coefficient-major: per splat,
// sh_rest_coeffs(sh_degree) coefficients x 3 channels with the coefficient index outermost
// (c0.rgb, c1.rgb, ...), unlike the planar-per-channel PLY layout.
struct SplatCloud {
    std::size_t count = 0;
    int sh_degree = 0;         // 0-3 as read from PLY; our own outputs are degree 0 or 1 only
    bool antialiased = false;  // mip-splatting render hint; carried by SPZ, absent from PLY

    std::vector<float> positions;   // 3 * count
    std::vector<float> log_scales;  // 3 * count
    std::vector<float> rotations;   // 4 * count, (w, x, y, z)
    std::vector<float> opacities;   // count, logit
    std::vector<float> dc;          // 3 * count, raw f_dc
    std::vector<float> sh;          // 3 * sh_rest_coeffs(sh_degree) * count, coefficient-major

    void resize(std::size_t n, int degree) {
        count = n;
        sh_degree = degree;
        positions.resize(3 * n);
        log_scales.resize(3 * n);
        rotations.resize(4 * n);
        opacities.resize(n);
        dc.resize(3 * n);
        sh.resize(3 * static_cast<std::size_t>(sh_rest_coeffs(degree)) * n);
    }

    bool valid_sizes() const {
        return positions.size() == 3 * count && log_scales.size() == 3 * count &&
               rotations.size() == 4 * count && opacities.size() == count && dc.size() == 3 * count &&
               sh.size() == 3 * static_cast<std::size_t>(sh_rest_coeffs(sh_degree)) * count &&
               sh_degree >= 0 && sh_degree <= 3;
    }
};

}  // namespace splatcore
