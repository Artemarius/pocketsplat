# Cleaning report — voxel51_drjohnson

Source: Voxel51/gaussian_splatting HuggingFace dataset, FO_dataset/drjohnson iteration_30000 (independent retrain) (HF tag Apache-2.0, but trained on Deep Blending imagery (research terms) - treat as local-only)

Input 3,177,554 splats (SH 3) -> output 1,000,000 splats (SH 1), 17,326,141 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 3,177,554 | 0 | 0.0 | 318 |  |
| prune_opacity | 3,177,554 | 450,426 | 14.2 | 150 | min_alpha=0.020000 |
| prune_scale | 2,727,128 | 49 | 0.0 | 382 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=15.890538 |
| clamp_needle_scales | 2,727,079 | 0 | 0.0 | 305 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=4784 |
| remove_floaters_sor | 2,727,079 | 344 | 0.0 | 31834 | k=16 m=2.000000 |
| truncate_sh | 2,726,735 | 0 | 0.0 | 44 | degree 3 -> 1 |
| prune_budget | 2,726,735 | 1,726,735 | 63.3 | 150 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 428 | bits=10/axis |
