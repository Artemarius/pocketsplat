#version 460
// Pass: splat draw (vertex)
// Inputs:  records[] written by splat_preprocess.comp, order[] (sorted position -> record), the
//          visible count (sort args, written by splat_prepare_indirect.comp), push constants
//          (viewport size in pixels). No vertex buffers. How a vertex finds its splat and corner
//          depends on kDrawMode (SplatRenderer::DrawMode, the same draw arguments' layout):
//            0 = 6 vertices per splat from gl_VertexIndex (non-indexed triangle list);
//            1 = one instance per splat, 4-vertex triangle strip (gl_InstanceIndex, gl_VertexIndex);
//            2 = indexed batches: a shared index buffer of kQuadsPerBatch quads {0,1,2, 0,2,3}+4q,
//                one instance per batch, so the post-transform cache can shade each corner once;
//                the last batch's surplus quads (beyond the visible count) are clipped here;
//            3 = one triangle per splat (3 vertices from gl_VertexIndex): the equilateral
//                triangle around the support circle r = s_cut (1.3x the quad's area, half its
//                triangles). The fragment shader discards everything outside r = s_cut already
//                (alpha < 1/255 there by the definition of s_cut), so the image is unchanged.
// Outputs: ellipse-oriented quad spanned by the record's two half-axis vectors around the splat
//          center; interpolated quad-local coordinates in units of sigma (v_uv) plus flat color
//          for the fragment shader. The kernel is evaluated from v_uv — no conic anywhere.
// Sync:    records, order and the count were made visible by the compute -> vertex barrier in
//          SplatRenderer::record_compute.
// Probes:  kProbe != 0 builds a timing-only variant of the draw (SplatRenderer::DrawProbe; wrong
//          image by design, never a default): 1 = read records in compacted order instead of
//          through order[] (no gather; blend order wrong); 2 = shrink every primitive to ~0 area
//          (same vertices and primitives, almost no fragments); 3 = place every primitive outside
//          the clip volume after reading its record (vertex work only). Probes 4 (blending off)
//          and 5 (every fragment discarded, splat.frag) change the pipeline and the fragment
//          shader; this shader runs as normal for them. Probe 6 is not a wrong image: binding 0
//          then holds the records already copied into draw order (splat_sorted_copy.comp), read
//          at the draw position itself - the same blend order without the order[] gather.

layout(constant_id = 0) const uint kDrawMode = 0u;
layout(constant_id = 1) const uint kQuadsPerBatch = 1024u;
layout(constant_id = 2) const uint kProbe = 0u;

layout(std430, set = 0, binding = 0) readonly buffer Records { uint records[]; };
layout(std430, set = 0, binding = 1) readonly buffer Order { uint order[]; };
layout(std430, set = 0, binding = 2) readonly buffer SortArgs { uint visible_count; };

layout(push_constant) uniform Push {
    vec2 viewport;  // pixels
} push;

layout(location = 0) out vec2 v_uv;  // quad-local coords, sigma units, +-s_cut at the edges
layout(location = 1) flat out vec4 v_color;

// Quad corner i of a pattern as (+-1, +-1), from two bit masks (bit i set = +1). No constant
// arrays: indexed per vertex they cost ~10x per invocation on the Adreno 750 (S18, three arrays;
// the driver keeps such arrays in per-invocation memory - see docs/benchmarks/README.md).
//   6-vertex list (-1,-1) (1,-1) (1,1) (-1,-1) (1,1) (-1,1): x bits 1,2,4; y bits 2,4,5
//   strip         (-1,-1) (1,-1) (-1,1) (1,1):               x bits 1,3;   y bits 2,3
//   indexed quad  (-1,-1) (1,-1) (1,1) (-1,1):               x bits 1,2;   y bits 2,3
vec2 corner(uint i, uint x_mask, uint y_mask) {
    return vec2(float((x_mask >> i) & 1u), float((y_mask >> i) & 1u)) * 2.0 - 1.0;
}

void clip_vertex() {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // outside the clip volume; the quad collapses
    v_uv = vec2(0.0);
    v_color = vec4(0.0);
}

void main() {
    uint sorted_pos;
    vec2 c;
    if (kDrawMode == 1u) {
        sorted_pos = uint(gl_InstanceIndex);
        c = corner(uint(gl_VertexIndex) & 3u, 0xAu, 0xCu);
    } else if (kDrawMode == 2u) {
        sorted_pos = uint(gl_InstanceIndex) * kQuadsPerBatch + uint(gl_VertexIndex) / 4u;
        c = corner(uint(gl_VertexIndex) & 3u, 0x6u, 0xCu);
        if (sorted_pos >= visible_count) {
            clip_vertex();
            return;
        }
    } else if (kDrawMode == 3u) {
        sorted_pos = uint(gl_VertexIndex) / 3u;
        uint i = uint(gl_VertexIndex) % 3u;  // (0, 2), (sqrt 3, -1), (-sqrt 3, -1): incircle radius 1
        c = i == 0u ? vec2(0.0, 2.0) : vec2(i == 1u ? 1.7320508 : -1.7320508, -1.0);
    } else {
        sorted_pos = uint(gl_VertexIndex) / 6u;
        c = corner(uint(gl_VertexIndex) % 6u, 0x16u, 0x34u);
    }
    // sorted position -> compacted record index (probe 1: the compacted order itself; probe 6:
    // the records are in draw order already)
    uint base = (kProbe == 1u || kProbe == 6u ? sorted_pos : order[sorted_pos]) * 8u;

    float s_cut = uintBitsToFloat(records[base + 7u]);
    if (s_cut <= 0.0) {
        clip_vertex();
        return;
    }

    vec2 center_px = vec2(uintBitsToFloat(records[base + 0u]), uintBitsToFloat(records[base + 1u]));
    vec2 axis1 = vec2(uintBitsToFloat(records[base + 2u]), uintBitsToFloat(records[base + 3u]));
    vec2 axis2 = vec2(uintBitsToFloat(records[base + 4u]), uintBitsToFloat(records[base + 5u]));
    float extent = kProbe == 2u ? 1e-3 : 1.0;  // probe 2: ~1e-6 of the area, no pixel centre covered
    vec2 corner_px = center_px + extent * (c.x * axis1 + c.y * axis2);
    vec2 ndc = corner_px / push.viewport * 2.0 - 1.0;
    // Probe 3: x > 1 at every vertex, so clipping rejects the primitive; the tiny ndc term keeps
    // the record reads alive.
    if (kProbe == 3u) ndc = vec2(3.0 + 1e-6 * ndc.x, ndc.y);

    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = c * s_cut;  // all w = 1, so interpolation is affine and exact in screen space
    v_color = unpackUnorm4x8(records[base + 6u]);
}
