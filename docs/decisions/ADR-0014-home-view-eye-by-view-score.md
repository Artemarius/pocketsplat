# ADR-0014 — Home view: the eye is chosen by what its view shows

**Status**: accepted (2026-11-01, S24). Proposed in S22 after the desktop renders (Evidence),
with the owner's look on the phones open. The owner opened the bench scenes on the S25U (APK
`2023540`, the S22 home view unchanged) and judged every home view fine. Home view at scene open
in that session: S25U 625–1038 ms (stump 625, truck 966, counter 994, drjohnson 1038), S24U
656–750 ms (stump, bicycle-4m).

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
