# Cleaning report — stump

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), stump/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 4,961,797 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,860,609 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 4,961,797 | 0 | 0.0 | 445 |  |
| prune_opacity | 4,961,797 | 912,228 | 18.4 | 219 | min_alpha=0.020000 |
| prune_scale | 4,049,569 | 0 | 0.0 | 371 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=219.031616 |
| clamp_needle_scales | 4,049,569 | 0 | 0.0 | 425 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=58 |
| remove_floaters_sor | 4,049,569 | 2 | 0.0 | 34337 | k=16 m=2.000000 |
| truncate_sh | 4,049,567 | 0 | 0.0 | 65 | degree 3 -> 1 |
| prune_budget | 4,049,567 | 3,049,567 | 75.3 | 200 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 480 | bits=10/axis |
