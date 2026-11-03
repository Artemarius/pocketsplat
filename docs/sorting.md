# Why the depth sort is *the* mobile problem, and what PocketSplat does about it

## 1. Why sorting exists at all

A 3DGS scene is rendered by projecting every Gaussian to a soft 2D ellipse and compositing the
ellipses with alpha blending: `C = Σ_i c_i α_i Π_{j<i} (1 − α_j)`. That product term is the
transmittance of everything *in front of* splat `i`, so the sum is only correct if the splats are
composited in depth order. Standard hardware alpha blending (the "over" operator) gives the same
result when drawing back-to-front. Order matters — swap two overlapping splats and the color changes.
The reference implementation sorts per tile with a 64-bit (tile | depth) key; a hardware-rasterized
viewer sorts all visible splats once per frame by view-space depth and draws them in that order.

Consequence: **any camera motion invalidates the order**, so a naive viewer sorts ~all splats
every frame. Meshes don't have this problem (opaque, Z-buffer); this is the price of the
semi-transparent representation.

## 2. Why it's cheap on desktop and expensive on a phone

On an RTX 3060 a CUB radix sort of 1 M 32-bit key/value pairs is ~1 ms — an afterthought.
On a phone four things stack up:

1. **The fastest GPU sorts are not legal Vulkan on mobile.** State-of-the-art sorts (Onesweep, single-pass
   "decoupled look-back" scans) let workgroup *k* spin-wait on a flag written by workgroup *k−1*. That
   only terminates if the scheduler guarantees *k−1* is already running (forward progress). CUDA
   effectively guarantees it; Vulkan does not, and tile-based mobile drivers (Adreno, Mali) really do
   schedule workgroups in ways that deadlock or hang such code. The portable alternative,
   **reduce-then-scan** (histogram/upsweep → spine scan → downsweep/scatter), reads the keys
   ~2× per digit pass instead of once. On desktop that's a 1.2–1.4× penalty; on mobile, where the
   whole thing is bandwidth-bound, it's the difference between working and not.
2. **Bandwidth and shared memory are ~5–10× smaller.** Radix sort is a memory-bound scatter. 1 M
   key/value pairs × 4 passes of 8 bits × (read + write) ≈ 64 MB of traffic per pass structure, plus
   histograms. At mobile bandwidth (~50–70 GB/s theoretical, far less sustained) that lands in the
   5–10 ms range — the published WebGPU number on the Adreno 750 is 9.7 ms for 1.06 M splats.
3. **Subgroup sizes differ and libraries are tuned for one of them.** NVIDIA 32, AMD 32/64, Adreno 64
   (with wave32 modes), Mali 16, Apple 32. Sorts that hard-code subgroup size or need ≥ 20 KB of
   workgroup memory break or crawl elsewhere. `vulkan_radix_sort` (MIT, the obvious pick-up) now
   states outright that small-subgroup mobile GPUs are unsupported, needs Vulkan 1.4 push descriptors
   and Slang — so it's a benchmark reference, not a dependency.
4. **Thermal.** Whatever the sort costs cold, it costs ~1.5–2× after ten minutes on an S24 Ultra.

That is why "no suitable mobile solution" is true in the *packaged* sense: the algorithms are known
(reduce-then-scan; WebSplatter's wait-free hierarchical variant is the same idea in WebGPU), but there is
no permissively licensed native Vulkan implementation written for Adreno that you can drop in. Writing
one is ~300 lines of GLSL plus dispatch code — a well-bounded, portfolio-grade piece of work.

## 3. Why our targets make this tractable

With ≤ 1 M splats at SH 0 and a 15–30 fps floor on the S24U (33–66 ms budget), even a 10 ms sort is
fine. The sort only becomes the constraint at the S25U 60 fps target (16.6 ms budget): there we need
the sort ≤ ~5 ms so that preprocess + draw fit in the rest.

## 4. PocketSplat's sort design (Phase 2)

* **Algorithm**: reduce-then-scan LSD radix sort, key/value (key = depth, value = splat index),
  8-bit digits. Three compute passes per digit: `upsweep` (per-block digit histogram, stored
  digit-major), `spine` (one workgroup per digit scans that digit's row of block counts and writes
  the digit's total; until S16 a single workgroup scanned the whole array — §7), `downsweep` (scans
  the 256 digit totals into digit starts, re-reads keys, ranks them stably, scatters). Ping-pong
  buffers. The last digit pass writes only the values when the caller needs only the order (the
  renderer).
* **No forward-progress assumptions, no fixed subgroup size**: the downsweep finds same-digit peers
  inside a subgroup with an 8-round ballot match (any subgroup size) and ranks across subgroups
  through shared per-subgroup count rows — 16 by default, enough for any subgroup of ≥ 16 lanes
  (checked at init). The row count is a specialization constant because it sets the shared-memory
  footprint, which on Adreno decides how many workgroups a core keeps resident (§7). Workgroup size
  256, 16 elements per invocation. Two opt-in downsweep variants exist for per-GPU A/B tests: a
  shared-bitmask ranking (§7) and a block-staged scatter that writes in contiguous runs (§8).
* **Key width is a runtime option**: 32-bit (4 passes, order-preserving float→uint of view depth) or
  16-bit (2 passes, ln d quantized over the frame's possible depth range — the pack AABB corners in
  view space, clamped to the near cull — computed on the host per frame; a constant relative step
  wherever the content sits). 16-bit halves the cost; splats whose keys collide blend in the
  non-deterministic order of the compaction atomics. ADR-0008 measured the image cost against 32-bit
  keys and made 16-bit the phone default; the desktop host keeps 32-bit keys for golden images. The
  earlier d/(d+s) quantization (s = AABB diagonal) was 1–4 dB worse on three of four heavy scenes.
* **Contribution culling before the sort** (ADR-0008): the preprocess drops splats whose drawn
  coverage — compensated opacity × 2π√det Σ₂D, in pixels — is below a threshold (phone default
  0.1 px of the 864×1872 reference image, scaled to the actual viewport), so they never take a sort
  slot.
* **Sort only what's visible**: the cull/preprocess pass compacts visible splats and writes the count;
  every sort dispatch is `vkCmdDispatchIndirect` on that count. The CPU never reads it back.
* **Skip when static**: if the camera hasn't moved beyond a small threshold since the last sort, reuse
  the previous order. Editing sessions are mostly static; this is free fps.
* **Draw order**: ascending depth key, drawn from the end (or sort descending) so the farthest splat is
  drawn first for the "over" blend.

## 5. Options we are explicitly *not* taking (and why)

| Option | Why not (for now) |
|---|---|
| CPU sort + async upload (antimatter15-style) | 1 M elements on a mobile CPU ≈ 15–25 ms plus upload; caps you around 15–20 fps and causes popping while the order lags |
| Sort-free rendering (Weighted Sum, Mobile-GS OIR) | Requires retraining the scene under a modified model — coupling the viewer to a trainer we don't have yet. Revisit only when the companion trainer exists |
| Per-tile / per-pixel resorting (StopThePop) | Removes popping, adds ~4 % cost on desktop and a much more complex rasterizer; not needed for a viewer/editor |
| Fuchsia/AMD FidelityFX sort ports | Written for desktop assumptions; re-tuning them is more work than writing the small sort we need |

Measurements and negative results go to `benchmarks/README.md` — including the first on-device sort
timings for 256 K / 1 M / 2 M keys at both key widths.

## 6. Device sort cost (measured 2026-10-27)

Standalone `sortbench` cross-compiled for arm64 and run from `adb shell` on the Galaxy S24 Ultra
(Adreno 750, build `9b86bdd`), against the same binary on the RTX 3060. Random keys, values = indices,
correctness + stability verified against `std::stable_sort` at every size. *Presorted* re-sorts already
sorted data (coherent scatter — the only figure the tool reported before); *unsorted* restores the
random input before every run (scattered writes). Medians of 15 runs:

| Keys | S24U 32-bit presorted | S24U 32-bit unsorted | RTX 3060 32-bit unsorted |
|---|---|---|---|
| 100 K | 0.81 ms | 0.85 ms (8.5 ns/key) | 0.26 ms |
| 262 K | 1.97 ms | 2.14 ms (8.2 ns/key) | 0.59 ms |
| 1 M | 7.73 ms | 8.28 ms (8.3 ns/key) | 2.30 ms |
| 2 M | 15.53 ms | 16.92 ms (8.5 ns/key) | 4.99 ms |
| 4 M | 33.45 ms | 35.64 ms (8.9 ns/key) | 8.26 ms |

16-bit keys (two passes): S24U 1 M = 3.15 ms presorted / **4.39 ms unsorted (4.4 ns/key)**; RTX 3060
1 M = 0.65 / 0.96 ms. Findings:

* **Linear, ~8.3 ns/key at 32 bits on the 750** — 3.6× the RTX 3060 per key. 16-bit keys halve it
  on the phone (−47 %), the largest sort lever available; the in-app benchmark ran 32-bit keys.
* **Input order matters more on desktop at 16 bits (+49 %) than on the phone (+39 %)**, and little
  at 32 bits on either (+7–10 %); the renderer's depth keys arrive in Morton order, between the
  two bounds.
* **Timestamps are honored on the 750.** `sortbench --stamp-probe` records two dependent sorts with
  one intermediate timestamp per pipeline-stage flag: segment A (unsorted) = 8.3 ms and B
  (presorted) = 7.7 ms for ALL_COMMANDS, BOTTOM_OF_PIPE and TOP_OF_PIPE alike (COMPUTE_SHADER
  9.8 / 7.7 ms). The per-pass split of the in-app benchmark can be trusted on this GPU.
* **Galaxy S25 Ultra (Adreno 830, 2026-10-28, same binary)**: 32-bit 7.6 / 7.1 / 7.2 / 7.0 / 6.9 ns/key
  unsorted at 100 K / 262 K / 1 M / 2 M / 4 M (1 M = 7.22 ms), 16-bit 3.8 ns/key at 1 M (3.76 ms) —
  ~15 % cheaper than the 750. **The 830 honors intermediate timestamps in `sortbench` too** (A 7.1–7.2 /
  B 6.6–6.7 ms for every stage flag), yet in the app its cull and preprocess segments still read
  ~0 ms and their time lands in the "sort" segment (S25U stump: preprocess 0.18 ms vs 3.9 ms on the
  750). The collapse is therefore specific to the app's pass sequence (the indirect preprocess
  dispatch is the prime suspect), not the driver's timestamps in general; until it is explained,
  use frame totals and the "preprocess + sort" sum on the 830. *(Root cause and fix in §8.)*

**The "sort anomaly" in the September device CSVs was an instrumentation bug, not the sort.** Those
runs made the sort look up to 4× more expensive per key on one scene than on another. The visible
counter was a single host-visible buffer shared by both frames in flight and read by the host while
the GPU was clearing and refilling it, so 26–65 % of the logged counts were 0 and many others
partial (museum1: logged mean 201 K, true 309 K), and per-key costs came out inflated. With exact
counts (per-slot snapshot after the preprocess, `9b86bdd`), museum1's 2.93 ms sorts 309 K keys
(9.5 ns/key including the prepare pass and dispatch overhead) — consistent with the standalone
figure. Two side findings of the same investigation: on the RTX 3060 the host-visible counter's
per-splat atomics cost 0.49 ms of preprocess (0.575 → 0.087 ms once device-local, museum1
1600×900); on the Adreno 750 the same change is neutral (0.86 → 0.82 ms, unified memory). And
`RadixSort`'s descriptor cache keyed on raw `VkBuffer` values went stale when the Adreno driver
recycled handles across `sortbench` sizes (6 of 7 sizes "sorted" freed memory); it now keys on
unique buffer ids.

## 7. Sort rework (S16, 2026-10-28): parallel spine, 16 count rows, values-only last pass

`sortbench --profile` (new) writes a timestamp after every dispatch's barrier and sums them per
stage. It showed the old single-workgroup spine at about half of every sort on the RTX 3060 (1 M
32-bit keys: 0.92 of 1.76 ms) and at 35–37 % on the Adreno 750 (1 M: 1.44 of 4.12 ms at 16 bits,
2.89 of 7.86 ms at 32): one workgroup on one shader core walked 245 chunks of 256 histogram entries
while the rest of the GPU idled. The idea for the fix comes from splatkit's per-digit scan (MIT,
read for ideas, code written here independently).

Changes (`f0d8618`, `2002c7e`):

* **Spine**: one workgroup per digit scans that digit's row of block counts (rows are contiguous —
  the upsweep already stores digit-major) and writes the digit total; each downsweep workgroup
  scans the 256 totals into digit starts once (16 barriers). No cross-workgroup communication.
* **Downsweep**: the next batch's key/value loads are issued before the current batch's ballots and
  barriers; fold and clear of the per-subgroup count rows merged (3 barriers per batch instead of 4).
* **Count rows**: a specialization constant, default 16 (§4). Sizing it from the minimum subgroup
  size (4 rows on Adreno, 6 KB of shared memory instead of 18 KB) was the first attempt — **it made
  the sort 25–30 % slower on both Adrenos** (negative result, below).
* **Values-only last pass**: the renderer reads only the order, so the last digit pass skips the
  key writes (`keep_sorted_keys = false`): −11 % on the 750 and −27 % on the 830 at 1 M 16-bit keys
  (measured with 4 rows; `sortbench --keep-keys` times the old mode).

Unsorted random keys, medians of 15 runs, ns/key, the renderer's values-only mode (old sorts always
wrote keys). "Old (same day)" is the old sort with its spine launched as 64 workgroups of which one
works (the timestamp experiment below; same work as the 1-workgroup launch). Every phone row is in
`benchmarks/sortbench-s16-2026-10-28.csv`:

| GPU | Sort | 1 M 32-bit | 1 M 16-bit | 4 M 32-bit | 4 M 16-bit |
|---|---|---|---|---|---|
| Adreno 750 (S24U) | old (same day) | 7.86 | 4.12 | 7.58 | 3.95 |
| | parallel spine only | 4.93 | 2.64 | 5.15 | 2.85 |
| | + prefetch / merged clear (16 rows) | 4.38 | 2.40 | 4.68 | 2.60 |
| | **shipped `2002c7e`** | **4.34** | **2.36** | **4.21** | **2.29** |
| Adreno 830 (S25U) | old (same day) | 7.22 | 3.72 | 6.80 | 3.60 |
| | parallel spine only | 4.58 | 2.49 | 4.32 | 2.32 |
| | **shipped `2002c7e`** | **4.03** | **2.22** | **3.76** | **2.07** |
| RTX 3060 | old | 2.30 | 0.96 | 2.07 | 1.11 |
| | shipped | 0.74–0.96 | 0.42–0.55 | 0.98 | 0.43–0.56 |

−45 % (32-bit) / −43 % (16-bit) on the 750 and −44 % / −40 % on the 830 at 1 M keys. The laptop
RTX 3060 alternates between two clock states run to run, hence ranges. splatkit's in-app figure
(~1.4 ns/key at 16 bits on the 830, §6 / public-baseline) is still ~1.6× lower; its scatter ranks
2048-key blocks with 128 invocations through shared bitmasks instead of ballots.

**In the app** (cold, 864×1872, 16-bit keys, 0.1 px contribution cull — identical visible counts
before/after, preprocess and draw unchanged as a control): S24U sort 4.70 → 2.51 ms on stump
(1.20 M keys, 2.1 ns/key), 4.91 → 2.89 on bicycle-4m, 7.29 → 4.56 on drjohnson; the S25U's "sort"
segment (which also holds its preprocess, below) drops 1.6–2.8 ms. Frame rates are in
`benchmarks/public-baseline.md`.

**Count-row sweep** (`sortbench --count-rows`, 1 M keys, ns/key 32-bit / 16-bit; 16 rows run first
and last as a drift control, within 2 %):

| Rows (shared KB) | 4 (6) | 8 (10) | 12 (14) | 16 (18) | 20 (22) | 24 (26) | 28 (30) |
|---|---|---|---|---|---|---|---|
| Adreno 750 | 5.58 / 3.04 | 5.02 / 2.78 | 4.33 / 2.36 | **4.34 / 2.36** | 5.38 / 2.80 | 5.41 / 2.80 | 5.41 / 2.81 |
| Adreno 830 | 5.45 / 2.78 | 4.41 / 2.43 | 4.03 / 2.20 | **4.03 / 2.22** | 4.71 / 2.47 | 4.71 / 2.48 | 4.73 / 2.48 |

Both Adrenos have the same plateau: 12–16 rows (14–18 KB) fastest, a cliff above 18 KB, a slope
below. Shared memory sets how many 256-wide workgroups a core keeps resident; the reading consistent
with both ends is that fewer resident workgroups hide less latency and more resident workgroups
keep more scatter streams open at once (256 digit destinations each), which costs more than the
extra latency hiding buys. The RTX 3060 shows no row dependence within its clock noise.

**The 830's timestamps misattribute the spine.** On the 750 the spine segment reads what the totals
imply (old: 1.44 ms; new: 0.07 ms per 1 M 16-bit sort). On the 830 it reads 0.004–0.017 ms in every
build — the old 1-workgroup spine, the same spine launched as 64 workgroups (one working), and the
new 256-workgroup spine — and the time lands in the following downsweep segment; by totals the old
spine cost ≈ 1.2 ms there (profile totals: old 3.68 vs spine-only 2.46 ms). So the collapse is not about
single-workgroup dispatches; it is the same effect as the app's cull/preprocess segments reading ~0
on the 830 (§6). Until it is explained, only stage sums that end at a downsweep or at the frame end
are trustworthy on the 830. *(Explained in §8: the 830 writes `ALL_COMMANDS` stamps early;
`BOTTOM_OF_PIPE` stamps are honest.)*

**Ranking A/B: ballot vs shared bitmasks** (`8599af6`, opt-in `RadixRanking::mask`, after the
battery runs: phones warm, on battery, interleaved with ballot controls first and last; ns/key):

| | 1 M 16-bit | 4 M 16-bit | 1 M 32-bit | 4 M 32-bit |
|---|---|---|---|---|
| Adreno 750, ballot 16 rows (first / last) | 2.46 / 2.41 | 2.63 / 2.52 | 4.49 / 4.43 | 4.70 / 4.50 |
| Adreno 750, **mask 16 rows** (twice) | **2.22 / 2.22** | **2.11 / 2.11** | **4.18 / 4.19** | **3.96 / 3.96** |
| Adreno 830, **ballot 16 rows** (first / last) | **2.21 / 2.23** | **2.07 / 2.07** | **4.01 / 3.98** | **3.78 / 3.71** |
| Adreno 830, mask 16 rows (twice) | 2.35 / 2.35 | 2.25 / 2.26 | 4.28 / 4.27 | 4.07 / 4.06 |

The winner depends on the GPU: the bitmask ranking (one shared `atomicOr` per key, no subgroup
operations) is 6–10 % faster at 1 M keys and 12–20 % at 4 M on the 750, and 5–10 % slower on the
830, whose subgroup ballots are evidently cheap. Mask at 12 rows (14 KB) only ties the ballot on the
750 (2.43 / 4.35 ns at 1 M) and 20 rows equals 16 — the footprint still matters. At the app's ~1.2 M keys either choice
moves a frame by about ±0.2 ms, so the ballot ranking stays the default; a per-GPU choice is an
open item (it wants a cold USB A/B on the 750 first). The remaining gap to splatkit's in-app
~1.4 ns/key is therefore not the ranking method; the scatter itself (presorted input runs at
0.50–0.61 of the unsorted time at 16 bits on both phones) is the larger half.

## 8. Block-staged scatter, Adreno driver findings, per-GPU choice, 830 timestamps (S17, 2026-10-28)

Section 7 left the scatter as the larger half of the remaining cost. On the 750 the downsweep is
87 % of a 1 M-key sort, and presorted input sorts in 0.57 of the unsorted time. The direct
downsweep writes each 256-key batch straight to its global slots. With 256 digits that is about
one key per digit per batch, so almost every store opens its own cache line.

**`RadixScatter::staged`** (`3e22953`, opt-in) ranks the whole 4096-key block first (the same
ballot ranking, batch by batch), then stages it in shared memory in digit order and writes it back
with consecutive invocations on consecutive addresses. That gives runs of about 16 keys per digit
for uniform digits, and longer runs for the skewed high byte of depth keys. Values follow the same
slots. The tile's digit counts are the differences of adjacent spine offsets, so the tile-local
digit starts come out of the same 8-step scan as the global ones. The count rows and the staging
tile share one 16 KB array, so shared memory stays at 18 KB, the §7 optimum.

ns/key, unsorted, medians of 15 runs, the same binary on all three GPUs. Phones at thermal status
0 over USB, with the configurations interleaved and the direct/ballot control run first and last
(`benchmarks/sortbench-s17-2026-10-28.csv`):

| GPU | Downsweep | 1 M 32-bit | 4 M 32-bit | 1 M 16-bit | 4 M 16-bit | presorted 1 M 16-bit (ms) |
|---|---|---|---|---|---|---|
| Adreno 750 | direct, ballot (first / last) | 4.56 / 4.67 | 4.77 / 4.65 | 2.53 / 2.57 | 2.70 / 2.59 | 1.45 |
| | staged, ballot | 4.79 / 4.77 | 4.56 / 4.56 | 2.51 / 2.51 | 2.39 / 2.39 | 2.00 |
| | **direct, mask** | **4.19** | **3.96** | **2.22** | **2.11** | **1.42** |
| Adreno 830 | direct, ballot (first / last) | 4.01 / 3.99 | 3.77 / 3.73 | 2.21 / 2.26 | 2.09 / 2.07 | 1.19 |
| | **staged, ballot** | **3.32 / 3.32** | **3.06 / 3.07** | **1.75 / 1.76** | **1.62 / 1.62** | 1.34 |
| | direct, mask | 4.28 | 4.07 | 2.35 | 2.25 | 1.15 |
| RTX 3060 | direct, ballot (first / last) | 0.96 / 0.96 | 0.99 / 0.89 | 0.42 / 0.46 | 0.44 / 0.47 | 0.18 |
| | **staged, ballot** | **0.38 / 0.49** | **0.41 / 0.41** | **0.25 / 0.25** | **0.21 / 0.21** | 0.24 |

* **The three GPUs disagree.** Staging halves the sort on the RTX 3060 (the laptop's two clock
  states show in the 1 M 32-bit pair). It is worth −17 to −22 % on the Adreno 830. On the Adreno 750
  it is neutral at 1 M (−10 % at 4 M 16-bit) and 38 % slower on presorted input. The stage profile
  says the same about the downsweep: 0.96 → 0.37 ms on the RTX (1 M 32-bit), 2.04 → 1.59 ms on
  the 830 and 2.06 → 2.18 ms on the 750 (1 M 16-bit). On the 750 the cost of scattered stores was
  never the scatter's main cost: staging removes it and adds about as much back.
* **The bitmask ranking wins on the 750 cold as well**: 12–20 % faster than the ballot ranking
  (S16 measured 6–10 % warm), and 6 % slower on the 830, as in S16.
* **In the app (S24U, 864×1872, phone defaults, 45 s cold runs, alternating APKs that differ only in
  the default ranking; `benchmarks/ranking-ab-s24u-2026-10-28.csv`)**: drjohnson sort 4.04 / 4.03 ms
  (ballot) vs 3.77 / 3.82 ms (mask), frame rate +1 %. On stump the effect is within run-to-run noise
  (2.55 / 2.63 vs 2.49 ms; the other mask run drew at a lower clock on every pass and is not
  comparable). The app's keys arrive in Morton order, where the presorted-like share of the work
  narrows every difference.
* **In the app on the S25U (same protocol, direct vs staged APKs, `benchmarks/scatter-ab-s25u-2026-10-28.csv`)**,
  the 830's synthetic 17–22 % shrinks to 2–6 % of a segment that, on that build, also held the
  preprocess (see below). Stump: 4.73 / 4.94 → 4.63 / 4.82 ms. Drjohnson: 6.80 / 6.89 → 6.51 /
  6.24 ms. Frame rate: −1 % / +2 %. The phone sat at thermal status 1–2, so both rows carry noise.
  The app's Morton-ordered keys behave closer to the presorted case, where staging costs 11–16 %
  on this GPU.
* **Decision: no per-GPU sort variant.** In the app the best variant per GPU is worth 0.1–0.5 ms
  per frame (≤ 2 % fps) on heavy scenes. That is below run-to-run noise and not worth a device
  table. Ballot ranking with the direct scatter stays the default everywhere; both alternatives
  stay opt-in for sortbench A/B tests and for GPUs not measured yet (Mali, Xclipse).

**The 830's timestamp misattribution (open since S10) is the driver's `ALL_COMMANDS` stamp.**
`sortbench --profile --stamp-stage` measured the same 1 M-key sorts with different intermediate
timestamp stages (`benchmarks/sortbench-s17-2026-10-28.csv`, the `--stamp-stage` rows). On the 830,
`ALL_COMMANDS` and `TOP_OF_PIPE` stamps put the spine at 0.01–0.02 ms and fold it into the
downsweep. `BOTTOM_OF_PIPE` and `COMPUTE_SHADER` stamps read 0.17 / 0.085 ms (32 / 16-bit), in line
with the 750's honest 0.14 / 0.07 ms, and an extra full barrier before an `ALL_COMMANDS` stamp does
not help. So this driver writes an `ALL_COMMANDS` timestamp when the command is reached, not when the
work before it has finished. The renderer's `GpuTimer` now stamps pass boundaries at `BOTTOM_OF_PIPE`
(`ea99500`). The S25U's in-app split on stump went from cull 0.00 / preprocess 0.21 / sort 4.73 ms
to 0.25 / 2.81 / 2.42 ms, while the S24U split is unchanged (0.25 / 3.72 / 2.57). Per-pass columns
of 830 CSVs before `ea99500` keep the collapsed split; their frame totals were always right.

**Two shader shapes the Adreno 750 driver gets wrong.** Both were caught by `sortbench`'s
correctness check on the phone after passing on the RTX 3060 with validation layers on. This is
the reason sort changes are verified on every target GPU before they merge. The new mismatch
summary (values wrong / zero / indices missing) told misplacement apart from lost writes:

1. *Per-invocation arrays carried across barriers.* The first staged shader kept each invocation's
   16 keys, slots and destinations in `uint x[16]` arrays across the ranking loop's barriers. On the
   750, 99 % of the values came out wrong and a quarter of the indices were missing, with or without
   `[[unroll]]` / `[[dont_unroll]]`. The same shader with 16 scalar variables per phase
   (macro-expanded) is correct. The committed shader is the scalar form, and the comment in it
   says why.
2. *Subgroup-ranked staging.* This is the warp-level multisplit of Onesweep (Adinets & Merrill
   2022), written from the paper: each subgroup ranks its own contiguous chunk of the tile with
   subgroup operations only, which removes 45 of the 48 workgroup barriers per tile. It was correct
   on the RTX 3060 (validation clean, 0.18 ns/key at 1 M 16-bit, 2.3× the direct scatter) and in a
   CPU model of the algorithm at subgroup sizes 32, 64 and 128. On the 750 it was wrong in every
   form tried: arrays, scalars, subgroup barriers, workgroup barriers instead, the full-subgroups
   pipeline flag on or off, and lane indices taken from ballots instead of the builtins. A variant
   with a leader `atomicAdd` and a `subgroupShuffle` broadcast made `vkCreateComputePipelines`
   segfault inside `vulkan.adreno.so`. Dropped (negative result); it was not tried on the 830.

**`sortbench --subgroup-probe`** reports the subgroup layout of a 256-wide workgroup and checks the
mapping the ballot ranking's stability relies on: local index = subgroup id × size + invocation id,
which Vulkan does not guarantee. Adreno 750 and 830 report 64 lanes × 4 subgroups, the RTX 3060
32 × 8. All three are full and linear, and their builtins agree with what ballots see. Run it
first on any new GPU (Mali, Xclipse).

**Variable subgroup sizes (2026-11-01).** The downsweep keeps one count row per subgroup of the
256-wide workgroup, so it needs at least 16 lanes (16 rows; 28 is the shared-memory ceiling). The
Intel UHD (Tiger Lake) on the development laptop reports 8–32 lanes, and the sort refused it.
`RadixSort::init` now asks for 16-lane, full subgroups
(`VkPipelineShaderStageRequiredSubgroupSizeCreateInfo`, 1.3 core) on the downsweep pipeline of any
GPU whose minimum is below 16 and whose compute stage allows the request. Fixed-size GPUs are
untouched. On the Intel GPU the sort passes 100 K–4 M keys at both widths (15.2 ns per key at
32 bits, 7.4 at 16; `docs/benchmarks/devices.md`).
The phones still run the same pipelines, but the device now enables `subgroupSizeControl`. A gate run
(S25, `benchmarks/sortbench-s25-2026-11-01.csv`) put sortbench built from the commit before the
change next to HEAD, alternating old and new, three runs each, on both phones. Every size passes on
both builds. Median ns per key differs by −3.4 % to +2.0 % with no consistent direction (S24U 1 M
keys 4.64 → 4.70 at 32 bits, S25U 4.14 → 4.00). The S24U's 1 M-key rows run 2–4 % above the S17
table with either build, so that offset is drift between days, not the change.
