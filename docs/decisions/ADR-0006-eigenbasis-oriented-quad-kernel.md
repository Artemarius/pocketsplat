# ADR-0006 — Eigenbasis oriented-quad splat kernel (conic removed)

**Status**: accepted (2026-09-02)

**Context**: A residue of the S10 silhouette-ray artifact survived four fixes (fp32 conic,
sqrt(8)σ cutoff, kernel renormalization, `clamp_needle_scales`): at some view angles, extreme
needle splats drew as thin bright straight rays crossing into the background, on NVIDIA and
Adreno alike, while reference renderers showed only soft fuzz on the same (even uncleaned) data.
Root cause (`docs/silhouette-rays-research.md`, root-cause addendum): at silhouettes a needle's
projected covariance has λmax up to ~1e7 px² and a λmin that is pure fp32 cancellation noise
(±0.1–1 px² — the same order as the +0.3 dilation, so the post-dilation `det ≤ 0` reject cannot
catch it). Inverting such a matrix yields an indefinite-to-noise conic whose quadratic form sits
≈ 0 along the ridge eigendirection: the fragment kernel paints α ≈ a along a straight line across
the (effectively uncapped, 4096 px) screen-aligned square quad. The λmin sign flips with small
camera rotations — hence the angle-dependent appear/disappear. Our own eigenvalue discriminant
`mid² − det` suffered the same cancellation. Independent confirmation: five MIT/Apache web
viewers (PlayCanvas, mkkellogg, antimatter15, Babylon, Spark) all evaluate the kernel from
interpolated quad-local coordinates and never invert Σ₂D.

**Decision**: Replace the per-fragment conic with an eigenbasis parameterization, implemented
from the math (no code taken; the scheme is common to the viewers above):

* Preprocess computes the 2×2 eigendecomposition in the cancellation-free form
  λ± = mid ± √(((a−c)/2)² + b²), with an axis-aligned eigenvector fallback for |b| < 1e-3, and
  **rejects the splat when λmin ≤ 0** (degenerate projection — mathematically impossible, so
  numerically meaningless).
* The draw record (still 32 bytes) carries the splat center, the two half-axis vectors
  (eigenvectors × extent, pixels), packed color, and the support cutoff `s_cut` in σ units.
* **Opacity-adaptive support**: s_cut = √(−2·ln(e⁻⁴ + (1−e⁻⁴)/(255·α))) ≤ √8 — the radius where
  the renormalized kernel falls below 1/255, so faint splats get proportionally shorter quads
  (idea from gsplat's opacity-adaptive extent, adapted to our renormalized kernel).
* **Per-axis extent cap 2048 px** (the mkkellogg/GaussianSplats3D and antimatter15 value). The
  fragment UV span stays ±s_cut regardless, so a capped axis renders a compressed Gaussian (soft
  end), not a truncated one. Splats with max extent < 0.5 px are culled (below one LSB everywhere).
* The fragment shader computes r² = dot(uv, uv) on the interpolated varying — exact for any
  Σ₂D conditioning — and keeps the existing renormalized kernel and 1/255 floor. The
  `power > 0` guard and the fp32-conic-storage workaround are moot and removed.
* The +0.3 dilation with opacity compensation (S6 halo fix) is unchanged.

**Consequences**: The ray failure mode is unrepresentable by construction. Oriented quads also
collapse needle overdraw (a square quad on a k:1-aspect splat covered ~k× the pixels): museum1
zoomed repro view dropped 4.46 → 1.51 ms GPU on the RTX 3060; Adreno gains re-measured in Phase 3
benches. All 54 tests pass, goldens unchanged (≥ 40 dB), validation clean; a 360-pose automated
before/after sweep (yaw × pitch × zoom) is identical within 4 LSB, so behavior deltas are
sub-LSB except for removed rays and the new λmin ≤ 0 rejects (the residual ray itself never
reproduced from canned poses — the kill is established structurally, with interactive
spot-checks at the known angles as the remaining confirmation). The 32-byte record layout contract
between `splat_preprocess.comp` and `splat.vert` changed — both shaders and the header comments
are the reference. Follow-ups deliberately deferred until the residue (if any) is re-hunted:
tangent-plane projection, gated 3D smoothing filter, normal-aware cleaning (research doc §plan).
