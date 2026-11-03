# Cleaning report — voxel51_playroom

Source: Voxel51/gaussian_splatting HuggingFace dataset, FO_dataset/playroom iteration_30000 (independent retrain) (HF tag Apache-2.0, but trained on Deep Blending imagery (research terms) - treat as local-only)

Input 1,916,379 splats (SH 3) -> output 1,000,000 splats (SH 1), 17,709,026 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 1,916,379 | 0 | 0.0 | 173 |  |
| prune_opacity | 1,916,379 | 209,465 | 10.9 | 81 | min_alpha=0.020000 |
| prune_scale | 1,706,914 | 224 | 0.0 | 223 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=19.558018 |
| clamp_needle_scales | 1,706,690 | 0 | 0.0 | 177 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=10484 |
| remove_floaters_sor | 1,706,690 | 165 | 0.0 | 12343 | k=16 m=2.000000 |
| truncate_sh | 1,706,525 | 0 | 0.0 | 27 | degree 3 -> 1 |
| prune_budget | 1,706,525 | 706,525 | 41.4 | 100 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 405 | bits=10/axis |
