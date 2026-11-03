# Cleaning report — treehill

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), treehill/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 3,783,761 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,803,273 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 3,783,761 | 0 | 0.0 | 340 |  |
| prune_opacity | 3,783,761 | 703,498 | 18.6 | 165 | min_alpha=0.020000 |
| prune_scale | 3,080,263 | 14 | 0.0 | 350 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=98.716393 |
| clamp_needle_scales | 3,080,249 | 0 | 0.0 | 312 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=1062 |
| remove_floaters_sor | 3,080,249 | 35 | 0.0 | 32020 | k=16 m=2.000000 |
| truncate_sh | 3,080,214 | 0 | 0.0 | 50 | degree 3 -> 1 |
| prune_budget | 3,080,214 | 2,080,214 | 67.5 | 175 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 443 | bits=10/axis |
