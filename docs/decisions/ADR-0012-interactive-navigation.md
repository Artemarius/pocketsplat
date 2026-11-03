# ADR-0012 — Interactive navigation: a start view from the splats, a quaternion trackball, a depth-picked pivot

**Status**: accepted (2026-10-31, S21), after the owner's hand-held test on the S24U.

**Context**: feedback from the first hand-held session (S21, S24U, stump):
* The whole-scene start view showed the background as floating fragments. A capture's background
  only makes sense from near the capture trajectory, and the main object shows its detail only
  from that close.
* The orbit turned about world +y with the pitch clamped, and the middle of that range did not
  match stump's ground.
* Lifting one finger after a pinch made the camera jump.
* Wanted: unlimited rotation with quaternion state, and a double tap that moves the rotation
  point to the tapped spot.

The training cameras of the 13 INRIA scenes (`cameras.json` from the pretrained models archive,
read for validation only; the metrics are in `benchmarks/home-view-inria-2026-10-31.csv`) put
numbers on the first two points:
* the start view (1.2 × the 5–95 % box diagonal) sat 3.9–45.5 ring radii out (stump 45.5);
* the box centre was 0.05–7.2 ring radii from the point the cameras look at;
* the cameras' mean up was 5–88° from world +y (stump 46°).

**Decision**:

1. **Start at a home view estimated from the splats** (`splatcore::estimate_home_view`, no capture
   cameras needed, so it works for any imported PLY or SPZ).
   * Focus: the mode of a detail density, opacity / mean scale² (small opaque splats are where
     the cameras looked closely), on a 32³ grid over the robust box, refined by mean shift.
   * Distance: 2.3 × the radius around the focus that holds half the detail weight.
   * Up: the normal of the dominant plane near the focus (RANSAC: ground, floor or table), signed
     toward +y.
   * Eye: 15° above that plane (else 35°, else closer), on the first of 8 sides where no splat
     reaches into the line of sight.

   Against the training cameras of the 13 scenes:
   * home distance 0.45–1.62 ring radii on 11 of them;
   * focus 0.08–0.41 ring radii from the cameras' look-at point on the 11 orbit captures;
   * eye within 1.5–11.6° of a real camera on 12;
   * up within 0.7–6.4° on 12.

   Rendered, 9 of 13 open at a capture-like view. The rest:
   * truck starts 2.5 radii out;
   * room is tilted, because a wall wins the plane fit (up 30° off);
   * drjohnson and playroom are walk-through captures with no central object, and the eye lands
     in clutter.

   The estimate takes 0.5–0.6 s for stump's 4 M splats on both phones, at scene open. Every INRIA
   ring has a radius near 4 because COLMAP normalizes scene extent. The estimator therefore uses
   only scene-relative quantities, never absolute ones.
2. **A quaternion trackball around a pivot.** The camera is (pivot, distance, orientation): a
   unit quaternion, camera-to-world, renormalized after every update. One-finger drags turn it
   about the camera's own up and right axes. There is no fixed axis and no limit (the owner's
   choice over a level orbit). Pinch scales the distance to the pivot. A two-finger drag moves
   the pivot in the camera plane, so the content under the fingers follows them.

   The trackball's known cost was measured: from the home view's 15° downward look, one 500 px
   horizontal swipe takes the eye to 26° below the ground with 28° of roll. A HUD toggle
   therefore switches horizontal drags to the scene's fitted up axis (level orbit), and
   "Home view" returns to the start pose.
3. **Double tap moves the pivot to the point under the finger, as drawn.** The point comes from a
   GPU depth-pick pass (`SplatRenderer::request_pick`, `take_pick`):
   * the frame after a request keeps its sorted keys;
   * a compute pass walks the draw order, evaluates the fragment shader's kernel from each record
     at the pixel, and appends (depth from the key, alpha) for every covering splat;
   * when the slot completes, the host composites the pairs front to back and takes the depth
     where the opacity reaches half its total; under 10 % coverage is a miss;
   * the camera then turns toward the point in 300 ms with the eye fixed, and pinch zooms
     toward it.

   Validation:
   * desktop validation layers clean;
   * 32- and 16-bit keys agree to 0.1 mm;
   * the Adreno 750 and 830 return the same point to the last printed digit for the same tap on
     stump's home view: (0.095, −2.407, −1.233), 2.439 from the eye.

   Normal frames are unchanged. A pick costs one sort that keeps its keys and one dispatch.
4. **One-finger drags follow a pointer id.** Reading pointer index 0 after the other finger of a
   pinch lifted picked up that finger's position, and the gap between the fingers became an orbit.
5. The bench keeps its deterministic yaw orbit around robust_framing's target. Interactive
   changes do not touch benchmark comparability.

**Consequences**:
* The second hand-held session (S21) ran on these controls. Its trace is the first record of how
  people use the new start view and pivot.
* The capture cameras, where a pipeline ships them (INRIA output folders do), would give an exact
  start pose and the observed-direction mask idea (per-splat view coverage to fade splats seen
  from directions no camera saw). Both need a sidecar or pack change; not done here.

**Alternatives considered**:
* A CPU pick over a kept copy of the splats: about 64 MB more host memory for stump, and it
  would not see the frame's culling. Recomputing the projection inside the pick shader would
  duplicate the preprocess.
* The old turntable (yaw about +y, pitch clamped at ±89°): unusable when the scene's up is not +y.
* Embedding the training cameras' poses in the pack: exact where available, absent for phone
  imports. The estimator covers both, and the cameras remain a possible refinement.
* Splat size as a distance cue: trained splats are about one training pixel at the ring
  (0.6–1.9 px), but training resolutions differ 4× (1264–5187 px wide), so the distance it gives
  varies 4× between scenes.
