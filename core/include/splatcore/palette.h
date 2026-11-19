#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "splatcore/result.h"
#include "splatcore/splat_cloud.h"

// SH 1 palette (docs/sh1-palette-research.md, Phase 8): cluster the scene's 9-D SH1 vectors
// (coefficient-major c0.rgb c1.rgb c2.rgb, as in SplatCloud::sh at degree 1) into at most 65,536
// centroids, so each splat can carry a 16-bit index instead of its own 12 B of SH1. Experimental:
// Phase A measures the image-quality price with ordinary packs whose SH1 is snapped to the
// centroids (apply_sh1_palette); no pack format uses the palette yet.

namespace splatcore {

inline constexpr int kSh1Dims = 9;                       // 3 coefficients x 3 channels
inline constexpr std::uint32_t kMaxPaletteSize = 65536;  // indices fit 16 bits

enum class PaletteTrainer { kLloyd };

struct PaletteParams {
    std::uint32_t k = 4096;     // palette size, 1..65536; clamped to the splat count
    std::uint32_t epochs = 25;  // update steps; Lloyd stops earlier when no label changes
    PaletteTrainer trainer = PaletteTrainer::kLloyd;
    std::uint64_t seed = 1;
    std::uint32_t train_cap = 0;  // train on at most this many splats (seeded sample); 0 = all
    std::uint32_t threads = 0;    // 0 = std::thread::hardware_concurrency()
};

struct PaletteStats {
    double mse = 0.0;           // mean over splats of |sh1 - centroid|^2 (squared distance in 9-D)
    std::uint32_t dead = 0;     // palette entries no splat uses after the final assignment
    double used_entropy = 0.0;  // Shannon entropy of the label histogram, bits (<= log2 k)
    std::uint32_t epochs_run = 0;
    double wall_ms = 0.0;  // training plus the final assignment of every splat
};

struct Sh1Palette {
    std::vector<float> centroids;       // kSh1Dims * k, coefficient-major like SplatCloud::sh
    std::vector<std::uint16_t> labels;  // count: palette entry of each splat
    PaletteStats stats;

    std::uint32_t size() const { return static_cast<std::uint32_t>(centroids.size() / kSh1Dims); }
};

// Trains a palette over the cloud's SH1 vectors. Deterministic for a fixed seed and thread count.
// Fails for a cloud that is not at SH degree 1, an empty cloud, or k outside 1..65536.
Result<Sh1Palette> train_sh1_palette(const SplatCloud& cloud, const PaletteParams& params);

// Replaces every splat's SH1 by its palette entry. The cloud must be the one the palette was
// trained on (same count, degree 1).
Status apply_sh1_palette(SplatCloud& cloud, const Sh1Palette& palette);

// The assignment kernel every trainer shares: for each of the n points (kSh1Dims floats each),
// the index of the nearest of the k centroids (lowest index on ties) and, if sq_dist is not null,
// the squared distance to it. Splits the points over `threads` std::threads (0 = hardware).
void assign_nearest(const float* points, std::size_t n, const float* centroids, std::uint32_t k,
                    std::uint16_t* labels, float* sq_dist, std::uint32_t threads);

}  // namespace splatcore
