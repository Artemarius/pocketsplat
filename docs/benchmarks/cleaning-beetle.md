# Cleaning report — beetle

Source: loopce/gaussiansplats HuggingFace dataset, beetle/model/point_cloud/iteration_30000 (CC-BY-4.0 (HF dataset tag), attribution: loopce/gaussiansplats)

Input 2,066,389 splats (SH 3) -> output 1,000,000 splats (SH 1), 18,845,587 bytes SPZ. Default parameters per docs/cleaning.md.

| Op | Input | Removed | Removed % | ms | Params |
|---|---|---|---|---|---|
| drop_invalid | 2,066,389 | 0 | 0.0 | 191 |  |
| prune_opacity | 2,066,389 | 384,384 | 18.6 | 93 | min_alpha=0.020000 |
| prune_scale | 1,682,005 | 15 | 0.0 | 189 | max_diag_fraction=0.050000 min_diag_fraction=0.000000 diag=106.079918 |
| clamp_needle_scales | 1,681,990 | 0 | 0.0 | 173 | len_cap_fraction=0.010000 aspect_cap=6.000000 clamped=741 |
| remove_floaters_sor | 1,681,990 | 7 | 0.0 | 17506 | k=16 m=2.000000 |
| truncate_sh | 1,681,983 | 0 | 0.0 | 27 | degree 3 -> 1 |
| prune_budget | 1,681,983 | 681,983 | 40.5 | 92 | max_splats=1000000 |
| reorder_morton | 1,000,000 | 0 | 0.0 | 381 | bits=10/axis |
