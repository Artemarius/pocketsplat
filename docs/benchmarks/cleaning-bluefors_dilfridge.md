# Cleaning report — bluefors_dilfridge

Source: jwt625/splat HuggingFace dataset, 20251118_BlueFors_Dilfridge_v0.ply (MIT (HF dataset card))

Input 23,603,842 splats (SH 0) -> output 1,000,000 splats (SH 1), 15,152,676 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 23,603,842 | 76 | 0.0 | 837 |  |
| prune_opacity | 23,603,766 | 19,343,015 | 81.9 | 313 | min_alpha=0.020000 |
| prune_scale | 4,260,751 | 1,604 | 0.0 | 270 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=55.135109 |
| clamp_needle_scales | 4,259,147 | 0 | 0.0 | 257 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=4780 |
| remove_floaters_sor | 4,259,147 | 19,601 | 0.5 | 30542 | k=16 m=2.000000 |
| truncate_sh | 4,239,546 | 0 | 0.0 | 0 | degree 0 -> 1 (no-op) |
| prune_budget | 4,239,546 | 3,239,546 | 76.4 | 190 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 340 | bits=10/axis |
