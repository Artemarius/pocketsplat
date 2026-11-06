# ADR-0015 — Backside info: camera files in three formats, else virtual cameras for a scene type the user picks

**Status**: accepted (2026-11-05, S27). Implemented and measured against the INRIA scenes' real
cameras on the desktop (Evidence), and run on the S24U for timing and cross-platform agreement.
The owner then used it hand-held on the S24U (release APK 0.2.0): "works great", "so useful on
drjohnson" (a guessed room), and beetle with a guessed object "works amazingly". Two follow-ups
from that session: the release ramp stalled when the fingers rested before lifting (on-demand
rendering left it without frames; fixed, Evidence), and the wording now offers the guess
("Guess: room (walked through)", "Guess: object (went around it)") where a scene has none.

**Extends**: ADR-0013 (the coverage fade and its `.pscov` sidecar). The fade, the shader and the
sidecar layout are unchanged; the header's `reserved` word becomes `source`.

**Context**: the owner (S27): the fade ("backside filtering") is very useful, but only stump had
a sidecar. Walls block the view as soon as the orbit swings behind them in an indoor scene, and
the same happens with orbited central objects. Downloaded scenes rarely ship `cameras.json`,
the one camera format ADR-0013 read; training pipelines write COLMAP models or a
`transforms.json` more often. The owner asked what the splats alone can tell, and accepted a
choice of scene type by the user where no cameras exist ("best guess"). Later in S27 the owner
added: show the fade only while a gesture is under way, by default, so that a wrong guess never
hides content from a view the user has settled on.

**Decision**:

1. **Camera files, three formats** (`splatcore/capture.h`), all in the PLY's world:
   * 3DGS `cameras.json` (as before);
   * COLMAP sparse models: `cameras` + `images`, `.bin` or `.txt`, any camera model (distortion
     ignored, principal point assumed centred);
   * nerfstudio / instant-ngp / Brush `transforms.json` (OpenGL camera-to-world). nerfstudio
     re-orients the COLMAP world when it writes one (`applied_transform`), and a splatfacto
     export may sit in the normalized frame of its `dataparser_transforms.json`. Which frame a
     PLY uses depends on the tool and its version, so every plausible frame is tried (as given,
     `applied_transform` undone, dataparser transform applied when that file is among the
     picked ones) and the one whose cameras see the most of the scene wins.
   * The format is sniffed from the content. Files whose cameras see under 90 % of the opaque
     splats (`capture_fit`) are rejected: real captures see 99.9–100 %.
2. **Without cameras, virtual cameras for a scene type the user picks**, fed to the unchanged
   `compute_coverage` (with its occlusion). Both use the home view (ADR-0014): its focus,
   ground plane and full distance.
   * **Room** (a space walked through): 24 omnidirectional cameras (6 cube faces each) at free
     points within 0.5 × the full distance of the focus, sampled farthest-first from the home
     eye among the points where at least 23 of 26 axis and diagonal rays hit geometry. Rays
     walk a 128³ grid on the focus (half size 2 × the full distance), then a 64³ grid over the
     whole scene, so a wall beyond the fine grid still counts. The sampling runs on integer
     voxel distances.
   * **Object** (orbited): a ring looking at the focus, 16 azimuths × 5 / 20 / 35 / 50° above
     the ground × 0.7 / 1.0 / 1.4 × the full distance, skipping eyes inside geometry.
3. **The sidecar records its source** in the header word that was `reserved` (0 cameras,
   1 room, 2 object; older files read as 0). The app shows it ("Backside info: inferred: room").
4. **In the app**:
   * Import: "Backside: off / room / object / cameras…" (default off). Cameras… opens a
     multi-file picker. The sidecar is computed after the pack is written.
   * Viewer: for any open pack, "Add / Redo: room · object · cameras…", which recomputes the
     sidecar and reopens the scene. Scripted: `open_scene` with `backside` = room | object |
     cameras and `backside_files`.
   * **The fade shows while a gesture moves the view, by default** (owner, S27): a drag past
     the touch slop or a pinch turns it on at the slider's strength (default 0.75); after the
     last finger lifts it eases out over 250 ms. "Fade: while moving / always / never" switches
     (S28, owner: one tap to turn a guessed fade off; "never" sends strength 0 and keeps the
     slider's value for the other two).
     Taps and the double-tap pick never trigger it. Scripted runs that pass
     `coverage_strength` (every bench since ADR-0013) fade always, as before.
5. `splatconv coverage <scene> <camera files…> <out>` or `… <out> --infer room|object`; both
   paths and the app share `make_coverage_sidecar`.

**Evidence**:
* Readers (`splatcore_tests` `Capture.*`, 8 tests): COLMAP binary and text and `transforms.json`
  agree with `cameras.json` on the same synthetic cameras; the format sniffing; a truncated
  `images.bin` is an error; a `transforms.json` in nerfstudio's frame is undone; foreign
  cameras are rejected. ctest 111/111 at acceptance.
* Gesture gating (S24U, scripted `input motionevent`, verbose log): drag then rest 1.5 s then
  lift -> full strength back 0.33 s after the lift (the 0.25 s ease-out; before the fix it stayed
  faded until the next touch); a tap never fades; lifting while moving and resting before the
  drag behave alike.
* Real data (`tandt_db.zip`'s COLMAP models, local only): COLMAP `.bin` and INRIA `cameras.json`
  give byte-identical sidecars on truck and drjohnson. A `transforms.json` written as
  `ns-process-data` writes it from truck's COLMAP model is undone automatically and gives the
  same sidecar byte for byte. A copy of truck moved into a normalized frame matches through its
  `dataparser_transforms.json` (fit 0.999); without that file the same cameras fit at most
  0.765 and are now rejected (the first threshold, 0.5, let them through).
* Virtual cameras against the real ones (`scripts/coverage_eval.py`,
  `benchmarks/coverage-infer-inria-2026-11-05.csv`): eyes orbiting the home focus at 0.35 and
  0.7 × the full distance in 26 directions, point depth maps of the splats that stay opaque.
  *Recall* = share of the pixels the real cameras' fade reveals that the virtual one reveals
  too; *holes* = pixels only the virtual one reveals, per real reveal. Strength 1, recall / holes:

  | Scene | Capture | Real fade reveals | Room | Object |
  |---|---|---|---|---|
  | room | walked through | 23 % of the pixels | **0.69 / 0.28** | 0.38 / 0.15 |
  | drjohnson | walked through | 32 % | **0.89 / 0.16** | 0.25 / 0.05 |
  | playroom | walked through | 36 % | **0.90 / 0.16** | 0.27 / 0.03 |
  | kitchen | orbit, table top | 11 % | 0.09 / 0.51 | **0.47 / 0.26** |
  | counter | orbit, table top | 7.7 % | 0.50 / 8.48 | **0.63 / 0.38** |
  | bonsai | orbit, table top | 6.5 % | 0.26 / 1.97 | **0.52 / 0.23** |
  | stump | orbit, outdoors | 2.3 % | 0.95 / 7.62 | **0.70 / 2.59** |
  | bicycle | orbit, outdoors | 11 % | 0.89 / 1.10 | **0.73 / 0.41** |
  | garden | orbit, outdoors | 23 % | 0.94 / 1.57 | 0.36 / 0.03 |
  | truck | around a truck | 26 % | 0.48 / 0.20 | 0.16 / 0.08 |

  The type matters as the owner expected: room is right for the walk-throughs (0.69-0.90 of the
  real reveals, wrong reveals 0.16-0.28 of them) and object for the orbits (fewer wrong reveals
  everywhere, and more recall on the table tops). Garden and truck sit between: room reveals
  far more of what the cameras would (0.94, 0.48), at 1.6 and 0.2 wrong reveals. Stump's ratios
  are large because its real fade reveals only 2.3 % of the pixels from these eyes. Strength 0.5
  gives the same picture (the CSV). The object ring's radii come from a sweep over 0.7-2.0 x
  the full distance and two elevation sets (research note, S27).
* Phone (S24U, APK `0.2.0-7b73af8-dirty`, scripted, `benchmarks/coverage-compute-s24u-2026-11-05.csv`):
  pack load + cameras + masks in 2.5–8.7 s (room 3.0 s for 1.16 M splats; bicycle 8.7 s for
  4.48 M). Camera-file sidecars match the desktop's in all but 0.0004–0.0012 % of the masks.
  The inferred sidecars differ in 0.0001-0.0022 % of the masks and score
  the same recall and holes to two decimals on all six scenes compared. The first phone build
  sampled the room positions on float distances and diverged from the desktop in 10-32 % of
  the masks (negative results log); the sampling now runs on integer voxel distances.

**Consequences**:
* Every scene can get backside info: exact from its cameras where they exist, a guess from its
  type where not. The guess is good for rooms walked through and weaker for orbited objects:
  the ring is placed from the home view's distance, which is off on captures whose home view
  is (truck: ADR-0012, 2.5 ring radii). Picking the wrong type costs a lot (counter as a room:
  8.5 wrong reveals per right one), so the app explains the types by how the scene was captured
  ("room: captured walking through it - object: captured going around it") and the fade
  defaults to showing only while moving.
* The default "while moving" changes nothing for still frames, screenshots or benchmarks. A
  wrong guess shows only mid-gesture.
* The sidecar still follows the pack: cleaning or re-packing invalidates it; the app recomputes
  it in seconds from the same choice.
* Sidecars are not bit-identical across platforms (float differences between the toolchains
  move a few splats across a depth-test tolerance), but the same input gives the same cameras on
  the desktop and the phone, and masks that agree to about 1 in 50,000.

**Alternatives considered** (measured in `../coverage-mask-research.md`, S27 section):
* Inferring each flat splat's observed side directly (faces the focus: 73–93 %; faces enclosed
  space: 49–94 %; patch majority votes: no gain): every fifth wall splat wrong.
* Camera positions from needle splats' axes, or from a flood fill of the free space: needles do
  not point at cameras; 3DGS walls leak.
* Splat size as distance to the cameras (owner's idea): the correlation is real on orbit
  captures (Spearman +0.47 to +0.69), absent on walk-throughs, and its scale constant could not
  be found without cameras. Kept as a lead.
* One observed direction per splat toward the focus: half of the real reveals in rooms, 2.5–7×
  too many in orbit scenes.
* Detecting the scene type automatically: not attempted; the owner preferred the choice.
