# Cleaning report — museum1.sh0

Source: Carsamba/gaussian-splats HuggingFace dataset (MIT tag), truncated from museum1.sh3 by gsplat-samples tools/truncate_sh.py (MIT (HF dataset tag))

Input 321,465 splats (SH 0) -> output 314,457 splats (SH 1), 5,002,649 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 321,465 | 0 | 0.0 | 10 |  |
| prune_opacity | 321,465 | 6,956 | 2.2 | 5 | min_alpha=0.020000 |
| prune_scale | 314,509 | 47 | 0.0 | 18 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=6.065029 |
| clamp_needle_scales | 314,462 | 0 | 0.0 | 20 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=13833 |
| remove_floaters_sor | 314,462 | 5 | 0.0 | 705 | k=16 m=2.000000 |
| truncate_sh | 314,457 | 0 | 0.0 | 0 | degree 0 -> 1 (no-op) |
| prune_budget | 314,457 | 0 | 0.0 | 0 | max_splats=1000000 (within budget) |
| reorder_morton | 314,457 | 0 | 0.0 | 55 | bits=10/axis |
