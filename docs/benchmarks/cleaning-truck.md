# Cleaning report — truck

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), truck/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 2,541,226 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,710,406 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 2,541,226 | 0 | 0.0 | 234 |  |
| prune_opacity | 2,541,226 | 887,296 | 34.9 | 120 | min_alpha=0.020000 |
| prune_scale | 1,653,930 | 1 | 0.0 | 156 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=125.000420 |
| clamp_needle_scales | 1,653,929 | 0 | 0.0 | 168 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=361 |
| remove_floaters_sor | 1,653,929 | 2 | 0.0 | 16359 | k=16 m=2.000000 |
| truncate_sh | 1,653,927 | 0 | 0.0 | 26 | degree 3 -> 1 |
| prune_budget | 1,653,927 | 653,927 | 39.5 | 114 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 414 | bits=10/axis |
