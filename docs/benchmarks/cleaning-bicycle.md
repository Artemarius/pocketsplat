# Cleaning report — bicycle

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), bicycle/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 6,131,954 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,546,982 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 6,131,954 | 0 | 0.0 | 557 |  |
| prune_opacity | 6,131,954 | 1,648,717 | 26.9 | 280 | min_alpha=0.020000 |
| prune_scale | 4,483,237 | 5 | 0.0 | 467 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=81.664635 |
| clamp_needle_scales | 4,483,232 | 0 | 0.0 | 503 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=1059 |
| remove_floaters_sor | 4,483,232 | 12 | 0.0 | 55102 | k=16 m=2.000000 |
| truncate_sh | 4,483,220 | 0 | 0.0 | 70 | degree 3 -> 1 |
| prune_budget | 4,483,220 | 3,483,220 | 77.7 | 214 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 513 | bits=10/axis |
