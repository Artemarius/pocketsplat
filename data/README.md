# data/ — test models (gitignored)

Nothing in this directory is committed except this file and `manifest.json` (paths, source URL, splat
count, SH degree, license, sha256). Keep the models themselves local.

## Where to get test PLYs (all standard INRIA-layout 3DGS PLY)
| Source | Why | License note |
|---|---|---|
| INRIA 3DGS pre-trained models (Mip-NeRF 360 / T&T / Deep Blending scenes) | Large SH-3 scenes (1.5–6 M splats): stress the converter (SH 3→1→0, cleaning, budget prune) | Non-commercial research license — **local testing only, never redistribute, never derive code** |
| PlayCanvas / SuperSplat sample assets | Small–medium scenes with known-good rendering in a reference viewer | Check each asset page |
| Scaniverse / Polycam / KIRI Engine captures of your own | Real phone captures, floaters and all — the honest cleaning test | Your own content |
| Own captures trained later by the (separate) trainer repo | End-to-end pipeline test | Own content |

Target test set (Phase 0/1): 3 scenes at SH 0, 3 at SH 1, spanning ~300 K, ~1 M and ~2–3 M splats,
plus one deliberately "dirty" phone capture for the cleaning pipeline.

`manifest.json` schema:
```json
[{ "name": "garden", "file": "raw/garden.ply", "source": "<url>", "splats": 5834784,
   "sh_degree": 3, "license": "INRIA non-commercial", "sha256": "..." }]
```
