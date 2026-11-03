#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "splatcore/splat_cloud.h"

namespace splatcore {

// Per-op report record; a cleaning run is a vector of these (see docs/cleaning.md "Reporting").
struct CleanOpStats {
    std::string op;
    std::size_t input_count = 0;
    std::size_t removed = 0;
    std::string params;  // human-readable parameter summary, e.g. "min_alpha=0.02"
};

struct Aabb {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
    float diagonal() const;
};

// Robust scene box: per-axis 1st-99th percentile of finite positions, so a few far-away outliers
// don't inflate the thresholds derived from it (docs/cleaning.md). Falls back to min/max for
// clouds with fewer than 100 splats.
Aabb robust_aabb(const SplatCloud& cloud);

// Importance proxy used by prune_budget and later by the on-device adaptive controller:
// alpha * (sx*sy + sy*sz + sz*sx) (opacity times mean projected area, docs/cleaning.md op 6).
float importance(const SplatCloud& cloud, std::size_t i);

// All ops mutate the cloud in place and preserve the relative order of surviving splats
// (except reorder_morton, whose purpose is to change the order). Default parameters are the
// documented defaults from docs/cleaning.md; changing them there requires an ADR or benchmark.

// Op 1: remove splats with non-finite fields, zero-norm quaternions, or scales that collapse to
// zero after exp().
CleanOpStats drop_invalid(SplatCloud& cloud);

// Op 2: remove splats with sigmoid(opacity) < min_alpha.
CleanOpStats prune_opacity(SplatCloud& cloud, float min_alpha = 0.02f);

// Op 3: remove splats whose largest axis exceeds max_diag_fraction of the scene diagonal or whose
// smallest axis is below min_diag_fraction of it. `scene` should come from robust_aabb() after
// ops 1-2. The smallest-axis test is off by default (ADR-0007): trained scenes are mostly flat,
// surface-aligned disks whose thin axis sits far below any diagonal fraction.
CleanOpStats prune_scale(SplatCloud& cloud, const Aabb& scene, float max_diag_fraction = 0.05f,
                         float min_diag_fraction = 0.0f);

// Op 3b: clamp needle-shaped splats' longest axis to max(len_cap_fraction * scene diagonal,
// aspect_cap * second-largest axis). Removes nothing. Reconstruction-noise needles (one axis far
// longer than the other two) otherwise render as rays crossing silhouettes in every faithful
// viewer (docs/benchmarks/README.md, S10 streak investigation); disks and walls have a
// comparable second axis, so the aspect term leaves them untouched.
CleanOpStats clamp_needle_scales(SplatCloud& cloud, const Aabb& scene, float len_cap_fraction = 0.01f,
                                 float aspect_cap = 6.0f);

// Op 4: statistical outlier removal on centers - remove splats whose mean distance to the
// k_neighbors nearest neighbours exceeds mean + stddev_multiplier * stddev over the scene.
CleanOpStats remove_floaters_sor(SplatCloud& cloud, int k_neighbors = 16, float stddev_multiplier = 2.0f);

// Op 5: drop SH bands above target_degree (no re-fit; DC already holds the mean color). No-op if
// the cloud is already at or below the target.
CleanOpStats truncate_sh(SplatCloud& cloud, int target_degree);

// Op 6: keep the top max_splats by importance(); ties are broken by original order. No-op if the
// cloud is already within budget.
CleanOpStats prune_budget(SplatCloud& cloud, std::size_t max_splats = 1000000);

// Op 7: reorder splats by 30-bit Morton code of the position quantized inside `scene` (positions
// outside it clamp to the box). Deterministic; removes nothing.
CleanOpStats reorder_morton(SplatCloud& cloud, const Aabb& scene);

}  // namespace splatcore
