# Cleaning report — hornedlizard

Source: nianticlabs/spz sample assets (MIT), converted from hornedlizard.spz; first ~20k splats are a sky-sphere shell at radius ~200 (MIT)

Input 786,233 splats (SH 3) -> output 764,985 splats (SH 1), 12,896,732 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 786,233 | 0 | 0.0 | 72 |  |
| prune_opacity | 786,233 | 14 | 0.0 | 25 | min_alpha=0.020000 |
| prune_scale | 786,219 | 0 | 0.0 | 46 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=428.920929 |
| clamp_needle_scales | 786,219 | 0 | 0.0 | 52 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=0 |
| remove_floaters_sor | 786,219 | 21,234 | 2.7 | 4524 | k=16 m=2.000000 |
| truncate_sh | 764,985 | 0 | 0.0 | 12 | degree 3 -> 1 |
| prune_budget | 764,985 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 764,985 | 0 | 0.0 | 141 | bits=10/axis |
