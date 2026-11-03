# ADR-0010 — One triangle per splat

**Status**: accepted (2026-10-28, S18). Amends the primitive of ADR-0006: the eigenbasis kernel,
the record and the fragment shader are unchanged; each splat is now drawn as one triangle instead
of a two-triangle quad.

**Context**: ADR-0009's rung runs showed that the draw of a heavy scene from the bench camera does
not depend on pixels. On S24U stump, 51 % fewer pixels shortened the draw by 3 % (Adreno 830: 7.70
→ 7.44 ms), with the same GPU cycles. On the S25U the draw was half of our stump frame and carried
the whole cycle gap to SuperSplat at the same pose (draw 5.94 M cycles on the 830 against 3.54 M on
the 750, same splats and pixels; `public-baseline.md` S18). The draw emitted 6 vertices (two
triangles) per splat from `gl_VertexIndex`, with the record re-read by every vertex.

**Measurement**: `SplatRenderer::DrawMode` (desktop `--draw-mode`, bench intent `bench_draw_mode`,
`bench_android.py --draw-mode`) A/B'd four ways to emit a splat. Cold runs, 864×1872, phone
defaults (16-bit keys, 0.1 px cull), uncapped, 45 s from skin ≤ 38.5 °C, modes interleaved, two
rounds per scene and phone (`draw-ab-s24u-9a7c38f.csv`, `draw-ab-s25u-9a7c38f.csv`). Change of draw
time and frame time against the 6-vertex quads, means of both rounds:

| Mode | S24U stump | S24U bicycle-4m | S25U stump | S25U bicycle-4m |
|---|---|---|---|---|
| 6 vertices, two triangles (was the default) | 4.49 ms draw, 87.8 fps | 6.19 ms, 73.0 fps | 5.61 ms, 92.4 fps | 7.27 ms, 77.3 fps |
| **one triangle (3 vertices)** | **−14.2 % draw, −5.8 % frame** | **−11.8 %, −6.2 %** | **−19.4 %, −9.7 %** | **−16.1 %, −9.2 %** |
| indexed quads (4 shaded vertices) | −1.4 %, −0.2 % | −3.0 %, −2.3 % | −5.5 %, −2.2 % | −2.9 %, −1.4 % |
| instanced 4-vertex strip | +17.3 %, +8.0 % | +2.2 %, +0.3 % | +104 %, +53 % | +80 %, +44 % |

Truck runs at 140–230 fps, where pacing and clock swings dominate milliseconds. Its draw cycles
per frame (ms × the app's own `gpu_mhz`) agree: one triangle −9 % (S24U) and −17 % (S25U),
indexed ±0 to −3 %, instanced +5 % / +75 %. RTX 3060 at 864×1872 (desktop bench orbit): stump frame
6.58 → 5.38 ms, truck 2.52 → 2.17 ms.

* **The draw is bound by triangles, not by vertex shading.** Shading 4 instead of 6 vertices per
  quad (indexed batches through the post-transform cache) gains at most 5.5 %. Halving the
  triangles gains 12–19 %.
* **One instance per splat is pathological on the 830** (2× the draw time). Instancing tiny
  4-vertex strips costs per-instance overhead that the 750 tolerates and the 830 does not.
* **The image is unchanged.** The triangle is equilateral around the support circle r = s_cut
  (vertices (0, 2), (±√3, −1) in σ-unit quad coordinates, inradius 1), mapped through the same two
  half-axis vectors, so it encloses the ellipse with 1.3× the quad's area. The fragment shader
  already discards every fragment beyond s_cut (α < 1/255 there, by the definition of s_cut). Against
  the quads: 58.6–63.5 dB min at the six `quality_ab.py` poses, ≤ 0.02 % of pixels moved by more
  than 2 LSB (fragments at the 1/255 threshold, from the unorm8 opacity). Goldens 63.6 dB (museum1)
  and 64.1 dB (sphere) against their stored references, chirality gate passing, validation clean.

**Decision**: `SplatRenderer::kDefaultDrawMode = DrawMode::kTriangle` for the library, the desktop
host and the Android app. The quad modes stay selectable for A/B runs. Bench file names keep
"no suffix = 6-vertex quads" (every run before S18), so default runs now carry `-triangle`.

**Consequences**:
* The S25U's stump gap to SuperSplat at the same pose narrows. Our cold frame drops from ~11.9 M
  to ~10.6 M GPU cycles, against its 8.4 M. Energy per frame on battery is in `public-baseline.md`.
* The triangle covers 1.3× the quad's area, so a fill-bound view (close-ups, few large splats)
  could get slower. The bench orbit and the desktop close poses are faster or neutral. Close-up
  views were not timed on the phones.
  **Follow-up 2026-10-29 (overnight S25U, `draw-ab-s25u-ef86526-allscenes.csv`):** every public
  bench scene at 864×1872 and 540×975, two interleaved rounds. The triangle gains 12–22 % draw time and
  6–13 % frame time in 11 of the 12 scene × size cases, the light scenes included (counter −12 / −16 %
  draw, truck −13 / −21 %, museum1 at 540×975 −19 %). The fill-bound case is museum1 (0.31 M splats,
  the largest on-screen splats of the set) at 864×1872: +3.5 % draw cycles and +1.5 % frame cycles.
  In milliseconds it is +11 %, but the triangle runs there sat at 905–926 MHz against up to 1061 MHz
  for the quads, with the governor pacing at 175–250 fps. A per-splat choice (quad for large
  footprints, triangle for small) would recover it. The default stands.
* **Shader lesson (negative result)**: the first build of these modes took its corners from three
  `const vec2[]` tables indexed per vertex. On the Adreno 750 every vertex became ~10× more
  expensive (draw 4.4 → 48.9 ms, correct image), while the RTX got faster. Corners now come from
  bit masks and closed-form expressions (CLAUDE.md, `docs/benchmarks/README.md`).

**Alternatives considered**:
* Indexed quads in shared batches (PlayCanvas-style instancing of fixed quad batches): correct and
  cheap, but at most 5.5 % faster here.
* One instance per splat with a 4-vertex strip: slower on both GPUs, 2× on the 830.
* A screen-aligned bounding triangle of the ellipse's AABB: larger than the equilateral
  triangle around the eigenbasis circle, so more discarded fragments for no fewer triangles.
* Point sprites (`gl_PointSize`): limited to square sprites, a device-dependent maximum size, and
  no anisotropic footprint; rejected without measurement.
