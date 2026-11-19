# SH 1 palette research: view-dependent colour at the SH 0 footprint

Status: **gate A passed** (2026-11-19, S29, §6); phases B and C open. Written 2026-11-19 against
`212b91b` (v0.3.0); measured at `c6f0a7f`. Where the implementation departs from §2, §6 says so. This note follows
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
  re-imported.
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
codes, used-code entropy, wall time, and wall time to reach the Lloyd-25 loss.

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

Training wall time, Lloyd 25 epochs (seconds; threads in brackets; upper bounds, see below):

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
* The question moves to training cost. Even on the laptop, plain Lloyd at k 4,096 adds 9–167 s per
  scene; the phone's import time comes in Phase C. Phase B (trainers, training cap) is where the
  work is.

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

## 7. Negative results

* **An auto-vectorized assignment kernel under MSVC** (2026-11-19): one scoring loop over a
  512-centroid tile plus a lane-blocked minimum ran at 0.59 G pairs/s on one thread (~6.5 cycles
  per pair) and 3.2 G pairs/s on 12, although the vectorizer report confirmed both loops vectorized
  (SSE2). Explicit SSE2 runs at 0.77 and 3.6. Details are in the negative results log of
  `benchmarks/README.md`.
