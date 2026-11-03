# Cleaning report — room300k.sh3

Source: Carsamba/gaussian-splats HuggingFace dataset (MIT tag) (55k_300ksplats_raw.ply) (MIT (HF dataset tag))

Input 300,000 splats (SH 3) -> output 243,253 splats (SH 1), 4,196,931 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 300,000 | 0 | 0.0 | 27 |  |
| prune_opacity | 300,000 | 55,616 | 18.5 | 13 | min_alpha=0.020000 |
| prune_scale | 244,384 | 1,029 | 0.4 | 29 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=19.084265 |
| clamp_needle_scales | 243,355 | 0 | 0.0 | 24 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=3390 |
| remove_floaters_sor | 243,355 | 102 | 0.0 | 1442 | k=16 m=2.000000 |
| truncate_sh | 243,253 | 0 | 0.0 | 4 | degree 3 -> 1 |
| prune_budget | 243,253 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 243,253 | 0 | 0.0 | 95 | bits=10/axis |
