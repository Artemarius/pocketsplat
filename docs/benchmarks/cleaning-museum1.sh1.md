# Cleaning report — museum1.sh1

Source: Carsamba/gaussian-splats HuggingFace dataset (MIT tag), truncated from museum1.sh3 by gsplat-samples tools/truncate_sh.py (MIT (HF dataset tag))

Input 321,465 splats (SH 1) -> output 314,457 splats (SH 1), 5,697,135 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 321,465 | 0 | 0.0 | 13 |  |
| prune_opacity | 321,465 | 6,956 | 2.2 | 9 | min_alpha=0.020000 |
| prune_scale | 314,509 | 47 | 0.0 | 21 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=6.065029 |
| clamp_needle_scales | 314,462 | 0 | 0.0 | 21 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=13833 |
| remove_floaters_sor | 314,462 | 5 | 0.0 | 712 | k=16 m=2.000000 |
| truncate_sh | 314,457 | 0 | 0.0 | 0 | degree 1 -> 1 (no-op) |
| prune_budget | 314,457 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 314,457 | 0 | 0.0 | 54 | bits=10/axis |
