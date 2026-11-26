# ADR-0017 — SH 1 palette: a 16-bit index per splat into a palette of SH1 vectors trained at import

**Status**: accepted (2026-11-24, S32). Implemented and measured on the desktop and on both phones
(Evidence; the three gates of `docs/sh1-palette-research.md` §5 passed). The owner turned the app's
import option on by default after gate C, chose the same for the Windows viewer, and accepted this
ADR after reviewing it (S32).

**Context**: the app imports at SH 1, so a scene costs 40 B per splat on the GPU, against 28 B at
SH 0. SH 0 is not a free substitute: against direct SH 1 it renders at 29.6–38.7 dB at the worst
of six poses on the six test scenes, below the 40 dB bar used elsewhere in this project. SH1
vectors repeat across a scene, and palettes for Gaussian attributes are established practice
(Compact3D, Niedermayr et al., PlayCanvas SOG). The high 16 bits of `scale_rot` word 2 were unused
in pack v2, and the header had one reserved word.

**Options**:

(a) Keep direct SH 1 only. No work; SH 1 stays 43 % heavier than SH 0.
(b) Fewer bits per SH1 coefficient. No training, but it cannot reach zero extra bytes per splat.
(c) SOG as the runtime format. Rejected in ADR-0002.
(d) A palette, with a 16-bit index in the unused bits of `scale_rot`, trained at import. **Chosen.**
(e) Link Faiss for the training. Rejected: a heavy dependency with no Android story, for a trainer
    of a few hundred lines.

**Decision**:

1. **Pack version 3** (`formats.md` §3). Header word 28 holds `palette_count` in its low 24 bits
   (0 = direct SH 1, at most 65,536 entries, SH degree 1 only) and flags in its high 8 bits. No
   flag is defined; readers reject any set flag, so a future flag makes old readers refuse a pack
   instead of misrendering it. The entry index sits in the high 16 bits of `scale_rot` word 2. The
   palette occupies the `sh1` array in the existing 3 × u32 snorm8 encoding, and `sh1_scale` is the
   max-abs over the entries in use. A palette pack therefore decodes bit for bit like the direct
   pack of the snapped cloud. Readers reject indices outside the palette. `subset_pack` keeps the
   palette whole.
2. **Version 2 packs are read as version 3 direct packs.** v2 wrote zero in both places, so its
   bytes are a valid v3 pack without a palette. Nobody has to re-import, on the phones or in the
   bench caches. Only v3 is written, and v1 stays rejected. This deviates from the research note's
   plan, which rejected v2.
3. **The preprocess reads `sh1[3 · (w2 >> 16)]` in palette mode**, selected by one push-constant
   flag. Nothing else in the renderer changes.
4. **Training at import** (`splatcore/palette.h`, `ImportOptions::sh1_palette`): **e-means** (Meta
   FAIR, arXiv 2611.09501), 3 epochs, **k 4,096 at every import budget**, no training cap, on all
   hardware threads. The scaling laws are the published ones. The assignment kernel is explicit SSE2
   on x86-64 and NEON on arm64, with a dynamic work split. Training runs after the Morton reorder,
   reports its progress, and can be cancelled. `palette.cpp` and, since S32, all of `splatcore` build
   without floating-point contraction, so the phones train the same palettes as the desktop.
5. **Defaults**:

   | Caller | Default | Why |
   |---|---|---|
   | Android app, import option "SH palette (30 % smaller scene)" | **on** | Owner, 2026-11-24, after gate C |
   | Windows viewer (`pocketsplat-viewer`) | **on**, no switch | Owner, 2026-11-24 (S32): the same import as the phone |
   | `ImportOptions::sh1_palette` (library) | 0 (direct SH 1) | Callers decide |
   | `splatconv pack` | direct SH 1; `--sh1-palette K` opts in, with Lloyd as the default trainer | `palette_ab.py` stays reproducible |
   | Scripted import intent `import_palette` | off | Bench runs stay comparable with earlier ones |

   Scenes imported before the palette keep their direct SH 1 packs. The app does not re-import
   them, and the viewer's import cache version is not bumped.
6. **Direct SH 1 packs remain supported**, as the reference that the palette golden compares
   against and as the fallback.

**Evidence** (all in `docs/sh1-palette-research.md` §6, with the CSVs under `docs/benchmarks/`):

* **Image quality, gate A** (`palette-quality-c6f0a7f-2026-11-19.csv`, desktop, 864 × 1872, worst
  of six poses against direct SH 1): SH 0 is at 29.6–38.7 dB. A palette of 1,024 entries reaches at
  least 40.6 dB on all six scenes, and k 4,096 reaches 42.6–52.7 dB (Lloyd-25). The e-means-3
  palettes of the import render at 41.8 dB (giraffes_statue, the hardest scene) and 52.4 dB
  (hornedlizard) (`palette-quality-emeans-4cee8a0-2026-11-20.csv`).
* **Trainer, gate B** (`palette-trainers-1cdd4cc-2026-11-19.csv`, five seeds): e-means-3 reaches
  0.88–1.01 of the Lloyd-25 loss with 12 % of the distance work, and is 7–18 % below Lloyd-3 at equal
  work. On SPZ imports its revival leaves almost no entry unused (hornedlizard: 245 dead for Lloyd-25,
  1.6 for e-means-5). Hamerly's bounds save only 6–30 %, which is a negative result.
* **Training time on the phones** (`palette-trainers-s24u-cd347ef-2026-11-23.csv`, S24 Ultra,
  deciding; NEON kernel): e-means-3 at k 4,096 takes 3.1 s on hornedlizard (765 K splats) and 18.9 s on
  stump (4.05 M). The S25 Ultra takes 2.2 s and 12.0 s. Phone palettes equal the desktop's bit for bit.
* **Rendering, gate C** (`palette-gatec-cd347ef-2026-11-23.csv`, stump at 864 × 1872, preprocess ms at
  minutes 4–5):

  | Phone | Splats | Palette | Direct SH 1 | SH 0 |
  |---|---|---|---|---|
  | S24 Ultra | 1 M | 3.41 / 3.46 | 3.53 | 3.32 |
  | S24 Ultra | 4.05 M | 5.29 | 5.52 | 5.35 |
  | S25 Ultra | 1 M | 2.18 | 2.23 | 2.15 |
  | S25 Ultra | 4.05 M | 3.56 | 3.72 | 3.49 |

  On both phones at both sizes, the palette is 2–4 % faster than direct SH 1, and between 1 % faster
  and 4 % slower than SH 0.
* **Import time** (`palette-import-cd347ef-2026-11-23.csv`, scripted app imports of stump): the
  palette adds 4.5 s at the 1 M default and 19.6 s for all 4.05 M splats on the S24 Ultra; on the
  S25 Ultra it adds 3.0 s and 13.0 s.
* **Memory**: the GPU-resident pack is 28.16 MB instead of 40.11 MB at 1 M splats, and 113.9 MB
  instead of 162.4 MB at 4.05 M. These figures come from the buffer sizes; `dumpsys meminfo` cannot
  resolve the difference (negative result).

**Consequences**:

* SH 1 scenes take the SH 0 footprint plus a palette of 48 KB. The Adreno 750's scene limit does not
  move (11,184,810 splats, ADR-0016): `positions` and `scale_rot` are still 12 B per splat.
* Imports get slower by the training time above. The status line shows "training SH palette… NN %",
  and cancelling works during training.
* A palette scene renders a few dB below direct SH 1, but well above SH 0. Users who want the
  exact colours can import with the option off.
* The release note says that earlier packs keep working without a re-import.
* Byte-exact pack tests, the parse rejections and the `golden_palette_equivalence` ctest gate the
  format. On the synthetic sphere, the palette render matches the snapped direct pack and sits
  34 dB away from SH 0.
* ADR-0009 planned an SH 1 → 0 rung for the quality controller "where its measured price fits".
  On a palette pack, SH 0 saves at most 4 % of the preprocess, so that rung is not worth adding for
  scenes imported with the palette.
* Not adopted: a training cap of 64 points per entry, which cuts training work to 1–4 % but raises
  the loss by 6–8 % (image cost not measured), and per-budget k.

**Amendment (S36, 2026-11-26, accepted by the owner; S24U import check pending): palette size by
the imported splat count.** Imports train **16,384 entries up to 1,000,000 splats and 4,096 above**
(`splatcore::sh1_palette_for_count`, resolved inside `import_scene` after cleaning and the budget
prune; the app, the Windows viewer and `splatconv import --sh1-palette auto` use it). This replaces
"k 4,096 at every import budget" in Decision 4; trainer, epochs and format are unchanged.

*Evidence* (`docs/palette-size.md`, CSVs `palette-size-*-2026-11-26`):
* Image: e-means-3, six scenes, three paired seeds. At the app's home view 16,384 entries gain
  +0.96 to +2.55 dB worst pose over 4,096 on every scene, all 180 paired poses positive, seed spread
  ≤ 0.27 dB; +1.11 dB on a 1 M stump import. The gain does not grow with scene size (museum1
  0.31 M: +1.35 dB; stump 4.05 M: +0.96 dB).
* Training time, S24 Ultra (deciding, cool start, battery): hornedlizard 0.76 M 3.8 → 16.9 s, stump
  4.05 M 20.8 → 96.2 s (4.6×, ending at thermal status 2). Cost grows with the count, gain does not:
  the threshold keeps the extra time at about 15 s for the default 1 M budget and spares large
  "all" imports the 96 s.
* Rendering, S25 Ultra, ABBA: at 4.05 M preprocess +2 to +4 % (both orders), frame up to +1.7 %;
  at 1 M no difference resolved. Scenes above 1 M keep 4,096 entries, so their cost is unchanged.

*Consequences.* Scenes of up to 1 M splats imported from now on carry a 192 KB palette instead of
48 KB (+147 KB per pack). Earlier imports are not redone (no viewer cache bump). Still to measure:
the app's palette stage at 1 M with 16,384 entries on the S24U (estimated ~20 s against 4.5 s), and
gate C at 1 M on the S24U; a threshold change follows if the import turns out much slower.
