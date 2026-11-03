# ADR-0007 — `prune_scale` keeps flat splats (thin-axis test off by default)

**Status**: accepted (2026-10-27)

**Context**: `prune_scale` (docs/cleaning.md, op 3) removed a splat when its largest axis exceeds
5 % of the scene diagonal *or* its smallest axis is below ε = 1e-6 · diagonal, on the theory that
"needle-thin ones are noise". The defaults were tuned on museum1, room300k and racoonfamily. The
first run over the public benchmark scenes (INRIA pretrained Mip-NeRF 360 / Tanks and Temples
models, a Postshot-trained MIT scene) showed the thin-axis test dominating the whole pipeline:

| Scene | After opacity prune | Removed by `max > 5 % diag` | Removed by `min < ε` | Share |
|---|---|---|---|---|
| truck | 1,653,930 | 1 | 587,736 | 35.5 % |
| bicycle | 4,483,237 | 5 | 1,151,821 | 25.7 % |
| counter | 973,997 | 19 | 75,567 | 7.8 % |
| giraffes_statue | 2,881,483 | 0 | 1,976,432 | 68.6 % |

Trained 3DGS scenes are mostly flat, surface-aligned disks: the median splat has a largest axis
around 1e-2 and a smallest around 1e-4 scene units (giraffes: 3e-5, already below ε). Those are
the surfaces, not noise. Desktop renders from the same camera (`pocketsplat-desktop
--framing-from`, 1280×720, three yaw angles), PSNR against the uncleaned scene at SH 1:

| Scene | Default clean (ε = 1e-6 · diag) | Same clean, ε = 0 |
|---|---|---|
| truck | 27.0 / 27.7 / 30.5 dB | 32.0 / 38.3 / 34.3 dB |
| giraffes_statue | 5.2 / 8.5 / 10.1 dB | 26.2 / 28.4 / 34.2 dB |

The default cleaning hollowed giraffes out — most of the scene was gone — and the on-device import
pipeline (`splatcore::import_scene`) uses the same defaults. Truly degenerate splats (scale ≤ 0
after `exp`, NaN/Inf) are already removed by `drop_invalid`.

**Decision**: The thin-axis test stays available (`min_diag_fraction`, `splatconv clean
--min-scale-frac`) but defaults to 0 (off) in `splatcore::prune_scale` and `splatconv`. The
largest-axis cap is unchanged. Regression test `Cleaning.PruneScaleDefaultsKeepFlatDisks`.

**Consequences**: Cleaned scenes keep 8–69 % more splats, so they are heavier to sort and draw;
every benchmark variant in `data/derived/bench/` and every committed cleaning report was
regenerated, and frame numbers measured on pre-ADR cleaned scenes are not comparable. The
remaining PSNR gap of the ε = 0 clean (26–38 dB) comes from the opacity prune (α < 0.02, 20–35 %
of trained scenes) and the needle clamp; whether those defaults also cost visible quality on
trained scenes is the next cleaning question, to be answered with the same `--framing-from`
A/B before any change.

**Alternatives considered**: an aspect-based thinness test (min/max < k) — rejected, flatness is
exactly what trained splats look like; scaling ε by the splat's own largest axis — same objection;
keeping ε but lowering it — any fixed fraction misfires on some scene scale (giraffes' median
thin axis is 2.6e-7 of its diagonal).
