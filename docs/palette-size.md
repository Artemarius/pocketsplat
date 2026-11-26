# SH 1 palette size: 16,384 entries against 4,096

PocketSplat stores each splat's degree-1 spherical-harmonic colour terms (SH1, 9 floats) as a 16-bit
index into a palette of SH1 vectors, trained with e-means (3 epochs) while a scene is imported
([ADR-0017](decisions/ADR-0017-sh1-palette.md), [e-means against Lloyd](e-means-vs-lloyd.md)). The
palette has 4,096 entries at every import budget. Phase 10 asked whether 16,384 entries are worth
their training time, first for "all" imports (no splat budget), where scenes are largest.

**The result in one line**: 16,384 entries gain **+0.9 to +2.6 dB at the view the app opens with**,
on all six test scenes and every seed, and as much at the standard quality poses. The gain does not
grow with scene size, but the training time grows with it: about **4–5× the palette time** of
4,096 entries, on both phones. Rendering cost: preprocess +3–6 %, frame +1–2 % on the S25 Ultra.
S24 Ultra stump timings and gate C are pending (§4). The default stays at 4,096 until the owner
decides (ADR-0017 amendment).

## 1. Protocol

Six scenes from 0.31 M to 4.05 M splats, e-means with 3 epochs (the import's trainer), three seeds.
Each 16,384-entry palette is paired with the 4,096-entry palette of the same seed. All renders are
864 × 1872 (the phone's portrait frame), compared against the scene's direct SH 1 pack (no palette)
with 32-bit sort keys and no contribution cull. Training on the desktop used 4 threads at idle
priority (the palette depends on the thread count); the 4,096-entry packs came out byte-identical to
the ones the earlier Phase 10 sweep built.

Two sets of views:

* **Home view (deciding)**: the camera the app opens a scene with
  ([ADR-0014](decisions/ADR-0014-home-view-eye-by-view-score.md)), swung 0°, ±12.5° and ±25° about
  the up axis, at its own distance and at 0.6 of it: ten poses (`scripts/palette_home_ab.py`).
  It is close to the scene's fine detail, which is what users look at first.
* **Gate A poses**: the six orbit poses every palette and quantization change has been measured at
  (`scripts/quality_ab.py`, four at the scene's framing distance, two at 0.6 of it). They look at the
  scene from outside its capture, where floaters can fill the frame. Kept for continuity with
  ADR-0017's numbers.

Sources: `benchmarks/palette-size-f4f28ac-2026-11-26.csv` (gate A, `scripts/palette_ab.py --trainer
emeans --epochs 3 --k 4096 16384 --seed 1 2 3`) and `benchmarks/palette-size-home-f4f28ac-2026-11-26.csv`.

## 2. Image quality

Worst-pose PSNR against direct SH 1, mean of three seeds (dB), and the change from 4,096 to 16,384
entries. n / k is the number of splats per palette entry at 16,384.

| Scene | Splats | n / k | Home: k 4,096 | k 16,384 | Δ | Gate A: k 4,096 | k 16,384 | Δ | SH1 loss ratio |
|---|---|---|---|---|---|---|---|---|---|
| museum1 | 314,457 | 19 | 46.84 | 48.19 | **+1.35** | 47.58 | 48.83 | +1.25 | 0.63 |
| hornedlizard | 764,985 | 47 | 45.05 | 47.60 | **+2.55** | 52.00 | 54.37 | +2.37 | 0.46 |
| beetle | 1,681,983 | 103 | 45.89 | 47.07 | **+1.18** | 47.05 | 49.66 | +2.61 | 0.68 |
| truck | 1,653,927 | 101 | 46.89 | 47.94 | **+1.04** | 51.91 | 53.15 | +1.24 | 0.67 |
| giraffes_statue | 2,880,821 | 176 | 45.44 | 46.91 | **+1.47** | 42.59 | 46.03 | +3.44 | 0.68 |
| stump | 4,049,567 | 247 | 46.76 | 47.72 | **+0.96** | 52.60 | 53.54 | +0.94 | 0.70 |

* **Every one of the 180 paired home-view poses improves**, by +0.56 to +3.18 dB. The spread over
  seeds at 4,096 entries is at most 0.27 dB (beetle), so the gain is far outside it.
* The SH1 loss (mean squared distance of a splat to its entry) falls to 0.46–0.70 of the 4,096 loss.
  The PSNR gain follows it: hornedlizard, with the largest loss drop, gains most.
* **The gain does not grow with scene size.** museum1 (19 splats per entry) gains +1.35 dB, stump
  (247 per entry) +0.96. Training work grows with the splat count, so the price per dB is lowest on
  small scenes. A rule by imported splat count fits the data better than "only for all imports".
* hornedlizard is an SPZ import with only 106 K distinct SH1 vectors among 765 K splats: 860–892 of
  its 16,384 entries stay unused, and it still gains most.
* SH 0 (no SH1 at all) is at 29.6–38.7 dB worst pose: both palette sizes are far above it and above
  the 40 dB bar.
* Pack size: +147 KB (12 B per extra entry), nothing per splat.

### Where the gain lands

Per-pixel error against direct SH 1 at the home view (seed 1), on one shared colour scale, over the
reference in dimmed grey. Below each overview: the window where 16,384 entries gain most and the one
where they lose most, both chosen from the error maps with no hand-picking, enlarged × 3, with the
rendered pixels under the error maps (`scripts/make_palette_error_media.py`).

<img alt="hornedlizard at the home view: the direct SH 1 render of the lizard's spiny back, then error maps for k 4,096 (46.85 dB) and k 16,384 (49.07 dB). At 4,096 bright error spots of 6 or more 8-bit levels sit on the lit ridge of the back; at 16,384 most of them are gone. The gain window: 41.1 against 45.1 dB. The loss window, on the dark ground: 54.2 against 51.5 dB, both nearly black on the scale" src="media/palette-size-hornedlizard-home.webp" width="846">

<img alt="stump at the home view: error maps for k 4,096 (46.92 dB) and k 16,384 (47.81 dB). The error at 4,096 sits on the sunlit leaves at the top of the frame; at 16,384 the same spots are dimmer. The gain window on the stump's top: 44.8 against 46.5 dB. The loss window on the ground: 48.8 against 48.7 dB" src="media/palette-size-stump-home.webp" width="846">

* The 4,096-entry error is not spread evenly: it peaks at 4–6+ levels on bright, view-dependent
  surfaces (the lit ridge of hornedlizard's back, stump's sunlit leaves), where a splat's SH1 vector
  is far from its entry. 16,384 entries remove most of these peaks.
* Side by side, the rendered crops are hard to tell apart at a glance: the gain is in the error
  peaks, not in a visible change over the whole frame. Where 16,384 entries lose, the error is
  below one level at either size.

## 3. Training time

Palette training plus the final assignment of every splat, e-means 3 epochs, 8 threads, NEON kernel,
seed 1, two runs each (`benchmarks/palette-size-trainers-s24u-f3c295b-2026-11-26.csv`,
`…-s25u-f3c295b-2026-11-26.csv`, `scripts/palette_trainers.py run --adb`). Losses and dead entries
match the desktop's bit for bit. The work is 4× the point–centroid pairs.

| Phone | Scene | k 4,096 | k 16,384 | Ratio |
|---|---|---|---|---|
| S24 Ultra (deciding) | hornedlizard 0.76 M | 3.4 / 4.5 s | 15.1 / 16.7 s | 4.0× |
| S24 Ultra | stump 4.05 M | (19.6 s at import, ADR-0017) | pending | — |
| S25 Ultra | hornedlizard 0.76 M | 2.1 / 2.2 s | 12.2 / 11.5 s | 5.5× |
| S25 Ultra | stump 4.05 M | 13.0 / 13.7 s | 59.8 / 70.1 s | 4.9× |

* The time grows slightly faster than the pairs: the 16,384-entry table is 590 KB instead of 147 KB
  and no longer sits in a core's L2. The second S25U stump run reached thermal status 1 (skin 40 °C).
* Predicted from the S24U's pair rate (pending measurement): a 1 M import's palette stage from 4.5 s
  to about 19 s, a 4.05 M "all" import's from 19.6 s to about 80 s.

## 4. Rendering cost (gate C)

The renderer reads one 12-byte entry per visible splat in the preprocess pass; at 16,384 entries the
table is 192 KB instead of 48 KB. Stump at the default 1 M budget and at all 4.05 M, the app's
defaults (16-bit keys, contribution cull 0.1 px), 1872 × 864, 5-minute runs, one run per pack, on
the charger (`benchmarks/palette-size-gatec-s25u-bdc7953-2026-11-26.csv`). Packs from `splatconv
import` (the app's import), the APK unchanged (debuggable 0.4.0-bdc7953).

| Phone | Splats | Window | Preprocess k 4,096 | k 16,384 | GPU frame k 4,096 | k 16,384 | fps |
|---|---|---|---|---|---|---|---|
| S25 Ultra | 1 M | 0–30 s | 1.368 ms | 1.449 (+5.9 %) | 5.86 ms | 5.99 (+2.3 %) | 158.5 → 157.8 |
| S25 Ultra | 1 M | min 4–5 | 2.187 | 2.278 (+4.2 %) | 8.57 | 8.73 (+1.9 %) | 116.5 → 114.4 |
| S25 Ultra | 4.05 M | 0–30 s | 2.521 | 2.585 (+2.5 %) | 9.31 | 9.37 (+0.6 %) | 106.9 → 106.5 |
| S25 Ultra | 4.05 M | min 4–5 | 3.786 | 3.954 (+4.4 %) | 13.91 | 14.23 (+2.3 %) | 71.8 → 70.2 |
| S24 Ultra (deciding) | | | pending | | | | |

* Preprocess is 3–6 % slower in all four windows. One run each, and the 16,384-entry run of each
  pair came second (skin 0.2–1.2 °C warmer at the end), so part of it may be heat. S31's repeated
  S24U runs differed by 1.5 %.
* For scale: the palette saved 2–4 % of preprocess against direct SH 1 in ADR-0017's gate C. At
  16,384 entries that saving is about used up; the frame cost stays within 2.3 %.

## 5. Open, for the owner's decision

* S24 Ultra: stump training time and gate C (both packs, both sizes), then this table is complete.
* The choice: k 16,384 for every import (+1 to +2.6 dB at the home view, palette time ×4–5), for
  imports up to some splat count (the gain per second of training is largest on small scenes), or
  4,096 everywhere. Any change amends ADR-0017's "k 4,096 at every import budget".
