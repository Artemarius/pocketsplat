#pragma once

#include "splatcore/pack.h"

namespace splatcore {

// Start view for interactive use, estimated from the splats alone (no capture cameras needed).
// A capture orbits its main object at a distance far smaller than the scene's extent: the 13
// INRIA scenes' training cameras sit 4-45x closer than a whole-scene framing (robust_framing's
// 1.2 x diagonal), and their background splats only make sense from near that trajectory.
// Validated against those cameras (ADR-0012, benchmarks/home-view-inria-2026-10-31.csv):
//  * focus: the mode of the "detail density" (opacity / mean scale^2: small opaque splats are
//    where the cameras looked closely) on a 32^3 grid over the robust box, refined by mean shift;
//    0.08-0.41 ring radii from the point the cameras look at on the 11 orbit captures.
//  * full distance: 2.3 x the radius around the focus that holds half the detail weight;
//    0.45-1.62 x the ring radius on 11 of 13 scenes (truck 2.5, the walk-through playroom 3.2).
//  * up: the normal of the dominant plane (RANSAC over opaque splats near the focus: ground,
//    floor or table), signed toward +y; 0.7-6.4 deg from the cameras' mean up on 12 of 13 scenes
//    (room: a wall wins, 30 deg).
//  * eye (ADR-0014, replacing ADR-0012's first-clear-side rule): of 96 candidates looking at the
//    focus - 8 sides x 15 / 35 deg above the plane x 1 to 0.18 of the full distance - the one
//    whose view shows the most fine detail, scored on a 48 x 48 raster of sampled splats (share
//    of pixels whose front splat has a 1-sigma radius <= 1 px; large near splats and empty
//    background score 0). Rendered, all 13 INRIA scenes (raw PLYs) and the 5 bench packs on the
//    phones open on detail; ADR-0012's rule failed drjohnson and playroom (eye inside geometry)
//    and opened truck on a far overview.
// Deterministic: the same pack always gives the same view (ties go to the first candidate).
struct HomeView {
    float focus[3];    // orbit pivot, RUB world
    float up[3];       // unit; +y when no plane was found
    float eye[3];      // camera position, looking at the focus
    float distance;    // |eye - focus|, > 0
    bool plane_found;  // false: up is +y
};
HomeView estimate_home_view(const GpuPack& pack);

}  // namespace splatcore
