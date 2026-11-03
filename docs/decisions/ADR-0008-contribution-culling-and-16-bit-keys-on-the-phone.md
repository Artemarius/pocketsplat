# ADR-0008 — Contribution culling before the sort, 16-bit keys on the phone

**Status**: accepted (2026-10-27 for image quality, desktop A/B below; 2026-10-28 frame time measured
on the S24U and S25U).

**Context**: On the heavy public scenes (2.7–4 M splats) the S24U spends 57–62 % of the frame in
the sort: 2.1–2.3 M visible keys × ~9 ns (32-bit) every frame. At equal pixels the SuperSplat viewer
is 1.8–2× faster there (`docs/benchmarks/public-baseline.md`). Two levers were open: fewer keys and
cheaper keys.

*Fewer keys.* Seen from the protocol camera, most visible splats of a trained scene are sub-pixel and
faint. The SuperSplat viewer drops a splat before sorting when `opacity · 2π√det Σ₂D < 1`, with the
raw opacity and the 0.3-px-dilated covariance. That is the coverage its own renderer draws, because
it does not compensate opacity for the dilation. PocketSplat does compensate (the S6 halo fix:
α' = α·√(det Σ / det Σ_dilated)). The coverage a splat adds to *our* image is therefore

  m = α' · 2π √det Σ_dilated = α · 2π √det Σ   (pixels of full coverage),

which does not depend on the dilation. Culling on m removes exactly the splats that change the image
least. SuperSplat's measure never culls a splat with α ≥ 0.53, however small, because
√det Σ_dilated ≥ 0.3 px. In our renderer such tiny opaque splats draw almost nothing.

*Cheaper keys.* 16-bit keys (2 radix passes) halve the sort on the Adreno 750 (8.3 → 4.4 ns/key,
`docs/sorting.md` §6). Their image cost had never been measured.

**Measurement** (`scripts/quality_ab.py`). The desktop host renders offscreen at the phone's pixel
counts with the phone bench camera, at six fixed poses: four yaws at the protocol distance and two
closer views. PSNR is against 32-bit keys without culling, as the minimum over the poses; *visible* is
the mean visible-key count over the 600-frame bench orbit. The reference re-rendered against itself
scores 75–106 dB, so the differences below are not noise. CSVs: `docs/benchmarks/quality-rtx3060-
b05913e-*.csv` (final build), `quality-rtx3060-explore-*.csv` (exploration build, see below).

Shipped phone setting (16-bit keys + c = 0.1) and its parts, 864×1872 (the phone protocol):

| Scene | Splats | 16-bit keys | cull c = 0.1 | **both (shipped)** | visible keys | both, phone background |
|---|---|---|---|---|---|---|
| museum1 | 0.31 M | 59.4 dB | 73.3 dB | **61.4 dB** | −2 % | 61.3 dB |
| counter | 0.97 M | 57.5 dB | 55.4 dB | **54.1 dB** | −23 % | 54.2 dB |
| truck | 1.65 M | 55.0 dB | 51.9 dB | **50.7 dB** | −45 % | 51.9 dB |
| drjohnson | 2.67 M | 63.3 dB | 65.1 dB | **62.9 dB** | −23 % | 62.1 dB |
| bicycle-4m | 4.00 M | 45.1 dB | 51.5 dB | **44.9 dB** | −43 % | 44.9 dB |
| stump | 4.05 M | 50.4 dB | 43.9 dB | **43.5 dB** | −44 % | 45.4 dB |

At 540×975 (the SuperSplat viewer's canvas on these phones) the shipped setting scores 45.9 dB
(bicycle-4m) to 60.6 dB (museum1), and visible keys drop by 1–29 %.

Cull threshold sweep at 864×1872 (PSNR min, visible keys vs. reference), and SuperSplat's measure
for comparison:

| Scene | c = 0.05 | **c = 0.1** | c = 0.2 | SuperSplat measure, 1 px |
|---|---|---|---|---|
| counter | 62.7 dB, −13 % | **55.4 dB, −23 %** | 49.3 dB, −36 % | 47.8 dB, −31 % |
| truck | 58.5 dB, −30 % | **51.9 dB, −45 %** | 46.4 dB, −58 % | 46.3 dB, −29 % |
| drjohnson | 70.6 dB, −13 % | **65.1 dB, −23 %** | 59.2 dB, −36 % | 59.6 dB, −34 % |
| bicycle-4m | 58.1 dB, −28 % | **51.5 dB, −43 %** | 45.5 dB, −57 % | 45.5 dB, −29 % |
| stump | 50.0 dB, −29 % | **43.9 dB, −44 %** | 38.1 dB, −59 % | 40.8 dB, −31 % |

* **Our coverage measure matches or beats SuperSplat's at equal splat counts.** stump at 1.48–1.51 M
  visible keys: 50.0 dB with ours (c = 0.05) vs 40.8 dB with theirs. bicycle-4m at 1.64–1.66 M: 58.1
  vs 45.5 dB. truck: 58.5 vs 46.3 dB. drjohnson is a tie (59.2 vs 59.6 dB at 1.40 vs 1.44 M). Their measure was run as a temporary shader variant (raw α × dilated area) and not kept.
* **c = 0.1 is the largest threshold that keeps every scene and pose ≥ 40 dB.** The worst pose (stump,
  43.9 dB without and 43.5 dB with 16-bit keys) looks identical by eye at full resolution. Its
  difference image, amplified 8×, is sparse speckle in the foliage.
* **The threshold is defined at the 864×1872 reference image and scaled by the viewport's pixel
  count each frame.** A fixed per-pixel threshold culls physically larger splats at lower internal
  resolutions: at 540×975 a fixed 0.1 px dropped stump to 36.2 dB min. Scaled, stump holds 47.8 dB
  and still sheds 29 % of its keys. A future resolution controller then cannot compound with the
  cull.
* **16-bit key quantization: log depth over the frame's AABB depth range.** The first 16-bit mode
  was t = d/(d+s), with s the AABB diagonal. It was replaced by ln d mapped over the nearest and
  farthest AABB-corner depths of the frame (clamped to the near cull), computed on the host. At
  864×1872, mean PSNR against 32-bit keys went 49.7 → 51.5 dB on bicycle-4m (min 42.7 → 45.1),
  63.6 → 67.2 on drjohnson and 55.2 → 56.7 on truck, and 53.1 → 52.0 on stump
  (`quality-rtx3060-keyquant-864x1872.csv`). An earlier check at 1280×720 on three scenes had
  wrongly suggested no gain.

**Decision**: The preprocess culls splats with m below `min_contribution`, in pixels of the 864×1872
reference image (`kContributionReferencePixels`), before they take a sort slot. It is exposed as
`SplatRenderer::set_min_contribution`, desktop `--min-contribution`, bench intent
`bench_min_contribution` and `bench_android.py --min-contribution`. The Android viewer's interactive
defaults are 16-bit keys and `min_contribution` = 0.1 (`kDefaultKeyBits`, `kDefaultMinContribution`
in `android_host.h`). The renderer library and the desktop host keep 32-bit keys and no culling by
default, so golden images and desktop references stay exact. Benchmark runs state both settings
explicitly (CSV header, `importance_threshold` column).

**Consequences**:
* **Measured on the phones (2026-10-28, build `86b7789`, cold, 864×1872, same-session controls;
  `docs/benchmarks/public-baseline.md`):**

  | Scene | S24U 32-bit → **shipped** | S25U 32-bit → **shipped** | Predicted (S24U) |
  |---|---|---|---|
  | stump | 33.8 → **75.4 fps** | 33.2 → **83.2 fps** | 53–65 fps |
  | bicycle-4m | 29.1 → **64.2 fps** | 30.6 → **71.1 fps** | 46–57 fps |
  | drjohnson | 30.3 → **49.1 fps** | 31.1 → **53.6 fps** | 40–45 fps |
  | truck | 84.6 → **168.2 fps** | 87.3 → **172.3 fps** | 123–161 fps |

  Better than predicted because the cull also cuts the preprocess's record writes and the draw (S24U
  stump: preprocess 5.6 → 3.9 ms, draw 6.7 → 4.3 ms, sort 17.1 → 4.9 ms).
* **Remaining fixed cost.** The preprocess still projects every splat in a visible chunk (~4 ms on
  4 M splats on the 750 after the cull). The next lever is a per-chunk bound on m that skips whole far chunks:
  max α·s_a·s_b over the chunk × (f/d_near)². It needs a new pack field (pack v3).
  **Follow-up 2026-10-28 (S16): done without pack v3, and the lever is small.** The pack-v2 chunk
  record's `importance_max` (α times the sum of the three scale pairs) already bounds α·s₁s₂, so
  chunk cull computes a conservative bound from data it has: `1.04·(importance_max + (min AABB
  extent/6)²/510) · 2π·fx·fy·√(1+u²+v²) / d_near²` (u, v from the preprocess's |ndc| ≤ 1.3 band;
  margins for unorm8 α and fp16 log-scales; derivation in `splat_chunk_cull.comp`). Desktop bench
  orbit, 864×1872, 0.1 px: visible splats identical in all 600 frames; chunks entering the
  preprocess −8.1 % on stump, −0.0 % on bicycle-4m, 0 % on drjohnson; images 82.6 dB min vs off
  with 32-bit keys (16-bit: 56.6 dB, the tied-key order above). Dividing the bound by 3 — about the
  best a tighter pack-v3 field (exact max α·s₁s₂, center AABB for d_near) could do — reaches only
  −13.7 % / −1.1 % / 0 %: the culled splats sit inside Morton chunks with survivors, not in whole far
  chunks. Pack v3 is not worth a format change; the bound stays on by default
  (`set_chunk_contribution_cull`, desktop `--no-chunk-contribution` to A/B).
* **Shimmer from tied keys.** Splats with equal 16-bit keys blend in the order of the compaction
  atomics, which varies between frames. The "repeat" PSNR measures it (54–70 dB at 864×1872).
  Exact 32-bit keys are one call away for screenshots and comparisons.
* **Quality is scene-dependent.** It is lowest on foliage-heavy outdoor scenes (stump, bicycle),
  where much of the image is sub-pixel splats. The thermal controller (Phase 4) can trade the
  threshold against frame time per scene.

**Alternatives considered**:
* SuperSplat's raw-opacity × dilated-area measure: worse at equal counts (above).
* A fixed screen-extent cull (`max axis < 0.5 px`): already in place, but it ignores opacity.
* A per-frame adaptive key width of ≈ log2(N/4) bits, as in the SuperSplat viewer: our radix uses
  8-bit digits, so the only widths are 16, 24 and 32 bits, and 16 is already within budget.
* Amortized or asynchronous sorting: the order lags the camera. Kept as a lever for the thermal
  controller (Phase 4).
