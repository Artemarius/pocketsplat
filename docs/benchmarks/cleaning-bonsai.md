# Cleaning report — bonsai

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), bonsai/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 1,244,819 splats (SH 3) -> output 979,539 splats (SH 1), 17,464,604 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 1,244,819 | 0 | 0.0 | 113 |  |
| prune_opacity | 1,244,819 | 265,272 | 21.3 | 55 | min_alpha=0.020000 |
| prune_scale | 979,547 | 2 | 0.0 | 98 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=45.349094 |
| clamp_needle_scales | 979,545 | 0 | 0.0 | 99 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=1460 |
| remove_floaters_sor | 979,545 | 6 | 0.0 | 7355 | k=16 m=2.000000 |
| truncate_sh | 979,539 | 0 | 0.0 | 15 | degree 3 -> 1 |
| prune_budget | 979,539 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 979,539 | 0 | 0.0 | 370 | bits=10/axis |
