# Cleaning report — truck-it1000

Source: own trainer (cuda-gaussian-splatting repo), Tanks and Temples 'truck' scene, iteration 1000 (own training output on Tanks and Temples imagery - local testing only, do not redistribute)

Input 50,000 splats (SH 3) -> output 34,966 splats (SH 1), 579,354 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 50,000 | 0 | 0.0 | 4 |  |
| prune_opacity | 50,000 | 14,970 | 29.9 | 2 | min_alpha=0.020000 |
| prune_scale | 35,030 | 59 | 0.2 | 4 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=64.781853 |
| clamp_needle_scales | 34,971 | 0 | 0.0 | 4 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=54 |
| remove_floaters_sor | 34,971 | 5 | 0.0 | 308 | k=16 m=2.000000 |
| truncate_sh | 34,966 | 0 | 0.0 | 1 | degree 3 -> 1 |
| prune_budget | 34,966 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 34,966 | 0 | 0.0 | 11 | bits=10/axis |
