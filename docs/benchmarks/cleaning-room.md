# Cleaning report — room

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), room/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 1,593,376 splats (SH 3) -> output 1,000,000 splats (SH 1), 17,158,610 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 1,593,376 | 0 | 0.0 | 144 |  |
| prune_opacity | 1,593,376 | 437,447 | 27.5 | 73 | min_alpha=0.020000 |
| prune_scale | 1,155,929 | 36 | 0.0 | 138 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=26.020821 |
| clamp_needle_scales | 1,155,893 | 0 | 0.0 | 122 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=2675 |
| remove_floaters_sor | 1,155,893 | 37 | 0.0 | 9491 | k=16 m=2.000000 |
| truncate_sh | 1,155,856 | 0 | 0.0 | 18 | degree 3 -> 1 |
| prune_budget | 1,155,856 | 155,856 | 13.5 | 66 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 384 | bits=10/axis |
