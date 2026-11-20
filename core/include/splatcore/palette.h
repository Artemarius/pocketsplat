#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "splatcore/result.h"
#include "splatcore/splat_cloud.h"

// SH 1 palette (docs/sh1-palette-research.md, Phase 8): cluster the scene's 9-D SH1 vectors
// (coefficient-major c0.rgb c1.rgb c2.rgb, as in SplatCloud::sh at degree 1) into at most 65,536
// centroids, so each splat can carry a 16-bit index instead of its own 12 B of SH1: pack v3
// (build_pack with a palette, docs/formats.md §3). apply_sh1_palette snaps a cloud's SH1 to the
// centroids instead - the direct-pack reference a palette pack renders identically to. Phase B
// compared three trainers on one assignment kernel (research note §3):
//   kLloyd    full passes, empty clusters refilled by splitting the largest;
//   kHamerly  the same iterations with Hamerly's bounds (G. Hamerly, "Making k-means even faster",
//             SDM 2010): one upper and one lower bound per point skip most distance evaluations,
//             with the same labels and, bit for bit, the same centroids as kLloyd;
//   kEMeans   mini-batch k-means with EMA updates, a cosine step schedule, scaling laws for the
//             batch and the step, dead-cluster revival and last-epoch-mean finalization
//             (arXiv 2611.09501 v1; implemented from the paper, constants per its Table 2).

namespace splatcore {

inline constexpr int kSh1Dims = 9;                       // 3 coefficients x 3 channels
inline constexpr std::uint32_t kMaxPaletteSize = 65536;  // indices fit 16 bits

enum class PaletteTrainer { kLloyd, kHamerly, kEMeans };

const char* palette_trainer_name(PaletteTrainer trainer);  // "lloyd", "hamerly", "emeans"

struct PaletteParams {
    std::uint32_t k = 4096;     // palette size, 1..65536; clamped to the splat count
    std::uint32_t epochs = 25;  // Lloyd/Hamerly: update steps, fewer when no label changes;
                                // e-means: passes over the training set
    PaletteTrainer trainer = PaletteTrainer::kLloyd;
    std::uint64_t seed = 1;
    std::uint32_t train_cap = 0;  // train on at most this many splats (seeded sample); 0 = all
    std::uint32_t threads = 0;    // 0 = std::thread::hardware_concurrency()
    std::uint32_t batch = 0;      // e-means batch size; 0 = the scaling law (emeans_schedule)
    double alpha0 = 0.0;          // e-means initial step; 0 = the scaling law
};

// e-means schedule from the paper's scaling laws (§3.3, Table 2) for n training points, k clusters
// and `epochs` passes. Batch: the "knee / 8" law b = 0.0044 k^0.13 n^0.88 T^0.81, rounded to the
// nearest power of two (by ratio, ties up) and capped at n / 2. The paper uses that law for
// n > 2 M and the undivided one below; its reference code picks the divided law for d <= 16
// whatever n, which is our case (d = 9). Step: alpha0 = min(0.824 k^-0.46 B^0.90 n^-0.45 T^-0.11, 1)
// from the final batch size. The laws were fitted for 1e5 <= n <= 1e8, 256 <= k <= 65,536,
// n / k >= 8 at d = 96; d = 9 is outside what the paper tested.
struct EMeansSchedule {
    std::uint32_t batch = 0;
    double alpha0 = 0.0;
};
EMeansSchedule emeans_schedule(std::size_t n, std::uint32_t k, std::uint32_t epochs);

struct PaletteStats {
    double mse = 0.0;           // mean over splats of |sh1 - centroid|^2 (squared distance in 9-D)
    std::uint32_t dead = 0;     // palette entries no splat uses after the final assignment
    double used_entropy = 0.0;  // Shannon entropy of the label histogram, bits (<= log2 k)
    std::uint32_t epochs_run = 0;
    std::uint64_t pairs = 0;    // point-centroid distances evaluated while training (the work
                                // measure that does not depend on the machine); the final
                                // assignment of every splat (count x k) is not included
    std::uint32_t batch = 0;    // e-means: batch size used; 0 for the other trainers
    double alpha0 = 0.0;        // e-means: initial step used
    std::uint32_t updates = 0;  // e-means: centroid updates (batches) run
    std::uint32_t revived = 0;  // e-means: dead clusters revived by splitting a donor
    double train_ms = 0.0;      // training only: sampling, initialisation and the update steps
    double wall_ms = 0.0;       // training plus the final assignment of every splat
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

// The same over `count` SH1 vectors of kSh1Dims floats each (a cloud's `sh` array at degree 1).
Result<Sh1Palette> train_sh1_palette(const float* sh1, std::size_t count, const PaletteParams& params);

// Replaces every splat's SH1 by its palette entry. The cloud must be the one the palette was
// trained on (same count, degree 1).
Status apply_sh1_palette(SplatCloud& cloud, const Sh1Palette& palette);

// The assignment kernel every trainer shares: for each of the n points (kSh1Dims floats each),
// the index of the nearest of the k centroids (lowest index on ties) and, if sq_dist is not null,
// the squared distance to it. Splits the points over `threads` std::threads (0 = hardware).
void assign_nearest(const float* points, std::size_t n, const float* centroids, std::uint32_t k,
                    std::uint16_t* labels, float* sq_dist, std::uint32_t threads);

}  // namespace splatcore
