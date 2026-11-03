# ADR-0009 — Thermal-adaptive quality controller: a measured cull-threshold ladder, two loops

**Status**: accepted (2026-10-29, S20), with the frame-guard and forecast warm-up amendments
below. Proposed 2026-10-28 (S18), implemented in S19, A/B on both phones in S20 (results below).

**Context**: Phase 4's goal is to hold a frame-rate floor for ten minutes of continuous rendering
at the smallest measured image cost. Uncapped, the app reaches thermal status 2 within 1.5–5 min
on both phones (S16). Capped at the rate SuperSplat delivers, the heavy scene still reaches status 1
on the S24U and status 2 on the S25U (S17, `docs/benchmarks/public-baseline.md`). No published
splat viewer adapts to thermal state at runtime: the SuperSplat viewer uses fixed device-class
budgets, Spark a fixed budget, splatkit a manual policy.

Six measurements constrain the design:

* **At a fixed frame rate, neither GPU busy % nor GPU time per frame measures the work.** S24U
  stump, 540×975, capped at 53 fps (`cap-lever-s24u-1707c96.csv`):

  | cull threshold | splats sorted | GPU clock | kgsl busy | GPU ms/frame | GPU cycles/frame |
  |---|---|---|---|---|---|
  | 0.1 px | 1.19 M | 595 MHz | 79 % | 15.6 | 9.29 M |
  | 0.2 px | 0.87 M | 500 MHz | 78 % | 15.5 | 7.76 M |
  | 0.5 px | 0.51 M | 310 MHz | 76 % | 15.1 | 4.68 M |
  | 1.0 px | 0.31 M | 231 MHz (floor) | 68 % | 13.6 | 3.13 M |

  The governor holds busy near 75–80 % and lowers the clock instead, so GPU time per frame stays
  near 15 ms for a 3.8× change in sorted splats. Cycles per frame (GPU ms × clock) does track the
  work: 6.9 cycles per sorted splat plus 1.4 M per frame fits the three points above the clock
  floor.
* **Price per rung** (S24U, stump, 864×1872, capped at 60 fps, 4 min per rung on battery, each from
  skin ≤ 36 °C, build `4bcadb9`; `energy-s24u-f0f242c-2026-10-28.csv`; PSNR from the desktop
  against exact rendering at the six `quality_ab.py` poses,
  `quality-rtx3060-rungs-2026-10-28-864x1872.csv`):

  | rung | sorted | GPU clock | GPU Mcycles/frame | power | mJ/frame | skin max (status) | PSNR min / mean |
  |---|---|---|---|---|---|---|---|
  | c 0.1, 1.0 scale | 1.19 M | 664 MHz | 9.47 | 5.25 W | 87.5 | 41.7 °C (1) | 43.4 / 46.1 dB |
  | c 0.2 | 0.89 M | 570 MHz | 7.60 | 4.19 W | 69.9 | 40.5 °C (1) | 38.0 / 41.4 dB |
  | c 0.5 | 0.53 M | 385 MHz | 5.21 | 3.10 W | 51.6 | 38.2 °C (0) | 31.1 / 34.4 dB |
  | c 1.0 | 0.32 M | 285 MHz | 3.46 | 2.99 W | 49.8 | 36.8 °C (0) | 26.6 / 29.6 dB |
  | c 0.1, 0.85 scale | 1.19 M | 666 MHz | 9.45 | 5.43 W | 90.4 | 41.3 °C (1) | 33.1 / 34.9 dB |
  | c 0.1, 0.7 scale | 1.19 M | 659 MHz | 9.32 | 5.28 W | 88.1 | 41.8 °C (1) | 30.0 / 31.8 dB |

  The same table on the S25U (Adreno 830, APK `4bcadb9`, `energy-s25u-4bcadb9-2026-10-28.csv`):

  | rung | GPU clock | GPU Mcycles/frame | power | mJ/frame | skin max (status) |
  |---|---|---|---|---|---|
  | c 0.1, 1.0 scale | 758 MHz | 10.87 | 5.48 W | 91.3 | 43.5 °C (2) |
  | c 0.2 | 609 MHz | 8.32 | 3.95 W | 65.8 | 41.1 °C (1) |
  | c 0.5 | 411 MHz | 5.58 | 3.04 W | 50.7 | 37.8 °C (0) |
  | c 1.0 | 342 MHz | 4.30 | 2.64 W | 44.0 | 36.7 °C (0) |
  | c 0.1, 0.85 scale | 772 MHz | 10.95 | 5.16 W | 86.0 | 42.9 °C (2) |
  | c 0.1, 0.7 scale | 764 MHz | 10.76 | 4.86 W | 81.0 | 43.1 °C (2) |

  Both GPUs agree. The threshold is the lever and its returns diminish past 0.5 px. Resolution
  leaves the cycles unchanged (the 830 draw is 7.70 ms at full size and 7.44 ms at 0.7). At 60 fps on
  stump, c 0.1 reaches status 2 within 4 minutes, c 0.2 status 1, and only c 0.5 stays at 0. The
  S25U's skin thresholds are the S24U's (40 / 42 / 45 °C).
  The S24U fuel gauge refreshes `current now` only about every 10 s, so the power column is
  ±10–15 % on a 4-minute run. Example: c 1.0 at 1.0 and 0.7 scale drew the same 39.5 mAh by charge
  counter, yet their mean power reads 2.99 and 2.08 W. Cycles are the precise column.
  The threshold is the lever, and it has diminishing returns. 0.1 → 0.2 saves 17.6 mJ/frame for
  5.4 dB; 0.2 → 0.5 saves 18.3 mJ for 6.9 dB. 0.5 → 1.0 saves about 2 mJ (within noise) for 4.5 dB:
  the GPU is near its clock floor, and display and CPU (about 2–3 W) dominate.
* **Internal resolution is not an energy lever on these scenes.** With 51 % fewer pixels (0.7
  scale) the draw is 3 % shorter (5.79 → 5.60 ms) and the cycles do not change. The draw of a heavy
  scene from the bench camera is bound by per-splat primitive work, not by fill. On truck, 864×1872
  at 60 fps and 540×975 uncapped (2.9× fewer pixels) both cost 4.2 M cycles per frame (S18). The
  resolution rungs cost 10–13 dB and buy nothing.
* **The S24U's skin thresholds are 40 / 42 / 45 °C** (LIGHT / MODERATE / SEVERE,
  `dumpsys thermalservice`, skin sensor). Android's headroom is 1.0 at SEVERE and moves 1/30 per
  °C, so headroom = (T_skin − 15) / 30: LIGHT = 0.833, MODERATE = 0.90. This matches the logs:
  status 2 at headroom ≈ 0.9 (S11), 40.6 °C → status 1 and 42.8 °C → status 2 (S17).
* **Headroom moves slowly, and steady temperature follows power.** Ours on stump, capped at 53 fps
  (S17, the app's 1 Hz headroom column): 0.63 at the start, 0.72 after one minute, then +0.01–0.02
  per minute (0.3–0.6 °C/min) to 0.85 at minute 9. Status 1 appeared exactly as headroom crossed
  0.833. Truck at 64 fps settled at 0.71 within two minutes and stayed there. A step changes power
  at once, but skin temperature answers only over minutes. S17's 10-minute runs put steady skin
  temperature on the S24U at about 30.8 °C + 2.5 °C/W × whole-phone power (36.6 °C at 2.29 W,
  40.6 °C at 3.86 W; S25U about 1.8 °C/W). By that model the rungs above would settle near 44 °C
  (c 0.1), 41.5 °C (c 0.2) and 38.7 °C (c 0.5) at 60 fps on stump. Only c 0.5 stays below LIGHT,
  and the controller's A/B run will test this prediction.
* **The app can read the kgsl clock itself.** On the S24U (Android 16), `/sys/class/kgsl/kgsl-3d0/
  clock_mhz` opens from the app process (`untrusted_app` domain). The bench CSV's new `gpu_mhz`
  column was valid in every row of a test run (`4bcadb9`) and of the S25U runs. With `run-as`,
  `devfreq/cur_freq` and the GPU `temp` are readable too; `gpuclk` is not. Other vendors' kernels
  may differ, hence the fallback in decision 3.

**Decision**:

1. **A discrete ladder of contribution-cull thresholds, one index, one step at a time.**
   Rungs: 0.1 → 0.2 → 0.5 px, with 16-bit keys (ADR-0008 phone defaults at rung 0). A 0.3 px rung
   is added if measuring it shows a price between its neighbours. The controller moves an index,
   not a continuous budget, because every rung has its own measured price in dB and mJ/frame, and
   a PID loop on a continuous splat budget would hide that price table.
   * No rung below 0.5 px: it saves almost no energy (above).
   * SH 1 → 0 is inserted where its measured price fits, on SH 1 scenes only.
   * 32-bit keys without culling stay outside the ladder (screenshots, comparisons).
   * The frame-rate target is a parameter, not a rung. If the last rung still runs too hot, the
     controller reports it (logged reason `ladder_exhausted`) instead of lowering the frame rate
     on its own.
   * **Rung order rule**: each step down must save the most energy per frame for each dB it costs,
     measured on battery at a fixed rate and against exact rendering at the `quality_ab.py` poses.
2. **Two loops.**
   * *Frame guard* (fast, evaluated each second over the last second): step down immediately when
     more than 10 % of frames missed the target interval **and** GPU time was at least 90 % of the
     interval. The second condition means the GPU was the bottleneck, not a UI or CPU hitch.
     At most one step per second.
   * *Thermal loop* (slow): input = `AThermal_getThermalHeadroom` with a 30 s forecast, polled at
     1 Hz (faster polls return NaN), plus its slope over the last 60 s. Thresholds come from
     `AThermal_getThermalHeadroomThresholds` (API 35+). Below API 35, use (T − 15) / 30 with the
     skin thresholds above. The setpoint band sits below LIGHT:
     step down when the forecast is ≥ LIGHT − 0.017 (≈ 0.5 °C below LIGHT) **and** the slope is
     still ≥ 0, at most once per 60 s. Skin temperature lags power by minutes, so a faster loop
     would keep stepping down while the heat from earlier frames arrives, and overshoot.
     Step up when the forecast is ≤ LIGHT − 0.083 (≈ 2.5 °C below) **and** the dwell at this rung
     has passed **and** the GPU clock is below 60 % of its maximum. Backoff: a step up followed by
     a step down within 120 s doubles that rung's dwell (60 s initially, at most 10 min). At the
     drift measured above (0.01–0.02/min), the 2 °C band is several minutes wide, so the loop
     cannot chatter.
3. **Inputs**: frame present times and GPU timestamps (already recorded), thermal headroom
   forecast and status, and the kgsl GPU clock. **Logged but not control inputs**: battery current
   × voltage (`BatteryManager`; the Samsung gauge refreshes about every 10 s, and the reading
   includes display and CPU) and GPU busy % (shown above to reflect the governor's target rather
   than the work). Both are evidence columns for energy per frame. If the clock cannot be read, the
   step-up gate falls back to the dwell alone.
4. **Internal resolution is not a rung.** It cost 10–13 dB and saved no measurable work on
   either scene (above). The surface keeps the 0.6× panel size from Phase 3. Should a fill-bound
   case appear (close-up views, other GPUs), the path is `SurfaceHolder.setFixedSize`: the
   compositor scales the layer for free on its overlay path, while an offscreen target plus an
   upscale blit would add a full-screen write and read per frame.
5. **Code layout**: the policy is pure C++ with no platform includes:
   `splatcore/quality_controller.h`, fed one sample struct per frame, returning the rung and the
   reason for a change. It is unit-tested in ctest on synthetic traces (heat ramp → steps down;
   cool-down → steps up after the dwell; oscillation → backoff; frame-miss fast path). The Android
   host samples AThermal, kgsl and BatteryManager and applies rungs. The same policy can replay a
   recorded bench CSV offline, so a ladder can be evaluated against recorded traces before it
   runs on a phone.
6. **Every decision is logged**: rung, reason (`frame_miss`, `thermal_high`, `thermal_low`,
   `ladder_exhausted`), the forecast, the clock, and power, as bench CSV columns, and to logcat in
   interactive use.

**Implementation (S19)**: the policy is `splatcore/quality_controller.h`, with 15 ctest traces. One
of them closes the loop through a first-order skin-temperature model built from the steady states
above: c 0.1 → c 0.2 → c 0.5 within three minutes, then no further change. `splatconv replay
<bench.csv>` runs the policy over a recorded bench CSV. The Android bench runs it with the
`bench_controller` extra (`bench_android.py --controller`, files labelled `-ctl`). Details the
decision above left open:
* **One headroom call per second.** AThermal returns NaN when polled much faster than 1 Hz, so
  the app alternates the 30 s forecast (control input) with the current headroom (the column
  every earlier CSV logged). Each value updates every 2 s, which is plenty for a loop that acts
  at most once per 60 s.
* **Inputs found at run time.** LIGHT comes from `AThermal_getThermalHeadroomThresholds` (looked
  up with `dlsym`, since minSdk is 33), with (40 − 15) / 30 as the fallback. The clock maximum is
  the largest entry of kgsl `gpu_available_frequencies` (else `max_clock_mhz`). The CSV header
  records both, and where LIGHT came from.
* **Edge rules.** Until the controller has 20 s of forecasts there is no slope, which counts as
  rising, so a phone that starts hot steps at the first usable forecast (20 s in, see the S20
  amendment below). A thermal step down waits 60 s after any step, frame-guard steps included.
  `ladder_exhausted` is reported at most once per 60 s.
* **Exact replay.** The app feeds the controller the values the CSV prints (rounded to 3
  decimals). Each row carries the rung, the reason, the forecast and which reading the frame
  took, so `splatconv replay` on a controller run must reproduce its logged decisions and exits
  1 if it does not.

**Amendment, measured before the first phone run: the frame guard waits for 4 bad windows out
of the last 6.** Replaying the policy as decided above over the 32 recorded capped runs on both
phones (S17–S18, fixed rung, ≥ 100 s), the single-window frame guard would have stepped down in 8
of them. Every one of those was a clock-governor dip, not an overload. The kgsl clock drops for
1–3 s, nearly every frame misses, and the clock recovers on its own:
* S24U stump 734×1591 at 50–52 s: headroom 0.79, 629 → 500 MHz, 100 % of frames missed for 2 s and
  39 % in the third.
* S25U stump 864×1872 at 25–27 s: 734 → 607 MHz, against a 1200 MHz maximum.

A wrong step costs minutes at the lower rung, because stepping back up needs the forecast 2.5 °C
below LIGHT plus the dwell. The guard now steps when 4 of the last 6 one-second windows were bad.
Replay counts of runs that would step, by rule: 1 of 1 → 8, 3 of 5 → 1 (the 2.4 s dip above),
4 of 6 → 0, 5 of 8 → 0. A rung that cannot hold the target misses window after window and still
steps within about 4 s (ctest trace).

The one sustained shortfall on record does not trip the 4 of 6 rule. S25U stump with 6-vertex
quads at a 63 fps cap ran at status 2 with the clock capped at 660 MHz, 58.4 fps at minutes 8–10,
and missed in bursts of 1–3 windows. There the thermal loop acts first: open loop on that
recording, it steps at 45 s and 105 s.

Open-loop predictions for the A/B setting (stump, 864×1872, 60 fps cap, rung 0 recordings from
S18; these logged only the current headroom, so the 30 s forecast crosses a little earlier):
* S24U: steps at 1.4 min and 2.4 min.
* S25U: steps at 0.7 min and 1.7 min.
* Truck at its equal-work caps: never steps.

**Amendment, from the first phone run (S20, USB smoke test of `684f4fa`): forecasts from the
platform forecaster's first 20 s are ignored.** On the S24U (stump, 60 fps cap, skin 31 °C) the
controller stepped to c 0.2 at 2 s and back at 63 s. The forecasts, every 2 s from the start:
0.533, **1.066**, 0.766, 0.673, 0.633, 0.612, then 0.66–0.74 for the rest of the run, while the
current headroom read 0.567–0.60. The S25U shows the same transient: 0.423 flat for 4 s, then
0.614 against a current 0.453 at 6 s, settling near 0.53 by 14 s. 1.066 is 47 °C: the current
32 °C plus 0.5 °C/s × 30 s. The headroom API is computed in the framework, not the HAL (AOSP
`ThermalManagerService.TemperatureWatcher`, Apache-2.0; the logged values fit it). It samples skin
at 1 Hz into a 30-sample history and extrapolates a least-squares slope once it has 3 samples.
It drops the history after 10 s without a call.
A single 1 °C step of the sensor at the newest of n samples moves a 30 s forecast by
180 / (n (n + 1)) °C: 15 °C at n = 3 (the logged spike), 1.6 °C at 10, 0.43 °C at 20. That is
inside the 0.5 °C step-down margin. So:
* the thermal loop ignores forecasts taken less than 20 s after the forecaster started, and
  they do not enter the slope window (the spike would read as a falling slope for a minute);
* a gap of more than 10 s between forecasts restarts the warm-up, as the platform does. This
  matters for interactive, on-demand rendering, which can sit idle;
* the bench CSV header records `forecast_warmup_s`. Replay of controller runs recorded without
  it assumes 0, so they still reproduce exactly. `splatconv replay --forecast-warmup S` shows
  what another setting would have done.

The cost: a phone that starts above the band steps 20 s later, about 0.1 °C of extra heating at
the measured drift. On the S18 rung-0 recordings the open-loop step times above do not change.
Two ctest traces cover it: the recorded S24U start-up (no step) and a 30 s gap followed by a
spike (no step; the same spike without the gap steps).

**Result (S20, A/B on both phones)**: stump, 864×1872, 60 fps cap, 16-bit keys, APK `1d3faec`, on
battery over Wi-Fi adb, Do Not Disturb and heads-up notifications off, 20 min per leg. Every leg
started from skin ≤ 33 °C held for 3 minutes, so the legs started within 1.6 °C of each other
(`docs/benchmarks/controller-ab-1d3faec-2026-10-29.csv`; benchmarks README protocol step 4). The
S24U ran controller → fixed and the S25U fixed → controller.

| phone, leg | start | p5 fps (5–20 min) | status: first 1 / first 2 | skin max | GPU Mcycles/frame, min 10–20 | energy/frame | time at c 0.1 / 0.2 / 0.5 | PSNR min / mean (time-weighted) |
|---|---|---|---|---|---|---|---|---|
| S24U fixed c 0.1 | 32.1 °C | 60 | 308 s / 578 s | 44.1 °C | 9.11 | 83.1 mJ | 1200 / 0 / 0 s | 43.4 / 46.1 dB |
| S24U controller | 30.5 °C | 60 | never / never | 39.5 °C | 4.82 | 61.7 mJ | 357 / 202 / 641 s | 35.9 / 39.1 dB |
| S25U fixed c 0.1 | 31.8 °C | 60 | 387 s / 568 s | 44.3 °C | 9.67 | 77.1 mJ | 1200 / 0 / 0 s | 43.4 / 46.1 dB |
| S25U controller | 32.4 °C | 60 | 336 s / 908 s | 42.0 °C | 6.00 | 57.3 mJ | 331 / 331 / 538 s | 36.4 / 39.6 dB |

* **S24U: the prediction holds.** The controller stepped down at 357 s and 559 s, then stayed at
  c 0.5 for the remaining 11 minutes. The skin peaked at 39.5 °C in minute 7 and cooled to
  38.3 °C, with the clock at 366 MHz. It never reached status 1. A step up needs a forecast of
  ≤ 37.5 °C, so holding c 0.5 is correct. Fixed c 0.1 reached status 2 at minute 9.6 and was still
  rising at minute 20 (44.1 °C). Cost and savings: −26 % energy per frame and −47 % GPU cycles
  per frame (minutes 10–20), for 46.1 → 39.1 dB mean, time-weighted.
* **S25U: the ladder runs out at this frame rate.** Stepping to c 0.5 at 420 s cooled the skin
  from 40.6 to 38.1 °C. At 610 s the forecast reached 0.747 and the controller stepped up to
  c 0.2. The skin climbed 0.3 °C/min, and the controller stepped back down at 852 s. From 908 s
  the phone sat at status 2 *on c 0.5*, with the GPU at 342 MHz and the skin swinging
  40–41.6 °C with the AP temperature and the battery current, and `ladder_exhausted` was
  reported once a minute. At c 0.5 the S25U draws 2.6–3.0 W whole-phone (fuel gauge), most of
  it display and CPU: S18 measured 0.5 → 1.0 px saving only ~2 mJ/frame. The 4-minute rung
  table (37.8 °C at c 0.5) was not a steady state. Against fixed, the
  controller still cut energy per frame by 26 % and moved status 2 from 568 s to 908 s, for
  46.1 → 39.6 dB mean.
* **Every leg held the 60 fps floor** (p5 = 60, ≥ 99.5 % of frames on target), the fixed rung
  included. Since one triangle per splat (ADR-0010), stump at 60 fps is no longer frame-bound
  on either phone. The frame guard never fired in this A/B. The controller's measured effect is
  thermal state and energy, at a measured image cost.
* **Open: the step up was undone after 242 s, outside the 120 s backoff window**, so it did not
  lengthen the dwell. A thermal failure of a step up shows over minutes, and a backoff window
  near the skin time constant (~4–10 min) would catch it. Not changed on one event: in this
  run the forecast never came low enough to try again.
* The first S20 run (10 min, legs started from a single ≤ 36 °C reading, up to 6 °C apart) agrees
  on the S24U: the controller stayed at status 0 from a warmer start, while fixed reached
  status 1. It is inconclusive on the S25U: the controller leg started 6 °C cooler and reached
  the band only at 585 s. That run is kept in the same CSV as the protocol's negative result.

**Consequences**:
* **Prediction tested (S20)**: on the S24U the controller settles at c 0.5 below LIGHT, as
  predicted, while fixed c 0.1 reaches status 2. On the S25U at 60 fps no rung of the ladder
  stays below LIGHT over 20 minutes. There the frame-rate target is the remaining lever. It is
  reported (`ladder_exhausted`), not taken automatically.
* **The draw itself got cheaper since.** On the S25U the draw was half of our stump frame and
  carried the cycle gap to SuperSplat at the same pose (11.6 M vs 8.4 M cycles, S18 cold). It is
  primitive-bound, and one triangle per splat cut it by 12–19 % on both GPUs (ADR-0010). The rung
  tables above predate that change. The threshold lever and the ladder's order do not depend on
  it, but each rung's absolute mJ/frame will drop.
* **A/B protocol (Phase 4 gate)**: 10 min at the same frame cap, fixed rung 0 vs controller, on the
  S24U and S25U, public scenes ≥ 1.5 M. Report p5 fps and p99 frame time over min 5–10, % frames on
  target, peak thermal status, energy per frame, and the time-weighted image cost (each rung's PSNR
  × time at that rung). Gate: a 30 fps (S24U) / 60 fps (S25U) floor through minute 10, with a
  measured PSNR cost.

**Alternatives considered**:
* GPU busy % or GPU time per frame as the work signal: both are governor-shaped at a fixed frame
  rate (first table).
* Internal-resolution rungs: measured above; no saving on primitive-bound frames.
* Battery power as the control input: it is the true objective, but it includes the display and
  CPU, refreshes about every 10 s on the Samsung gauge, and is noisy. Logged as the evidence instead.
* Skin temperature from `dumpsys`: not available to apps; headroom is the public API for it.
* ADPF (`APerformanceHint`) work-duration hints: they tell the CPU scheduler and governor about
  CPU work. That complements this controller but adapts nothing on the GPU side. Revisit if the
  render thread's CPU time ever matters.
* A continuous PID controller on a splat budget: see decision 1.
* Asynchronous or amortized sorting (WebGL2 viewers sort on a worker): the order then lags the
  camera, which is an image-cost lever of a different kind. Kept out of the first ladder.
