# Cleaning report — counter

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), counter/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 1,222,956 splats (SH 3) -> output 973,948 splats (SH 1), 17,151,379 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 1,222,956 | 0 | 0.0 | 110 |  |
| prune_opacity | 1,222,956 | 248,959 | 20.4 | 54 | min_alpha=0.020000 |
| prune_scale | 973,997 | 19 | 0.0 | 130 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=26.762516 |
| clamp_needle_scales | 973,978 | 0 | 0.0 | 106 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=2024 |
| remove_floaters_sor | 973,978 | 30 | 0.0 | 7952 | k=16 m=2.000000 |
| truncate_sh | 973,948 | 0 | 0.0 | 16 | degree 3 -> 1 |
| prune_budget | 973,948 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 973,948 | 0 | 0.0 | 349 | bits=10/axis |
