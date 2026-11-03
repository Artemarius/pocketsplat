# Public baseline — PocketSplat vs browser and native splat renderers on the same phone

Like-for-like cold measurements on the Galaxy S24 Ultra (Adreno 750, Android 16, Chrome 156): every
renderer draws **the same splats** (one cleaned, SH-0-truncated cloud per scene, `scripts/
prepare_bench_scenes.py`), and PocketSplat is run both at its protocol resolution and at the exact
canvas size the web viewers choose. Scenes are public (`data/manifest.json`; the INRIA models are
under a non-commercial license and are never redistributed — only measurements are published).
Protocol: `README.md` § "Protocol additions for competitor baselines".

## Update 2026-10-31 (S23) — our cold table re-measured on battery with the triangle draw

Ours only (APK `0.1-ee64fe0`: S16 sort, one triangle per splat per ADR-0010, phone defaults
16-bit keys + 0.1 px cull), uncapped bench orbit, 45 s per run from skin ≤ 38.5 °C, both phones on
battery over Wi-Fi adb at the same time, Do Not Disturb and heads-up notifications off, two rounds
(`readme-cold-2026-10-31.csv`). fps = frames / time over the first 30 s, mean of the rounds:

| Scene | S24U 864×1872 | S25U 864×1872 | S24U 540×975 | S25U 540×975 |
|---|---|---|---|---|
| counter (0.97 M) | 135.0 (137.1 / 132.9) | 152.1 (157.4 / 146.8) | | |
| truck (1.65 M) | 168.2 (173.6 / 162.9) | 208.4 (176.1 / 240.7) | | |
| drjohnson (2.67 M) | 60.1 (60.5 / 59.7) | 72.7 (76.0 / 69.4) | 70.0 (70.9 / 69.0) | 76.8 (76.7 / 76.8) |
| bicycle-4m (4.00 M) | 79.2 (79.3 / 79.0) | 89.8 (92.0 / 87.5) | 83.1 (83.7 / 82.4) | 89.8 (89.2 / 90.4) |
| stump (4.05 M) | 92.3 (92.7 / 92.0) | 104.8 (106.1 / 103.6) | 95.4 (94.4 / 96.4) | 105.9 (106.2 / 105.6) |

* Against the S16 column (`2002c7e`, same cold protocol, 6-vertex quads): S24U drjohnson / bicycle / stump
  +5 / +5 / +4 % at 864×1872, the S25U +15 / +7 / +9 %, in line with ADR-0010's +6–10 % fps on
  heavy scenes.
* Truck runs at 160–240 fps with GPU utilisation 0.84–0.91 in some runs: pacing, not the GPU,
  sets its rate, so its rounds differ by up to 37 %.
* The "cool" start criterion (status 0, skin ≤ 38.5 °C) passes about a minute after a run ends,
  while the phone body is still warm: the S25U's second round ran 2–9 % slower on the
  864×1872 scenes, the S24U's within 3 %.
* SuperSplat was not re-run; the README compares against its S16 figures.

## Update 2026-10-28 (S18) — SuperSplat at our camera pose: the S24U stump gap was the framing; the S25U keeps a draw-side gap

S17 left one gap unexplained: on stump, ours used 38 % (S24U) and 2.1× (S25U) more energy per frame
than SuperSplat at the same frame rate. SuperSplat had been orbiting its own default view all along.
That view uses an 85° FOV, sits at a distance computed from the full bounding box (300.7 on stump),
and is pitched −19.5°. Our bench orbit is 60°, 1.2 × the 5th–95th-percentile diagonal (170.7), pitch
+17.2°. `bench_web.py --pose ours` now puts SuperSplat's orbit on our pose: same target, distance,
pitch, start yaw, per-frame step and vertical FOV, mapped into the viewer's frame (it turns PLYs
180° about z). The start pose was checked against `pocketsplat-desktop --offscreen` on stump and
truck: the images align (`--hold-shot`). At 540×975 SuperSplat's default view of stump lights 7 % of
the canvas, and our pose lights 46 %.

**Cold, S24U, USB, 540×975, forced 120 Hz, build `fba401f`** (interleaved per scene, each run from
skin ≤ 36 °C; `pose-ab-s24u-fba401f.csv`, `web-s24u-supersplat-*-pose.csv`,
`frame-s24u-fba401f-*-540x975-*`). SuperSplat is paced by Chrome, so its frame rate alone hides the
work. The comparable number is GPU cycles per frame, kgsl busy × clock ÷ fps, averaged from 10 s
on. For ours this estimate matches GPU timestamps × clock within 1 % (stump 9.42 vs 9.56 M). For
SuperSplat it includes Chrome's compositing, which is part of what the page costs.

| Scene | Renderer, camera | fps | GPU busy | GPU clock | GPU Mcycles/frame |
|---|---|---|---|---|---|
| stump (4.05 M) | SuperSplat, its default view | 61.3 | 74 % | 532 MHz | 6.42 |
| | SuperSplat, **our pose** | 53.6 | 80 % | 677 MHz | **10.05** |
| | ours (uncapped) | 88.5 | 99 % | 842 MHz | **9.42** |
| truck (1.65 M) | SuperSplat, its default view | 63.3 | 70 % | 299 MHz | 3.28 |
| | SuperSplat, **our pose** | 58.7 | 74 % | 462 MHz | **5.85** |
| | ours (uncapped) | 177.0 | 88 % | 839 MHz | **4.19** |

* **At our pose, SuperSplat's frame costs 57 % (stump) and 78 % (truck) more GPU cycles than at its
  own view.** This accounts for the S17 stump result: ours +38 % energy per frame against a renderer
  whose frames were roughly a third cheaper because of its camera.
* **At the same pose, ours does less GPU work per frame**: −6 % on stump, −28 % on truck.
* SuperSplat cannot hold its paced rate at our pose on stump (53.6 fps, p95 32 ms, against 61.3 at
  its own view), so the camera moves its frame rate, not only its cost.

**Equal work at our pose, 10 min on battery (S24U, stump, 540×975, Wi-Fi adb).** SuperSplat ran
first at our pose. Ours was then capped at the rate SuperSplat delivered from 60 s on (37 fps).
APK built at `4bcadb9`; the per-run files carry `f0f242c`, which changed only docs and scripts
(`energy-s24u-f0f242c-2026-10-28.csv`, `web-s24u-supersplat-orbit-ourpose-touch-stump-full.sh0-equal.csv`,
`frame-s24u-f0f242c-stump-full.sh0-16bit-c0.1-540x975-cap37-*`):

| Renderer | fps cold → min 8–10 | GPU busy | GPU clock | GPU Mcycles/frame | skin max | status | power | energy/frame | charge |
|---|---|---|---|---|---|---|---|---|---|
| SuperSplat, our pose | 53.8 → 35.3 | 85 % | 439 MHz | 10.16 | 38.6 °C | 0 | 3.67 W | 99.3 mJ | 138 mAh |
| ours, cap 37 | 37.0 → 37.0 | 79 % | 469 MHz | 9.96 | 38.4 °C | 0 | 3.60 W | **97.2 mJ** | 133 mAh |

* **At the same pose and frame rate the two are at parity on the S24U** (−2 % energy per frame,
  −2 % GPU cycles). With S17's equal-work rows at SuperSplat's own view, this attributes the stump
  gap to the framing.
* SuperSplat's rate fell from 53.8 to 35.3 fps over the ten minutes at status 0, with the GPU at
  85 % busy and 439 MHz. Ours at 60 fps on the same phone ran the GPU at 664 MHz (ADR-0009 rung
  table), so the clock was not capped by temperature. Why the governor settled lower for the page
  was not investigated.

**S25U, cold, USB, 540×975, build `f0f242c`** (same protocol; `pose-ab-s25u-f0f242c.csv`,
`web-s25u-supersplat-*-pose.csv`, `frame-s25u-f0f242c-*-540x975-*`):

| Scene | Renderer, camera | fps | GPU busy | GPU clock | GPU Mcycles/frame |
|---|---|---|---|---|---|
| stump | SuperSplat, its default view | 70.4 | 73 % | 514 MHz | 5.31 |
| | SuperSplat, **our pose** | 69.9 | 78 % | 758 MHz | **8.41** |
| | ours (uncapped) | 94.3 | 99 % | 1103 MHz | **11.59** |
| truck | SuperSplat, its default view | 71.2 | 60 % | 342 MHz | 2.88 |
| | SuperSplat, **our pose** | 69.7 | 78 % | 403 MHz | **4.52** |
| | ours (uncapped) | 160.5 | 90 % | 841 MHz | **4.73** |

* The framing effect repeats on the 830: SuperSplat's frame at our pose costs +58 % (stump) and
  +57 % (truck) cycles.
* **Pose-matched, the 830 still leaves a gap on stump**: ours 11.6 M cycles per frame against
  8.4 M (+38 %; truck +5 %). Ours ran uncapped at the top clock, where memory stalls cost more
  cycles, so an equal-rate run would narrow this somewhat. The extra cost is in the draw. With
  the same splats and pixels, the preprocess and sort take similar cycles on both GPUs (830:
  2.96 / 2.55 M; 750: 3.29 / 2.19 M), while the draw takes 5.94 M on the 830 against 3.54 M on the
  750, half of our 830 frame. On the S24U the draw proved primitive-bound rather than fill-bound
  (ADR-0009: 51 % fewer pixels, 3 % less draw time). We emit 6 vertices per splat, so per-splat
  vertex work is the lead. *Followed up the same day (ADR-0010):* the draw is bound by triangles,
  not vertices. One triangle per splat cuts it by 12–19 % on both GPUs and is now the default.
  *Located 2026-10-31 (S23, draw probes in `README.md`):* the 830 spends 2.07× the 750's cycles
  on the work each visible triangle causes (rasterization, fragment launch, the per-bin vertex
  re-run, blending: 1.9×). Its vertex work over all triangles and its fragment arithmetic cost
  the same or less. *Checked 2026-11-01 (S24): the remaining gap is not culling either.* At
  540×975, SuperSplat's measure at 1 px (raw opacity × dilated area, evaluated in our preprocess
  as the exploration variant `ss1`) keeps 1.21 M stump splats along our orbit
  (`quality-rtx3060-explore-540x975.csv`), against the 1.18 M our 0.1 px setting keeps at that size.
  In the pose-matched runs SuperSplat therefore draws as many splats as we do, or 2 % more, and
  its frame is still cheaper on the 830: with one triangle per splat, ours costs +11 % GPU cycles
  (9.52 vs 8.56 M) and +9 % energy per frame (72.1 vs 66.3 mJ, `equal-work-pose-2026-10-28.csv`).
  The difference is in the cost per drawn splat. The record gather is not it either (copy probe,
  `README.md`).

**Equal work at our pose, both phones, 10 min on battery, 540×975 (Wi-Fi adb, Do Not Disturb on for
the later runs).** For each scene and phone, SuperSplat ran first at our pose. Ours was then capped at
the rate it delivered from 60 s on, first with the 6-vertex quads, then again with one triangle per
splat (ADR-0010). Summary: `equal-work-pose-2026-10-28.csv`. APKs: `4bcadb9` for the quad rows, `9a7c38f`
(triangle mode selectable, same quads otherwise) for the triangle rows; archived files carry the APK's hash.

| Phone | Scene | Renderer | fps cold → min 8–10 | GPU Mcycles/frame | skin max (status) | power | energy/frame |
|---|---|---|---|---|---|---|---|
| S24U | stump | SuperSplat | 53.8 → 35.3 | 10.16 | 38.6 °C (0) | 3.67 W | 99.3 mJ |
| | | ours, quads, cap 37 | 37.0 → 37.0 | 9.96 | 38.4 °C (0) | 3.60 W | 97.2 mJ |
| | | ours, **triangle**, cap 37 | 37.0 → 37.0 | **9.19** | 37.0 °C (0) | 2.84 W | (76.8 mJ, see below) |
| S24U | truck | SuperSplat | 55.9 → 58.3 | 6.00 | 37.2 °C (0) | 2.91 W | 50.9 mJ |
| | | ours, quads, cap 57 | 57.0 → 57.0 | 4.10 | 35.9 °C (0) | 2.34 W | 41.1 mJ |
| | | ours, **triangle**, cap 57 | 57.0 → 57.0 | **3.89** | 35.9 °C (0) | 2.21 W | **38.8 mJ** |
| S25U | stump | SuperSplat | 69.1 → 50.6 | 8.56 | 38.8 °C (0) | 4.14 W | 66.3 mJ |
| | | ours, quads, cap 63 | 63.0 → **58.4** | 10.68 | 44.6 °C (**2**) | 5.29 W | 85.6 mJ |
| | | ours, **triangle**, cap 63 | 63.0 → **63.0** | **9.52** | 42.8 °C (**2**) | 4.54 W | **72.1 mJ** |
| S25U | truck | SuperSplat | 68.2 → 68.9 | 5.02 | 39.4 °C (0) | 3.11 W | 46.0 mJ |
| | | ours, quads, cap 68 | 68.0 → 68.0 | 4.63 | 37.3 °C (0) | 2.54 W | 37.4 mJ |
| | | ours, **triangle**, cap 68 | 68.0 → 68.0 | **4.38** | 36.8 °C (0) | 2.11 W | **31.1 mJ** |

* **Truck: ours is cheaper on both phones**, by 19 % (S24U) and 19 % (S25U) energy per frame with
  quads, and 24 % / 32 % with one triangle per splat.
* **Stump: parity on the S24U, a narrower gap on the S25U.** Ours with quads was +29 % energy
  per frame on the 830 and could not hold its cap (58.4 fps at minute 8–10, status 2). With one
  triangle per splat it holds 63 fps for the full ten minutes at +9 % energy and +11 % cycles, still
  at status 2 while SuperSplat stays at 0. The remaining gap is GPU work per frame. SuperSplat
  also dropped 69 → 51 fps over the run, so its frames came at a falling rate.
* The S24U stump triangle row ran four hours after its quad row. The charge counter drew the same
  133 mAh for both, so only the 8 % cycle saving is claimed there, not the mean-power difference.
  Within one session the gauge's ~10 s refresh is the only noise source.

## Update 2026-10-28 (S17) — equal frame rate on battery: power, heat, and where the gap is

The S16 battery runs compared an uncapped PocketSplat with a Chrome-paced SuperSplat. Here ours is
frame-capped (`bench_android.py --target-fps`, build `1707c96`) at the rate SuperSplat delivered on
that phone and scene in S16. Both renderers draw the same splats on a 540×975 canvas for 10 minutes
on battery, over Wi-Fi adb, with fixed brightness and skin ≤ 36 °C at the start. Both drivers now
sample kgsl GPU busy % and the fuel gauge (`dumpsys battery` current × voltage, plus the charge
counter). Power is the mean from 60 s to the end; energy per frame = power ÷ frames per second over
the same window. The phone's whole draw is included (display, CPU, Chrome or our app), so compare
rows of one phone only. CSVs: `equal-work-2026-10-28.csv` (summary,
`scripts/summarize_equal_work.py`), `frame-*-1707c96-*-cap*`, `thermal-*-1707c96-*-cap*`,
`web-*-equal.csv`.

| Phone | Scene | Renderer | fps cold → min 8–10 | GPU busy | skin max | status max | power | energy/frame | charge drawn |
|---|---|---|---|---|---|---|---|---|---|
| S24U | stump (4.05 M) | ours, cap 53 | 52.9 → 53.0 | 82 % | 40.6 °C | 1 | 3.86 W | 72.9 mJ | 168 mAh |
| | | SuperSplat | 54.3 → 53.2 | 77 % | 38.7 °C | 0 | 2.85 W | 53.0 mJ | 133 mAh |
| S24U | truck (1.65 M) | ours, cap 64 | 64.0 → 64.0 | 82 % | 36.6 °C | 0 | 2.29 W | **35.8 mJ** | 99 mAh |
| | | SuperSplat | 63.2 → 64.7 | 70 % | 36.8 °C | 0 | 2.36 W | 36.8 mJ | 114 mAh |
| S25U | stump | ours, cap 66 | 66.0 → 66.0 | 85 % | 42.8 °C | **2** | 5.71 W | 86.6 mJ | 217 mAh |
| | | SuperSplat | 63.6 → 70.2 | 74 % | 38.7 °C | 0 | 2.86 W | 40.8 mJ | 118 mAh |
| S25U | truck | ours, cap 69 | 69.0 → 69.0 | 81 % | 36.8 °C | 0 | 2.38 W | 34.4 mJ | 94 mAh |
| | | SuperSplat | 70.1 → 70.5 | 64 % | 36.7 °C | 0 | 2.19 W | 31.2 mJ | 89 mAh |

* **Medium scene: parity.** On truck at equal frame rate, ours uses 3 % less energy per frame than
  SuperSplat on the S24U and 10 % more on the S25U. Neither phone leaves thermal status 0.
* **Heavy scene: ours costs more per frame**, 38 % more on the S24U and 2.1× on the S25U. Only ours
  heats the phone: the S25U reaches status 2 while capped at 66 fps. SuperSplat delivered 70.2 fps
  there in this session (65.8 in S16), so our capped run even drew fewer frames.
* **The gap is not explained by culling.** SuperSplat's own measure at 1 px keeps *more* splats on
  stump than our 0.1 px setting (1.48 M vs 1.19 M at our orbit, `quality-rtx3060-b05913e-864x1872.csv`).
  What differs is the view: SuperSplat orbits its own default framing (distance 300.7, pitch
  −19.5° on stump), and every pixel-side cost (fill, overdraw) follows the framing. From the page we
  cannot see what its GPU spends; a pose-matched SuperSplat run is the next step to attribute the gap.
  *Done in S18 (above): at our pose SuperSplat's frame costs 57 % more GPU cycles on stump, and
  ours costs 6 % less than SuperSplat's.*
* **GPU busy % at a cap measures the governor, not the work.** At a fixed frame rate the governor
  holds kgsl busy near 75–85 % and lowers the clock instead. Ours on S24U stump at 53 fps (60 s
  runs over USB, `cap-lever-s24u-1707c96.csv`):

  | contribution cull | splats sorted | GPU clock | kgsl busy | GPU ms/frame |
  |---|---|---|---|---|
  | 0.1 px (phone default) | 1.19 M | 595 MHz | 79 % | 15.6 |
  | 0.2 px | 0.87 M | 500 MHz | 78 % | 15.5 |
  | 0.5 px | 0.51 M | 310 MHz | 76 % | 15.1 |
  | 1.0 px | 0.31 M | 231 MHz (floor) | 68 % | 13.6 |

  The bench scripts now log the kgsl clock next to busy %. The threshold is the power lever a
  thermal controller can pull: halving the sorted splats halves the clock at the same frame rate.
  The image cost is measured (43.9 dB at 0.1 px, 38.1 dB at 0.2 px against exact on stump at
  864×1872), so the controller can price each step.

## Update 2026-10-28 (S16) — sort ~1.8× faster per key, SuperSplat re-measured

Build `2002c7e` (sort rework, `docs/sorting.md` §7), cold, median fps over the first 30 s, SH 0, the
Android default (16-bit keys, 0.1 px contribution cull); same scenes, splats and orbit as the
`86b7789` rows below, so visible counts are identical and only the sort changed (S24U stump: sort
4.70 → 2.51 ms, preprocess and draw unchanged). CSVs: `frame-{s24u,s25u}-2002c7e-*`,
`web-{s24u,s25u}-*-orbit-touch-*`.

| Scene | S24U 864×1872 `86b7789` → **`2002c7e`** | S25U 864×1872 | S24U GPU ms | S25U GPU ms |
|---|---|---|---|---|
| drjohnson (2.67 M) | 49.1 → **57.2** | 53.6 → **63.3** | 19.8 → 16.8 | 17.6 → 15.0 |
| bicycle (budget 4 M) | 64.2 → **74.9** | 71.1 → **83.9** | 14.5 → 12.4 | 13.8 → 12.0 |
| stump (4.05 M) | 75.4 → **89.4** | 83.2 → **96.0** | 12.8 → 10.6 | 12.1 → 10.5 |

At 540×975, against the SuperSplat viewer re-measured with the fixed harness (below):

| Scene | S24U ours | S24U SuperSplat WebGPU | S25U ours | S25U SuperSplat WebGPU |
|---|---|---|---|---|
| drjohnson | 55.1 → **65.0** | 52.0 | 58.0 → 67.3 | **≥ 69.2** (GPU 79 % busy) |
| bicycle (budget 4 M) | 67.1 → **77.9** | 58.5 | 72.6 → **86.0** | ≥ 70.5 (76 %) |
| stump | 78.6 → **88.7** | 60.5 | 84.6 → **98.4** | ≥ 70.8 (73 %) |

* **Correction — every SuperSplat number before this update counted engine ticks, not frames.** The
  harness counted the engine's `frameend`, which fires on every tick whether or not the on-demand
  viewer rendered (desktop self-test: 137 "fps", 57 rendered). They are upper bounds; the S14/S15
  SuperSplat cells below are kept as recorded but should be read that way. The fixed harness counts
  `postrender` and orbits SuperSplat's own camera at 2π/600 per rendered frame from inside the page
  (its default framing, so poses still differ from ours), with touch drags for pacing only.
* **Chrome's pacing still caps the viewer on the S25U**: 69–71 fps at a 71.7 Hz page rate with the GPU
  73–79 % busy — the viewer has headroom we cannot reach from a browser page there (Chrome paces an
  untouched page at 60 Hz and a touched one at ~72 Hz even with 120 Hz forced). Those cells are lower
  bounds. On the S24U the viewer renders 52–61 fps at a ~68 Hz page rate with a 16.7–16.9 ms median
  frame interval; GPU busy was not yet logged there (the battery runs below log it).
* With that caveat: at equal pixels PocketSplat is ahead of SuperSplat on all three heavy scenes on
  the S24U (+25–47 %), and on stump and bicycle on the S25U; S25U drjohnson (67.3 vs ≥ 69.2) is
  SuperSplat's.
* **Against splatkit (pose-matched, S25U, GPU ms at 864×1872)**: ours 15.0 / 12.0 / 10.5 ms vs its
  25.8 / 23.0 / 16.5 on drjohnson / bicycle / stump. Its sort is now ~1.6× cheaper per key than ours
  (≈ 1.4 vs 2.2 ns at 16 bits on the 830), down from 2.7×.

### Sustained 10 min on battery (S16, both phones)

Wi-Fi adb, charger off, each run started at skin ≤ 36 °C, 540×975 for both renderers, fixed
brightness. Ours: the `2002c7e` APK (16-bit keys, 0.1 px cull), uncapped (MAILBOX). SuperSplat
WebGPU: in-page orbit + pacing drags, 120 Hz forced. Median fps over the first 30 s and over
minutes 8–10. (Ours' CSV names carry the repo HEAD at run time — `c8d7178`, `8599af6`; the app
was the `2002c7e` build throughout.) CSVs: `frame-{s24u,s25u}-{c8d7178,8599af6}-*-540x975-*`,
`thermal-*`, `web-*-orbit-touch-*-battery.csv`.

| Phone | Scene | Ours cold → min 8–10 | Ours at the end | SuperSplat cold → min 8–10 | SuperSplat at the end |
|---|---|---|---|---|---|
| S24U | stump (4.05 M) | 96.1 → **66.6** (−31 %) | status 2, skin ≤ 42.9 °C | 52.0 → 53.2 | status 0, GPU 78 % busy |
| S24U | truck (1.65 M) | 174.2 → **156.4** (−10 %) | status 2, skin ≤ 43.1 °C | 65.1 → 63.3 | status 0, GPU 68 % |
| S25U | stump | 99.3 → **68.5** (−31 %) | status 2, skin ≤ 43.4 °C | 67.7 → 65.8 | status 0, GPU 78 % |
| S25U | truck | 237.1 → **153.6** (−35 %) | status 2, skin ≤ 44.1 °C | 69.8 → 68.2 | status 0, GPU 63 % |

* **The two renderers do not do equal work, so this is not yet an equal-heat comparison.** Ours
  renders uncapped and drives both phones to thermal status 2 within 1.5–5 min; the SuperSplat page
  is held at Chrome's ~68 Hz page rate, its GPU never above ~78 % busy, and the phones stay at
  status 0. Even so, ours delivers more frames at minutes 8–10 on every row (stump +25 % on the
  S24U, +4 % on the S25U; truck 2.2–2.5×). The equal-work comparison — ours capped at the rate
  SuperSplat delivers, comparing GPU busy, skin temperature and drain — is the Phase 4 controller's
  job (no frame cap in the bench yet).
* Battery drain per run (1 % steps, including the cool-down wait before each run): S24U 92 → 86 %
  (ours stump), → 83 (SuperSplat), → 78 (ours truck), → 76 (SuperSplat); S25U 100 → 96 → 93 → 88
  → 86. Too coarse for energy per frame; that needs `BatteryManager` current sampling (Phase 6).
* **SuperSplat's frame gate** (`supersplat-nogate`, framesInFlight 0, cold stump): S24U 53.7 →
  68.9 fps with the gate off (the gate costs it ~22 % on the 750), S25U 65.2 → 66.9. Gate off it
  renders every tick at the page rate with the GPU still 76–78 % busy: from a page, its renderer's
  capacity is not observable on either phone — every SuperSplat cell is a lower bound
  (`web-*-stump-full.sh0-gatecheck.csv`).

## Update 2026-10-28 — ADR-0008 on both phones, S25U baseline, splatkit pose-matched

Build `86b7789` (renderer code = `b05913e`), cold, median fps over the first 30 s, SH 0, same scenes
and splats as below. "New default" = the Android viewer's ADR-0008 setting: 16-bit log-depth keys +
contribution cull at 0.1 px of the 864×1872 reference. "32-bit" = exact keys, no cull (the S14 setting).
Every row of a phone was run in the same session with the controls interleaved. CSVs:
`frame-{s24u,s25u}-86b7789-*`, `web-s25u-*`, `splatkit-s25u-orbit.csv`.

At 864×1872 (the app's protocol resolution):

| Scene | Splats | S24U 32-bit | S24U 16-bit only | **S24U new default** | S25U 32-bit | S25U 16-bit only | **S25U new default** |
|---|---|---|---|---|---|---|---|
| museum1 | 0.31 M | — | — | — | 148.7 | 162.9 | **171.7** |
| counter | 0.97 M | 74.2 | — | **111.4** | 80.7 | 97.4 | **123.1** |
| truck | 1.65 M | 84.6 | — | **168.2** | 87.3 | 111.1 | **172.3** |
| drjohnson | 2.67 M | 30.3 | 40.4 | **49.1** | 31.1 | 43.0 | **53.6** |
| bicycle (budget 4 M) | 4.00 M | 29.1 | 38.9 | **64.2** | 30.6 | 40.5 | **71.1** |
| stump | 4.05 M | 33.8 | 43.4 | **75.4** | 33.2 | 45.1 | **83.2** |
| bicycle (full) | 4.48 M | refused (750 range) | | | 28.8 | — | **65.0** |

At 540×975 (the SuperSplat viewer's canvas), against the web viewers:

| Scene | S24U new default | S24U SuperSplat WebGPU (S14) | S25U 32-bit | **S25U new default** | S25U SuperSplat WebGPU | S25U Spark |
|---|---|---|---|---|---|---|
| museum1 | — | ≥ 72.2 | 171.5 | **180.3** | ≥ 72.2 | ≥ 60.1 |
| counter | — | ≥ 70.6 | 91.9 | **134.2** | ≥ 72.2 | ≥ 60.1 |
| truck | — | ≥ 72.2 | 115.5 | **167.5** | ≥ 72.2 | 54.8 |
| drjohnson | 55.1 | **65.0** | 35.8 | 58.0 | ≥ 72.2 | 36.9 |
| bicycle (budget 4 M) | 67.1 | **70.0** | 39.8 | 72.6 | ≥ 72.2 | 33.5 |
| stump | **78.6** | 69.0 | 43.1 | **84.6** | ≥ 72.2 | 28.8 |

* **The new default doubles the heavy scenes on both phones** (stump 2.2× / 2.5×, bicycle 2.2× / 2.3×,
  drjohnson 1.6× / 1.7× at 864×1872) and lifts the light ones 1.15–2.0×. It beats the ADR-0008
  prediction (stump 53–65 fps predicted, 75.4 measured): the cull also shrinks preprocess writes and
  draw (S24U stump: preprocess 5.6 → 3.9 ms, draw 6.7 → 4.3 ms, sort 17.1 → 4.9 ms).
* **Against the SuperSplat viewer at equal pixels on the S24U**: ahead on stump (78.6 vs 69.0),
  within 4 % on bicycle (67.1 vs 70.0), 15 % behind on drjohnson (55.1 vs 65.0), the scene where the
  cull removes least (−23 % of keys) and draw dominates.
* **The S25U's SuperSplat runs are pacing-bound, not measurements**: every scene, WebGPU and WebGL2
  alike, sits at 72.0–72.4 fps — the on-demand viewer under the harness's synthetic drags (see the
  S14 pacing diagnostics). Spark still paces at 60 Hz on the light scenes with 120 Hz forced. A camera
  driver that keeps the viewer rendering continuously is needed before the S25U web rows say more.
* **The S25U is no faster than the S24U at 32 bits** (stump 33.2 vs 33.8) but gains more from the new
  default (83.2 vs 75.4): its sort per key is cheaper (7.0–7.6 / 3.8 ns at 32 / 16 bits, `sortbench`
  on the 830, vs 8.3 / 4.4 on the 750) and the draw share grows.
* bicycle-full (4.48 M) renders on the S25U (2 GB storage-buffer range); the S24U refuses it.
* Render on demand verified on both phones: GPU busy 0 % over 30 s with the viewer idle, 78–98 %
  while dragging (kgsl `gpu_busy_percentage`, 1–2 s samples).

### splatkit, pose-matched (S25U)

splatkit's dev app (`3e800ff` + a local patch adding intent extras for its existing orbit camera;
patch kept outside this repo) orbits the same circle as our bench camera: same target, 1.2 × diagonal
radius, yaw 0.6 / pitch 0.3, 60° vertical FOV, far plane 1000. Both use RUB world coordinates and the
same orbit formula; start-pose screenshots of the two apps frame identical views. splatkit is
vsync-paced (FIFO, 120 Hz cap), so GPU time per frame is the comparable figure:

| Scene | splatkit 864×1872 | **ours 864×1872** | splatkit 540×1170 | ours 540×975 | splatkit sort / drawn at 864×1872 |
|---|---|---|---|---|---|
| museum1 | 6.9 ms | **5.0 ms** | 6.6 ms | **4.4 ms** | 0.6 ms / 0.31 M |
| counter | 10.5 ms | **8.1 ms** | 7.8 ms | **7.4 ms** | 0.8 ms / 0.67 M |
| truck | 11.1 ms | **5.3 ms** | 7.0 ms | **5.2 ms** | 0.9 ms / 0.70 M |
| drjohnson | 25.8 ms | **18.6 ms** | 18.7 ms | **17.2 ms** | 2.2 ms / 1.80 M |
| bicycle (budget 4 M) | 23.0 ms | **14.0 ms** | **12.6 ms** | 13.8 ms | 2.0 ms / 1.63 M |
| stump | 16.5 ms | **12.0 ms** | **8.4 ms** | 11.8 ms | 1.5 ms / 1.12 M |

* At the protocol resolution PocketSplat's GPU frame is 23–52 % shorter on every scene. At splatkit's
  half scale (20 % more pixels than our 540×975) splatkit wins stump and bicycle: its sub-pixel cull
  scales with resolution (stump drawn 1.12 → 0.63 M), while ours is resolution-independent by design
  (ADR-0008) and our frame barely shrinks with pixels (stump 12.0 → 11.8 ms).
* **Its sort is ~2.7× cheaper per key**: 1.5 ms for ~1.1 M keys on the 830 (≈1.4 ns/key, its 16-bit
  relative keys) against our 3.8 ns/key at 16 bits (`sortbench` on the 830; the in-app "sort" segment
  on this GPU also carries the preprocess, see `docs/sorting.md` §6). The largest remaining lever.
* GPU-time methods differ (their timer vs our per-pass timestamps); treat differences under ~10 % as
  ties.

## Results — Galaxy S24 Ultra, cold (first 30 s), SH 0, 2026-10-27

Median fps; PocketSplat cells add the median GPU frame time. Build `c3e5299` (CSVs under
`s24u-c3e5299/`, `web-s24u-c3e5299/`). Splat counts are after cleaning (ADR-0007 defaults).

| Scene | Splats | Ours 864×1872 | Ours 864×1872, 16-bit keys | **Ours 540×975** | **SuperSplat WebGPU** | SuperSplat WebGL2 | SuperSplat sort-free (1080×1950) | Spark 2.4 |
|---|---|---|---|---|---|---|---|---|
| museum1 | 0.31 M | 127.3 (7.8 ms) | — | **163.1** (5.4 ms) | ≥ 72.2 | ≥ 72.2 | ≥ 71.8 | ≥ 60.1 |
| counter | 0.97 M | 70.1 (14.2 ms) | 89.6 (11.1 ms) | **86.3** (11.1 ms) | ≥ 70.6 | ≥ 72.2 | — | ≥ 60.1 |
| truck | 1.65 M | 78.0 (12.5 ms) | 104.7 (9.4 ms) | **110.1** (8.8 ms) | ≥ 72.2 | ≥ 72.4 | ≥ 70.8 | ≥ 60.1 |
| drjohnson | 2.67 M | 28.4 (35.0 ms) | — | **33.5** (29.9 ms) | **65.0** | ≥ 71.6 | — | ≥ 59.6 |
| bicycle (budget 4 M) | 4.00 M | 28.4 (35.1 ms) | 39.2 (25.9 ms) | **38.1** (26.1 ms) | **70.0** | ≥ 72.0 | — | 36.4 |
| stump | 4.05 M | 31.2 (32.0 ms) | 44.5 (22.2 ms) | **39.7** (25.3 ms) | **69.0** | ≥ 71.9 | 68.9 | 31.7 |
| bicycle (full, cleaned) | 4.48 M | refused: > 4,194,304 splats (Adreno 750 `maxStorageBufferRange` / 32 B records) | | | | | | |

"≥" = frame-pacing-bound (see below), a lower bound on the renderer, not a measurement of it. The
SuperSplat viewer reported all splats as rendered (`app.stats.frame.gsplats` = 4,000,000 on
bicycle-4m) — it did not decimate the scene.

### Pacing diagnostics (same session)

| Scene | SuperSplat WebGPU, frame gate off | Spark, with the same touch drags | Ours 864×1872, 16-bit keys |
|---|---|---|---|
| museum1 | 72.2 | **116.8** | — |
| truck | 71.4 | **116.7** | 104.7 |
| drjohnson | 56.5 | 63.6 | 37.6 (26.5 ms) |
| stump | 71.1 | — | 44.5 |
| bicycle (budget 4 M) | 71.2 | — | 39.2 |

* Turning the SuperSplat viewer's frame gate off (`framesInFlight: 0`) does not lift its ~72 fps:
  the gate is not the limit. The viewer renders **on demand** — only when the camera changes — and in
  this harness the camera is driven by synthetic touch drags, so its frame rate tracks the input
  cadence. Its light-scene numbers are lower bounds set by the harness, not GPU limits; only where
  it falls below that (drjohnson, 56–65 fps) is it GPU-bound.
* Chrome paces a page at 60 Hz unless it is being touched: the same Spark runs with the drags
  (which its fixture camera ignores) reach the 120 Hz panel rate on museum1 and truck, and 63.6 fps
  on drjohnson. Spark's plain runs in the main table are therefore 60 Hz-capped lower bounds.
* Net: **on light and medium scenes every renderer here reaches the panel rate or close to it at
  540×975** — PocketSplat by GPU throughput (110–163 fps uncapped), Spark at the 120 Hz cap with an
  asynchronous sort; **on heavy scenes PocketSplat is the slowest of the three** (34–40 fps vs Spark
  ≈ 64 and SuperSplat 56–70) because it sorts every visible splat with 32-bit keys inside the frame.


### What the table says

* **Light and medium scenes (≤ 1.7 M): PocketSplat is GPU-bound at 86–163 fps on the same canvas**
  and still 70–127 fps at three times the pixels; the web viewers sit at their pacing limits here
  (the diagnostics below show Spark reaching the 120 Hz panel rate once Chrome is touch-boosted).
* **Heavy scenes (2.7–4 M): the SuperSplat viewer is about 2× faster than PocketSplat at equal pixels**
  (65–70 vs 33–40 fps) while drawing every splat. Reading its MIT engine source explains it: its projection pass drops
  every splat whose integrated screen contribution — opacity × 2π√det Σ₂D, i.e. pixels of full
  coverage — is below 1 (`minContribution = 1`) before anything is sorted, it sorts keys of
  ≈ log2(N/4) bits (~20 at 2–4 M) instead of 32, and its default camera frames the scene from
  farther away (less fill, more culling). PocketSplat sorts every visible splat with 32-bit keys:
  the sort is 15–20 ms of a 25–35 ms frame there (2.0–2.3 M keys × ~9 ns, `docs/sorting.md` §6).
  16-bit keys alone take stump from 31.2 to 44.5 fps. Contribution culling plus 16-bit keys is now
  the phone default ([ADR-0008](../decisions/ADR-0008-contribution-culling-and-16-bit-keys-on-the-phone.md)):
  23–45 % fewer keys on the medium and heavy scenes at ≥ 43.5 dB PSNR against exact rendering
  (desktop A/B); measured on both phones in the 2026-10-28 update above.
* **Against Spark — a WebGL2 renderer with an asynchronous CPU sort — PocketSplat is level or
  ahead on heavy scenes** (stump 39.7 vs 31.7, bicycle-4m 38.1 vs 36.4 at the same canvas).
* **SuperSplat WebGL2 matches or beats its own WebGPU path on heavy scenes**: its WebGL2 path sorts
  on a CPU worker, so the frame rate no longer includes the sort.

### splatkit — measured, not comparable yet (superseded by the pose-matched runs above)

splatkit's dev app (release build of `3e800ff`, render scale 0.8 = 864×1872 and 0.5) was run on the
same splats (SPZ). Its built-in benchmark turns the camera a full circle *in place at the world
origin*, which suits walk-in worlds but not these captures: it drew **0 of 314 K** splats on museum1
(the camera sits inside the statue) and at most 0.2–0.75 M on the other scenes during the turn
(counter ≤ 202 K of 974 K, truck ≤ 226 K of 1.65 M, drjohnson ≤ 751 K of 2.67 M, stump ≤ 327 K of
4.05 M, bicycle-4m ≤ 330 K of 4.0 M) — against 0.78–2.3 M visible in PocketSplat's orbit. Its
108–120 fps (vsync-capped, FIFO; GPU 5–8 ms) therefore measures mostly empty frames, and is not
tabulated. One signal worth following
up: it reports 0.4–0.9 ms of sort for those 120–190 K drawn splats, which points at its default
"relative 16-bit" keys sorting faster per key than ours. A pose-matched comparison needs an orbit
camera in its dev app (next session).



## Renderers

| Renderer | Version | API | Sort | Canvas on the S24U |
|---|---|---|---|---|
| **PocketSplat** | this repo (build in CSV names) | Vulkan 1.3, native | GPU radix every changed frame, 32- or 16-bit keys | 864×1872 protocol; 540×975 like-for-like |
| SuperSplat viewer | 1.41.0 (PlayCanvas engine 2.23.3) | WebGPU (default) or WebGL2 (`?webgl`) | GPU radix (WebGPU compute) | 540×975 (half the device pixel ratio) |
| SuperSplat viewer, `?stochastic=mobile` | 1.41.0 | WebGPU | none (sort-free, stochastic transparency) | 1080×1950 |
| Spark | 2.4.0 (three.js 0.180) | WebGL2 | CPU sort in a worker, asynchronous to rendering | pinned to 540×974 |
| splatkit | `3e800ff` (dev app, release build) | Vulkan, native | GPU radix | 864×1872 (render scale 0.8) and 540×1170 (0.5) |

## Reading the numbers

* **Frame pacing caps light scenes, not the GPU.** The SuperSplat viewer renders on demand, so under
  synthetic drags it settles near 72 fps on both WebGPU and WebGL2 regardless of load (its frame
  gate is not the cause — see the diagnostics). Chrome paces untouched pages at 60 Hz (flat 16.6 ms
  intervals). Such rows are marked "≥" — they bound the renderer from below, they do not measure it.
  Our app presents with MAILBOX and reports GPU-bound throughput.
* **Asynchronous sorting is cheaper per frame but not equivalent.** Spark renders every frame with the
  last finished sort from a CPU worker, so its frame rate does not include the sort (order lags motion);
  PocketSplat and SuperSplat-WebGPU sort on the GPU inside the frame that uses the order.
* **Cameras differ.** Ours and Spark orbit identically; SuperSplat uses its own default framing under
  synthetic drags; splatkit turns in place. Visible-splat counts therefore differ between renderers.
* **SPZ quantization** for splatkit (24-bit fixed-point positions with 12 fractional bits, 8-bit
  log-scales, colors and opacity) — the same splats up to SPZ precision.

## Published numbers on the same chip (Snapdragon 8 Gen 3 / Adreno 750) — for context only

Different scene versions, resolutions and methods; not like-for-like.

| Source | Setup | Scene (splats) | Result |
|---|---|---|---|
| Qualcomm, *Sort-free GS via Weighted Sum Rendering*, arXiv 2410.18931, Tab. 2 | Vulkan, 1920×1080; their 3DGS graphics baseline / their sort-free method | counter / truck / bicycle (their training, avg 3.98 M) | 42.3 / 107.6 / 678.1 ms baseline; 34.1 / 59.8 / 79.0 ms sort-free |
| Han et al., *WebSplatter*, arXiv 2602.03207, Tab. 1/3 | WebGPU in Chrome, Redmi K70 Pro, resolution unstated | bicycle-c (1.06 M) / truck (2.54 M) | 33.6 / 52.7 ms per frame |
| Du et al., *Mobile-GS*, ICLR 2026, Tab. 12 | Vulkan, 1600×1063, Mip-NeRF 360 average | retrained 0.47 M; plain 3DGS | 127 cold / 74 steady fps; 3DGS 8 / 3 fps |
