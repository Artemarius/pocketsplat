# Phase 3 on-device frame benchmarks (Android hosts)

First native numbers from the in-app benchmark mode (`android/README.md`): deterministic orbit
(2π/600 per frame), vsync off (MAILBOX), internal resolution 864×1872 (0.6× of the native panel,
the Phase 0 protocol decision), per-frame GPU/pass times + 1 Hz thermal recorded on device,
host-side `dumpsys thermalservice` sampled alongside. Build `3446acb` (fp32 conic, renormalized
sqrt(8)σ kernel, `clamp_needle_scales`-cleaned packs). CSVs: `frame-<device>-3446acb-*.csv` +
`thermal-*.csv` in this directory.

**Instrumentation caveat (found 2026-10-27):** the `visible` column of every CSV in this file is
unreliable. The host read the visible-splat counter from a single host-visible buffer shared by
both frames in flight while the GPU was clearing and refilling it for the next frame, so a row
holds the true count, a partial count, or 0 (26 % zeros on museum1). The window means below are
biased low accordingly. Fixed in the renderer (per-slot counter snapshot); see `docs/sorting.md`
§ "Device sort cost" for the corrected per-key sort analysis.

## Galaxy S24 Ultra (Adreno 750) — museum1 312K SH 1, full 10-min protocol

Battery power, Wi-Fi adb, brightness fixed, thermal status 0 at start, 2026-09-01.

| Window | fps | GPU ms (p50/p95) | cull | preprocess | sort | draw | visible* | thermal |
|---|---|---|---|---|---|---|---|---|
| Cold (0–30 s) | **57.5** | 17.4 (17.1/19.1) | 0.02 | 0.86 | 2.93 | 13.57 | 209 K | 0 |
| Min 2–4 | 41.3 | 24.2 (24.3/25.9) | 0.03 | 1.21 | 4.00 | 18.96 | 236 K | 2 |
| Min 5–7 | 37.1 | 26.9 (26.5/31.4) | 0.04 | 1.33 | 4.51 | 21.02 | 253 K | 2 |
| Sustained (min 8–10) | **32.4** | 30.8 (31.2/32.6) | 0.05 | 1.51 | 5.69 | 23.60 | 257 K | 2 |

\* Biased low, see the caveat above.

Sustained loss ≈ 44 % — inside the risk-register's predicted 35–50 % band. Throttle status 1
appeared within the first minute, status 2 from ~minute 2 onward; skin held ≈ 42 °C (thermal CSV).
Whole-run average 39.0 fps over 23,427 frames.

## Phase 3 gate (1 M splats SH 0) — re-established on public scenes

The original gate run (S24U, 1 M SH 0, 10-min protocol, 2026-09-02) used a scene that cannot be
redistributed, so its CSVs and numbers are withdrawn from this repository. The gate (S24U ≥ 30 fps
cold and ≥ 15 fps sustained at ≤ 1 M SH 0) has since been cleared on public scenes four times its
size:
* 10 minutes on battery, 540×975, uncapped, from a cool start: stump (4.05 M) 96 → 67 fps on the
  S24U and 99 → 68.5 on the S25U; truck (1.65 M) 174 → 156 / 237 → 154 (`public-baseline.md`).
* 20 minutes on battery at the protocol's 864×1872, capped at 60 fps: stump held p5 60 fps
  through minute 20 on both phones, with the thermal controller and with the fixed setting
  (ADR-0009, `controller-ab-1d3faec-2026-10-29.csv`).

## Galaxy S25 Ultra (Adreno 830) — first light, 90-s cold pass

USB-attached (cold-pass precedent from Phase 0), thermal status 0 at start, 2026-09-01.

| Scene | Cold fps | GPU ms (p50/p95) | sort | draw | visible* |
|---|---|---|---|---|---|
| museum1 312 K SH 1 | **71.1** | 14.1 (14.1/14.7) | 2.99 | 11.05 | 242 K |

Adreno 830 runs museum1 ≈ 24 % faster than the 750 at identical settings.

**Per-pass caveat (Adreno 830):** the cull and preprocess segments read ≈ 0 ms on this driver
while sort/draw/total stay self-consistent (their sum matches the frame total and fps) — the
compute-pass timestamp boundaries appear to collapse the first segments into the sort window.
Treat 830 per-pass splits as approximate until investigated; frame totals are trustworthy.
*Resolved 2026-10-28 (S17):* the 830 driver writes `ALL_COMMANDS` timestamps before the preceding
dispatch has finished; the timer now stamps `BOTTOM_OF_PIPE` and the 830's split is honest from
build `ea99500` on (`docs/sorting.md` §8). Per-pass columns of earlier 830 CSVs remain collapsed.

## Reading so far

* museum1 (fill-heavy SH 1) is draw-bound on both devices. These runs predate ADR-0006
  (eigenbasis oriented quads), which cut museum1 GPU time by 37–49 % on the desktop. On the S24U
  museum1 later ran 127 fps at 864×1872 (S14, `public-baseline.md`), with every renderer change
  since included.
* Sort cost per key on the 750 was re-derived with the corrected visible counts and a standalone
  on-device `sortbench`: 8.3 ns per key at 32 bits (S14), 4.3 after the S16 rework
  (`docs/sorting.md`).
