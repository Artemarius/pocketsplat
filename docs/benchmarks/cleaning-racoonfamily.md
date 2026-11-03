# Cleaning report — racoonfamily

Source: nianticlabs/spz sample assets (MIT), converted from racoonfamily.spz (antialiased/Mip-Splatting) (MIT)

Input 932,560 splats (SH 3) -> output 921,481 splats (SH 1), 16,397,601 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 932,560 | 0 | 0.0 | 85 |  |
| prune_opacity | 932,560 | 97 | 0.0 | 31 | min_alpha=0.020000 |
| prune_scale | 932,463 | 62 | 0.0 | 73 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=157.134598 |
| clamp_needle_scales | 932,401 | 0 | 0.0 | 66 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=453 |
| remove_floaters_sor | 932,401 | 10,920 | 1.2 | 6013 | k=16 m=2.000000 |
| truncate_sh | 921,481 | 0 | 0.0 | 14 | degree 3 -> 1 |
| prune_budget | 921,481 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 921,481 | 0 | 0.0 | 218 | bits=10/axis |
