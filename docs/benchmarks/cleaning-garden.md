# Cleaning report — garden

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), garden/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 5,834,784 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,641,882 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 5,834,784 | 0 | 0.0 | 527 |  |
| prune_opacity | 5,834,784 | 1,426,141 | 24.4 | 261 | min_alpha=0.020000 |
| prune_scale | 4,408,643 | 0 | 0.0 | 404 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=80.491623 |
| clamp_needle_scales | 4,408,643 | 0 | 0.0 | 462 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=145 |
| remove_floaters_sor | 4,408,643 | 18 | 0.0 | 48134 | k=16 m=2.000000 |
| truncate_sh | 4,408,625 | 0 | 0.0 | 72 | degree 3 -> 1 |
| prune_budget | 4,408,625 | 3,408,625 | 77.3 | 239 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 493 | bits=10/axis |
