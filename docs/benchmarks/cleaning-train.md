# Cleaning report — train

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), train/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 1,026,508 splats (SH 3) -> output 823,923 splats (SH 1), 14,838,246 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 1,026,508 | 0 | 0.0 | 92 |  |
| prune_opacity | 1,026,508 | 202,562 | 19.7 | 44 | min_alpha=0.020000 |
| prune_scale | 823,946 | 7 | 0.0 | 87 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=121.890617 |
| clamp_needle_scales | 823,939 | 0 | 0.0 | 83 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=427 |
| remove_floaters_sor | 823,939 | 16 | 0.0 | 8521 | k=16 m=2.000000 |
| truncate_sh | 823,923 | 0 | 0.0 | 13 | degree 3 -> 1 |
| prune_budget | 823,923 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 823,923 | 0 | 0.0 | 304 | bits=10/axis |
