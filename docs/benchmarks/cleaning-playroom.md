# Cleaning report — playroom

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), playroom/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 2,546,116 splats (SH 3) -> output 1,000,000 splats (SH 1), 17,574,235 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 2,546,116 | 0 | 0.0 | 234 |  |
| prune_opacity | 2,546,116 | 482,895 | 19.0 | 114 | min_alpha=0.020000 |
| prune_scale | 2,063,221 | 141 | 0.0 | 248 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=19.517076 |
| clamp_needle_scales | 2,063,080 | 0 | 0.0 | 235 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=10378 |
| remove_floaters_sor | 2,063,080 | 122 | 0.0 | 15609 | k=16 m=2.000000 |
| truncate_sh | 2,062,958 | 0 | 0.0 | 33 | degree 3 -> 1 |
| prune_budget | 2,062,958 | 1,062,958 | 51.5 | 120 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 436 | bits=10/axis |
