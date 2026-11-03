# Cleaning report — jimny

Source: roman-apollo/3dgs HuggingFace dataset, postshot/jimny/jimny.ply (MIT (HF dataset tag); capture provenance unverified)

Input 1,124,170 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,023,809 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 1,124,170 | 0 | 0.0 | 99 |  |
| prune_opacity | 1,124,170 | 2,023 | 0.2 | 41 | min_alpha=0.020000 |
| prune_scale | 1,122,147 | 0 | 0.0 | 105 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=140.824295 |
| clamp_needle_scales | 1,122,147 | 0 | 0.0 | 122 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=2807 |
| remove_floaters_sor | 1,122,147 | 16 | 0.0 | 11488 | k=16 m=2.000000 |
| truncate_sh | 1,122,131 | 0 | 0.0 | 18 | degree 3 -> 1 |
| prune_budget | 1,122,131 | 122,131 | 10.9 | 62 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 375 | bits=10/axis |
