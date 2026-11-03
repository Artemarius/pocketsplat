# Cleaning report — giraffes_statue

Source: roman-apollo/3dgs HuggingFace dataset, postshot/giraffes-statue/giraffes-statue-sRGB.ply (MIT (HF dataset tag); capture provenance unverified)

Input 2,997,629 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,245,668 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 2,997,629 | 0 | 0.0 | 271 |  |
| prune_opacity | 2,997,629 | 116,146 | 3.9 | 144 | min_alpha=0.020000 |
| prune_scale | 2,881,483 | 0 | 0.0 | 303 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=117.766602 |
| clamp_needle_scales | 2,881,483 | 0 | 0.0 | 318 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=6700 |
| remove_floaters_sor | 2,881,483 | 662 | 0.0 | 38200 | k=16 m=2.000000 |
| truncate_sh | 2,880,821 | 0 | 0.0 | 47 | degree 3 -> 1 |
| prune_budget | 2,880,821 | 1,880,821 | 65.3 | 181 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 441 | bits=10/axis |
