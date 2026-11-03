#include "splatcore/coords.h"

#include <array>
#include <cstddef>

namespace splatcore {
namespace {

// Parity of each f_rest basis function in (x, y, z), in the standard real-SH coefficient order
// used by 3DGS (band 1: y, z, x; band 2: xy, yz, 3z^2-r^2, xz, x^2-y^2; band 3: m = -3..3).
// Negating an axis multiplies a coefficient by (-1)^parity for that axis. Derived from the basis
// polynomials, cross-checked against the SPZ reference converter's flip table.
struct Parity {
    int x, y, z;
};
constexpr std::array<Parity, 15> kRestParity = {{
    {0, 1, 0},
    {0, 0, 1},
    {1, 0, 0},  // band 1
    {1, 1, 0},
    {0, 1, 1},
    {0, 0, 0},
    {1, 0, 1},
    {0, 0, 0},  // band 2
    {0, 1, 0},
    {1, 1, 1},
    {0, 1, 0},
    {0, 0, 1},
    {1, 0, 0},
    {0, 0, 1},
    {1, 0, 0},  // band 3
}};

}  // namespace

void convert_coordinates(SplatCloud& cloud, CoordSystem from, CoordSystem to) {
    if (from == to) return;
    // kRdf <-> kRub either way is the same transform: negate y and z.
    for (std::size_t i = 0; i < cloud.count; ++i) {
        cloud.positions[3 * i + 1] = -cloud.positions[3 * i + 1];
        cloud.positions[3 * i + 2] = -cloud.positions[3 * i + 2];
        // (w, x, y, z) storage: conjugation by R_x(pi) negates the y and z components.
        cloud.rotations[4 * i + 2] = -cloud.rotations[4 * i + 2];
        cloud.rotations[4 * i + 3] = -cloud.rotations[4 * i + 3];
    }
    const int rest = sh_rest_coeffs(cloud.sh_degree);
    for (std::size_t i = 0; i < cloud.count; ++i) {
        float* sh = cloud.sh.data() + 3 * static_cast<std::size_t>(rest) * i;
        for (int k = 0; k < rest; ++k) {
            const int sign_flips =
                kRestParity[static_cast<std::size_t>(k)].y + kRestParity[static_cast<std::size_t>(k)].z;
            if (sign_flips % 2 != 0) {
                sh[3 * k + 0] = -sh[3 * k + 0];
                sh[3 * k + 1] = -sh[3 * k + 1];
                sh[3 * k + 2] = -sh[3 * k + 2];
            }
        }
    }
}

}  // namespace splatcore
