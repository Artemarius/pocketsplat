# ADR-0014 — Home view: the eye is chosen by what its view shows

**Status**: accepted (2026-11-01, S24). Proposed in S22 after the desktop renders (Evidence),
with the owner's look on the phones open. The owner opened the bench scenes on the S25U (APK
`2023540`, the S22 home view unchanged) and judged every home view fine. Home view at scene open
in that session: S25U 625–1038 ms (stump 625, truck 966, counter 994, drjohnson 1038), S24U
656–750 ms (stump, bicycle-4m).
Amended in S33 (2026-11-24, accepted after the owner's look on the S24U): the score raster takes the shape of the frame the view
is shown in, and a weak plane far from +y no longer sets the up axis (amendment at the end).

**Supersedes**: the eye rule of ADR-0012 decision 1 ("15° above the plane, else 35°, else
closer, on the first of 8 sides where no splat reaches into the line of sight"). The focus,
distance and up estimates of ADR-0012 are unchanged.

**Context**: rendered from the INRIA scenes' raw PLYs, ADR-0012's home view opened 11 of 13 scenes
on real detail:
* drjohnson and playroom, walk-through captures of rooms, opened inside or behind geometry. The
  screen was full of blobs: every side was "blocked" by walls, so the rule took the least blocked
  side.
* truck opened on a far overview behind a foreground of streaks: the cleaned bench pack's full
  distance is 21.7 units, against 9.7 for the raw PLY. The cleaning drops 887 K splats below 2 %
  opacity, and those faint tiny splats had carried much of the detail weight near the truck.

The owner's criterion (S22): what matters is the region looked at and the distance. The
orientation may be off, there are many good poses, and the choice must be deterministic. It must
show detail, not an overview of floating garbage.

**Decision**: score each candidate eye by the image it would produce and take the best.
* **Candidates** (96): 8 sides × 15° / 35° above the fitted plane × 1, 0.7, 0.5, 0.35, 0.25 and
  0.18 of the full distance, all looking at the focus.
* **Score**: about 100 K sampled splats with opacity above 0.3 are rastered nearest-first into a
  48 × 48 image, each as a square of its 1σ radius, with a 60° FOV. The score is the share of
  pixels whose front splat has a 1σ radius of at most 1 px: fine detail.
  * A large splat in front, from an eye inside or against geometry or a floater in the way, scores
    0 on those pixels. Empty background scores 0.
  * When the median depth of the 4 × 4 centre block is under 0.6 × the distance, the focus is
    hidden and the score is halved.
* **Ties** go to the first candidate in the order above, so the view is deterministic.

**Evidence** (desktop renders, 960 × 540, the app's background):
* Raw PLYs of all 13 INRIA scenes: every scene opens on detail. drjohnson shows the room with
  its carpet, playroom the play corner, truck the truck close up. Bicycle now frames the bike
  and bench (before: the bench and path).
* The 5 bench packs on the phones (stump, truck, drjohnson, bicycle, counter): all open on
  detail. Truck needed the 0.25 and 0.18 distances: with candidates down to 0.35 only, its
  pack still opened at 21.7.
* An intermediate variant with a 60° elevation tier turned 6 scenes into near-top-down views of
  ground texture: the score rewards fine texture, and a ground seen from above is all fine
  texture. Elevations stay at 15° / 35°.
* `HomeView.PicksTheSideWhoseViewShowsDetail`: a thick slab of large blobs beside the object
  holds or hides every candidate eye on its side; the eye goes at least 60° away from it.
* Cost: about 96 × 100 K projections. On the S25U the whole home view takes 647–1091 ms at
  scene open (stump 647, bicycle 705, truck 991, drjohnson 1091). Truck took 898 ms under
  ADR-0012's rule in S21.

**Consequences**:
* The home view moves closer on many scenes (0.35–0.7 × the full distance is common). The Home
  button returns to it.
* The validation numbers of ADR-0012 (distance and eye angle against the training cameras)
  describe the old eye. The new eye is judged by renders, which is the owner's criterion.
* The score favours fine texture. A scene whose main object is smooth, next to a highly
  textured background, could be framed on the background. Not seen in the 13 scenes.

**Alternatives considered**:
* Up from the axial mode of flat-splat normals (to fix room's 30° tilt): failed worse, logged in
  the negative-results table. The owner does not need the orientation fixed.
* A third, 60° elevation tier: top-down ground views, see Evidence.

**Amendment (S33, 2026-11-24, accepted): the frame the view is shown in, and weak tilted planes.**
Accepted by the owner after opening beetle, museum1, counter and racoonfamily on the S24U
(APK `bd3379b`); the 20 % threshold and the raw-PLY portrait changes accepted as they are.

*Context.* The S33 phone check before v0.4.0 (S24U, APK `905e9c6`): the owner found beetle's and
museum1's start views too close, and museum1 tilted. Two causes, both older than S32
(`docs/benchmarks/README.md`, S33 section):
* The 48 × 48 score raster is a square at 60°, but the app shows a portrait frame: 60° along,
  ~30° across at 1080 × 2340. The winning view was framed for a screen twice as wide, so its
  subject ran off the sides.
* On museum1 (a statue) and racoonfamily (a carved stump), +y-up captures of a lone object, the
  best plane near the focus is a face of the object: 80–85° from +y with 7.6–9.0 % of the points.
  This amends ADR-0012's up estimate, which ADR-0014 had left unchanged.

*Decision.*
* `estimate_home_view(pack, frame)`: `HomeViewFrame` gives the width, height and FOV across the
  long side. A portrait frame narrows the raster to its width (22 × 48 at 1080 × 2340, one focal
  length for both sides). A landscape frame is scored on the full 48 × 48 square: cutting the
  square's top and bottom opened the truck pack on a 21.7 m overview of floaters at 16:9 (desktop,
  1280 × 720), because it drops the empty-sky rows that keep the score off overviews. Candidates, elevations, distances, the centre-block test
  and tie order are unchanged. The default frame is the old square, so focus, up, `full_distance`,
  virtual capture (ADR-0015) and `splatconv` are unchanged.
  * The app passes its surface size when the scene opens (portrait 9:19.5 if no surface exists
    yet) and keeps that view on rotation. The Windows viewer passes its framebuffer, and the
    desktop host its window.
* A plane more than 60° from +y must hold at least 20 % of the points near the focus; otherwise no
  plane is found and up is +y. The only correct planes that far from +y on the 37 probe scenes are
  the drjohnson family's floors, at 89° (COLMAP frame) with 30.4–44.8 %. Every other plane is
  within 48.5° of +y.

*Evidence.*
* Desktop renders at 540 × 1170 (the phone's portrait at half size) of the 8 phone scenes, square
  vs frame. Better: beetle (the whole car, side-on), counter (the whole counter instead of a pot
  close-up), bicycle (bike and bench), museum1 (more of the statue). Unchanged: stump, truck,
  drjohnson. The owner judged them fine.
* Up axis over 37 scenes (`home-view-tilt-probe-0d73046-2026-11-24.csv`; 36 with a plane):
  bit-identical on 33. Changed on the 3 museum1 packs and racoonfamily, which now open upright.
* Home distance under the square, portrait and landscape frames on all 37 scenes (desktop): with
  the frame applied both ways, only the truck pack in landscape jumped to an overview. Portrait
  changes stay within 0.36–2.0 × the square's distance. Raw PLYs at portrait, judged by eye: garden
  and treehill better, room and voxel51_playroom about the same, truck.ply (the bed instead of the
  cab) and drjohnson.ply (a corridor with a dark floater) worse. The cleaned packs of those two,
  which are what the app imports, keep their views.
* Tests: `HomeView.AWeakPlaneFarFromPlusYKeepsPlusY` (fails with the rule off),
  `AStrongGroundFarFromPlusYIsKept`, `DefaultAndDegenerateFramesAreTheSquare`,
  `APortraitFrameMovesOnlyTheEye`.
* Cost: the portrait raster is 46 % of the square's pixels and projects the same splats. Open time
  before the change, S24U: 86–199 ms (`home-view-open-s24u-905e9c6-2026-11-24.csv`). With it (APK `bd3379b`, 30 opens): 76–160 ms, median
  100.5 ms; the app logs the portrait distances (counter 3.301, room 1.950) and no plane on
  museum1 and racoonfamily.

*Alternatives rejected* (negative-results log):
* Moving the chosen eye back along its ray, × 1.3–2.0: poles, signs and a wall come between the
  camera and the subject. The score had checked the line of sight only at its own distance.
* Candidates beyond the full distance, or "the farthest within 15 % / 30 % of the best score":
  truck opens on a 22–35 m overview from outside the capture.
* Any +y gate on its own: drjohnson's correct floor is 89° from +y.
* Separating museum1 by share alone (0.090 against hornedlizard's correct 0.105), in-plane
  spread, focus height, or the strength of the best rival plane: no clean gap.

*Consequences.*
* Bench runs with `--pose home` and the home-view numbers in earlier CSVs describe the square
  frame. After this change they are not like-for-like on scenes whose view moved.
* The tilt rule turns a real ground into +y when the ground holds under 20 % of the points near
  the focus and lies more than 60° from +y. `AWeakPlaneFarFromPlusYKeepsPlusY` builds exactly that
  case (a true ground at 75° with ~13 %): it now opens rolled. The 20 % line rests on one family of
  correct far-from-+y scenes (drjohnson, 30–45 %); correct grounds near +y go down to 10.5 %
  (hornedlizard). The owner kept 20 %.
* museum1 now opens level but still close on one figure. Its focus is the statue's detail peak, and
  ADR-0012's distance rule is unchanged.
