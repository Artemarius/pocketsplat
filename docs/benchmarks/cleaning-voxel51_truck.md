# Cleaning report — voxel51_truck

Source: Voxel51/gaussian_splatting HuggingFace dataset, FO_dataset/truck iteration_30000 (independent retrain) (HF tag Apache-2.0, but trained on Tanks and Temples imagery (non-commercial research terms) - treat as local-only)

Input 2,056,645 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,843,275 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 2,056,645 | 0 | 0.0 | 186 |  |
| prune_opacity | 2,056,645 | 321,890 | 15.7 | 90 | min_alpha=0.020000 |
| prune_scale | 1,734,755 | 5 | 0.0 | 187 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=138.359039 |
| clamp_needle_scales | 1,734,750 | 0 | 0.0 | 182 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=172 |
| remove_floaters_sor | 1,734,750 | 5 | 0.0 | 16850 | k=16 m=2.000000 |
| truncate_sh | 1,734,745 | 0 | 0.0 | 26 | degree 3 -> 1 |
| prune_budget | 1,734,745 | 734,745 | 42.4 | 107 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 395 | bits=10/axis |
