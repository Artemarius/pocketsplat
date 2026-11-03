# Cleaning report — drjohnson

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), drjohnson/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 3,405,153 splats (SH 3) -> output 1,000,000 splats (SH 1), 17,216,228 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 3,405,153 | 0 | 0.0 | 305 |  |
| prune_opacity | 3,405,153 | 736,451 | 21.6 | 152 | min_alpha=0.020000 |
| prune_scale | 2,668,702 | 17 | 0.0 | 322 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=15.946640 |
| clamp_needle_scales | 2,668,685 | 0 | 0.0 | 297 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=3428 |
| remove_floaters_sor | 2,668,685 | 188 | 0.0 | 23446 | k=16 m=2.000000 |
| truncate_sh | 2,668,497 | 0 | 0.0 | 42 | degree 3 -> 1 |
| prune_budget | 2,668,497 | 1,668,497 | 62.5 | 156 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 442 | bits=10/axis |
