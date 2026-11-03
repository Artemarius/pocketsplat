# Cleaning report — kitchen

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), kitchen/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 1,852,335 splats (SH 3) -> output 1,000,000 splats (SH 1), 17,570,748 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 1,852,335 | 0 | 0.0 | 182 |  |
| prune_opacity | 1,852,335 | 267,873 | 14.5 | 80 | min_alpha=0.020000 |
| prune_scale | 1,584,462 | 5 | 0.0 | 161 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=54.179321 |
| clamp_needle_scales | 1,584,457 | 0 | 0.0 | 161 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=424 |
| remove_floaters_sor | 1,584,457 | 28 | 0.0 | 18962 | k=16 m=2.000000 |
| truncate_sh | 1,584,429 | 0 | 0.0 | 26 | degree 3 -> 1 |
| prune_budget | 1,584,429 | 584,429 | 36.9 | 89 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 363 | bits=10/axis |
