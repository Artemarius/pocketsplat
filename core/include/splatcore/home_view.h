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
//    (room: a wall wins, 30 deg). A plane more than 60 deg from +y needs >= 20 % of the points,
//    else up stays +y (S33: on museum1 and racoonfamily, +y-up captures of a lone object, a face
//    of the object won with 8-9 %; COLMAP floors far from +y, drjohnson's, hold 30-45 %).
//  * eye (ADR-0014, replacing ADR-0012's first-clear-side rule): of 96 candidates looking at the
//    focus - 8 sides x 15 / 35 deg above the plane x 1 to 0.18 of the full distance - the one
//    whose view shows the most fine detail, scored on a 48 x 48 raster of sampled splats (share
//    of pixels whose front splat has a 1-sigma radius <= 1 px; large near splats and empty
//    background score 0). Rendered, all 13 INRIA scenes (raw PLYs) and the 5 bench packs on the
//    phones open on detail; ADR-0012's rule failed drjohnson and playroom (eye inside geometry)
//    and opened truck on a far overview. S33: the raster has the shape of the frame the view is
//    shown in (HomeViewFrame); a square one picked views a portrait phone crops at the sides.
// Deterministic: the same pack always gives the same view (ties go to the first candidate).
struct HomeView {
    float focus[3];              // orbit pivot, RUB world
    float up[3];                 // unit; +y when no plane was found
    float eye[3];                // camera position, looking at the focus
    float distance;              // |eye - focus|, > 0
    bool plane_found;            // false: up is +y
    float full_distance = 1.0f;  // 2.3 x the half-detail radius (capped at the box diagonal): the
                                 // farthest candidate eye; ~1 ring radius on orbit captures (ADR-0012)
};
// The image the start view will be shown in: the eye candidates are scored through this frustum
// (portrait phone: ~30 deg across, 60 deg along). The FOV spans the long side, as in the app and
// the viewers. The default, a square at 60 deg, is the frame of ADR-0014. Focus, up and
// full_distance do not depend on it.
struct HomeViewFrame {
    float width = 1.0f;  // any unit; only the aspect ratio matters
    float height = 1.0f;
    float fov_long_rad = 1.0471976f;  // FOV across the long side
};
HomeView estimate_home_view(const GpuPack& pack, const HomeViewFrame& frame = {});

}  // namespace splatcore
