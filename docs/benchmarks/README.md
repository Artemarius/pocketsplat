# Benchmarks

Every measurement lives here as a CSV plus a short note. Nothing is quoted from memory.

## Files
- `devices.md` — capability table per device (Vulkan version, subgroup size, max shared memory, timestamp
  period, thermal API availability). Filled in Phase 0.
- `phase0-webgpu-s24u.csv` — third-party WebGPU viewer baseline on the S24U (fps vs splats, fps vs time).
- `sort-<device>-<hash>.csv` — sort timings by N and key width (Phase 2+).
- `frame-<device>-<hash>.csv` — per-pass GPU time per scene/resolution/SH degree, cold and after 10 min.
  Android runs (`scripts/bench_android.py`) split this into `…-cold.csv` (per-frame rows, first
  600 frames) and `…-5s.csv` (5-second aggregate over the full run, the CSV columns below);
  `thermal-<device>-<hash>-<scene>.csv` carries the host-sampled skin/AP temperatures alongside.
- `cleaning-<scene>.md` — cleaning reports from `scripts/clean_all.py`.
- `phase3-android.md` — on-device frame-benchmark summary (S24U/S25U), updated as protocol runs land.
- `public-baseline.md` — like-for-like comparison on public scenes: PocketSplat vs the SuperSplat
  viewer (WebGPU / WebGL2 / sort-free), Spark and splatkit on the same phone, splats and canvas.
- `web-<device>-<viewer>-<scene>.csv` — per-second web-viewer rows from `scripts/bench_web.py`
  (`#` header: viewer version, browser, backend actually requested, canvas size, load time).
- `side-by-side-s24u-f89eb4d-2026-11-03.csv` — the README's side-by-side clip
  (`docs/media/side-by-side-s24u.webp`). It holds per-second fps of PocketSplat and the SuperSplat
  viewer on the same start-view swing (`--pose home` in both drivers), recorded and unrecorded,
  with GPU cycles per frame in the header.
  * **Display stretch:** with `--width/--height`, the app's 540×975 buffer is scaled to the full
    1080×2340 screen, so the phone shows it about 20 % taller than it is. This is display-only: the
    GPU renders the same pixels. The clip undoes the stretch.
  * **Recording changes Chrome's pacing:** SuperSplat ran faster while screen-recorded, at 69.9 vs
    57.4 fps (page rAF 71 vs 67 Hz, GPU < 80 % busy). That is Chrome's pacing, not its GPU.
  * **Interference:** a first set of these runs (14:55–15:05) overlapped another project's app
    install and camera recording on the same phone and was discarded. The rerun's event log was
    clean, and its numbers matched within 7 %.
- `splatkit-s25u-orbit.csv` — splatkit's dev app on the S25U, pose-matched to our bench orbit (GPU ms,
  sort/cull ms, drawn splats per scene and render scale); see `public-baseline.md`.
- `idle-gpubusy-<hash>.csv` — GPU busy % with the Android viewer idle vs dragging (on-demand frames).
- `sortbench-s16-2026-10-28.csv`, `sortbench-s17-2026-10-28.csv` — standalone sort timings per GPU
  and downsweep variant (count rows, ranking, scatter), with stage profiles (`docs/sorting.md` §7–8).
- `ranking-ab-s24u-2026-10-28.csv` — in-app A/B of the downsweep ranking (two APKs, interleaved runs).
- `scatter-ab-s25u-2026-10-28.csv` — in-app A/B of the downsweep scatter on the S25U (same method).
- `equal-work-2026-10-28.csv` — frame-capped PocketSplat vs SuperSplat on battery: fps, GPU busy,
  skin, thermal status, battery power, energy per frame (`scripts/summarize_equal_work.py`); the
  per-run files are `frame-*-cap*`, `thermal-*-cap*` (host samples incl. GPU busy / clock and the
  fuel gauge) and `web-*-equal.csv`.
- `cap-lever-s24u-<hash>.csv` — contribution-cull threshold vs GPU clock at a fixed frame rate.
- `pose-ab-<device>-<hash>.csv` — SuperSplat at its default view vs at our bench pose vs ours, cold:
  fps, GPU busy, clock and GPU cycles per frame (`summarize_equal_work.py --all --from 10`); per-run
  files `web-*-pose.csv` and `frame-s24u-<hash>-*-540x975-*`. Since S18 our per-frame CSVs carry
  `gpu_mhz`, the kgsl clock read by the app itself at 1 Hz (−1 = unreadable).
- `energy-s24u-<hash>-<date>.csv` — S18 battery session: equal work at our pose (SuperSplat vs
  ours capped at its rate, 10 min) and the controller's rungs (cull threshold × internal resolution
  at 60 fps, 4 min each): fps, GPU busy, clock, cycles per frame, skin, status, power, energy per
  frame, charge. Per-run files `frame-s24u-<hash>-*-cap37-*`, `*-cap60-5s.csv`, `thermal-*-cap60.csv`.
  The S24U fuel gauge refreshes `current now` only about every 10 s: power on short runs is
  ±10–15 %, while cycles per frame are precise.
- `equal-work-pose-2026-10-28.csv` — equal work at our bench pose on both phones, 10 min on
  battery: SuperSplat vs ours (quads and one triangle per splat) capped at its rate; per-run files
  `web-*-ourpose-*-equal.csv`, `frame-*-540x975-cap*`, `thermal-*-540x975-cap*`.
- `draw-ab-<device>-<hash>.csv` — in-app A/B of how the draw emits a splat (6-vertex quads, one
  triangle, indexed quads, instanced strips; ADR-0010), cold, interleaved, per-pass ms and the
  app's GPU clock. Bench files without a draw-mode suffix are 6-vertex quads (all runs before S18).
  `draw-ab-s25u-ef86526-allscenes.csv`: the same A/B overnight on all six public bench scenes at
  both canvas sizes (triangle vs quads; the phone was charging and warm, so only the comparison counts).
- `readme-cold-<date>.csv` — the README's cold table, both phones on battery at the same time
  (Wi-Fi adb), the current default renderer, two rounds per scene and size (`public-baseline.md`,
  S23 update).
- `draw-probe-<device>-<date>.csv` — timing-only variants of the triangle draw (`DrawProbe`, wrong
  image by design: desktop `--draw-probe`, `bench_android.py --draw-probe`) that split its GPU
  cycles into vertex work, rasterization and fragment launch, shading and blend (S23, section
  "Draw probes" below). Cold, interleaved, two rounds, stump 864×1872.
- `controller-ab-<hash>-<date>.csv` — Phase 4 gate A/B (ADR-0009): fixed rung 0 (c 0.1) vs the
  quality controller, stump 864×1872 capped at 60 fps, on battery, both phones
  (`summarize_controller.py` rows plus each leg's start skin and first status 1 / 2 times).
  `20min-equal-start` = every leg from skin ≤ 33 °C held for 3 min, windows 5–10 and 10–20 min;
  `10min-unequal-start` = the first run, from a single ≤ 36 °C reading (legs started up to 6 °C
  apart: kept as the protocol's negative result). Per-leg files `frame-*-1d3faec-*-cap60-triangle-5s/-cold.csv`
  and `thermal-*-1d3faec-*-cap60-triangle.csv` are the equal-start legs.
- `fps-sweep-s25u-<hash>-<date>.csv` — the last rung (c 0.5, stump 864×1872) at 60 fps without
  and with the frame-rate vote (panel 120 / 60 Hz) and at 48 and 30 fps voted, 20 min per leg from
  equal cool starts on battery (`summarize_controller.py` rows). Current × voltage power 2.47 /
  2.39 / 1.95 / 1.59 W: the panel rate is inside gauge noise, the frame rate is a lever (−18 % at
  48, −33 % at 30), and no leg left thermal status 0. The first leg started at 100 %, where the
  S25U's charge counter stays pinned for ~10 min (CLAUDE.md), so energy comes from the current.
- `handheld-s24u-<date>.csv` — hand-held sessions (`scripts/session_android.py`, 20 min, stump):
  share of time interacting, fps and late intervals while interacting, GPU ms and clock, time at
  each threshold, the controller's steps, thermal maxima, and whether `splatconv replay`
  reproduces the trace and what ADR-0011's rules would decide on it. The traces themselves
  (`trace-*.csv`, interactive-trace format above) stay in the run folders.
- `controller-s25u-<hash>-<date>.csv` — the S20 controller leg's setup on the S25U under ADR-0011
  (`summarize_controller.py` row).
- `controller-stepup-s25u-<hash>-<date>.csv` — a warm-start controller leg (S22), the run ADR-0011
  asked for to see its step-up gate and backoff act on a device: the fixed c 0.1 bench uncapped
  until skin ≥ 41 °C (197 s from 26.5 °C), then at once stump 864×1872 at 60 fps with the vote,
  30 min on battery (`summarize_controller.py` row; per-leg `frame-*-a745524-*-ctl-*-vote-triangle-5s/-cold.csv`,
  `thermal-*`). Five decisions, replayed exactly: down at 607 s, up at 906 s, down at 1061 s, up
  at 1370 s, down at 1517 s, all between c 0.1 and c 0.2; 60 fps throughout, status ≤ 1.
  Replayed with forecast-only step ups (ADR-0009), the first step up comes 18 s earlier (887.8 s)
  with the current headroom at 0.773, 0.023 above the band: decision 3 acted as designed. Both
  step downs after a step up (155 s, 147 s) fall inside the 600 s backoff window but not the old
  120 s one, so decision 4 doubled the dwell; the doubled dwell (120 s) never bound, because the
  current-headroom gate held each step up for about 300 s. The cycle (about 7.7 min) is not slow
  heating against the gap between the thresholds, as first read: every decision sat on a step of
  the S25U's skin reading (S23). Each step down followed a +1.4–1.7 °C jump within 20–25 s as the
  reading rose through 38.7 °C, with the AP temperature falling 0.8–1.3 °C meanwhile; each step up
  followed the matching −1.2 °C fall about 260 s later, which left the current headroom at
  0.750 / 0.747, on the band edge. Between steps the reading moved ≤ 0.2 °C/min. No change
  (negative results log). Started at 100 % charge: energy is not this run's evidence.
- `coverage-cost-s25u-<date>.csv` — cost of the coverage fade (ADR-0013) on the S25U: stump
  864×1872, 16-bit, c 0.1, the uncapped bench orbit (a far-outside view), strength 0 vs 0.5, 45 s
  legs interleaved over two rounds, per-run medians of seconds 5–45 and GPU cycles (ms × the
  app's kgsl clock; the phone was warm from the step-up leg, status 1–2). Strength 0.5: preprocess
  −18 / −21 % cycles, frame −33 / −34 %, visible splats −28 % (faded splats fall under the
  contribution cull). APK `0.1-fd0883a-dirty` = the code of `e5389a9` (the commit that added the fade) before clang-format.
- `coverage-cost-s24u-<date>.csv` — the same on the S24U (S23, APK `0.1-ee64fe0`, phone from cool, status
  ≤ 1, USB): strength 0.5 preprocess −8 / −11 % cycles, frame −20 / −21 %, visible −28 %; the
  750 runs the mask loop without the slowdowns its driver has shown on per-invocation arrays. The
  per-frame visible counts equal the S25U's at both strengths from the third frame on (frames 0–1
  read the counter before the first compute finished): both GPUs fade the same splats.
- `coverage-infer-inria-<date>.csv` — backside info without cameras (ADR-0015): virtual cameras
  for a room and for an orbited object against each scene's real training cameras, 10 INRIA bench
  packs, `scripts/coverage_eval.py` (eyes orbiting the home focus at 0.35 / 0.7 × the full
  distance, 26 directions; recall = share of the real fade's revealed pixels the virtual one
  reveals too, holes = pixels only the virtual one reveals, per real reveal; strength 0.5 and 1).
- `coverage-compute-s24u-<date>.csv` — computing the backside sidecar on the S24U (S27, scripted
  `open_scene` + `backside`, APK `0.2.0-7b73af8-dirty`): seconds from pack load to written file,
  per scene and source (COLMAP, `transforms.json`, `cameras.json`, room, object), and how many
  masks differ from the desktop's sidecar for the same input.
- `home-view-inria-<date>.csv` — the interactive start view (`splatcore::estimate_home_view`,
  ADR-0012) against each INRIA scene's training cameras: ring radius and elevation, the old
  whole-scene start in ring radii, home distance and focus error in ring radii, angle to the
  nearest training camera, error of the fitted up axis. The cameras were read from the pretrained
  models archive for this check only; they are not committed.
- `quality-rtx3060-rungs-<date>-864x1872.csv` — image cost of the thermal controller's candidate
  rungs (cull threshold × internal-resolution scale, ADR-0009); scaled variants are upscaled
  bilinearly to 864×1872 before PSNR (`quality_ab.py --scale`).
- `quality-<device>-<hash>-<w>x<h>.csv` — image-quality A/B of renderer variants from
  `scripts/quality_ab.py` (desktop host, offscreen at the phone's pixel count): PSNR against the
  exact reference at six fixed poses, share of pixels that moved > 2 LSB, and the visible-splat
  count over the bench orbit (device-independent, so the sort cost on a phone follows from
  `docs/sorting.md` §6).

## CSV columns (frame benchmarks)
`device, build_hash, scene, splats_total, splats_visible, sh_degree, internal_w, internal_h, key_bits,
t_min_s, fps, ms_frame, ms_cull_preprocess, ms_sort, ms_draw, thermal_status, thermal_headroom,
res_scale, importance_threshold`

`importance_threshold` carries the contribution-cull threshold in pixels of full coverage of the
864×1872 reference image, scaled to the run's resolution by the renderer (ADR-0008; empty in runs
that predate it, 0 = off). In controller runs it is the bucket's mean threshold.

The per-frame Android CSVs (`…-raw.csv`, `…-cold.csv`) carry, from S19 on, `min_contribution` (the
threshold the frame rendered with), `rung` and `reason` (the quality controller's rung and the
decision taken after the frame; rung −1 = fixed run), `thermal_forecast` (30 s headroom forecast)
and `thermal_sample` (which reading the frame took: the app alternates the forecast with the
current headroom, one call per second). A `# controller=…` header line records the ladder, the
LIGHT threshold and its source, the GPU clock maximum and the frame-guard rule. `splatconv replay`
runs the controller over any of these files (ADR-0009). File labels name the build that ran: the
APK's versionName carries its git hash (`-dirty` = built with uncommitted changes); before S19 they
named the checkout the driver ran from.

From S21 on, every row also carries `drawn` (1 = a rendered frame), and the headers record
`frame_rate_vote` (0 = the compositor's choice, which kept both panels at 120 Hz at every bench
cap; `bench_android.py --vote-fps` asks for the cap and the panel follows; files get `-vote`
after the cap), `backoff_window_s` and `step_up_current` (ADR-0011). **Interactive traces**
(`trace-<device>-<hash>-<scene>-<name>.csv`, `scripts/session_android.py`, intent extra
`interactive_trace`) use the same columns for interactive use: rendered frames plus the 1 Hz
thermal ticks of an idle viewer (`drawn` 0). There `cpu_ms` is the frame interval the controller
counted (`nan` after an idle gap), and `# scene=…` lines can appear between rows. `splatconv
replay` reproduces a trace like a bench run.

## Protocol (Android)
1. Phone at room temperature, screen brightness fixed, airplane mode, charger disconnected, 60/120 Hz
   fixed in developer options.
2. `scripts/bench_android.py --scenes manifest.json --minutes 10` runs each scene, samples every 5 s,
   pulls the CSV and `dumpsys thermalservice` log.
3. Report cold (first 30 s) and sustained (minute 8–10) separately. Never report only cold numbers.
4. A/B legs on one phone start from the same state: skin ≤ 33 °C on 6 readings 30 s apart before
   *every* leg. One reading at the threshold is not enough: the skin sensors on both phones fall
   several °C within a minute of stopping, and the warm body brings them back as soon as the
   load returns (S20: an S25U leg that started at 36.0 °C reached status 1 after 68 s).

## Protocol additions for competitor baselines (2026-10-27)
1. **Identical splats**: `scripts/prepare_bench_scenes.py` cleans once and writes the SH 0/1 PLY the
   web viewers load and the pack our app loads from the same cloud (splatkit gets the same splats as
   SPZ, which quantizes positions/scales — noted per row).
2. **Same pixels**: web viewers choose their own canvas (the SuperSplat viewer renders at half the
   device pixel ratio on phones, 540×975 on the S24U in FHD+); our app is run at that exact size with
   `bench_android.py --width/--height` as well as at the 864×1872 protocol size. Spark is pinned to
   the same canvas with `--pixel-ratio`.
3. **Refresh rate**: force 120 Hz for browser runs (Chrome paces pages at 60 Hz otherwise on Samsung
   adaptive panels); a run whose frame interval sits flat at a refresh period is reported as capped
   ("≥"), not as a measurement. Our app runs MAILBOX (uncapped).
4. **Camera**: our app and the Spark fixture orbit with the same camera (5th–95th percentile framing,
   1.2× diagonal, 0.3 rad pitch, 60° FOV, 2π/600 per frame); the SuperSplat viewer keeps its own
   default framing and, since 2026-10-28, orbits at the same 2π/600 per rendered frame about its own
   focus point, driven from inside the page (`bench_web.py --camera orbit`; until then synthetic
   one-finger drags, which paced its on-demand renderer at the input rate); splatkit turns in place
   (its built-in benchmark) or follows our orbit with the patched dev app. Visible-splat counts
   therefore differ between renderers; the counts we can read are recorded. Since S18
   `bench_web.py --pose ours` puts the SuperSplat orbit on our pose too (files labelled `-ourpose`);
   its default view costs ~35–45 % fewer GPU cycles per frame than ours, so cost comparisons use
   the matched pose.
5. **Frames counted and pacing** (2026-10-28): SuperSplat frames are counted on the engine's
   `postrender` (rows before counted every tick — upper bounds). Chrome paces an untouched page at
   60 Hz even at a forced 120 Hz and a touched one at ~72 Hz on the S25U, so the harness sends drags
   for pacing only (the orbit ignores them) and logs the page's rAF rate (`raf_fps`) and the Adreno
   busy percentage (`gpu_busy_pct`): a row with the GPU well below 100 % busy is paced, not GPU-bound,
   and is reported as a lower bound (≥).
6. **Cold start**: skin ≤ 38.5 °C before every run (USB-powered; the phone idles at ~37.5 °C with the
   screen held on), 45 s per run, first 30 s after load reported.

## Phase 0 baseline summary (S24 Ultra, 2026-09-01)

`phase0-webgpu-s24u.csv` — PlayCanvas model-viewer in Chrome 151 (**WebGL2**; the viewer does not
use WebGPU, recorded per row), scenes served over adb reverse, camera driven by synthetic touch
orbits, fps sampled via a requestAnimationFrame counter through the Chrome DevTools protocol,
thermal state from `dumpsys thermalservice`. Cold pass ran on USB power; the 10-min sustained run
on battery over Wi-Fi adb. Display in 120 Hz mode (fps above 60 are real).

* Cold: museum1 300 K ≈ **59.5 fps**.
* The 1 M / 2 M rows and the 10-min sustained run were recorded on a scene that cannot be
  redistributed and are withdrawn. The baseline was re-measured on public scenes (INRIA Mip-NeRF
  360 / Tanks and Temples / Deep Blending, `data/manifest.json`) against the current WebGPU and
  WebGL2 viewers from 2026-10-27 on: `public-baseline.md`.
* Not yet done from the protocol: independent thermal stress curve (3DMark), lowest-internal-
  resolution variant, S25 Ultra.

## Desktop sort baseline (RTX 3060, 2026-09-01, sortbench)

Reduce-then-scan radix sort, key/value u32, correctness + stability verified 1e5–4e6:
32-bit keys 1 M = **2.09 ms** (479 Mkeys/s), 4 M = 9.22 ms; 16-bit keys 1 M = **0.63 ms**
(1587 Mkeys/s) — desktop sanity gate (< 1.5 ms @ 1 M) met by the 16-bit mode, and the renderer
integration sorts every frame.

## Desktop frame pipeline (RTX 3060, 2026-09-01, `--bench` orbit @1600×900)

`frame-rtx3060-3549b49-*.csv` — per-frame per-pass GPU times from `pocketsplat-desktop --bench`
(deterministic full orbit, vsync off, 300 frames, first 10 dropped as warmup; the CSV metadata
header records scene/flags). Steady-state averages:

| Scene / view | Config | Frame | cull | preprocess | prepare+sort | draw | visible |
|---|---|---|---|---|---|---|---|
| museum1 312 K SH1 | cull on, 32-bit | **4.17 ms** | 0.01 | 0.55 | 0.71 | 2.92 | 305 K |

The 1 M-splat rows first recorded here came from a scene that cannot be redistributed and are
withdrawn. Re-run on public scenes on 2026-11-01 (S24, build `183a6cc`, triangle draw, 1600×900,
`frame-pipeline-rtx3060-183a6cc-2026-11-01.csv`): GPU ms per frame, chunk cull off → on.

| Scene | View | Keys | Frame | preprocess | sort | visible | chunks drawn |
|---|---|---|---|---|---|---|---|
| truck 1.65 M | full (dist 1) | 16 | 1.30 → 1.35 | 0.23 → 0.23 | 0.24 | 0.62 M | 6418 of 6461 |
| | full | 32 | 1.56 → 1.61 | 0.23 → 0.23 | 0.60 | 0.62 M | |
| | zoomed (0.45) | 16 | 2.21 → 2.26 | 0.26 → 0.25 | 0.34 | 0.84 → 0.83 M | 6110 |
| | inside (0.15) | 16 | 1.69 → 1.70 | 0.20 → 0.16 | 0.25 → 0.24 | 0.62 → 0.59 M | 3652 |
| | full, 0.1 px cull | 16 | 0.80 → 0.86 | 0.19 → 0.19 | 0.14 | 0.30 M | 6395 |
| stump 4.05 M | full (dist 1) | 16 | 2.87 → 3.00 | 0.58 → 0.58 | 0.63 | 1.69 M | 15803 of 15819 |
| | full | 32 | 3.96 → 4.08 | 0.58 → 0.58 | 1.70 | 1.69 M | |
| | zoomed (0.45) | 16 | 3.82 → 3.88 | 0.62 → 0.59 | 0.79 → 0.76 | 2.14 → 2.06 M | 14238 |
| | zoomed | 32 | 5.39 → 5.38 | 0.62 → 0.59 | 2.20 → 2.13 | 2.14 → 2.06 M | |
| | inside (0.15) | 16 | 2.29 → **2.20** | 0.42 → **0.29** | 0.50 → 0.43 | 1.28 → **1.10 M** | 6189 |
| | full, 0.1 px cull | 16 | 1.58 → 1.67 | 0.45 → 0.41 | 0.31 | 0.74 M | 13831 |

Chunk cull verdict (desktop, re-confirmed on public scenes): **a small cost at full view, a win
only from inside the scene.** With every chunk in view the pass adds its own 0.06 ms (truck) /
0.13 ms (stump), 2–6 % of the frame. From inside stump it culls 61 % of the chunks: the
preprocess falls 32 %, 14 % fewer records reach the sort and the draw (guard-band splats whose
triangles never touch the screen), and the frame falls 4 %. The culled threads were cheap
early-outs, so the saving is bounded by the preprocess's read-and-reject cost. The visible-chunk
count is a free scene-vs-view statistic. The pass stays on by default for the phones, where reads
are the bottleneck; its effect there has not been measured in isolation. 16-bit keys cut the sort
by 60–63 % on both scenes (ADR-0008).

Skip-compute-when-static: with an unchanged camera the whole compute chain is skipped and the
draw reuses the sorted buffers, so a static frame costs the draw only. Phones idle at a fixed
view most of the time, so this is the common case, not the exception.

Determinism / golden gate: two identical runs differ only by unordered blending of splats at
exactly equal float32 depth (museum1: ~5 % of splats sit in equal-depth groups at the default
view) — measured **~91 dB PSNR** run-to-run and cull on/off (`--no-hud`; with the HUD in frame
its timing text dominates any comparison). Golden-image ctest gate at ≥ 40 dB.

## Silhouette-streak investigation (2026-09-01, S10)

Directional hair-like streaks radiating from object silhouettes into the background (museum1,
zoomed in, S24U and desktop alike) were reported as absent in the WebGL2 baseline viewer.
Findings, in order:

1. **fp16 conic underflow (real bug, fixed)** — the draw record stored the conic at fp16;
   zoomed-in splats have screen-space sigma in the hundreds of pixels, so conic values fall
   below fp16's normal range (~6e-5). Adreno flushes fp16 subnormals to zero, collapsing one
   axis of the fragment quadratic form (tails drawn as bright rays); NVIDIA keeps subnormals, so
   the desktop only sharpened slightly. Conic is now fp32 in the same 32-byte record
   (the stored depth was never read). Desktop-neutral: 54.9 dB before/after zoomed, goldens pass.
2. **Gaussian tail cutoff (behavior change, kept)** — we followed the INRIA reference: 3-sigma
   quads with a 1/255 floor on the *final* alpha, so a high-opacity elongated splat keeps its
   sub-2% tail to the quad edge. Web viewers cut at sqrt(8)*sigma (e^-4 ~ 1.8% of peak). Adopted
   the sqrt(8)*sigma cutoff (fragment discard + matching quad extent, ~11% less quad area);
   trims the faintest outer wisps, goldens unchanged.
3. **Kernel normalization (behavior change, kept — the actual ray-killer)** — even with the
   sqrt(8)-sigma cutoff, an elongated splat's tail ran at up to ~1.8% of peak all the way to the
   support edge and ended in a step, so needles at silhouettes still read as rays crossing into
   the background. The reference viewer (PlayCanvas gsplat, MIT — read for the idea) evaluates a
   truncated Gaussian *renormalized to reach zero at the support edge*:
   `alpha = a * (exp(power) - e^-4) / (1 - e^-4)`. At 2.5-sigma that is ~40% dimmer than the raw
   Gaussian, so needle tips melt away instead of terminating visibly. Adopted; goldens unchanged.
   PlayCanvas also draws *oriented* quads with an elliptical discard — no visual difference vs
   our exact-conic square quads, but a large fill saving on needles (a square quad on a 100:1
   splat covers ~100x the pixels) — follow-up perf candidate for the Adreno tuning pass.
4. **Root cause of the remaining perceived difference: background contrast** — the
   remaining streaks are trained-in thin splats at the silhouette (they legitimately survive
   default cleaning: alpha >= 0.02, under the scale cap, well-connected for SOR). The WebGL2
   viewer draws them too, but its default light-gradient background hides white fuzz; our
   near-black clear color maximizes its contrast. Rendering the identical pack/view with
   `--bg 0.55,0.58,0.62` reads as clean as the baseline screenshots. A thin-splat
   (anisotropy) prune is a Phase 4 cleaning-op candidate if black-background viewing should
   stay the default.

**Known issue (accepted 2026-09-01):** a residue of the silhouette rays survives all four fixes —
at some view angles a few extreme splats still draw as faint straight rays crossing the border
(museum1; the reference WebGL2 viewer looks cleaner at the same spots). Tightening
`clamp_needle_scales` further starts to eat legitimate fur detail, so this is parked as a
data-quality limitation for now. Future work item (Phase 4/5 research pass): survey how other
3DGS pipelines handle boundary detail — e.g. filtering or fading splats whose major axis is
inconsistent with the local surface orientation at the silhouette (normal-aware outlier
detection), view-angle-dependent opacity, or training-side anisotropy regularizers.
**Update 2026-09-02 (S12): root cause found, structural fix landed (ADR-0006).** The research
pass ran — findings in `docs/silhouette-rays-research.md`. The residue was not a threshold
problem: fp32 cancellation makes an extreme needle's λmin numerical noise, the inverted conic
goes indefinite, and the fragment quadratic form paints the ridge line as a bright ray
(angle-dependent because the λmin sign flips with small rotations). Fix: the conic is gone —
eigenbasis oriented quads with interpolated σ-unit UV (the failure mode is unrepresentable),
λmin ≤ 0 reject, opacity-adaptive support, 2048 px per-axis cap. Verification status: goldens
pass; an automated before/after sweep over 360 poses (yaw × pitch −10/5/30° × dist 0.6/0.45/0.35)
is identical within 4 LSB — the fix harms nothing, and no canned pose reproduces the residual
ray (it never did; S11 noted interactive hunting is required). Side effect: needle
overdraw collapsed.
**Interactive spot-check result (same day): the bright spikes PERSISTED** on the raw uncleaned
SH-0 data, with dilation on/off, SH on/off, at 60° and 25° FOV — while an independent reference
renderer showed none on the same file. **RESOLVED later the same day by the direct pipeline
side-by-side: the EWA Jacobian's y row had the wrong sign** — the computed 2D covariance was the
y-mirror of the true one (b sign-flipped; eigenvalues unchanged, so every λ-level instrument and
every image-vs-self golden was structurally blind to it). Diagonal elongated splats drew across
their features instead of along them: the silhouette spikes AND the crispness gap were this one
bug. One-line shader fix (shared by both hosts); goldens regenerated (the old ones were images
of the bug); new always-on `golden_chiral_orientation` gate asserts projected-slope handedness
and fails on any future mirror. Verified against the reference at three matched poses: spikes
gone, detail parity. Full story: `docs/silhouette-rays-research.md` (two stacked root causes).
Known issue CLOSED. RTX 3060 600-frame bench orbit, before (8111957) → after (b701442) GPU ms:
museum1 far 3.61 → 2.28 (−37 %), museum1 near (`--dist-scale 0.45`) 6.09 → 3.10 (−49 %) — the
win tracks anisotropy (needle overdraw), so compact-splat scenes gain little.
CSVs: `frame-rtx3060-{8111957,b701442}-*.csv`. On the S24U, museum1 ran 127 fps at 864×1872 in the
S14 public matrix (`public-baseline.md`) against 57.5 in the September protocol run; that gain also
holds every renderer change in between, so this fix's own share on the device was not isolated.

## Contribution culling and 16-bit keys — desktop image A/B (2026-10-27, ADR-0008)

`scripts/quality_ab.py` on the RTX 3060, offscreen at the phone's pixel counts, phone bench camera,
six fixed poses per scene, PSNR (minimum over poses) against 32-bit keys without culling, visible
keys as the mean over the 600-frame bench orbit. Visible counts do not depend on the GPU, so the
phone's sort cost follows from `docs/sorting.md` §6; the desktop GPU columns are informational only.

* `quality-rtx3060-b05913e-864x1872.csv`, `…-540x975.csv`: final build. The shipped phone setting
  (16-bit keys + 0.1 px) scores **43.5–62.9 dB at 864×1872 and 45.9–60.6 dB at 540×975**, with
  23–45 % fewer sorted keys on the medium and heavy scenes at 864×1872 (museum1 −2 %). Tables:
  ADR-0008.
* `quality-rtx3060-b05913e-864x1872-phonebg.csv`: the same over the app's light-gray background,
  44.9–62.1 dB.
* `quality-rtx3060-explore-*.csv`: exploration build, run before `ee56d08` was final. Variant
  `ss1` (flag `--min-contribution -1`) was a temporary shader branch evaluating SuperSplat's measure
  (raw opacity × dilated area, 1 px). Its `k16` rows used the log-depth keys that `b05913e`
  later adopted. Thresholds there are *unscaled* pixels (the scaling came with `e3faa21`).
* `quality-rtx3060-keyquant-864x1872.csv`: 16-bit key quantization. `k16` = d/(d+s) (build
  `e3faa21`); `log16` = log depth over the frame's AABB range (the exploration build, same code as
  `b05913e`).
* Every reference run against itself scores 75–106 dB, so these differences are not noise.

## Draw probes: where the triangle draw's cycles go (2026-10-31, S23)

The S25U's stump draw cost more GPU cycles than the S24U's for the same splats and pixels (S18:
5.94 against 3.54 M cycles with quads; `public-baseline.md`), and that difference carried our gap
to SuperSplat on the 830. To find which part of the draw grows, the triangle draw got five
timing-only variants, selected by a specialization constant (`SplatRenderer::DrawProbe`):

* `seq`: records read in their compacted (Morton) order instead of through the sorted `order[]`:
  no gather, but also a different blend order.
* `tiny`: every triangle shrunk to ~1e-6 of its area, so no pixel centre is covered. Same
  vertices and primitives, no fragments.
* `clip`: every triangle moved outside the clip volume after its record is read: vertex work
  only.
* `noblend`: the full fragment shader with blending off.
* `nofrag`: every fragment discarded first, by a test the compiler cannot fold: rasterization and
  fragment launch without shading or blending.

Stump 864×1872, 16-bit keys, 0.1 px cull, uncapped bench orbit (~1.19 M visible splats), 45 s
cold runs, probes interleaved, two rounds per phone (`draw-probe-s24u-2026-10-31.csv`,
`draw-probe-s25u-2026-10-31.csv`). Draw cycles per frame = draw ms × the app's kgsl clock; every
probe repeated within 0.11 M cycles.

| M GPU cycles per frame | Adreno 750 (S24U) | Adreno 830 (S25U) | 830 / 750 |
|---|---|---|---|
| Draw | 3.30 | 4.72–4.80 | 1.44× |
| Vertex work (`clip`; `tiny` the same) | 1.58 | 1.29 | 0.82× |
| Work per visible triangle (draw − `tiny`) | 1.69 | 3.50 | **2.07×** |
| … rasterization, fragment launch, vertex re-run per bin (`nofrag` − `tiny`) | 0.30 | 2.01 | 6.7× |
| … fragment shading (`noblend` − `nofrag`) | 0.63 | ≈ 0 | — |
| … blend (draw − `noblend`) | 0.75 | 1.41 | 1.9× |
| Morton order instead of depth order (`seq` − draw) | +2.67 (+81 %) | +1.82 (+38 %) | |

* **The 830's extra cycles are all in the work each visible triangle causes.** Per visible
  triangle it spends twice the 750's cycles, while the vertex work for all triangles is cheaper.
  Most of the difference sits in the `nofrag` − `tiny` bucket: rasterization, fragment launch
  and the vertex re-run (see "What these probes can and cannot clear" below). On the 830 the
  shader's arithmetic costs nothing measurable, presumably because it overlaps that stage. On the
  750 shading is visible (0.63 M) and rasterization cheap. Blending costs the 830 1.9× the 750's
  cycles. In time the 830's higher clock (~1.1 vs ~0.88 GHz) hides part of it: the draw is
  4.2–4.5 ms against 3.8 ms.
* **What these probes can and cannot clear.** The fragment arithmetic and the vertex shader's
  first run (`clip`: every triangle, record gather included) cost the same or less on the 830.
  The bucket that doubles holds rasterization, fragment launch, blending and, on a binned GPU,
  the vertex shader's second run for each visible triangle in the rendering pass (once per bin
  it touches). The probes cannot separate that re-run from rasterization, so the gather is not
  cleared there. The lever we have for certain is the number of visible triangles, which the
  contribution cull and the quality controller already set.
* Follow-ups, both closed in S24 (2026-11-01):
  1. **The gather is not the gap: copy probe.** `copy` (`DrawProbe::kSortedCopy`, `2023540`) adds
     a compute pass after the sort that writes `sorted_records[j] = records[order[j]]`, and the
     draw reads the record at its own position. The blend order and the image stay those of the
     normal draw (RTX: 34 px differ by ≤ 2 levels, against 47 px between two normal runs). Same
     protocol as the probes, three interleaved rounds per phone (`copy-probe-s25u-2026-10-31.csv`,
     `copy-probe-s24u-2026-11-01.csv`):

     | M GPU cycles per frame | 750: normal → copy | 830: normal → copy |
     |---|---|---|
     | Draw | 3.30 → 3.29 | 4.79 → 4.47 (−7 %) |
     | Sort segment, copy included | 2.36 → 4.70 | 2.56 → 4.79 |
     | Frame | 9.29 → 11.63 (+25 %) | 10.60 → 12.53 (+18 %) |
     | fps | 95.1 → 76.5 | 108.6 → 90.7 |

     Reading the records in draw order saves nothing on the 750 and 0.32 M cycles on the 830:
     under a fifth of the 1.81 M by which the 830's work per visible triangle exceeds the 750's.
     The rest is rasterization, fragment launch and blending. The copy itself moves ~76 MB per
     frame (1.19 M records gathered and written) and costs 2.3 M cycles on both GPUs, seven times
     what it saves. The probe stays as an A/B option, off. On the RTX the draw is unchanged
     (1.176 → 1.173 ms) and the copy costs 0.38 ms.
  2. **Not culling either.** At the pose-matched 540×975 runs, SuperSplat's measure keeps as many
     stump splats as ours (1.21 vs 1.18 M along our orbit), and its frame is still 11 % cheaper
     in cycles on the 830 (`public-baseline.md`, S18 section). The difference is the cost per
     drawn splat.
* **Draw order matters on both GPUs.** Drawing the same splats in compacted Morton order costs
  +81 % (750) and +38 % (830) of the draw. The visual result differs too (wrong order). The
  timing says that blending consecutive, overlapping triangles on the same pixels is expensive,
  while the depth-sorted order scatters consecutive splats across the screen. A reorder that
  makes the draw spatially coherent (tile-local sorting, for example) would have to pay this
  first.
* The `seq` probe does not isolate the gather, because it changes the blend order too
  (follow-up 1 above). On the RTX it costs the same as the normal draw (1.51 against 1.52 ms).
* RTX 3060 for contrast (desktop bench orbit, same scene and size): draw 1.52 ms, `tiny`/`clip`
  0.36 ms, `noblend` 1.24 ms, `nofrag` 1.05 ms. There the fragments dominate.

## Negative results log
| Date | What was tried | Result | Diagnosis |
|---|---|---|---|
| 2026-11-05 | Backside info without cameras, per flat splat (S27): the observed side is the side facing the home view's focus, or facing enclosed free space; then majority votes over planar patches | Facing the focus 0.73–0.93 right per splat (rooms 0.73–0.84), enclosed space 0.49–0.94, patch votes 0.73–0.78; about half of all flat splats were seen from both sides anyway | One wall splat in five on the wrong side is a hole or a speck each. Replaced by virtual cameras fed to the real coverage pass (ADR-0015) |
| 2026-11-05 | Capture camera positions from the splats (S27): voting along needle splats' long axes; flood fill of the free space from the most open point near the focus | Long axes vs the direction to the nearest camera: median \|cos\| 0.27–0.51 (random 0.50); vote peaks 0.4–2.7 ring radii from the cameras. The fill reached 91–98 % of the grid in room, counter and drjohnson | Needles are not ray-aligned in general (only the streak population of S22 is); 3DGS walls have gaps everywhere at any useful grid size |
| 2026-11-05 | Splat size as the distance to the cameras (owner's idea, S27): a voxel's fine-splat size predicts its distance to the nearest camera, so votes "a camera is k × size away" locate them | Spearman size vs distance +0.47 to +0.69 on counter, kitchen, bonsai, bicycle, garden; −0.07 to +0.08 on room, drjohnson, playroom. With the true k the votes find room cameras within 0.15–0.42 ring radii; k from the home distance is 2–15× too large in rooms, k chosen by vote agreement 0.12–0.8× | The signal is real on orbits and in rooms given k, but k (training resolution × splat-per-pixel density) is not recoverable from the splats so far. Not used; noted as a lead in ADR-0015 |
| 2026-11-05 | First virtual room cameras (S27): farthest-point sampling over free points within the radius, no enclosure test; occupancy grid over the scene's 1–99 % box | Room recall 0.12 (room, drjohnson): half the cameras stood outside the walls and "saw" their backs. Stump: 1 position and 2 ring cameras survived | Farthest-point sampling seeks the edge of the candidate ball, which in a room lies outside it; stump's background makes the 1–99 % box 40 ring radii wide (voxels of 1.2 units). Fixed by the enclosure test and a focus-centred grid with a coarse whole-scene fallback |
| 2026-11-05 | Room virtual cameras sampled farthest-first on float distances (S27) | The S24U's and the desktop's sidecars for the same pack had the same camera count but differed in 10 % (room) and 32 % (drjohnson) of the masks | Float differences between the toolchains flip a few occupancy voxels or tie-breaks, and farthest-point sampling cascades one different pick into different positions. Sampling on integer voxel distances: 0.0001-0.0022 % differ, recall and holes equal to two decimals |
| 2026-11-05 | Camera-file fit threshold 0.5 (S27) | A normalized copy of truck with its `transforms.json` but without `dataparser_transforms.json` was accepted in its best wrong frame at 0.765, with 25 % of the scene "never observed" | Real captures fit 0.999–1.000 on all 13 INRIA scenes: threshold raised to 0.9 |
| 2026-11-01 | Copying the draw records into draw order after the sort (`DrawProbe::kSortedCopy`), so the triangle draw reads them sequentially instead of gathering through `order[]`: the one part of the S25U's per-visible-triangle cost the S23 probes could not separate from the binned vertex re-run | Stump 864×1872, three interleaved rounds per phone: draw 3.30 → 3.29 M cycles (750), 4.79 → 4.47 M (830, −7 %); the copy pass +2.3 M on both; frame +25 % (750) / +18 % (830), fps 95.1 → 76.5 / 108.6 → 90.7 | The gather costs the 830 0.32 M cycles per frame, under a fifth of its excess over the 750 per visible triangle; the copy moves ~76 MB per frame. Kept as an A/B probe, off. The 830's extra cost is rasterization, fragment launch and blending |
| 2026-10-31 | Reading S22's 7.7-minute controller cycle (S25U warm start, c 0.1 ↔ 0.2) as thermal hysteresis between the step-down forecast (≈ 0.82) and the step-up band (≤ 0.750), to be damped by widening the band after a quick step down | All five decisions sat on steps of the skin reading: +1.7 / +1.4 / +1.4 °C within 20–25 s as it rose through 38.7 °C (AP −1.2 / −1.3 / −0.8 °C over the same seconds), and −1.2 °C falls about 260 s later that left the current headroom at 0.750 / 0.747. Across all host thermal logs, 7 of 8 S25U runs that rose through 38.8 °C after their first minute jumped +1.4 to +2.5 °C within 25 s (AP −0.5 to +0.5 °C); 0 of 9 S24U runs did | The S25U's skin value switches level like a model, not like a sensor following heat. On the S25U the controller never sees 38.7–40.3 °C on the way up, so its step down (forecast ≥ 0.816) always lands at LIGHT, and its step ups wait for the fall back. A band 0.5 °C wider would have blocked both step ups and kept c 0.2, giving up image quality to remove a cycle that cost nothing measured (60 fps, status ≤ 1). No change: ADR-0011's thresholds stay, and no ADR |
| 2026-10-31 | Home-view up from the axial mode of flat-splat normals instead of the RANSAC plane near the focus (S22; flat = smallest scale < 0.25 × middle, opacity > 0.5, inside the 5–95 % box; 10° tolerance; 13 INRIA scenes against the training cameras' mean up) | Room: 86.5° off (RANSAC 29.8°). Bonsai: 89.9° (RANSAC 3.2°). Others within 1–8° when counting splats; weighting by splat area fails on 6 scenes | The idea was that floors, ceilings and table tops share one normal while walls split. In room and bonsai one wall or backdrop holds more flat splats than all horizontal surfaces together. RANSAC kept (ADR-0012) |
| 2026-10-31 | Coverage-mask hole policies (S22, stump, 125 cameras, 5 × 5 octahedral mask): fade only splats seen from ≤ 2 cells, an opacity floor of 0.3, softer angles 45°/90° | From outside the camera ring every fading policy cuts a hole into the foreground ground (the cell-count rule as much as fading all); the floor ghosts the streak at 30 %; the soft angles remove the streak with the smallest hole | The streak and the hole are one population: content outside the ring, seen by 10–20 cameras from one or two inward directions, with matching size, elongation and nearest-camera distance. No training-free per-splat property separates them. Options are with the owner (`../coverage-mask-research.md`) |
| 2026-10-31 | The interactive quality controller as shipped in S20 (`486cd37`): frames drawn one per vsync while the camera moves | S24U stump, c 0.1, 6 s scripted drag: 104 fps on the 120 Hz panel (106 of 126 intervals 8.3 ms). The controller's lever saves work per frame at a fixed rate, and its frame guard (60 fps target) can never see a miss there | The viewer was paced by the display, not by the controller's target. Interactive frames now vote 60 fps and skip vsyncs in between: the panel switches to 60 Hz, 125 of 125 intervals at 16.67 ms (ADR-0011 decision 1) |
| 2026-10-31 | ADR-0009's frame guard in hand-held use (S24U, stump, 20 min, 60 fps paced; `trace-s24u-8395179-…-handheld1.csv`) | Thermal status 0 all session (skin max 37.2 °C), yet 7 frame-guard steps down and 5 step ups: 4.3 / 13.6 / 2.1 min at c 0.1 / 0.2 / 0.5. Late frames 20.8 / 21.2 / 18.2 % at those rungs and 18–21 % in every clock bin from 231 to 799 MHz; GPU time 13.3 / 12.9 / 12.3 ms per frame | The governor turns a cheaper frame into a lower clock (468 / 409 / 312 MHz), so the ladder cannot reduce the misses, and "GPU time ≥ 90 % of the interval" is the governor's choice, not overload. A bad window now also needs the clock at ≥ 0.9 of its maximum: the trace replays with no decisions, and the bench's governor dips drop from 12 runs to 1 under a 1-of-1 rule (ADR-0011 decision 2) |
| 2026-10-31 | ADR-0009's frame-guard amendment: "4 of 6 → 0" of the recorded capped runs | Replay finds one: S24U stump 734×1591 (S18) steps at 53.1 s, right after the 2.4 s governor dip the amendment cites for 3 of 5. The S19 `splatconv` (`684f4fa`) agrees | The S19 count was wrong for this run; the replay did not change. With the ADR-0011 clock condition it does not step (500 MHz of 1000 in the dip) |
| 2026-10-31 | Stepping up on the 30 s forecast alone, with a 120 s backoff window (ADR-0009) | S25U A/B (S20): at 609.7 s the forecast extrapolated the skin sensor's last 0.3 °C steps to 0.747 (band ≤ 0.750) while the current headroom read 0.770, 1.9 °C below LIGHT. The step up to c 0.2 was undone at 852 s, outside the 120 s window, so the dwell did not grow | The forecast is the current value plus 30 s of a fitted slope, so it calls a phone cool that is still cooling. A rung change answers in a fast part (pole 80–160 s) and a slow drift that 20-minute legs cannot resolve. Step ups now also need the current reading in the band, and the backoff window is 600 s (ADR-0011) |
| 2026-10-31 | One first-order skin model per run (one time constant, one steady state per rung) to size the backoff window | Fits on the four S20 legs disagree: τ 340 / 290 s on the fixed legs, 120 s on the S24U controller leg (steady states 39.5 / 39.3 / 38.3 °C, barely apart), 1490 s (the grid edge) on the S25U one | Skin has at least two poles. A two-pole fit driven by fuel-gauge power finds the fast one at 80–160 s on both phones; the slow one always lands at the grid edge (> 1 h) and trades off against ambient. Needs 60+ minute fixed runs to identify |
| 2026-10-29 | Predicting from 4-minute rung runs that c 0.5 holds the S25U below LIGHT at 60 fps on stump (ADR-0009 rung table: 37.8 °C after 4 min) | 20 min on battery from 32 °C with the controller: c 0.5 from 420 s cooled the skin to 38.1 °C, but after one 4-minute step up to c 0.2 the phone reached status 2 at 908 s *on c 0.5* (GPU at 342 MHz) and stayed at 40–41.6 °C; `ladder_exhausted` from 912 s | At c 0.5 the S25U draws 2.6–3.0 W whole-phone, mostly display and CPU, and its skin reading swings ±1 °C within 30 s with the AP temperature. Four minutes is not a steady state. At this frame rate the S25U runs out of ladder: the frame-rate target is the remaining lever (decision 1: reported, not taken automatically) |
| 2026-10-29 | A/B legs started after a single skin ≤ 36 °C reading (S19 harness) | S25U: the fixed leg passed the wait about 30 s after the controller leg ended, at exactly 36.0 °C, and reached status 1 after 68 s; the controller leg had started from 29.9 °C. The 10-minute A/B compared two different start states | Skin readings drop several °C within a minute of stopping the load while the body stays warm. Protocol step 4: ≤ 33 °C held for 3 minutes before every leg (legs then started within 1.6 °C of each other) |
| 2026-10-29 | Acting on Android's 30 s headroom forecast from the first call (ADR-0009 as implemented in S19) | USB smoke test, S24U stump from 31 °C skin: the controller stepped down at 2 s on a forecast of 1.066 (current 0.567) and back up at 63 s | The framework fits its slope to as few as 3 one-second skin samples: one 1 °C sensor step reads as +15 °C in 30 s. Forecasts from the forecaster's first 20 s (and after a 10 s gap) are now ignored (ADR-0009 S20 amendment) |
| 2026-10-29 | The quality controller's frame guard as first decided: step down whenever one second had > 10 % of frames late with the GPU ≥ 90 % busy (ADR-0009) | Replayed (`splatconv replay`) over 32 recorded capped runs on both phones, it would have stepped down in 8, each time 25–220 s into a run that held its cap: S24U stump 734×1591 at 50–52 s, clock 629 → 500 MHz, 100 % of frames late for 2 s; S25U 864×1872 at 25–27 s, 734 → 607 MHz of 1200 | Clock-governor dips of 1–3 s, which recover on their own. A step costs minutes at the lower rung (stepping up needs a cool phone plus the dwell). The guard now waits for 4 bad windows out of 6: 0 of the 32 runs step; a sustained shortfall still steps within ~4 s (ctest) |
| 2026-10-28 | Quad corners from three `const vec2[]` tables indexed by `gl_VertexIndex` (one per draw mode, selected by a specialization constant) | Adreno 750: every vertex invocation ~10× more expensive in all modes (stump 864×1872 draw 4.4 → 48.9 ms; 4-vertex modes 28 ms = 4/6 of it); RTX 3060 faster. The previous shader had one such table and was fine | The driver evidently keeps the tables in per-invocation memory once there are several. Corners now come from two bit masks per pattern, with no arrays, and the draw is back to 4.39 ms. Same family as the compute-shader array miscompile below: per-invocation arrays stay out of Adreno shaders (CLAUDE.md) |
| 2026-10-28 | Lowering the internal resolution as a thermal-controller rung (0.85 and 0.7 of 864×1872, compositor-upscaled) | S24U stump at 60 fps: GPU cycles per frame 9.47 → 9.45 / 9.32 M, draw 5.79 → 5.68 / 5.60 ms, no power difference beyond gauge noise; image cost 10–13 dB (43.4 → 33.1 / 30.0 dB min). Truck: 864×1872 and 540×975 both 4.2 M cycles | The draw of a heavy scene from the bench camera is bound by per-splat primitive work (1.2 M quads, mostly tiny), not fill. Resolution dropped from the ladder (ADR-0009); the per-splat vertex count is the draw lever to try |
| 2026-10-28 | Cull thresholds above 0.5 px as further energy steps | S24U stump at 60 fps: 0.5 → 1.0 px cuts GPU cycles 5.21 → 3.46 M but energy per frame only 51.6 → 49.8 mJ (within gauge noise) for −4.5 dB | The GPU clock is near its floor (385 → 285 MHz); display and CPU (~2–3 W) dominate the phone's draw. The ladder ends at 0.5 px |
| 2026-10-28 | Expecting the native renderer to beat SuperSplat WebGPU on power at equal frame rate (ours frame-capped at its rate, 10 min on battery, same splats and canvas) | Medium scene parity (truck: −3 % energy per frame on the S24U, +10 % on the S25U); heavy scene ours +38 % (S24U) and 2.1× (S25U) per frame, S25U at thermal status 2 while capped | Not culling: SuperSplat's 1 px measure keeps more splats than our 0.1 px. Its default framing differs from our orbit; attribution needs a pose-matched SuperSplat run. At a cap the clock (not busy %) follows the work: our cull threshold moves the S24U clock 595 → 231 MHz at 53 fps (`public-baseline.md` S17). *Resolved S18:* the framing. At our pose SuperSplat's frame costs 57 % (stump) / 78 % (truck) more GPU cycles than at its own view (830: +58 / +57 %). Equal work at our pose on the S24U: parity (97.2 vs 99.3 mJ/frame). The S25U keeps a stump gap (ours +38 % cycles cold, in the draw) |
| 2026-10-28 | Block-staged radix scatter (whole 4096-key block ranked, staged in shared memory in digit order, written in contiguous runs) as the default | RTX 3060 about 2× faster and Adreno 830 −17–22 % unsorted, but Adreno 750 neutral at 1 M keys (2.51 vs 2.53–2.57 ns/key at 16 bits) and 38 % slower on presorted input | On the 750 the scattered stores were not the scatter's main cost; staging removes them and adds about as much back. Direct stays the default, staged is opt-in and a per-GPU candidate for the 830 (`docs/sorting.md` §8) |
| 2026-10-28 | Subgroup-ranked staging (Onesweep-style warp-level multisplit: each subgroup ranks its own chunk without workgroup barriers) | RTX 3060 2.3× faster than the direct scatter and correct; Adreno 750 wrong results in every form tried; one variant (atomicAdd + subgroupShuffle) segfaults the driver's pipeline compiler | Algorithm checked in a CPU model at subgroup sizes 32/64/128 and with the 750's subgroup layout probed (64 × 4, linear): a driver problem on the 750. Dropped |
| 2026-10-28 | 16-entry per-invocation arrays (`uint x[16]`) carried across barriers in a compute shader | Adreno 750: 99 % of sorted values wrong, a quarter of the indices missing, with or without `[[unroll]]` | The same shader with scalar variables per item is correct; per-invocation state in the sort shaders stays scalar on Adreno (`docs/sorting.md` §8) |
| 2026-10-28 | Shared-bitmask downsweep ranking (splatkit's idea) instead of the 8-round subgroup ballot | Mixed: 750 6–10 % faster at 1 M keys (12–20 % at 4 M), 830 5–10 % slower | GPU-dependent cost of subgroup ballots vs shared atomics; ±0.2 ms per frame at app key counts. Ballot stays the default, mask kept opt-in (`docs/sorting.md` §7) |
| 2026-10-28 | Per-chunk contribution bound to skip whole chunks before the preprocess (planned as pack v3) | Conservative bound from pack-v2 data: −8.1 % chunks on stump, ~0 % on bicycle-4m / drjohnson; even a 3× tighter bound only −13.7 % / −1.1 % / 0 % | Contribution-culled splats are interleaved with survivors inside 256-splat Morton chunks; few chunks are entirely sub-threshold. Kept the free v2 bound, dropped pack v3 (ADR-0008 follow-up) |
| 2026-10-28 | Radix downsweep count rows sized from the minimum subgroup size (4 rows = 6 KB shared on Adreno, from 16 rows = 18 KB) to raise occupancy | 25–30 % slower sorts on the Adreno 750 and 830 (1 M 16-bit: 2.36 → 3.04 / 2.22 → 2.78 ns/key); sweep optimum 12–16 rows, cliff above 18 KB | More resident workgroups keep more scatter streams (256 destinations each) open at once; latency hiding does not pay for it. Default back to 16 rows, row count kept as a spec constant (`docs/sorting.md` §7) |
| 2026-10-28 | Counting SuperSplat frames on the engine's `frameend` event (S14/S15 harness) | Every SuperSplat row before 2026-10-28 is a tick rate: desktop self-test 137 "fps" while 57 frames rendered | This engine fires `frameend` on every tick, rendered or not; `postrender` fires only inside `app.render()`. Older SuperSplat rows are upper bounds; re-measured with the fixed harness (public-baseline.md) |
| 2026-10-28 | Explaining the 830's collapsed per-pass timestamps by single-workgroup dispatches | The spine reads ~0 on the 830 with 1, 64 and 256 workgroups alike (750: honest 1.44 / 0.07 ms) | Not the workgroup count. *Resolved S17:* the 830 writes `ALL_COMMANDS` (and `TOP_OF_PIPE`) stamps before the preceding dispatch finishes; `BOTTOM_OF_PIPE` stamps are honest (`docs/sorting.md` §8) |
| 2026-10-27 | SuperSplat's contribution measure (raw opacity × 2π√det of the *dilated* covariance, 1 px) as our pre-sort cull | At equal visible counts up to 13 dB worse than our coverage measure (stump 1.48 vs 1.51 M keys: 40.8 vs 50.0 dB; truck 46.3 vs 58.5; bicycle-4m 45.5 vs 58.1; museum1 57.2 vs 65.4); drjohnson a tie (59.6 vs 59.2 dB at 1.44 vs 1.40 M) | We compensate opacity for the dilation, so their measure overrates tiny opaque splats: every splat with α ≥ 0.53 survives, though it draws almost nothing in our image. Kept ours (ADR-0008) |
| 2026-10-27 | Contribution threshold as a fixed number of internal pixels | 0.1 px at 540×975 dropped stump to 36.2 dB min (43.9 dB at 864×1872) | Fewer pixels → each splat's coverage in pixels shrinks → physically larger splats get culled. Now scaled by viewport area relative to 864×1872: stump at 540×975 47.8 dB |
| 2026-10-27 | 16-bit keys t = d/(d+s), s = AABB diagonal (the S7 quantization) | 42.7 dB min on bicycle-4m against 32-bit keys at 864×1872 | The outlier-inflated AABB diagonal wastes most of the range; log depth over the frame's AABB depth range raised it to 45.1 dB (drjohnson +3.6, truck +1.5 dB mean; stump −1.1). A first check at 1280×720 on three scenes had wrongly shown no gain |
| 2026-09-01 | SOR kNN grid cell sized by volume density (2·cbrt(V/n)) | racoonfamily (932 K) did not finish in 10 min; museum1 (321 K) fine | Real captures are surface-dominated: on a ground plane the volume heuristic puts hundreds of points per cell and candidate gathering explodes. Fixed by measuring occupancy and shrinking the cell (≤3 rebuilds) + a 4096-candidate cap per point → 6.0 s for the same scene (i7 desktop, 20 threads) |
| 2026-09-01 | Chunk cull compacting the visible-chunk list with shared atomicAdd (arbitrary order) | Preprocess +18 %, frame +0.3–0.4 ms vs no cull, all chunks visible (measured on a scene since withdrawn; the atomic variant was removed, the S24 public re-run of the fixed pass is in "Desktop frame pipeline") | The scrambled chunk order broke the preprocess's Morton-coherent memory access. Fixed with an order-preserving shared-memory prefix scan (chunk_list ascending): full-view cull on/off is now a statistical tie |
| 2026-09-01 | Expecting a frame-time win from chunk cull on the RTX 3060 | Public re-run (S24): from inside stump (61 % of chunks culled) only −0.09 ms frame (−4 %), preprocess −32 %; at full view +0.06–0.13 ms (the pass itself) | Culled splats were already cheap early-outs in the preprocess (read position → project → reject); a desktop GPU hides that bandwidth. Kept on for the phones, where reads are the bottleneck (not isolated there) |
