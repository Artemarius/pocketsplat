# ADR-0011 — The controller in interactive use: paced frames, a clock-gated frame guard, a stricter step up

**Status**: accepted (2026-10-31, S21) after the phone runs below. Decisions 1 and 2 are confirmed
in hand-held use. No phone run has stepped up since, so decisions 3 and 4 rest on the replay and
the ctest traces. Supersedes three clauses of ADR-0009 decision 2: the
frame guard's bad-window test, the step-up condition and the 120 s backoff window. It also makes
explicit the fixed frame rate that ADR-0009's lever assumes. The ladder, the thermal step-down
rule and the forecast warm-up are unchanged.

**Context**: ADR-0009 was designed and A/B-tested on the bench: a deterministic orbit at a fixed
cap, every frame drawn. S21 ran it in interactive use for the first time, in a 20-minute hand-held
session on the S24U, and re-read the S20 S25U run. Four findings:

* **Interactive frames ran at the panel's rate, not at the controller's target.** The viewer draws
  on demand from the Choreographer, one frame per vsync while the camera moves. On the S24U's
  120 Hz panel a scripted 6 s drag on stump (c 0.1, 648×1404) presented 104 fps: 106 of 126
  intervals at 8.3 ms, 20 at 16.7 ms (SurfaceFlinger present times, APK `542f685`). ADR-0009's
  lever saves work per frame and assumes a fixed frame rate (every rung table and both A/B legs
  ran capped at 60 fps). At the panel's rate a step down mostly turns into more frames.
* **In hand-held use the frame guard cut quality for nothing.** Session: stump, 60 fps paced
  (decision 1), skin from 28.6 °C, APK `8395179` with ADR-0009's rules, the phone in hand on
  battery. It lasted 20 min (`trace-s24u-8395179-stump-full.sh0-handheld1.csv`,
  `scripts/session_android.py`; `splatconv replay` reproduces its 13 decisions). The user
  interacted 11.8 min of the 20 (59 %). The phone never left thermal status 0 (skin max
  37.2 °C, forecast max 0.792 against LIGHT 0.833). The frame guard still stepped down 7 times
  and the thermal loop stepped back up 5 times. The backoff damped the cycle (dwell 60 → 120 →
  240 → 480 s), but the session spent 4.3 min at c 0.1, 13.6 min at c 0.2 and 2.1 min at c 0.5.
  The steps bought no frame rate. Late intervals (> 1.1 × 16.67 ms) per threshold:

  | threshold | frames | late | GPU ms/frame | GPU clock | visible |
  |---|---|---|---|---|---|
  | c 0.1 | 8 798 | 20.8 % | 13.3 | 468 MHz | 0.79 M |
  | c 0.2 | 25 994 | 21.2 % | 12.9 | 409 MHz | 0.65 M |
  | c 0.5 | 3 411 | 18.2 % | 12.3 | 312 MHz | 0.49 M |

  By clock the share is flat too: 18–21 % in every 100 MHz bin from 231 to 799 MHz. Even the
  lightest views (< 0.2 M splats, 6.2 ms GPU) had 14.5 % late. The governor turns a cheaper
  frame into a lower clock, so GPU time per frame stays near 13 ms at every rung: ADR-0009's own
  first finding, now at work against the frame guard. Its "GPU time ≥ 90 % of the interval" test
  passed because the governor ran the GPU just fast enough. The step at 1199 s came at 5–9 fps
  with the clock at its 231 MHz floor.
* **The S25U's step up fired on an extrapolated forecast.** In the S20 A/B
  (`controller-ab-1d3faec-2026-10-29.csv`, controller leg, the app's readings every 2 s) the
  controller sat at c 0.5 from 420 s. The current headroom fell 0.833 → 0.793 by 570 s, then held.
  From 591 s it stepped down 0.793 → 0.790 → 0.780 → 0.770 (0.01 steps, about 0.3 °C, the sensor's
  resolution). The 30 s forecast is the current value plus 30 s of the platform's fitted slope,
  and it extrapolated those steps: 0.791 → 0.747 in 18 s. At 609.7 s it entered the step-up band
  (≤ LIGHT − 0.083 = 0.750, 2.5 °C below LIGHT) and the controller stepped up. The current
  reading was 0.770, 1.9 °C below LIGHT. The step up was undone 242 s later.
* **The skin answers a rung change in two parts.** After that step up (c 0.5 → 0.2, about +0.9 W
  by the S18 rung table) the forecast still fell for 10 s (0.729 at 620 s, the lag), then rose to
  0.785 by 705 s. A slow drift then took it to 0.798 by 846 s, and across the step-down
  threshold (0.816) at 852 s. Two-pole skin models driven by the fuel-gauge power, fitted to the
  four 20-minute S20 legs, put the fast pole at 80–160 s. The slow pole always ran to the edge
  of the fit (more than an hour), longer than the legs can resolve. ADR-0009's 120 s backoff
  window covers less than one fast time constant, so the 242 s failure did not lengthen the
  dwell.

**Decision**:

1. **Interactive frames are paced at the controller's target (60 fps).** The surface asks the
   compositor for that rate (`ANativeWindow_setFrameRate`, default compatibility). The render
   thread skips Choreographer callbacks earlier than the target interval minus half a 120 Hz
   period. Measured on the S24U (`1767039`): during a drag the panel switches to 60 Hz, and 125
   of 125 intervals were 16.67 ms. The bench keeps its own pacing and by default no vote. Without
   a vote the compositor kept both panels at 120 Hz at every cap (S25U, caps 60/48/30), as in
   every bench run before S21. `bench_android.py --vote-fps` asks for the cap, which the panel
   then follows (60 → 60 Hz, 48 → 48 Hz, 30 → 30 Hz). Interactive use therefore runs the panel
   at a lower rate than the S17–S20 bench runs: their energy numbers include a 120 Hz panel.
2. **A frame-guard window is bad only when the GPU clock is at least 0.9 of its maximum.**
   This is on top of ADR-0009's tests: > 10 % late, GPU time ≥ 90 % of the interval, 4 bad
   windows of 6. The clock used is the window's highest reading, else the last one. Below 0.9
   the governor still has headroom, and the frame times are its choice, so a step down would
   only lower the clock further. An unknown maximum or no reading leaves ADR-0009's test.
   Under thermal throttling the governor's ceiling can sit below 0.9 of the maximum, and then
   the guard is silent. The thermal loop is already stepping there (the one sustained shortfall
   on record, S25U at status 2 with the clock capped at 660 of 1200 MHz, is the ADR-0009
   amendment's example of exactly that).
3. **A step up needs the current headroom in the band too**, besides the forecast, the dwell and
   the clock gate: `max(forecast, current) ≤ LIGHT − 0.083`. The forecast keeps the step-down
   side, where anticipating heat is the point. Stepping up is the side where a mistake costs a
   cycle of heating, so it waits until the skin is cool, not until the extrapolation says it
   will be. The current reading is the platform's 0 s headroom, which the app already takes
   every 2 s (alternating with the forecast). It has no extrapolation and so no warm-up spike.
   A reading older than 10 s, or none (older recordings, other platforms), leaves the forecast
   alone to decide. During a steady cool-down the current value reaches the forecast's about
   30 s later, so the gate delays a step up by about that much. When the fall stalls, it blocks
   the step up. At 610 s on the S25U the skin was 0.6 °C warmer than the band. At the leg's
   average fall since 420 s (0.02 per minute) the current reading would have entered the band
   about a minute later, so the step up would have started 0.6 °C cooler.
4. **Backoff window 600 s.** A step down within 600 s of a step up doubles the dwell of the rung
   the step up left (60 s initially, at most 600 s, as before). 600 s is about four times the
   slowest fitted fast pole (160 s), so the fast response has settled. It holds the recorded
   242 s failure with margin and equals the dwell cap. A false backoff, where a step down 5–10
   minutes after a step up has another cause (a heavier view, a warmer hand), costs one doubled
   dwell. That is 60 → 120 s at the lower rung.

**Evidence (replay)**: `splatconv replay` over every recording of at least 100 s: 52 capped bench
runs (S17–S21, both phones) and the hand-held trace. With the rules each ran with, every
controller run reproduces its log. Under ADR-0011's rules (`--frame-guard-clock 0.9
--backoff-window 600 --step-up-current on`), two recordings decide differently:
* **Hand-held trace: no decisions at all.** It stays on c 0.1 for the 20 minutes. Its thermal
  state never came near the band, so that is the outcome the session called for.
* **S25U controller leg (S20):** the 610 s step up does not happen, and from 852 s the leg
  reports `ladder_exhausted`, as the recorded run did from 913 s. That is an open-loop result.
  After 610 s the recording heated at c 0.2, so its current reading never entered the band. At
  c 0.5 it would likely have done so about a minute later (decision 3). Either way the S25U's
  ladder runs out on this scene and frame rate (the ADR-0009 result; the frame-rate fallback is
  ADR-0012's subject).

The clock condition, checked against the governor dips that shaped ADR-0009's 4-of-6 rule: over
the 52 bench runs, with each phone's clock maximum supplied where an older header lacks it
(`--max-mhz` 1000 / 1200), the guard steps on a frame miss as follows:

| rule | runs that step |
|---|---|
| 1 bad window of 1 | 12 |
| 1 of 1 + clock ≥ 0.9 max | 1 |
| 4 of 6 | 1 |
| 4 of 6 + clock ≥ 0.9 max | 0 |

The clock condition alone removes 11 of the 12 dips.

**Correction to ADR-0009's frame-guard amendment.** It reports 4 of 6 → 0 of the recorded runs.
Today's replay finds one run that steps: S24U stump 734×1591 (S18, `f0f242c`), at 53.1 s, right
after the 2.4 s dip that the amendment cites for 3 of 5. The S19 `splatconv` (built from
`684f4fa`) agrees, so the replay did not change: the amendment's count was wrong for this run.
With the clock condition the run does not step (the dip's clock: 500 MHz of 1000).

**Implementation (S21)**: in `QualityControllerConfig`, `frame_guard_clock_fraction` = 0.9,
`backoff_window_s` = 600 and `step_up_needs_current` = true; `QualitySample::headroom_current`.
The Android host feeds the current reading on the frames that took it, in the bench and in
interactive use, quantized as the CSV prints it. Bench and interactive-trace headers record
`frame_guard_clock`, `backoff_window_s` and `step_up_current`. Replay of controller runs recorded
without these keys uses ADR-0009's rules (off, 120 s, forecast only), so they still reproduce
exactly. `--frame-guard-clock F`, `--backoff-window S` and `--step-up-current on|off` show other
settings. Four ctest traces (`quality_controller_test.cpp`):
* late frames at 770 MHz of 1000, which do not step, against 950 MHz, which does;
* the S25U shape: a forecast in the band with the current reading still out, which waits for the
  current reading;
* a stale current reading, where the forecast decides;
* a step up undone after 232 s, which backs off under the 600 s window and not under 120 s.

**Result (S21, phones)**: two 20-minute hand-held sessions on the S24U, the same person using
stump naturally, each from skin ≤ 33 °C held for 3 minutes, on battery
(`benchmarks/handheld-s24u-2026-10-31.csv`):

| session | rules, navigation | interacting | late intervals | time at c 0.1 / 0.2 / 0.5 | steps down / up | skin max, forecast max |
|---|---|---|---|---|---|---|
| 1 (`8395179`) | ADR-0009, whole-scene start | 59 % | 20.8 % | 4.3 / 13.6 / 2.1 min | 7 / 5 | 37.2 °C, 0.792 |
| 2 (`0592da9`) | ADR-0011, home view and trackball (ADR-0012) | 57 % | 21.9 % | 20.0 / 0 / 0 min | 0 / 0 | 36.4 °C, 0.729 |

Session 2 kept full quality for the whole session and stayed at thermal status 0, below the
band. Both traces replay exactly. Under ADR-0011's rules, session 1 replays with no decisions
too. The user saw no stutter in session 1, so the ~20 % late CPU-side intervals are jitter that
the swapchain queue absorbs, not visible misses.

S25U, the S20 controller setup under ADR-0011:
* stump 864×1872 at 60 fps, panel at 120 Hz, 20 minutes from skin 28.1 °C
  (`benchmarks/controller-s25u-0592da9-2026-10-31.csv`);
* one thermal step, c 0.1 → 0.2 at 957 s (forecast 0.846);
* thermal status 1 from 966 s, skin max 40.3 °C, no status 2, p5 59 fps;
* no step up, so the step-up gate and the 600 s backoff did not act;
* compared with S20's leg, which stepped at 331 s from 32.4 °C, the starting temperature and the
  day dominate.

**Consequences**:
* The frame guard's input in interactive use is a CPU-side interval: steady clock after
  `vkQueuePresentKHR`, with FIFO presentation. In the scripted drag above, 4.4 % of those
  intervals exceeded 18.3 ms while SurfaceFlinger presented 125 of 125 frames on vsync, so the
  swapchain queue absorbs some of that jitter. With decision 2 the guard acts only at a
  saturated clock. Measuring presentation instead (Choreographer frame times, or
  `VK_GOOGLE_display_timing`) is the next step if it ever matters.
* A phone run that steps up (a warm start, or a hot-then-cool trace) is still needed to see
  decisions 3 and 4 act on a device.
* The slow thermal pole is not identified by 20-minute legs. A longer fixed-rung run (60+ min)
  would size it and say whether a 600 s window misses slow failures. The cost of missing one is
  a duty cycle between two rungs at the period of the skin's response. The thermal step-down
  rule still bounds it below LIGHT.

**Alternatives considered**:
* Counting only intervals the render loop asked for (a frame was pending at the previous vsync).
  A proxy for it on the trace (the previous frame moved the camera) still stepped 8 times.
  Rounding the intervals to whole vsync periods (5.5 % ≥ 2 periods instead of 20.8 % > 1.1×)
  stepped 6 times, and so did both together. Most late frames followed a moving-camera frame,
  so the misses are the governor's, not gaps in touch input.
* Turning the frame guard off in interactive use: it would also be blind to a real overload, for
  instance a heavier scene at a saturated clock. Decision 2 keeps that case.
* Debouncing the forecast (several consecutive readings in the band): the S25U forecast fell on
  every reading for 18 s before the step, and once in the band it stayed there for the next
  10 s. The extrapolation was consistent, not noisy.
* Stepping up on the current headroom alone: it lags the forecast by up to 30 s on a real
  cool-down and gains nothing over requiring both.
* A backoff window tied to the slow pole: not identifiable from the recorded legs.
* A structural backoff rule (any step down that directly follows a step up backs off, however
  late): it punishes a step up that held for half an hour and was undone by a scene change.
  Within 10 minutes the step up is the likely cause; after that, other causes are at least as
  likely.
