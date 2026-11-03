#version 460
// Pass: splat draw (fragment)
// Inputs:  interpolated quad-local coordinates in units of sigma (v_uv), flat color.
// Outputs: premultiplied color; blend factors ONE / ONE_MINUS_SRC_ALPHA composite back-to-front
//          "over" (draw order = sorted order from the GPU sort).
//
// r^2 = dot(v_uv, v_uv) is exact for any conditioning of the 2D covariance because the quad
// itself is the level-set geometry (axes = eigenvectors, extents = s_cut * sqrt(lambda)). The
// previous per-fragment conic evaluation went indefinite under fp32 cancellation on extreme
// needle splats and painted their ridge line as a bright straight ray at silhouettes — that
// failure mode is unrepresentable in this parameterization (docs/silhouette-rays-research.md).

// Probe (SplatRenderer::DrawProbe, timing only): 5 = discard every fragment first, through a test
// the compiler cannot fold (unorm alpha is never negative), so rasterization and the fragment
// launch remain but no fragment shades or blends.
layout(constant_id = 0) const uint kProbe = 0u;

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    if (kProbe == 5u && v_color.a >= 0.0) discard;
    float r2 = dot(v_uv, v_uv);
    // Truncated-Gaussian kernel with support sqrt(8)*sigma, renormalized so the tail reaches
    // exactly zero at the support edge (idea from the MIT PlayCanvas gsplat shader) instead of
    // stepping off at e^-4 (~1.8% of peak). Without the renormalization, elongated splats end as
    // visible rays at silhouettes (docs/benchmarks/README.md, S10 streak investigation).
    if (r2 > 8.0) discard;
    const float kExp4 = 0.0183156393;  // e^-4
    float alpha = min(0.99, v_color.a * (exp(-0.5 * r2) - kExp4) * (1.0 / (1.0 - kExp4)));
    if (alpha < 1.0 / 255.0) discard;
    out_color = vec4(v_color.rgb * alpha, alpha);
}
