# ADR-0013 — Coverage fade: splats seen from unobserved directions drawn more transparent, by a strength the viewer sets

**Status**: accepted (2026-10-31, S23). Proposed in S22 with two items open: the owner's hand-held
judgment ("the fading works great, I like it a lot", S25U, end of S22) and the timing on the S24U
(Adreno 750, the driver with the known shader traps), measured in S23 (Evidence). The default
strength stays 0 (owner's call, S23).

**Context**: the owner's idea (S21). Floaters and stretched "smear" splats only look right from
where the capture cameras saw them. From elsewhere they cross the view as streaks, and they draw
attention away from the content that was observed from that side.

The S21–S22 prototype (`../coverage-mask-research.md`) recorded the observed directions per
splat and faded the rest out completely. From outside the camera ring every fading policy cut a
hole into the foreground. The streak and the hole are the same population: content outside the
ring, observed from one or two inward directions. No per-splat rule separates them.

The owner's direction (S22): keep every splat. Make the ones seen from unobserved directions
more transparent, so that the observed content dominates, by an amount the viewer controls.
Strength 0 renders as before. Distance may come in later.

**Decision**:

1. **Mask per splat**: a 5 × 5 octahedral grid over the directions splat → camera, 25 cells of
   ~40°, one `uint32` per splat. A camera observes a splat when the splat's centre projects into
   its image and lies in front of its coarse depth buffer (`splatcore::compute_coverage`).
   Computed offline from a 3DGS `cameras.json` by `splatconv coverage` (stump: 4.05 M splats ×
   125 cameras in 1.1 s on 12 threads).
2. **Sidecar file, not a pack field**: `<scene stem>.pscov` next to the scene (`../formats.md`,
   coverage sidecar). It is bound to the pack by the splat count and an FNV-1a hash of the packed
   positions, and a mismatched file is ignored with a log line. Packs stay at version 2 and every
   existing pack keeps working. Most scenes have no cameras, and they never pay the 4 B/splat
   (+14 % of an SH0 pack).
3. **Fade in the preprocess**: `α' = α · (1 − r · (1 − match))`.
   * `match` is the best `smoothstep(cos 90°, cos 35°, dot(v, centre(c)))` over the observed
     cells `c`, with `v` the direction splat → eye. It is 1 within 35° of an observed cell centre
     (about the farthest a direction inside a cell gets from its centre) and 0 at 90° or more.
   * Cell centres are computed arithmetically, with no lookup tables (CLAUDE.md, the 750 trap).
   * The loop runs once per set bit: one or two on most splats (55 % of stump's splats were seen
     from one cell, 13 % from two).
   * Faded splats then meet the contribution cull with their reduced opacity, so they also cost
     less.
4. **One strength knob `s` ∈ [0, 1]**, with `r = 1 − (1 − s)²`.
   * Floaters are stacks of near-opaque splats. With `r = s`, the stump test view changed by
     only 29 % of its full change at 0.5 and by 78 % at 0.9. With the square it changes by
     24 / 54 / 86 % at 0.25 / 0.5 / 0.75: the slider acts evenly.
   * `s = 0` skips the fade (a uniform branch, no mask read). Images then match the unmasked
     renderer within the run-to-run noise (1 LSB on 13 of 518,400 pixels; two identical
     unmasked runs differ on 20).
   * Default 0. The app shows a slider for scenes that have a sidecar. The desktop has
     `--coverage-strength` and a HUD slider. Scripts use the `coverage_strength` intent extra
     and `bench_android.py --coverage-strength`.

**Evidence**:
* `splatcore_tests` `Coverage.*` (7 tests): cell round trip for all 25 cells; the match is 1 at
  an observed centre, 0 opposite and monotone in between; cameras.json parsing and RDF → RUB;
  occlusion and frustum on a two-camera scene, identical for 1, 2 and 4 threads; sidecar round
  trip and rejection on another scene, count, size or magic. ctest 102/102; desktop validation
  clean.
* Image (desktop, stump, S21's streak view 3.5 ring radii outside the cameras): at 0.5 the streak
  turns half-transparent and the unobserved foreground ground thins out. At 0.75 the streak is
  nearly gone. At 1 the old prototype's hole appears.
* Image (S25U, stump home view): unchanged at any strength. The home view sits where the
  cameras were.
* Cost (S25U, stump 864 × 1872, 16-bit, c 0.1, uncapped bench orbit around the whole scene, 45 s
  per leg, interleaved, two rounds; `benchmarks/coverage-cost-s25u-2026-10-31.csv`):

  | Strength | Preprocess | Frame | Visible splats |
  |---|---|---|---|
  | 0 | 3.04 / 2.85 M GPU cycles | 11.1 / 10.5 M cycles | 1.20 M |
  | 0.5 | 2.48 / 2.26 M cycles | 7.45 / 6.91 M cycles (−33 %) | 0.86 M |

  The mask loop costs less than the records it saves. The bench orbit is a far-outside view;
  near the capture, few splats fade and the saving goes with them.
* Cost and correctness on the S24U (S23, same protocol, APK `0.1-ee64fe0`, the fade code of
  `e5389a9`; `benchmarks/coverage-cost-s24u-2026-10-31.csv`):

  | Strength | Preprocess | Frame | Visible splats |
  |---|---|---|---|
  | 0 | 3.33 / 3.32 M GPU cycles | 9.22 / 9.11 M cycles | 1.20 M |
  | 0.5 | 3.08 / 2.97 M cycles | 7.37 / 7.16 M cycles (−20 / −21 %) | 0.86 M |

  The dynamic loop over set bits runs at full speed on the 750 (no trap: the preprocess still
  gets cheaper). It saves less of the frame than on the 830 because the 750's draw costs fewer
  cycles per splat (strength 0, round 1: 3.64 ms × 903 MHz = 3.3 M cycles against the S25U's
  4.29 ms × 1200 MHz = 5.1 M), so dropping splats saves less there. The per-frame visible counts of the
  S24U runs equal those of the S25U runs at both strengths on every frame from the third on
  (the bench orbit is a function of the frame index): the 750 fades the same splats.

**Consequences**:
* Scenes need their capture cameras. INRIA-style `cameras.json` works now. COLMAP
  `images.bin`/`cameras.bin` and other exporters' formats are not read yet.
* The mask follows splat order. Re-packing or cleaning a scene invalidates its sidecar, and the
  positions hash rejects it.
* At strength 1, the fade reproduces the prototype's hole. The slider's top end is "show only what
  was seen from here".
* The loop over set bits is a dynamic loop on per-splat data, the kind of shader code the
  Adreno 750 has miscompiled or slowed before (CLAUDE.md). It measured clean there (S23); any
  change to it is timed on the S24U again.

**Open**:
* Distance: the owner's next idea, nothing designed yet. A cue might come from the distance
  between the eye and the cameras that observed a cell, kept per cell or per splat.
* Default strength: the owner kept 0 after using it hand-held (S23); the slider sets it per
  session.
* Whether a 6 × 6 grid (36 cells: two words, or a 64-bit mask without 64-bit atomics) would
  sharpen anything. The 35° full-match angle follows from the 5 × 5 cell size.

**Alternatives considered**:
* Fading out completely, with the hole policies A–D of the research note: holes or ghosting,
  measured S22.
* An optional section in the pack (pack v3): a format change for a feature most scenes cannot
  use. The sidecar keeps the pack untouched and can still move into one later.
* `r = s` (linear): measured uneven, see decision 4.
* Learned view-dependent opacity (VDGS 2312.13729, VoD-3DGS 2501.17978): needs training, which
  this repo does not do.
