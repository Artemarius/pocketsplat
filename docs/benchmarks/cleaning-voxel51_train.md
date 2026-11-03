# Cleaning report — voxel51_train

Source: Voxel51/gaussian_splatting HuggingFace dataset, FO_dataset/train iteration_30000 (independent retrain) (HF tag Apache-2.0, but trained on Tanks and Temples imagery (non-commercial research terms) - treat as local-only)

Input 1,074,761 splats (SH 3) -> output 964,326 splats (SH 1), 17,526,246 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 1,074,761 | 0 | 0.0 | 96 |  |
| prune_opacity | 1,074,761 | 110,415 | 10.3 | 44 | min_alpha=0.020000 |
| prune_scale | 964,346 | 5 | 0.0 | 104 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=144.460632 |
| clamp_needle_scales | 964,341 | 0 | 0.0 | 96 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=271 |
| remove_floaters_sor | 964,341 | 15 | 0.0 | 10784 | k=16 m=2.000000 |
| truncate_sh | 964,326 | 0 | 0.0 | 15 | degree 3 -> 1 |
| prune_budget | 964,326 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 964,326 | 0 | 0.0 | 316 | bits=10/axis |
