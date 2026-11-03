# Cleaning pipeline (high priority — Phase 1 in `splatconv`, Phase 4 on device)

Goal: turn raw trainer output (any SH degree, floaters, near-invisible and degenerate splats, often
2–6 M primitives) into a compact, honest mobile payload at SH 0–1 — **without retraining**. Every
operation is a pure function `SplatCloud → SplatCloud` that also emits a `CleanStats` record (removed
count, bytes before/after, parameters), so a run produces an auditable report.

## Operations (default order)

| # | Op | Rule | Default | Rationale |
|---|---|---|---|---|
| 1 | `drop_invalid` | NaN/Inf in any field; zero-norm quaternion; scale ≤ 0 after exp | always | Cheap; protects everything downstream |
| 2 | `prune_opacity` | `sigmoid(opacity) < τ_α` | τ_α = 0.02 | Trainers leave many near-transparent splats; invisible, but they cost sort/draw time |
| 3 | `prune_scale` | `max(s) > f · diag(scene AABB)` (optionally `min(s) < ε`) | f = 0.05, ε = 0 (off, ADR-0007) | Huge translucent blobs are the classic floater/background artifact. The thin-axis test is off: trained scenes are mostly flat surface disks, and ε = 1e-6 · diag removed 8–69 % of real scenes (giraffes 5–10 dB PSNR vs raw) |
| 3b | `clamp_needle_scales` | Clamp the largest axis to `max(g · diag, r · s_mid)` where `s_mid` is the second-largest axis; removes nothing | g = 0.01, r = 6 | Reconstruction-noise needles (one axis far longer than the other two, aspect up to 10⁴:1 in real captures) render as rays crossing silhouettes in every faithful viewer (S10 streak investigation, `docs/benchmarks/README.md`). The aspect term spares disks/walls, whose second axis is comparable; the length floor spares ordinary fur-scale needles |
| 4 | `remove_floaters_sor` | Statistical outlier removal on centers: mean distance to *k* nearest neighbours `d_i`; remove if `d_i > μ + m·σ` | k = 16, m = 2.0 | Point-cloud SOR (PCL-style) works well because floaters are spatially isolated. Optionally weight by opacity so dense low-α clusters near cameras are also caught |
| 5 | `truncate_sh` | Keep bands ≤ target degree; drop the rest | target = 1 (or 0) | DC is the mean color already; truncation is what every mobile pipeline does, quality loss is small (published distillation results lose < 0.2 dB PSNR at degree 1) |
| 6 | `prune_budget` | Keep the top-*N* splats by importance `I = α · (s_x s_y + s_y s_z + s_z s_x)` (opacity × mean projected area proxy) | N = 1 000 000 | Deterministic way to hit the mobile budget; also produces the per-splat importance used by the adaptive controller on device (threshold instead of hard cut) |
| 7 | `reorder_morton` | Sort by 30-bit Morton code of position quantized within the scene AABB; compute 256-splat chunk AABBs and `importance_max` | always | Spatial locality for chunk culling and cache behaviour; deterministic layout |

Scene AABB for thresholds is computed **after** ops 1–3 (robust to a few far-away outliers: use the
1st–99th percentile box, not the raw min/max).

## Reporting

`splatconv clean --report report.json` writes, per op: input count, removed count, removed %,
parameters; plus overall: bytes as PLY / as SPZ / as GPU pack, SH degree in/out, final count. A
Markdown table is generated into `docs/benchmarks/cleaning-<scene>.md` by `scripts/clean_all.py` for
the test set — this becomes the README evidence.

Quality check (Phase 2, once the renderer exists): render the raw and cleaned scenes from 8 fixed
cameras on the desktop host; report PSNR/SSIM between them. Cleaning is "free" if PSNR ≥ 35 dB
against the raw scene at the same SH degree; op-by-op ablation once per test scene.

## On-device (Phase 4)

* Compute-shader versions of `prune_opacity`, `prune_scale`, `truncate_sh (1→0)` and `prune_budget`
  (they're per-splat predicates + compaction, shared with the crop tool's compaction path).
* `remove_floaters_sor` and `reorder_morton` run through `splatcore` on a background thread (CPU, kNN
  via a uniform grid hash — no dependency needed; nanoflann (BSD-2) is the fallback if the grid is
  slow), with a progress bar; 1 M splats should be a few seconds on the S24U's CPU.
* Every op is undoable once (swap buffers) and visible as a before/after count in the UI.

## Non-goals
Rendering-based pruning (contribution scores from training views), SH distillation by re-fitting, mesh
extraction, densification. Those need the trainer and belong to the companion project.
