# Cleaning report — flowers

Source: INRIA 3DGS pretrained models.zip (repo-sam.inria.fr/fungraph/3d-gaussian-splatting/datasets/pretrained/models.zip), flowers/point_cloud/iteration_30000 (INRIA Gaussian-Splatting License: non-commercial research/evaluation only - local benchmarking, never commit or redistribute)

Input 3,636,448 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,823,946 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 3,636,448 | 0 | 0.0 | 332 |  |
| prune_opacity | 3,636,448 | 807,860 | 22.2 | 164 | min_alpha=0.020000 |
| prune_scale | 2,828,588 | 5 | 0.0 | 333 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=73.084717 |
| clamp_needle_scales | 2,828,583 | 0 | 0.0 | 294 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=1439 |
| remove_floaters_sor | 2,828,583 | 20 | 0.0 | 26784 | k=16 m=2.000000 |
| truncate_sh | 2,828,563 | 0 | 0.0 | 45 | degree 3 -> 1 |
| prune_budget | 2,828,563 | 1,828,563 | 64.6 | 161 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 439 | bits=10/axis |
