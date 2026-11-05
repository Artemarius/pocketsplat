# Coverage mask: what the capture cameras can tell the renderer (2026-10-31, S21–S22)

The owner's idea (S21): record, per splat, the set of directions it was observed from by the
capture cameras, and fade a splat out when the viewer looks at it from a direction outside that
set. Floaters and stretched "smear" splats only look right from where they were trained. Seen
from elsewhere they turn into streaks across the view, and the mask should hide them.

This note records the prototype, the hole-policy test that S21 left open, and the conclusion.
Status (end of S22): the owner chose partial transparency with a strength knob over the options
below. It is implemented as ADR-0013 (accepted S23): `.pscov` sidecar, preprocess fade,
`r = 1 − (1 − s)²`.

## Method (scratch prototype, stump)

* Scene: INRIA `stump` (4,961,797 splats) with its 125 training cameras (`cameras.json` from the
  INRIA `models.zip`, kept locally, never committed; `data/manifest.json` records the source).
* Coverage: for each camera, a splat counts as observed when its centre projects inside the image,
  in front of the camera, and not behind the camera's coarse depth buffer (the nearest opaque
  splat centre per pixel at 1/8 resolution, 3 % tolerance). The direction splat → camera is binned
  on a 5 × 5 octahedral grid of the sphere (25 cells of ~40°). The mask is the set of observed
  cells, one `uint32` per splat.
* Fade for an eye: `w = max over observed cells c of smoothstep(cos θ0, cos θ1, dot(v, c))`, with
  `v` the direction splat → eye: full opacity within θ1 of an observed cell centre, none beyond θ0.
  Never-observed splats (mask 0, 3.0 % of stump) get `w = 0`.
* The preview multiplies opacities by `w` in an SH0 PLY copy, rendered by the desktop host
  (960 × 540, 60° vertical FOV, the app's light background).

Coverage of stump: 3.0 % of splats never observed; **58.1 % observed from exactly one cell**,
13.4 % from two, 16.8 % from more than six.

## Hole policies tested (S22)

| Policy | Rule |
|---|---|
| A, fade all | θ1 = 25°, θ0 = 50° (the S21 prototype) |
| B, narrow only | A, but only for splats observed from ≤ 2 cells; the rest untouched |
| C, floor | max(A, 0.3) |
| D, soft | θ1 = 45°, θ0 = 90° |

Views: camera 0's pose; 1.5 and 3 ring radii out from the ring centre at camera height, looking at
the point nearest all camera axes; 2.5 ring radii above the centre; and S21's prototype view (12
units out along the home direction, ≈ 3.5 ring radii), the one with the floater streak.

Share of all splats faded (w < 0.01) / partly faded:

| View | A | B | C | D |
|---|---|---|---|---|
| camera 0 | 4.0 / 9.0 % | 3.3 / 5.7 % | 3.0 / 10.1 % | 3.1 / 1.0 % |
| 1.5 radii out | 4.9 / 8.8 % | 3.7 / 6.9 % | 3.0 / 10.8 % | 3.0 / 2.0 % |
| 3 radii out | 9.5 / 17.0 % | 6.9 / 13.3 % | 3.0 / 23.5 % | 4.4 / 5.2 % |
| above | 5.0 / 23.9 % | 4.8 / 15.0 % | 3.0 / 25.9 % | 3.0 / 2.1 % |
| S21 streak view | 10.1 / 19.3 % | 7.4 / 15.5 % | 3.0 / 26.4 % | 4.5 / 5.7 % |

What the images show:

* **Every fading policy cuts a hole into the foreground ground** wherever the eye is outside the
  ring: A, B and D alike. The hole shows the background. Even from camera 0's own position, A
  removes the strip of ground at the bottom of the frame: the render's 60° FOV sees more ground
  than that photo did, and the ground at a camera's feet is only ever seen at grazing angles from
  across the ring.
* **B does not help.** The ring-edge ground is observed from one or two cells just like the
  floaters, so "narrow" catches both.
* **C ghosts.** The streak and the smear stay visible at 30 %, which reads as a rendering fault.
* **D removes the streak** with the smallest hole, and it leaves the views from camera 0 and
  1.5 radii almost untouched (one small hole at the frame edge).

## Why the hole is inherent: streak and ground are the same population

The S21 streak view, splats under the streak against splats in the foreground hole (faded by D)
and against kept ground (p10 / p50 / p90):

| Region | n | cells | cameras seen by | nearest camera (ring radii) | max scale (ring radii) | long axis / middle axis |
|---|---|---|---|---|---|---|
| streak | 6,192 | 1 / 1 / 2 | 12 / 18 / 21 | 1.04 / 1.22 / 1.94 | 0.00 / 0.01 / 0.02 | 1.4 / 4.0 / 27 |
| hole | 9,262 | 1 / 2 / 2 | 10 / 13 / 16 | 0.71 / 1.12 / 1.45 | 0.00 / 0.01 / 0.03 | 1.4 / 3.6 / 16 |
| kept ground | 674,098 | 4 / 12 / 16 | 20 / 42 / 60 | 0.36 / 0.61 / 0.81 | 0.00 / 0.00 / 0.01 | 1.5 / 4.4 / 55 |

The streak and the hole are the same population: content **outside the camera ring**, observed by
10–20 cameras from one or two inward-looking directions. Their size, elongation, opacity and the
alignment of their long axis with the viewing rays have the same distributions. The kept
ground lies inside the ring and was seen from about twelve cells. From outside the ring, nothing
outside the ring has been observed from the viewer's side. A per-splat direction mask therefore
cannot remove the streak without the ground next to it. The hole is the mask working as
intended: it shows where the capture holds no information for this viewpoint.

Training-free alternatives do not separate them either. The cell count is B. The
nearest-camera distance overlaps, 1.0–1.9 against 0.7–1.5 ring radii. Elongation and the
ray-aligned "needle" test overlap too.

## Options for the owner (ADR-0013)

1. **Ship D, gated by the eye's position.** No fade while the eye is within the capture volume
   (near the cameras' positions), D beyond it. The viewer then shows full content where the
   capture supports it, and shows "nothing captured from here" as background once the user flies
   out. Cost: 4 B per splat (+14 % on the 28 B SH0 pack, +10 % on SH1), one octahedral encode
   and four bit tests per splat in the preprocess. It needs cameras, which the INRIA scenes ship and most
   downloaded PLYs do not.
2. **Constrain navigation instead.** Use the cameras (or the home view's ring estimate when there
   are none) to keep the orbit within the captured volume, softly (a resisting boundary) or with
   a "back to capture" hint. No per-splat data, no holes. The user never sees the streaks either.
3. **Drop the idea.** Keep the smear. The home view (ADR-0012) already starts users where the
   capture looks right.

The prototype and the renders are scratch-only (S21/S22 session scratchpads). Rebuilding them needs
`data/raw/public/stump.ply` and `data/raw/public/cameras/stump.json`.

## Without cameras: what the splats alone say about where the capture stood (2026-11-05, S27)

The owner's request (S27): the fade (ADR-0013) only worked on stump, the one scene with a
sidecar. Walls block the view once the orbit swings behind them, so rooms and orbited objects
are where it matters. Downloaded scenes rarely ship their cameras. Can the splats stand in for
them, given a scene type the user picks?

Ground truth for every probe: the real training cameras of the INRIA scenes (`cameras.json`,
local only). Probes 1-5 ran in numpy on 300 K-splat samples of the raw PLYs (scratch); 6-7 run
the shipped code on the full bench packs and are scored by `scripts/coverage_eval.py`.

**1. Which side of a flat splat was observed** (thin axis < 0.25 x the middle one; ground truth
= the side 80 % or more of its observing cameras were on; only about half of the flat splats
qualify, the rest were seen from both sides):

| Rule | room | counter | kitchen | drjohnson | playroom | stump | bicycle | garden |
|---|---|---|---|---|---|---|---|---|
| faces the home view's detail focus | 0.84 | 0.76 | 0.73 | 0.83 | 0.82 | 0.93 | 0.84 | 0.73 |
| faces enclosed free space (rays cross space, then hit) | 0.79 | 0.49 | 0.53 | 0.94 | 0.90 | 0.68 | 0.66 | 0.69 |

Majority votes over planar patches (voxel x normal bin) did not raise either (0.73-0.78).
Per splat, 80 % is a hole or a speck in every fifth wall splat.

**2. Needle splats point at the cameras?** Median |cos| between the long axis and the
direction to the nearest camera: 0.27 (stump), 0.35 (truck), 0.37-0.51 elsewhere; random is
0.50. Voting along the axes peaks 0.36-2.69 ring radii from the cameras. No.

**3. Flood fill from the viewer** (the observed side is the side touching free space connected
to the most open point near the focus): 3DGS walls leak. Even with every splat's footprint in
a 128^3 grid, the fill reached 91-98 % of all voxels of room, counter and drjohnson.

**4. Splat size as distance** (the owner's idea: training packs more, smaller splats where the
cameras were close). Spearman between a voxel's fine-splat size (10th percentile) and its
distance to the nearest camera:

| | counter | kitchen | bonsai | bicycle | garden | room | drjohnson | playroom | stump | truck |
|---|---|---|---|---|---|---|---|---|---|---|
| size | +0.51 | +0.69 | +0.49 | +0.47 | +0.65 | -0.07 | -0.06 | +0.08 | +0.06 | 0.00 |
| sparsity | +0.41 | +0.55 | +0.32 | +0.50 | +0.67 | +0.24 | +0.09 | +0.21 | +0.43 | +0.40 |

Real on orbit captures, absent on walk-throughs. With the true size-to-distance constant k,
voting "a camera is k x size away" put the top cells 0.15-0.42 ring radii from the cameras in
the three rooms, for any k within 0.3-2x the true one. k itself is the problem: from the home
distance it came out 2-15x too large in rooms, and choosing k where the votes agree most picked
0.12-0.8x of the true k on all 10 scenes. Not used; a better k estimate would revive it.

**5. One observed direction per splat** (no cameras at all: "seen from toward the focus"
within 60 deg): 51 % of the real cameras' reveals in room and drjohnson, with wrong reveals
of 30-34 % of that; in counter and stump it cut 2.5-7x more than the cameras. Replacing each
splat's real mask by a 60 deg cone around its mean observed direction keeps 73-82 %: the
ceiling of any one-direction rule.

**6. Virtual cameras, scene type from the user** (shipped, ADR-0015). Instead of guessing
masks, guess cameras and run the real `compute_coverage`, with its occlusion, over them:
* room: 24 omnidirectional cameras (6 cube faces each) at free points within 0.5 x the home
  view's full distance of its focus, where at least 23 of 26 rays hit geometry.
* object: a ring looking at the focus, 16 azimuths x 5-50 deg above the ground x 0.7 / 1.0 /
  1.4 x the full distance, skipping eyes inside geometry.

Two failures on the way, both visible in the numbers:
* Room positions by farthest-point sampling within the radius alone landed outside the walls
  (recall 0.12 in room and drjohnson): the enclosure test fixed it (0.69 / 0.89).
* An occupancy grid over the 1-99 % box made stump's voxels 1.2 units wide (its background
  spans 40 ring radii) and put every ring camera inside "geometry". The grid now sits on the
  focus (half size 2 x the full distance), and rays that leave it continue in a coarse grid
  over the whole scene, so a wall beyond the fine grid still encloses a point.

Object ring radii, swept on the orbit scenes (strength 1; recall / wrong reveals per real reveal):

| Ring (x full distance) | counter | bicycle | garden | bonsai | truck | kitchen | stump |
|---|---|---|---|---|---|---|---|
| 0.7, 1.0 | 0.69 / 0.51 | 0.74 / 0.55 | 0.43 / 0.05 | 0.61 / 0.39 | 0.20 / 0.11 | 0.53 / 0.40 | 0.72 / 3.24 |
| **0.7, 1.0, 1.4** | 0.63 / 0.38 | 0.73 / 0.41 | 0.36 / 0.03 | 0.52 / 0.23 | 0.16 / 0.08 | 0.47 / 0.26 | 0.70 / 2.59 |
| 1.0, 1.4, 2.0 | 0.64 / 0.53 | 0.76 / 0.42 | 0.37 / 0.03 | 0.48 / 0.34 | 0.16 / 0.07 | 0.50 / 0.33 | 0.76 / 3.06 |
| same, 0-60 deg | 0.61 / 0.44 | 0.77 / 0.49 | 0.43 / 0.04 | 0.34 / 0.22 | 0.10 / 0.05 | 0.52 / 0.31 | 0.77 / 2.40 |

0.7 / 1.0 / 1.4 has the best recall minus wrong reveals over the six scenes other than stump
(0.25 against 0.20). Stump's ratios are large because its cameras reveal only 2.3 % of the
pixels from these eyes: they stood all around it.

**7. Camera files**, for scenes that ship them: COLMAP binary and the INRIA `cameras.json` give
byte-identical sidecars on truck and drjohnson (`tandt_db.zip`'s sparse models). A
`transforms.json` written the way nerfstudio's `ns-process-data` writes it (re-oriented frame,
`applied_transform`) is undone automatically and also gives truck's `cameras.json` sidecar
byte for byte; a copy of truck moved into a normalized frame is matched through its
`dataparser_transforms.json` (fit 0.999). Without that file the same cameras fit only 0.765 in
their best frame; real captures fit 0.999-1.000 on all 13 scenes, so files under 0.9 are
rejected.
