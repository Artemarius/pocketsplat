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
- `cap-lift-2026-11-06.csv` — scenes over the old 4.19 M cap on both phones (ADR-0016): one row per
  60 s orbit run with fps, per-pass ms, visible and clipped counts, the clip guard's floor and
  peak memory. Real scenes are the uncleaned INRIA bicycle and garden (local only); `bicycle-xN` are
  synthetic tiles (`scripts/tile_scene.py`).
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
- `home-view-ransac-<hash>-<date>.csv` — the home view's ground-plane RANSAC before and after S32
  (fixed 400 hypotheses vs subset scoring until a miss is unlikely), per bench pack and raw public
  scene: points near the focus, best inliers old/new, best-plane share, the old code's chance of
  missing that plane, change of the up axis, home distance, and the up error against the INRIA
  training cameras where they exist.
- `home-view-open-<device>-<hash>-<date>.csv` — `estimate_home_view` time at scene open on a phone
  (logcat "home view in … ms", scripted `open_scene`), per scene and open, with the build and the
  commit `home_view.cpp` came from (S33 A/B of the S31 and S32 RANSAC), focus, distance and
  whether a plane was found.
- `home-view-tilt-probe-<hash>-<date>.csv` — the home view's accepted ground plane per probe scene
  (S33): share of the points near the focus, normal, angle to +y, the best rival plane > 30° away
  (share, angle, rival/winner ratio), and whether a plane is kept before and after the S33 rule
  (> 60° from +y needs ≥ 20 %), with the resulting up axis.
- `quantize-sim-<hash>-<date>.csv` — image price of 16-bit chunk-relative positions and 8-bit
  log-scales, simulated (`scripts/quantize_sim.py`, ADR-0018 option (e)): PSNR and pixels moved
  > 2 LSB per scene, variant and pose against the untouched pack, with a bounds-only control and
  `ref@repeat`. `quantize-steps-<hash>-<date>.csv` — the same scenes' step sizes: chunk extent
  against the scene diagonal, position error in sigmas of the splat's largest axis, log-scale
  range and step.
- `segments-<device>-<hash>-<date>.csv` — the per-splat arrays split into segments (ADR-0018)
  against one segment, `quality_ab.py` with `--segment-splats N`: PSNR at the six poses, pixels
  moved > 2 LSB, and the bench orbit's mean visible count, with `ref@repeat`.
- `segments-s25u-<hash>-<date>.csv` — segments on a phone (ADR-0018): per run the build,
  segment split, per-pass means over the first 30 s, visible mean, peak PSS and GL mtrack; the
  header states which runs were compared frame by frame.
- `quality-rtx3060-rungs-<date>-864x1872.csv` — image cost of the thermal controller's candidate
  rungs (cull threshold × internal-resolution scale, ADR-0009); scaled variants are upscaled
  bilinearly to 864×1872 before PSNR (`quality_ab.py --scale`).
- `quality-<device>-<hash>-<w>x<h>.csv` — image-quality A/B of renderer variants from
  `scripts/quality_ab.py` (desktop host, offscreen at the phone's pixel count): PSNR against the
  exact reference at six fixed poses, share of pixels that moved > 2 LSB, and the visible-splat
  count over the bench orbit (device-independent, so the sort cost on a phone follows from
  `docs/sorting.md` §6).
- `palette-quality-<hash>-<date>.csv` — SH 1 palette, Phase 8 gate A (`scripts/palette_ab.py`,
  `docs/sh1-palette-research.md`): per scene the direct SH 1 pack (reference), the SH 0 pack and
  packs whose SH1 is snapped to a Lloyd palette of k entries, rendered by the desktop host at
  864×1872 at the six `quality_ab.py` poses; PSNR min/mean and the share of pixels moved > 2 LSB
  against the reference, `direct@repeat` = the renderer's noise floor; per palette the training
  wall time, thread count, epochs run, MSE in SH1 space, unused entries and label entropy.
- `palette-trainers-<hash>-<date>.csv` — SH 1 palette trainers, Phase 8 B (`tools/palettebench` via
  `scripts/palette_trainers.py`): one row per training run (scene, trainer, k, epochs, seed, cap in
  points per entry) with batch and α0 (e-means), revivals, distance pairs evaluated, MSE of every splat
  against its nearest entry, unused entries, label entropy, training and total wall time, and, for
  phone rows, thermal status and skin temperature before and after. `label` names the machine;
  desktop wall times are indicative only, as for every laptop timing.
- `palette-gatec-<hash>-<date>.csv` — SH 1 palette, Phase 8 gate C (`scripts/bench_android.py` runs
  summarised by `scripts/palette_gatec.py frames`): per phone, scene size and pack variant (`pal4096`
  palette pack, `sh1` direct SH 1, `sh0`) and window (`cold_0_30s`, `min_4_5` = 240–300 s of a
  5-minute run) the fps, GPU and per-pass ms, visible splats and the highest thermal status; `run`
  numbers repeated runs; `power` = `charging` when the phone was on the charger.
- `palette-import-<hash>-<date>.csv` — SH 1 palette, Phase 8 gate C import cost (scripted imports
  through the app's `import_src` intent, `scripts/palette_gatec.py imports` over the app's
  `import timing` logcat lines): per import the budget, palette on/off, per-stage and total ms and,
  with a palette, training and training + final assignment ms and the palette MSE. The `palette`
  stage also includes building the pack.

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

## SH 1 palette — gate A, desktop image A/B (2026-11-19, Phase 8)

`palette-quality-c6f0a7f-2026-11-19.csv`: six scenes at their cleaned counts (museum1.sh1 cleaned
as the app imports it, hornedlizard as imported from its SPZ, the full bench clouds of truck, beetle,
giraffes_statue and stump), Lloyd 25 epochs, seed 1, 32-bit keys, no contribution cull, RTX 3060
renders, i5-11400H training. PSNR is the minimum over the six poses against the direct SH 1 pack.

* **SH 0 is below 40 dB on every scene** (29.6–38.7 dB; means 35.1–40.3): SH 1 matters most on
  beetle, giraffes_statue and museum1 (29.6–33.8 dB).
* **A 1,024-entry palette (12 KB) reaches 40 dB on every scene**: 40.6 dB on giraffes_statue,
  46.1–51.6 dB elsewhere. 4,096 entries: 42.6 / 47.5–52.7 dB; 16,384: 46.9 / 48.8–53.8 dB; 65,536:
  46.3 / 50.2–54.6 dB (giraffes_statue's worst pose is 0.6 dB lower at 65,536 than at 16,384; its
  mean rises). Every reference re-render scores 75–101 dB, so these differences are not noise.
* Training is the cost. The laptop times below are indicative only; per CLAUDE.md §7, timings that
  decide anything come from the S24 Ultra. Lloyd at 25 epochs takes 9–167 s at k 4,096 and 41–731 s at k 16,384 across
  0.31–4.05 M splats. That is count × k × (epochs + 1) assignment pairs at 3.0–3.9 G pairs/s on 12
  threads and 1.5–2.8 on 4. The 12-thread rows ran beside another project's jobs and the 4-thread
  rows at idle priority, so the times are upper bounds, not a clean kernel benchmark. museum1 at
  k 65,536 stopped after 14 epochs (no label changed; 4.8 splats per entry).
* Unused entries appear only where the palette approaches the number of distinct SH1 vectors:
  hornedlizard, imported from SPZ, has 106,045 distinct snorm8 SH1 vectors among 764,985 splats
  (25,896 of 65,536 entries unused); museum1, truck and giraffes_statue have 85–100 % distinct, and
  no PLY-sourced scene leaves more than 196 entries unused at 65,536.

## SH 1 palette — trainers (2026-11-19/20, Phase 8 B)

`palette-trainers-1cdd4cc-2026-11-19.csv`: the six gate A scenes, k 4,096 with 5 seeds (with and
without a 64-points-per-entry training cap), k 1,024 and 16,384 with 1 seed; i5-11400H, 4 threads,
idle priority. Hamerly rows give Lloyd's losses (identical iterations). Details are in
`docs/sh1-palette-research.md` §6.

* **e-means (arXiv 2611.09501) reaches the Lloyd-25 loss in 3 epochs on four scenes and in 5 on all
  six**, on every seed, with the paper's scaling laws untuned at d = 9. That is 12–20 % of Lloyd-25's
  distance work. At equal work (3 epochs) its loss is 7–18 % below Lloyd's.
* On hornedlizard, an SPZ import with many duplicate SH1 vectors, Lloyd-25 leaves 245 of 4,096
  entries unused; e-means-5 leaves 1.6, and its loss is 12 % lower.
* Hamerly's bounds save 6–30 % of Lloyd's distance work (negative results log).
* A 64-per-entry training cap cuts e-means-3's training work to 0.8–4 % of Lloyd-25 and costs
  6–8 % loss on the four larger PLY scenes.
* Phones (`palette-trainers-s24u-e87cf48-2026-11-20.csv`, deciding; `palette-trainers-s25u-…`,
  second phone; portable scalar kernel, 8 threads, k 4,096): on hornedlizard (765 K), e-means-3 takes
  15.4 s in total on the S24 Ultra, against 87.3 s for Lloyd-25 and 70.1 s for Hamerly-25, with 12 %
  lower loss (S25 Ultra: 8.7 / 59.0 / 48.2 s). On stump (4.05 M), e-means-3 takes 76.3 s on the S24U,
  of which the final assignment of every splat is ~19 s; with a 64-per-entry cap, 20.9 s. The kernel
  runs at 0.8–0.95 G pairs/s on the S24U and 1.2–1.4 on the S25U. At equal pairs, e-means' training
  takes 12–58 % longer than Lloyd's on the phones (a barrier per mini-batch with a static split over
  unequal cores is suspected). Both phones give identical losses.

## SH 1 palette — gate C on the phones (2026-11-23/24, Phase 8 C)

`palette-gatec-cd347ef-2026-11-23.csv`, `palette-import-cd347ef-2026-11-23.csv`,
`palette-trainers-s24u-cd347ef-2026-11-23.csv` (deciding) and `palette-trainers-s25u-cd347ef-…`.
Both phones on the charger (owner's setup), Wi-Fi adb, heads-up notifications off, cool start for
every run. Stump at the default 1 M budget and at its full 4.05 M; app defaults on the phone
(16-bit keys, contribution cull 0.1 px), protocol resolution 1872×864, 5-minute runs; the palette
packs use e-means-3 with k 4,096, trained on the desktop (the same palette the phone trains:
losses match bit for bit). Details: `docs/sh1-palette-research.md` §6.

* **Preprocess with the palette is no slower than direct SH 1 on either phone, at either size**
  (minute 4–5, thermal status 2 everywhere): S24U 3.41 / 3.46 ms (two runs) vs 3.53 ms at 1 M and
  5.29 vs 5.52 ms at 4 M; S25U 2.18 vs 2.23 and 3.56 vs 3.72 ms. SH 0 is between 4 % faster (S24U
  1 M) and 1 % slower than the palette (S32 correction of "within ±2 %").
  fps follows (S24U 4 M: 70.3 palette, 68.5 direct, 69.5 SH 0). **Gate C passed.**
* **Import cost** (S24U, deciding): the palette adds 4.5 s at the 1 M budget (5.6 s against 1.1 s)
  and 19.6 s for the 4.05 M "all" import (21.3 against 1.8 s). S25U: +3.0 s and +13.0 s. The source
  was an already cleaned SH 1 PLY in the page cache, so read and clean are short here.
* **Trainer** (S24U, NEON kernel + dynamic work split, against the S30 scalar kernel): hornedlizard
  e-means-3 3.1 s in total (was 15.4 s), stump e-means-3 18.9 s (was 76.3 s). At equal pairs e-means
  now costs the same as Lloyd (+0 % and +2.5 %; S30: +12 to +58 %), so the static split over unequal
  cores was the cause. S25U: 2.2 s and 12.0 s (was 8.7 and 55.6 s), ~5.6 G pairs/s.
* Phone palettes now equal the desktop's (same MSE, revivals and dead entries on both scenes): FP
  contraction is off in `palette.cpp`.
* GPU-resident pack: 28.16 MB against 40.11 MB at 1 M, 113.9 against 162.4 MB at 4 M (the buffer
  sizes; `dumpsys meminfo` does not resolve them, negative results log).

## Home view ground plane — RANSAC hypotheses (2026-11-24, S32)

`home-view-ransac-99529ba-2026-11-24.csv`, desktop only (no timings): `estimate_home_view` on the
11 cleaned bench packs and 22 raw public scenes, the S31 code against 99529ba.

* **Cause of the arm64 HomeView test failure**: the test's walled scene has its ground at ~13 % of
  the points near the focus, so a random triple lies on it with p ≈ 0.0023, and 400 fixed
  hypotheses miss it ~40 % of the time. arm64's libm moved the focus by ~1e-6, 85 points crossed
  the RANSAC radius, every pick changed, and that draw missed (1,167 inliers against 6,299 on MSVC).
* **Real scenes are as marginal**: the best plane holds 7.7 % of the points on museum1, 11–13 % on
  the beetle pack, giraffes_statue and bonsai, and 18–21 % on truck and room. At 400 hypotheses the
  chance of missing it is 0.83, 0.4–0.6 and 0.03–0.14. Every scene had found *a* plane, but a
  few-ulp change of the input could change its up axis.
* **Now**: hypotheses are scored on a fixed stride subset of 2,048 points. They continue until
  `it · w³ ≥ −ln 1e-4` for the best subset share w, with at least 400 and at most 20,000
  hypotheses (the cap covers a 7.7 % share), and the 8 best are scored again on every point.
  Inliers are never fewer than before. Up changes by 0–2.2° everywhere except museum1 (6.2°) and
  the beetle pack (10.4°, 10,877 → 20,309 inliers: the old draw had found a lesser plane; its home
  view became a level 3/4 view of the whole car instead of a close side crop). Against the 13 INRIA
  training camera sets the up error is unchanged within 0.8° (room stays at ~30°).
* Work in point tests: before, 400 × the points near the focus (7–55 M on these scenes); now, the
  hypotheses × 2,048 plus 8 × the points, which is less on scenes with a clear ground and at most
  41 M at the cap. Scene-open time on the phones is not measured yet.

## Home view on the phone: open time, portrait frame, tilt rule (2026-11-24, S33)

`home-view-open-s24u-905e9c6-2026-11-24.csv`: S24U on battery, APK 905e9c6 with the S32 RANSAC
against the same APK with `home_view.cpp` from 5142815 (the S31 RANSAC), opened through the app's
`open_scene` intent, 3 opens per scene, the two builds interleaved per scene with the order flipped
every scene. `home-view-tilt-probe-0d73046-2026-11-24.csv`: desktop, the accepted plane of every
probe scene (11 bench packs, the museum1 app pack, 25 raw public scenes; 36 with a plane).

* **Open time**: `estimate_home_view` takes 86–199 ms with the S32 RANSAC (median 119 ms) and
  95–234 ms with the S31 one (median 144.5 ms), same scenes and phone. ADR-0014's 647–1091 ms
  (S25U, S24) came from an older build and does not compare. The views were identical between the
  two builds except beetle's, the change S32 described.
* **Portrait frame**: the S32 owner check found beetle's and museum1's start views too close. The
  eye's score raster was a 48 × 48 square at 60°, but the app shows a portrait 1080 × 2340 frame,
  ~30° across: the winner was framed for a screen twice as wide. Scored through the frame (raster
  22 × 48), beetle opens on the whole car side-on, counter on the whole counter, bicycle on the
  bike and bench, museum1 on more of the statue; stump, truck and drjohnson are unchanged (owner:
  fine). Renders at 540 × 1170, desktop, not committed (scene media policy). A landscape frame
  (1280 × 720, the Windows viewer) scored the same way opened the truck pack on a 21.7 m floater
  overview: only a portrait frame narrows the raster, landscape keeps the square.
* **museum1's tilt was never caused by S32**: the old build's phone capture is tilted the same, and
  S32's "6.2° up change" compared two planes that were both a face of the statue. The probe found
  the same failure on racoonfamily: on both (+y-up captures of a lone object) the best plane near
  the focus is a face of the object, 80–85° from +y, holding 7.6–9.0 % of the points. The only
  correct planes far from +y are the drjohnson family's floors (89° from +y in COLMAP's frame,
  30.4–44.8 %); every other plane is within 48.5° of +y (stump). A plane more than 60° from +y now
  needs ≥ 20 % of the points, else up stays +y: up is bit-identical on 33 of the 37 probe scenes and
  changes only on the 3 museum1 packs and racoonfamily, which now open upright.
* Statistics that did **not** separate museum1 from correct grounds: share alone (0.090 against
  hornedlizard's correct 0.105), in-plane spread of the inliers (truck 0.11–0.26 of the radius,
  museum1 0.25), focus height above the plane (drjohnson's focus sits on its rug), and the best
  rival plane > 30° away divided by the winner (museum1 0.70–0.73, truck 0.87–0.89, hornedlizard
  0.87: a ground next to a building wall has a strong rival too).

## Smaller position and scale arrays — simulated image price (2026-11-24, S34, ADR-0018)

`quantize-sim-fb66214-2026-11-24.csv`, `quantize-steps-fb66214-2026-11-24.csv`: the six gate-A
packs (direct SH 1, the same files as `palette-quality-c6f0a7f-2026-11-19.csv`) rewritten by
`scripts/quantize_sim.py` with positions at 16 bits per axis inside each chunk's tight centre
bounds and/or log-scales at 8 bits over the scene's log-scale range, stored back in an ordinary
v3 pack. Rotation is untouched: the smallest-three encoding option (e) would use is finer than
today's snorm8. Desktop host, 864 × 1872 offscreen, 32-bit keys, no contribution cull, the six
`quality_ab.py` poses plus two at 0.35 × the protocol distance. PSNR in dB, worst of the six
poses / worst of the two close poses:

| Scene | Splats | Noise (`@repeat`) | Bounds only (control) | Positions 16 bit | Log-scales 8 bit | Both |
|---|---|---|---|---|---|---|
| museum1 | 314,457 | 91.2 / 88.1 | 94.3 / 87.9 | 59.0 / 58.3 | 45.2 / 45.0 | 45.2 / 45.0 |
| hornedlizard | 764,985 | 97.8 / 93.3 | 97.5 / 93.4 | 51.1 / 49.3 | 47.9 / 47.6 | 47.6 / 46.9 |
| beetle | 1,681,983 | 95.3 / 100.1 | 75.1 / 108.0 | 63.0 / 61.7 | 45.5 / 45.1 | 45.5 / 45.1 |
| giraffes_statue | 2,880,821 | 77.7 / 84.2 | 80.1 / 84.2 | 52.9 / 56.6 | 41.8 / 42.5 | 41.8 / 42.5 |
| truck | 1,653,927 | 89.2 / 93.5 | 89.0 / 96.3 | 61.5 / 58.4 | 41.5 / 41.2 | 41.4 / 41.2 |
| stump | 4,049,567 | 91.5 / 89.7 | 83.9 / 90.5 | 63.0 / 60.5 | 47.7 / 46.8 | 47.7 / 46.8 |

* **The simulation adds nothing of its own**: the control rebuilds the chunk bounds from the
  pack's values and lands inside the renderer's repeat noise. Its lowest value, beetle at 75.1 dB,
  equals gate A's `@repeat` minimum for the same pack and pose. The close poses show content,
  but on beetle, giraffes_statue and truck they sit inside floater clutter: harsh views, not
  typical ones.
* **Positions are not the risk**: 49.3–63.0 dB. Median chunks span 0.02–4.2 % of the scene
  diagonal, the widest 41–61 %. Half a step is a median 0.0002–0.0006 σ of the splat's largest
  axis, and more than 0.1 σ on at most 0.03 % of the splats. Those are tiny splats in wide
  chunks, and the worst is 17 σ on giraffes_statue. hornedlizard moves the most pixels (4.5 % by
  more than 2 LSB at a close pose).
* **Log-scales at 8 bits set the price**: 41.2–47.9 dB, above the 40 dB bar but moving up to
  39 % of the pixels by more than 2 LSB (giraffes_statue). The scene's range is stretched by
  splats far too small to see: truck spans log-scale −32.1 to 1.5, a step of 0.13 (±6.8 % in
  scale). A floor on the range or per-chunk ranges would shrink the step. Not tried.
* Both together cost what the scales cost alone.

## Segments: the per-splat arrays split for the 750's binding (2026-11-24, S34, ADR-0018)

`segments-rtx3060-f5fc198-2026-11-24.csv`: desktop host at f5fc198, 864 × 1872 offscreen, 32-bit
keys, no contribution cull. `--segment-splats N` forces segments on the RTX, whose own binding
(4 GB) never needs them, against the same pack in one segment. The six gate-A packs use
65,536-splat segments (5 on museum1 to 62 on stump); tiled bicycle ×3 (13.45 M, SH 0) is split as
the 750 splits it (2 × 11,141,120) and into 4 × 4,194,304.

| Scene | Segments | PSNR min / mean (dB) | `@repeat` min / mean (dB) | Orbit visible, ref = segments |
|---|---|---|---|---|
| museum1 | 5 | 91.9 / 97.2 | 91.1 / 96.9 | 310,789 = 310,789 |
| hornedlizard | 12 | 92.0 / 98.2 | 92.1 / 100.4 | 561,093 = 561,093 |
| beetle | 26 | 95.6 / 99.5 | 97.1 / 101.5 | 896,024 = 896,024 |
| giraffes_statue | 44 | 77.9 / 86.7 | 77.7 / 86.0 | 787,770 = 787,770 |
| truck | 26 | 89.5 / 95.4 | 90.4 / 97.0 | 779,562 = 779,562 |
| stump | 62 | 91.2 / 95.7 | 83.9 / 94.4 | 2,137,058 = 2,137,058 |
| bicycle ×3 (tiled) | 2 | 81.7 / 91.8 | 92.3 / 95.2 | 3,903,701 = 3,903,701 |
| bicycle ×3 (tiled) | 4 | 94.5 / 97.0 | (same) | 3,903,701 = 3,903,701 |

* Same splats, same image: every split draws the reference's visible count to the splat over the
  600-frame orbit. PSNR sits at the renderer's repeat noise (equal-key blend order, ADR-0008),
  which itself varies between runs. ×3 in two segments is 81.7 dB at its worst pose, below this
  run's `@repeat` minimum of 92.3 dB, but the same scene's `@repeat` minimum was 82.3 dB in an
  earlier run of the same comparison (3b9c2d3, the cull's previous close-out, 81.7 dB as well).
  At most 0.0085 % of the pixels move by more than 2 LSB (giraffes_statue, the same share as
  its `@repeat`). Images cannot be bit-identical:
  atomic slot order differs run to run.
* Validation layers: 0 messages on 120-frame bench orbits with forced segments: ×3 in 2 segments
  with and without the chunk cull, stump direct SH 1 in 62 with the phone's 16-bit keys and
  0.1 px cull, a museum1 palette pack in 5, stump-full with its coverage sidecar at strength 1
  in 62. `ctest` 146/146, including the 4 golden-image tests.
* GPU times in the CSV are the shared laptop's, indicative only. The deciding comparison, one
  segment against the v0.4.0 build at 4 M, is the S24U's (ADR-0018, "What decides it").

## Segments on the S25U (2026-11-24, S34, ADR-0018)

`segments-s25u-a344337-2026-11-24.csv`: S25U (Adreno 830), on USB power at 100 %, bench orbit at
540 × 975, 16-bit keys, 0.1 px cull unless stated; per-pass means over the first 30 s. Not the
deciding phone (that is the S24U, CLAUDE.md §7): these runs check correctness and memory on a
second driver and give the S24U runs their shape.

* **Device limits the design depends on** (`cmd gpu vkjson`, the app's "Vulkan ready" line):
  `maxComputeWorkGroupCount.x` = 65,535 (the spec minimum, so the 830 splits at 16,711,680 splats
  for its dispatches, not its 2 GB binding), no `VK_EXT_memory_budget`, `maxMemoryAllocationSize`
  1 GiB, `maxBufferSize` 4 GB, 16.7 M storage buffers per stage, buffer device address and dynamic
  indexing present. Recorded in `devices.md`.
* **One segment is no slower than v0.4.0** (bicycle-4m, ABBAAB with a cool-down to thermal
  status 0 and skin ≤ 38 °C before each run): preprocess 2.696 ms (v0.4.0, mean of 3) against
  2.677 ms, chunk cull 0.221 against 0.220 ms, GPU frame 10.82 against 10.71 ms; run-to-run
  spread ±0.04 ms.
* **Two segments draw the same splats**: bicycle-4m forced into 2 × 2,097,152 against one segment,
  per-frame visible count equal on 5,059 of 5,060 frames (frame 0 is the first readback, still
  0 in one run). bicycle ×3 split as the 750 splits it (2 × 11,141,120) against one segment:
  3,067 of 3,067 frames equal, preprocess 5.67 against 5.68 ms. (The forced bicycle-4m run ran
  warm, thermal 1 throughout; its times are not a comparison.)
* **×4 (17.93 M) in two segments against v0.4.0's one dispatch**: per-frame visible counts equal
  on all 1,742 (no cull) and 2,374 (0.1 px) frames, preprocess 9.28 / 7.53 ms against 9.25 /
  7.51 ms. 81 of those frames had more than 65,535 visible chunks (up to 66,265): v0.4.0's single
  dispatch exceeded the device's workgroup limit there, and the 830's driver ran it anyway, so
  v0.4.0 dropped nothing on this phone. Segments now stay within the limit on any driver.
* **Memory**: peak PSS 1.03 / 1.37 / 2.13 GB at ×2 / ×3 / ×4 (v0.4.0 at ×4: 2.54 GB), GPU
  memory (GL mtrack) 0.92 / 1.25 / 1.73 GB (v0.4.0 ×4: 1.82 GB). On the 830 the visible capacity
  covers the whole scene, so ×4's GPU memory is 29 B per splat plus 69.8 B per visible slot; the
  renderer's estimate became 72 B (it was 56).
* **The memory policy, corrected on the phone**: without `VK_EXT_memory_budget` VMA reported
  8.9 GB available (80 % of the 11.1 GB heap) against 6.0 GB MemAvailable, a limit of 123 M
  splats nothing could hold. The Android host now budgets min(VMA, MemAvailable / 2) and counts
  the 28 B CPU pack copy alive during upload: 23.3 M splats at 29 B on the S25U (MemAvailable
  5.8 GB). ×4 (17.93 M, with its coverage sidecar) opens whole; a tiled ×6 (26.9 M, not kept)
  opened as its most important 20.9 M under the partial banner, peak PSS 1.95 GB, 42 fps.
* Process: Wi-Fi adb installs of the 26 MB APK timed out (300 s) or raced the bench's launch
  (the package update killed the freshly started app). Installs went over USB, benches over
  Wi-Fi.

## Negative results log
| Date | What was tried | Result | Diagnosis |
|---|---|---|---|
| 2026-11-24 | Home view further out (owner: "step away a bit") by moving the chosen eye back along its ray, × 1.3 / 1.6 / 2.0 (S33, desktop renders at the phone's portrait frame) | Better on bicycle, counter and stump; beetle gets a pole in front of the car, truck a sign, drjohnson goes through a wall into the next room at × 1.3 | The eye's score checked the line of sight at its own distance only. Any step back has to be a candidate the score sees |
| 2026-11-24 | Candidate eyes beyond the full distance (× 1.3, 1.6) and "the farthest candidate within 15 % / 30 % of the best score" (S33, with the portrait frame) | Truck opens on a 22–35 m overview from outside the capture, full of floaters; at 30 % drjohnson opens in the next room | Truck's good view is its closest candidate (0.18 × 21.7 m); its far ones score nearly as well on fine detail. The portrait frame alone was kept (owner: stump and truck fine) |
| 2026-11-23 | `dumpsys meminfo` to show the SH 1 palette's memory saving (12 MB per 1 M splats) on the S24U and S25U | `GL mtrack` identical to the kilobyte for palette, direct SH 1 and SH 0 on the S25U (170,976 KB at 1 M); TOTAL PSS within 0.2 % at 4 M | The renderer allocates with VMA's defaults: the per-splat buffers are sub-allocated from shared large blocks, and the driver accounts whole blocks. A 12–48 MB change does not change the block count at these sizes. The saving is reported from the buffer sizes instead; an allocation-level measure would need VMA statistics in the bench CSV |
| 2026-11-19 | Hamerly's bounds (one upper, one lower bound per point) to speed up Lloyd for the SH1 palette, the classical choice at low dimension (`docs/sh1-palette-research.md` §3.1) | Same labels and centroids as Lloyd, bit for bit, but 0.70–0.94 of Lloyd's distance pairs at k 4,096 over 25 epochs (0.64–0.90 at k 1,024, 0.75–0.96 at k 16,384); 87–97 % of points need a full scan in each epoch (museum1, k 1,024) | SH1 vectors have little cluster structure: the second-nearest entry is only ~13 % farther than the nearest on average, and the lower bound shrinks by the largest centroid drift each epoch, 5–10× the median drift. e-means needs an eighth of Lloyd's work for the same loss, so per-centroid bounds (Elkan, n × k memory) were not tried |
| 2026-11-19 | SH1 palette assignment kernel written to auto-vectorize (`docs/sh1-palette-research.md` §2.2): one scoring loop over a 512-centroid tile, then a lane-blocked minimum, MSVC `/O2` (scratch microbenchmark, 200 K random points, k 4,096, i5-11400H at ~2.6 GHz) | 0.59 G pairs/s on one thread (~6.5 cycles per pair), 3.2 on 12; the vectorizer report confirmed both loops vectorized (SSE2) | x64 MSVC targets SSE2 without FMA, and the separate score and minimum passes plus a nine-add chain left ~2 iterations in flight. Explicit SSE2 with per-lane best value and index, a tree-summed dot product and two vectors per step: 0.77 on one thread, 3.6 on 12, close to the SSE2 µop limit at that clock. arm64 keeps the portable loop until a NEON kernel can be tested on a phone (Phase C) |
| 2026-11-06 | Clip guard lowering its contribution floor whenever the visible count stays under 60 % of the visible capacity for 30 computed frames (S28) | 18 M tiled bicycle orbit at capacity 4,194,304 (desktop): the floor flipped 0 ↔ 0.1 px 26 times in 600 frames; 4.4 % of frames clipped (70.3 % without a floor) | 0.1 px halves a large view's visible count (5.0 M → 2.4 M), so a count under 60 % at 0.1 overflowed again at 0. Lowering now predicts the count after the step from what the last raise removed and needs it under 90 %: 0.7 % clipped frames, 4 changes (ADR-0016) |
| 2026-11-06 | Clipping a frame's visible splats at the draw-record capacity with nothing else (S28, step 1 of ADR-0016) | Raw bicycle at `--visible-cap 2000000` (desktop): 13.4 dB against the full image, the ground in front of the camera gone; S24U no-cull orbits of 13 M / 18 M tiled scenes clipped 46 % / 99.8 % of frames | A workgroup is a 256-splat chunk, so whole chunks that append last are dropped: arbitrary regions, near ones included. Replaced by the contribution floor (52.0 dB on the same view) |
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
