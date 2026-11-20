# SH 1 palette research: view-dependent colour at the SH 0 footprint

Status: **gate A passed** (2026-11-19, S29, §6); **gate B complete** (2026-11-20, S30, §3.1, §6:
e-means, 3 epochs); phase C open. Written 2026-11-19 against `212b91b` (v0.3.0); gate A
measured at `c6f0a7f`, phase B at `1cdd4cc`. Where the implementation departs from §2, §6 says so;
for e-means, §3.1. This note follows
the pattern of `coverage-mask-research.md`: the question, the design, the protocol, then results and
negative results as they are measured. Numbers enter this file only from committed CSVs.

## 1. The question

The app imports every scene at SH 1 (`sh_target = 1`), so user scenes are 40 B/splat packs, while the
real-time promise and the phone benchmarks are for SH 0 (28 B). The `sh1` array is one of the 12 B
per-splat arrays that set the Adreno 750's scene limit (ADR-0016), and at that limit it is a full
128 MB binding of its own.

Per-splat SH1 vectors are redundant across a scene. Can a shared palette of SH1 vectors, indexed by
16 bits per splat, give SH 1 image quality at the SH 0 footprint, and what does training that palette
cost at import on the phone?

| Pack | Per splat | At 1 M splats | At 11.18 M (750 limit) |
|---|---|---|---|
| SH 0 | 28 B | 28 MB | 313 MB |
| SH 1 direct (today) | 40 B | 40 MB | 447 MB |
| SH 1 palette (proposed) | 28 B | 28 MB + palette | 313 MB + palette |

A palette entry is 12 B (the current SH1 encoding), so 4,096 entries are 48 KB, 16,384 are 192 KB and
65,536 are 768 KB.

Prior art: codebooks for Gaussian attributes are established practice. Compact3D / CompGS
(https://arxiv.org/abs/2311.18159) vector-quantizes Gaussian parameters with k-means. Niedermayr et al.
(https://arxiv.org/abs/2401.02436) use sensitivity-aware clustering. PlayCanvas SOG stores an optional SH
palette with per-Gaussian labels
(https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/). The palette itself is not
new. What this work adds is the measurement on Adreno phones, and a comparison of trainers at a
dimension the newest one was not tested at (§3).

## 2. Design

### 2.1 Where the bits go

* **Index**: `scale_rot` word 2 is `snorm8(y) | snorm8(z)<<8` with the high 16 bits zero
  (`formats.md` §3). The palette index goes into those 16 bits. No per-splat array grows.
* **Palette**: stored in the existing `sh1` array and bound at the existing binding, with
  `palette_count` entries of 3 × u32 in today's encoding (9 × snorm8 of `sh[k] / sh1_scale`). The
  preprocess shader already reads word 2, so palette mode changes one index: it reads `sh1[3 * (w2 >> 16)]`
  where it now reads `sh1[3 * idx]`. The decode is unchanged.
* **Header**: `kPackVersion` 2 → 3. The 4 reserved bytes at offset 28 are also wanted for the
  antialiased flag (open question in the roadmap), so they are split: low 24 bits `palette_count`
  (0 = direct SH 1), high 8 bits flags. Version 2 packs are rejected at load as before; scenes are
  re-imported. *Implemented differently (S31):* a v2 pack wrote zero in word 28 and in word 2's
  high 16 bits, so it is byte for byte a v3 pack without a palette; readers accept it as one and
  nothing needs re-importing (`formats.md` §3). v1 stays rejected.
* **`subset_pack`** copies `scale_rot` words, so indices survive; the palette is kept whole.
* **Coverage sidecar**: unaffected (bound to positions).
* The direct SH 1 pack stays. It is the quality reference and the fallback.

### 2.2 Training

Clustering runs on the float SH1 coefficients (9-D, coefficient-major) before snorm8 quantization, in
`splatcore`, on the CPU, inside the import's pack stage. The palette is trained once per import and
written into the `.pspk`.

Module `splatcore/palette.h`:

```
enum class PaletteTrainer { kLloyd, kEMeans };
struct PaletteParams { std::uint32_t k; std::uint32_t epochs; PaletteTrainer trainer;
                       std::uint64_t seed; std::uint32_t train_cap; };   // train_cap 0 = all splats
struct PaletteStats  { double mse; std::uint32_t dead; double used_entropy;
                       std::uint32_t epochs_run; double wall_ms; };
struct Sh1Palette    { std::vector<float> centroids;        // 9 * k
                       std::vector<std::uint16_t> labels;   // count
                       PaletteStats stats; };
Result<Sh1Palette> train_sh1_palette(const SplatCloud& cloud, const PaletteParams& params);
```

* One assignment kernel (`‖x‖² − 2 x·c + ‖c‖²`, argmin over k) shared by every trainer, so a comparison
  isolates the update rule. `std::thread`, no new dependency, written to auto-vectorise under MSVC,
  clang and the NDK's clang.
* Deterministic for a fixed seed and thread count.
* The import progress callback and cancellation apply between epochs.

### 2.3 Measuring quality before changing the format

Phase A needs no pack or shader change. `splatconv pack --sh1-palette K` trains a palette, replaces
each splat's SH1 by its centroid and writes an ordinary direct SH 1 pack (version 2). The existing
renderer and the `quality_ab.py` machinery then give the image-quality price of the palette. The
format and shader work happens only if that price is acceptable.

## 3. Trainers

**Lloyd** is the baseline: random initialisation from data points, full passes, empty clusters refilled
by splitting the largest.

**e-means** (Meta FAIR, arXiv 2611.09501 v1, 2026-11-06; https://e-means.github.io/; reference code in
the Faiss repository under `contrib/e_means`) is a mini-batch k-means with exponential-moving-average
updates. The paper reports reaching the 25-epoch Lloyd loss in 1.4 to 2.6 epochs on embedding datasets,
8 to 15× CPU wall-clock against Faiss, and far fewer dead clusters. These are the authors' numbers from
a preprint; none are reproduced here yet.

What is open for our case:

* The paper validates on embeddings of 200 to 3,072 dimensions. **SH1 is 9-D.** Nothing published says
  the epoch advantage holds there.
* Points per centroid. The app's default import budget is 1 M splats: k = 16,384 gives about 61 points
  per centroid (the paper's main ablation ratio), k = 65,536 only 15. Large "all" imports (up to
  11.18 M) support k = 65,536.
* At low dimension, bound-based exact accelerations of Lloyd (Hamerly, Elkan) are strong and are
  not what the paper compares against. An honest comparison at d = 9 should include one.
* A training subsample (a cap on points per centroid) is common practice and shortens every trainer.
  It is a configuration of the comparison, not an afterthought.

Summary of the method, as a reading aid only. **Implement from the paper, not from this list:**

* Per cluster, an EMA count `N_j` and an EMA sum `S_j`; the centroid is `S_j / N_j`.
* Per mini-batch: assign, accumulate batch counts `n_j` and sums `s_j`, then
  `N_j ← γ N_j + (1 − γ) n_j`, `S_j ← γ S_j + (1 − γ) s_j`, step `α = 1 − γ`.
* Initialisation `N_j = 1e-10`, `S_j = N_j · c_j⁽⁰⁾` from random data points.
* Cosine step schedule from `α₀` down to `0.1 · α₀` over all updates.
* Batch size and `α₀` from scaling laws in (n, k, epochs). **The formulas are in the paper's appendix
  and are not reproduced in this note.**
* Dead-cluster revival every 5 batches: clusters under 0.1 × the mean count are rebuilt by splitting a
  donor.
* Final centroids are the means of the last epoch's assignments.

Licence: Faiss is MIT, so its `contrib/e_means` may be read for constants. The licence header of that
directory is confirmed before anyone reads it, per ADR-0005, and no code is copied.

### 3.1 What was implemented (S30)

Licence check (2026-11-19): `contrib/e_means/e_means.py` and `base.py` carry Meta's MIT header and
refer to the repository's MIT `LICENSE`. The directory has no licence file of its own. One GPU
file there, `cute_kernel.py`, adapts Apache-2.0 code from QuACK; it was not read and is not needed.
The paper itself is CC BY 4.0. The two MIT files were read for constants and for points the paper
leaves open, described in prose; no code was copied.

`splatcore/palette.cpp` has three trainers over one assignment kernel and one worker pool:

* **Lloyd** as in Phase A.
* **Hamerly** (G. Hamerly, "Making k-means even faster", SDM 2010): Lloyd's iterations with one
  upper and one lower bound per point. Each centroid's distance to its nearest other centroid gives a
  second lower bound. The skip test carries a slack for the float error of the kernel's scores, so a
  skipped point is one the kernel would have labelled the same. Same update step, same ranges, same
  summation order: labels and centroids equal Lloyd's bit for bit (unit test).
* **e-means**, from the paper's Algorithm 1, Eq. 2–8 and Table 2. Where the paper is silent or the
  implementation departs from it:

| Point | Paper | Here |
|---|---|---|
| Batch law | Undivided law for n ≤ 2 M, "knee / 8" above | Knee / 8 at every n: the reference code picks it for d ≤ 16, and d = 9. Table 2's constants (0.0044 k^0.13 n^0.88 T^0.81); the reference code's refit differs in the third digit |
| Batch rounding | Nearest power of two, capped at n | The same; "nearest" by ratio, ties up (as the reference code) |
| α0 | Eq. 6 with B | From the final, rounded B (the reference code does the same) |
| Updates per epoch | M = T·⌊n/B⌋ | ⌈n/B⌉: the last partial batch is kept, so every point is in the last-epoch mean |
| Schedule counter | t = 0 … M − 1 | The same; the first update uses α0 |
| Initial N | 1e-10 | 1e-10 (the reference code: 2^-16) |
| N, S precision | fp32 | double, so untouched clusters can decay for thousands of steps |
| Split δ | bf16 machine epsilon | 2^-7, multiplicative, alternating signs per coordinate (the reference code: 1e-4). An all-zero donor gets an additive offset |
| Donors | Among all clusters, ∝ N, without replacement | Among live clusters only (N ≥ τ), ∝ N, Efraimidis–Spirakis keys |
| Revival | Every 5 batches | The same, also in the last epoch |
| Labels returned | Last-epoch assignments | A full assignment of every splat against the final palette, as for the other trainers (needed with a training cap anyway). It is in `wall_ms`, not in `train_ms` or `pairs` |
| Epochs | Fractional allowed | Integers only |

The laws were fitted at d = 96 with n/k ≥ 8. Here d = 9; museum1 at k 65,536 (4.8 points per entry)
would be outside the fitted range too, and that size is no longer swept.

Work is counted as point–centroid distance pairs, which do not depend on the machine: Lloyd evaluates
m·k per epoch, e-means m·k per epoch in batches, Hamerly m·k in its first epoch, then k² for the
centroid distances plus k per point it cannot skip.

## 4. Protocol

Scenes (SH 3 sources truncated to SH 1 by the normal import path):

* Shown and quoted publicly: museum1.sh1 (321 K, MIT), hornedlizard (786 K, MIT), beetle (2.07 M,
  CC-BY), giraffes_statue (3.0 M, MIT).
* Numbers only, never imagery: INRIA truck, stump, bicycle.
* Tiled stress scenes are never used for image-quality claims.

Image quality: phone-sized offscreen renders (864 × 1872) at the `quality_ab.py` poses, 32-bit keys, no
contribution cull. Reference = the direct SH 1 pack. Reported per scene: PSNR and the share of pixels
moved by more than 2 LSB, for (a) the SH 0 pack, (b) each palette size, plus (c) the reference
re-rendered (`@repeat`) as the noise floor.

Palette sizes: 1,024, 4,096, 16,384, 65,536. Five seeds each.

Trainer comparison (Phase B): Lloyd at 10 and 25 epochs, Lloyd with a bound-based acceleration,
e-means at 3 and 5 epochs, each with and without a training cap. Reported: MSE in SH1 space, dead
codes, used-code entropy, wall time, and wall time to reach the Lloyd-25 loss. Losses, dead codes and
entropy can come from the desktop, since they don't depend on the machine. **Wall times come from
the S24 Ultra** (CLAUDE.md §7; the owner approves each run): a headless trainer bench pushed to
`/data/local/tmp` like `sortbench`, fixed thread count, phone cool and unplugged, with clock and
thermal status logged.

Cost on the phone (Phase C): added import time on the S24 Ultra and S25 Ultra per scene and budget,
against the current import time of the same scene; preprocess-pass GPU time and frame rate, palette
pack against direct SH 1 pack and SH 0 pack, with the standard cold and 5-minute protocol; resident
memory from `dumpsys meminfo`.

Every run is a CSV under `benchmarks/` with device, build hash, scene and method, a summary sentence
in `benchmarks/README.md`, and a row in the negative-results log for anything that was slower or worse.

## 5. Phases and gates

**Phase A. Is the palette worth having? (desktop only)**
Palette module with the Lloyd trainer, `splatconv pack --sh1-palette`, quality table.
Gate A: at some k ≤ 16,384, palette against direct SH 1 reaches the repository's quantization bar
(PSNR ≥ 40 dB, `CLAUDE.md` §7) on the public scenes, on scenes where SH 0 does not already reach it.
If SH 0 itself is within the noise floor of SH 1 everywhere, or the palette misses the bar at every
size, the work stops here and is written up as a negative result.

**Phase B. Trainers (desktop only).**
e-means from the paper, a bound-accelerated Lloyd, the comparison of §4.
Gate B: the table is complete, whichever trainer wins.

**Phase C. Format, shader, phone.**
Pack version 3, palette mode in preprocess, byte-exact and golden tests, `formats.md`, then the phone
measurements. The ADR is filed as proposed once these numbers exist.
Gate C: on both phones, the palette pack's preprocess time is no worse than the direct SH 1 pack's,
and the added import time is recorded for the default 1 M budget and for an "all" import.

**Stretch.** fp16 palette entries (the palette is small enough that entries could be stored more
precisely than snorm8 at no per-splat cost); importance-weighted training; a Vulkan compute
assignment pass if CPU training at import turns out too slow on the phones.

## 6. Results

### Gate A: image quality (desktop, 2026-11-19)

Source: `benchmarks/palette-quality-c6f0a7f-2026-11-19.csv` (`scripts/palette_ab.py`). PSNR in dB,
minimum over the six `quality_ab.py` poses (mean in brackets), against the direct SH 1 pack at
864 × 1872, 32-bit keys, no contribution cull. Lloyd 25 epochs, seed 1, one seed per cell.

| Scene | Splats | SH 0 | k 1,024 | k 4,096 | k 16,384 | k 65,536 | Noise floor |
|---|---|---|---|---|---|---|---|
| museum1.sh1 | 314,457 | 33.8 (35.6) | 46.2 (48.6) | 47.6 (50.0) | 48.8 (51.2) | 50.2 (52.6) | 92.0 |
| hornedlizard | 764,985 | 35.3 (37.4) | 48.8 (49.6) | 51.3 (52.2) | 52.8 (53.6) | 54.6 (55.9) | 101.2 |
| beetle | 1,681,983 | 29.7 (36.5) | 46.1 (48.9) | 47.5 (50.1) | 49.9 (51.8) | 51.2 (53.1) | 75.1 |
| giraffes_statue | 2,880,821 | 29.6 (35.1) | 40.6 (45.6) | 42.6 (47.4) | 46.9 (49.2) | 46.3 (50.1) | 78.4 |
| truck (INRIA) | 1,653,927 | 38.0 (40.2) | 50.8 (51.8) | 51.8 (53.2) | 53.1 (54.4) | 54.2 (55.6) | 89.2 |
| stump (INRIA) | 4,049,567 | 38.7 (40.3) | 51.6 (52.5) | 52.7 (53.8) | 53.8 (54.8) | 54.6 (55.7) | 83.8 |

Training wall time, Lloyd 25 epochs, on the shared dev laptop (seconds; threads in brackets).
**Indicative only, not used for any decision**: the laptop ran other projects' jobs during the
sweep. Timings that count come from the S24 Ultra (§4).

| Scene | k 1,024 | k 4,096 | k 16,384 | k 65,536 |
|---|---|---|---|---|
| museum1.sh1 | 2.6 (12) | 9.1 (12) | 41 (12) | 98 (12, 14 epochs) |
| hornedlizard | 5.3 (12) | 21 (12) | 96 (12) | 420 (12) |
| beetle | 12 (12) | 50 (12) | 226 (12) | 917 (12) |
| giraffes_statue | 20 (12) | 84 (12) | 704 (4) | 3,181 (4) |
| truck | 12 (12) | 49 (12) | 236 (12) | 889 (12) |
| stump | 39 (4) | 167 (4) | 731 (4) | 3,054 (4) |

Verdict against gate A:

* SH 0 is below the 40 dB bar on every scene (minimum 29.6–38.7 dB). SH 1 matters most on
  giraffes_statue, beetle and museum1; truck and stump sit at the bar on average (40.2–40.3 dB mean)
  and fall below it at their worst poses.
* The smallest palette tried, 1,024 entries (12 KB), reaches 40 dB on all six scenes. giraffes_statue
  has the thinnest margin: 40.6 dB at 1,024, 42.6 at 4,096, 46.9 at 16,384. The rest are at 46.1 dB or
  more at 1,024. Gate A passes at every k ≤ 16,384.
* Each ×4 in entries gains 0.8–2.5 dB at the worst pose. giraffes_statue is the exception: +2.0 and
  +4.3 dB, then −0.6 dB from 16,384 to 65,536 (one seed; its mean rises by 0.9 dB).
* The question moves to training cost. Plain Lloyd is n · k · 9 multiply-adds per epoch: 3.7e10 for
  a 1 M import at k 4,096, 9.2e11 over 25 epochs. On the laptop it took 9–167 s per scene (indicative only).
  Phase B (trainers, training cap) is where the work is; it gets its timings from the S24 Ultra.

Deviations from §2 and facts found on the way:

* **Kernel.** The assignment kernel is explicit SSE2 on x86-64, not auto-vectorized code (negative
  result below). It sums the dot product as a tree in the same order as the portable loop, which
  arm64 uses until a NEON kernel can be tested on a phone (Phase C). Throughput: count × k ×
  (epochs + 1) pairs at 3.0–3.9 G pairs/s on 12 threads and 1.5–2.8 on 4. The laptop CPU ran at
  ~2.6 GHz. The 12-thread runs shared the CPU with another project's jobs; the 4-thread runs were at
  idle priority.
* **Lloyd details** (`splatcore/palette.cpp`): there is no split after the last update, so the
  returned palette is always cluster means. Lloyd stops early when no label changes. The labels are
  a final assignment of every splat against the trained palette.
* **Distinct vectors.** SPZ-imported scenes carry far fewer distinct SH1 vectors: hornedlizard has
  106,045 distinct snorm8 vectors among 764,985 splats, so 25,896 of 65,536 entries stay unused.
  museum1, truck and giraffes_statue have 85–100 % distinct.
* **Scale.** `sh1_scale` of a palette pack is the max-abs over the palette, never larger than the
  direct pack's. Snapping to centroids shrinks extremes, so snorm8 steps get finer.
* **Format, for Phase C.** `subset_pack` copies `sh1` per kept splat today. In palette mode it has
  to keep the palette whole instead (§2.1 assumed this happens by itself). `pack_test` pins
  `scale_rot` word 2 to zero for its test splat. The palette saves memory but does not raise the
  Adreno 750's 11.18 M splat limit, since `positions` and `scale_rot` are 12 B per splat too.

### Phase B: trainers, losses and work (desktop, 2026-11-19)

Source: `benchmarks/palette-trainers-1cdd4cc-2026-11-19.csv` (`scripts/palette_trainers.py`,
`tools/palettebench`), the six scenes of gate A as `splatconv pack` trains on them. k 4,096 with
seeds 1–5, with and without a training cap of 64 points per entry; k 1,024 and 16,384 with seed 1.
Hamerly runs give Lloyd's losses (identical iterations). Loss is the MSE of every splat against its
nearest entry, relative to Lloyd-25 with the same seed. Work is distance pairs during training,
relative to Lloyd-25. Laptop wall times are in the CSV, indicative only.

k 4,096, no cap, mean of 5 seeds (worst seed in brackets):

| Scene | Lloyd-3 | Lloyd-10 | e-means-1 | e-means-2 | e-means-3 | e-means-5 | Dead: Lloyd-25 / e-means-5 |
|---|---|---|---|---|---|---|---|
| museum1.sh1 | 1.077 | 1.021 | 1.054 | 1.011 | 0.998 (1.000) | 0.987 (0.989) | 0 / 0 |
| hornedlizard | 1.083 | 1.006 | 0.950 | 0.893 | 0.883 (0.889) | 0.881 (0.885) | 245 / 1.6 |
| beetle | 1.128 | 1.043 | 1.076 | 1.021 | 1.009 (1.011) | 0.997 (1.000) | 0 / 0 |
| giraffes_statue | 1.099 | 1.033 | 1.037 | 1.001 | 0.990 (0.993) | 0.979 (0.981) | 0 / 0 |
| truck | 1.125 | 1.041 | 1.066 | 1.017 | 1.005 (1.007) | 0.996 (0.997) | 0 / 0 |
| stump | 1.124 | 1.044 | 1.041 | 0.999 | 0.988 (0.992) | 0.979 (0.983) | 0 / 0 |
| Work vs Lloyd-25 | 0.12 | 0.40 | 0.04 | 0.08 | 0.12 | 0.20 | |

* **e-means reaches the Lloyd-25 loss in 3 epochs on four scenes and in 5 on all six**, on every
  seed: an eighth to a fifth of the distance work, plus one final assignment pass that every trainer
  pays. With that pass counted, e-means-3 costs 4/26 of Lloyd-25.
* **At equal work e-means wins clearly**: e-means-3 is 7–18 % below Lloyd-3 on every scene. The
  paper's advantage carries over to d = 9, which it did not test.
* **hornedlizard** (an SPZ import: 106 K distinct snorm8 SH1 vectors among 765 K splats) is where
  the revival matters. Lloyd leaves 245 of 4,096 entries without a splat; e-means-5 leaves 1.6, and
  its loss is 12 % below Lloyd-25. At k 16,384: 2,718 dead entries for Lloyd-25, 643 for e-means-5,
  loss 0.78 of Lloyd-25.
* Other sizes (seed 1): e-means-3 is 0.93–1.01 of Lloyd-25 at k 1,024 and 0.81–1.01 at k 16,384;
  e-means-5 is 0.94–1.01 and 0.78–0.99. Over all runs the scaling laws gave batches of
  1,024–32,768 and α0 = 0.016–0.35. They worked as published, with no tuning.
* **Hamerly saves 6–30 % of the distance work** at k 4,096 (25 epochs: 0.70 of Lloyd's pairs on
  hornedlizard, 0.89–0.94 elsewhere), 10–36 % at k 1,024, and 4–25 % at k 16,384. A negative result
  (§7).
* **Training cap of 64 points per entry** (262,144 at k 4,096): e-means-3 does 0.8–4 % of the
  Lloyd-25 work (museum1, barely capped, 10 %), but its loss is 1.06–1.08 of Lloyd-25 on beetle, giraffes_statue, truck and stump
  (museum1, barely capped, 1.008; hornedlizard 0.91). Capped Lloyd-25 is 1.07–1.08. The cap trades
  5–8 % loss for training work; whether that costs image quality is not measured yet.

Image check (`benchmarks/palette-quality-emeans-4cee8a0-2026-11-20.csv`, gate A protocol, seed 1):
e-means-3 palettes at k 4,096 render at 41.8 dB min / 47.0 mean on giraffes_statue (Lloyd-25: 42.6 /
47.4) and 52.4 / 53.1 on hornedlizard (Lloyd-25: 51.3 / 52.2). Both are above 40 dB. On giraffes_statue
the worst pose is 0.8 dB lower although the SH1-space loss is 1 % lower: the loss ranks palettes,
but it does not fix the worst-pose PSNR to a fraction of a dB.

### Phase B: wall times on the phones (2026-11-20)

Sources: `benchmarks/palette-trainers-s24u-e87cf48-2026-11-20.csv` (**S24 Ultra: the deciding
numbers**, CLAUDE.md §7) and `benchmarks/palette-trainers-s25u-e87cf48-2026-11-20.csv` (S25 Ultra,
second phone). The same palettebench binary (arm64, the portable scalar kernel: no NEON yet), 8 threads,
k 4,096, seed 1. Phones unplugged on Wi-Fi adb, heads-up notifications off, and every run started
from skin ≤ 32 °C. Do Not Disturb was on for the S25U run and for the first six S24U runs; after
that the owner ruled it out for good. Training and total time in seconds; total = training plus the
final assignment of every splat. Both phones produced identical losses and dead counts.

| Scene | Run | S24U training | S24U total | S25U training | S25U total | Loss vs Lloyd-25 | Dead |
|---|---|---|---|---|---|---|---|
| hornedlizard (765 K) | Lloyd-25 | 83.6 | 87.3 | 56.2 | 59.0 | 1.000 | 243 |
| | Hamerly-25 | 66.5 | 70.1 | 46.1 | 48.2 | 1.000 | 243 |
| | Lloyd-3 | 7.9 | 10.8 | 5.3 | 7.3 | 1.082 | 409 |
| | e-means-3 | 12.5 | 15.4 | 6.7 | 8.7 | 0.879 | 5 |
| | e-means-5 | 17.8 | 20.7 | 11.7 | 14.2 | 0.880 | 2 |
| | e-means-3, cap 64 | 4.8 | 7.7 | 2.5 | 4.3 | 0.914 | 26 |
| stump (4.05 M) | Lloyd-3 | 51.5 | 70.6 | 36.7 | 50.0 | 1.117 | 0 |
| | e-means-3 | 57.5 | 76.3 | 42.6 | 55.6 | 0.990 | 0 |
| | e-means-5 | 95.1 | 114.4 | 74.5 | 88.9 | 0.983 | 0 |
| | e-means-3, cap 64 | 5.0 | 20.9 | 2.5 | 16.4 | 1.081 | 0 |

Loss is the phone's own MSE relative to Lloyd-25 with seed 1: the phone's run for hornedlizard, the
desktop's for stump, where Lloyd-25 was not run on the phone. Phone and desktop runs are not bit-identical.
The phone's Lloyd-25 loss on hornedlizard is 0.03 % below the desktop's, most likely because
arm64 clang contracts multiply-adds into FMAs (`-ffp-contract=on` by default), so near-ties break
differently. Each platform is deterministic on its own. Phase C must not assume identical palettes
across platforms: a test that pins a palette needs `-ffp-contract=off` or a tolerance.

* **On the bundled-sample-sized scene, e-means-3 trains a better palette than Lloyd-25 in a sixth of
  the time**: 15.4 s against 87.3 s on the S24U (5.7×), 8.7 s against 59.0 s on the S25U. Hamerly-25
  is 18–20 % faster than Lloyd-25 on the two phones, for 29 % fewer pairs.
* The scalar kernel runs at 0.8–0.95 G pairs/s on the S24U and 1.2–1.4 on the S25U, on 8 threads.
  Every trainer is bound by n·k·passes. For an "all" import of 4 M splats, the final assignment
  alone takes 16–19 s on the S24U, and e-means-3 takes 76 s in total. The cap removes the training
  (5 s) but not that pass. A NEON kernel (Phase C) is the next lever.
* At equal pairs, e-means' training takes 12–58 % longer than Lloyd's on the S24U (12.5 vs 7.9 s on
  hornedlizard, 57.5 vs 51.5 s on stump), and 16–26 % longer on the S25U. On the laptop the
  difference was within noise. Suspect: e-means waits at a barrier after every mini-batch, and the
  equal static split makes each batch wait for its slowest core. The S24U's two little A520
  cores make that worse than the S25U's six equal performance cores. Lloyd pays the same wait once per epoch.
  A dynamic work split is the fix to try in Phase C.
* Thermal status stayed 0 on the S24U, with skin up to 39.4 °C after the long runs. On the S25U it
  reached LIGHT once, at the end of stump's Lloyd-3; its skin readings stepped by up to 8 °C within
  a run, which is the sensor behaviour noted in CLAUDE.md, not the load.

**Gate B: complete (2026-11-20).** Verdict for the import: **e-means, 3 epochs, k 4,096, no cap** for
the default 1 M budget. It is the best palette at a sixth of Lloyd-25's time: about 15 s on the
S24U for the bundled sample, with today's scalar kernel. For "all" imports, a NEON kernel and the
cap decide whether the added time is acceptable (Phase C, against today's import time).

### Phase C: format, shader and import on the desktop (2026-11-20, S31)

What landed, before any phone run:

* **Pack v3** (`formats.md` §3): header word 28 = `palette_count | flags << 24`, entry index in
  `scale_rot` word 2's high 16 bits, the palette in `sh1` (12 B per entry). `sh1_scale` is the
  max-abs over the entries in use, so a palette pack decodes bit for bit like the direct pack of
  the snapped cloud (`pack_test`, `PackPalette.*`). Readers reject unknown flags and indices
  outside the palette. v2 packs read as v3 direct packs (deviation from §2.1: v2 wrote zero in
  both places, so no re-import is needed). 1 M splats at k 4,096: 28.05 MB instead of 40 MB
  (museum1, 314 K splats: 8.89 MB against 12.61 MB).
* **Preprocess**: one push-constant flag selects `sh1[3 · (w2 >> 16)]` instead of `sh1[3 · idx]`.
  Desktop check on museum1 (e-means-3, k 4,096): palette pack against the snapped direct pack
  93–105 dB at the six `quality_ab` poses, inside the renderer's repeat noise (93–101 dB);
  validation clean. ctest `golden_palette_equivalence` (`scripts/palette_check.py`) pins the same
  on the synthetic SH 1 sphere and checks that the palette render is not SH 0 (34 dB apart).
* **Trainer**: a NEON assignment kernel (the SSE2 kernel lane for lane, separate multiplies and
  adds), a dynamic work split for e-means' per-batch assignment and for the final assignment
  (ranges handed out from a counter, so fast cores take more), and `#pragma clang fp contract(off)`
  for `palette.cpp`. The pragma targets the cause suspected in §6 for phone ≠ desktop palettes:
  the arm64 build fused the e-means EMA update (`s += α·x`, doubles) into `fmla`; objdump now shows
  no fused multiply-adds in the palette code. Desktop results are unchanged by all three (museum1:
  e-means-3 and Lloyd-3 MSE, revivals and entropy identical at 3 and 4 threads). **Not yet run
  on a phone:** the NEON kernel's correctness (`splatcore_tests` built for arm64), its speed, the
  effect of the dynamic split, and whether phone palettes now equal the desktop's.
* **Import**: `ImportOptions::sh1_palette` (e-means, 3 epochs) trains after the Morton reorder and
  writes a palette pack. Training reports its progress and can be cancelled between batches and
  in 16 slices of the final assignment. The app has an import toggle, off by default until the
  ADR, with "training SH palette… NN %" in the import status. Bench CSV headers record
  `sh1_palette`.

Gate C (preprocess no slower than direct SH 1 on both phones, import cost recorded) is open.

## 7. Negative results

* **Hamerly's bounds on SH1** (2026-11-19, S30): over 25 epochs at k 4,096 they skip only 6–30 % of
  the distance work (§6). Diagnosis on museum1
  at k 1,024, from a temporary instrumented build: the mean upper bound (distance to the own
  centroid) is 0.048 and the mean lower bound (to any other) 0.054. The second-nearest entry is only
  ~13 % farther than the nearest: SH1 vectors fill their space like noise, with little cluster
  structure. Each epoch the lower bound shrinks by the largest centroid drift, and that drift
  (0.02–0.13) is 5–10× the median (0.002–0.017), so it consumes the gap. Per epoch, 87–97 % of the
  points still needed a full scan. Elkan's or Yinyang's per-centroid or per-group bounds would follow
  the median drift instead, but Elkan needs n × k bounds, and e-means already does an eighth of
  Lloyd's work. Hamerly stays in the code: it reproduces Lloyd exactly, which makes it a free check.

* **An auto-vectorized assignment kernel under MSVC** (2026-11-19): one scoring loop over a
  512-centroid tile plus a lane-blocked minimum ran at 0.59 G pairs/s on one thread (~6.5 cycles
  per pair) and 3.2 G pairs/s on 12, although the vectorizer report confirmed both loops vectorized
  (SSE2). Explicit SSE2 runs at 0.77 and 3.6. Details are in the negative results log of
  `benchmarks/README.md`.
