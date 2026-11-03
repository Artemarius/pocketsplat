# Silhouette rays: research survey (2026-09-02, S12)

Follow-up to the S10 silhouette-streak investigation (`docs/benchmarks/README.md`), which ended
with a documented known issue: after four fixes (fp32 conic, sqrt(8)σ cutoff, kernel
renormalization, `clamp_needle_scales`), a residue of thin bright straight rays still radiates
from object silhouettes into the background at some view angles (museum1, desktop and Android
alike). Reference viewers (PlayCanvas WebGL2 model-viewer and a second, independent desktop
renderer) show only soft fuzz on the same scene — including on the *uncleaned* original, which
means the data is renderable cleanly and our pipeline is missing something they have.

This note records a survey of (a) production open-source viewer implementations at the shader
level, (b) the anti-aliasing/filtering theory line (EWA → 3DGS → Mip-Splatting and successors),
and (c) the needle/floater/cleaning literature, then ranks candidate fixes. Survey done via three
parallel research passes over papers and viewer sources; all code inspected is MIT/Apache unless
flagged (non-permissive sources are papers-only per CLAUDE.md §2).

## Finding 1 — the missing guard: per-axis extent clamp with falloff compression

Every popular web viewer clamps the projected splat's screen-space extent **per eigen-axis** to a
small constant, and evaluates the Gaussian kernel in **quad-local UV (eigen-basis) coordinates**:

| Viewer | Per-axis half-extent cap | Kernel eval | Dilation | Opacity comp. |
|---|---|---|---|---|
| PlayCanvas engine (MIT) | 2·min(√(2λ), min(1024, viewport_min)) | quad UV | +0.3 always | opt-in, default off |
| mkkellogg/GaussianSplats3D (MIT) | min(√8·√λ, 2048) px | quad UV | only in AA mode | opt-in, default off |
| antimatter15/splat (MIT) | ≤ 2048 px (axis min(√(2λ),1024), UV ±2) | quad UV | none | none |
| Babylon.js (Apache-2.0) | same as antimatter15 | quad UV | kernelSize 0.3 | opt-in, default off |
| Spark (MIT) | min(√8·√λ, **512**) px | quad UV | +0.3 always | **always on** |
| gsplat CUDA (Apache-2.0) | none (image-bounds cull only) | screen conic + `sigma<0` reject | eps2d 0.3 always | opt-in ("antialiased") |

The clamp semantics are the key subtlety: because the fragment kernel runs in quad UV and the UV
range is **not rescaled** when the axis is clamped, clamping *shrinks the Gaussian's σ along that
axis* — the full falloff (down to zero, for renormalized kernels) is compressed into the clamped
extent. A needle whose true √8σ footprint is tens of thousands of pixels renders as a ≤ 0.5–2 K px
smear that ends softly: "fuzz". Our pipeline (square quad sized by λmax, capped at 4096 px, exact
conic evaluated in screen space) draws the *true* Gaussian: a needle's major-axis falloff is
nearly flat across the visible extent → a constant-brightness straight ray. Same data, same
kernel formula (PlayCanvas's renormalized-truncated kernel is exactly ours) — the difference is
the clamp-and-compress.

Corroborating detail: PlayCanvas *default* runs dilation with **no** opacity compensation and
still shows no rays — so it is the extent clamp doing the work there, not the AA term.

Guards we checked ourselves against this survey and already have: post-compensation 1/255 alpha
cull in preprocess (`splat_preprocess.comp`), negative-quadratic-form discard in the fragment
shader (`splat.frag`, equivalent to gsplat's `sigma < 0` reject). Guards we lack: the per-axis
clamp (and oriented quads generally), a min-pixel-size cull (PlayCanvas kills splats under 2 px),
and gsplat's opacity-adaptive extent (below).

## Finding 2 — the affine Jacobian is a documented cause of needle rays

"On the Error Analysis of 3D Gaussian Splatting and an Optimal Projection Strategy"
(ECCV 2024, arXiv:2402.00752) analyzes the Taylor remainder of the EWA affine projection: the
error is minimal at image center and grows rapidly with eccentricity, and the paper **explicitly
names "needle-like structures" as a visible signature of the affine error**. The fix — build the
per-Gaussian Jacobian about the ray through the splat center (project onto the tangent plane of
the unit sphere, then unwarp to the image plane) — is a few extra rotations per splat in
preprocess, works on pre-trained scenes at render time, and is reported performance-neutral.
This matches our angle-dependence: the residual rays appear at eccentric silhouette positions and
vanish a few degrees later. AAA-Gaussians (arXiv:2504.12811) files elongated-streak artifacts
under the same aliasing+projection taxonomy. The affine error *adds fictitious elongation* to
anisotropic splats; the reference viewers' clamp merely hides that component.

## Finding 3 — where our pipeline already sits in the AA landscape

* Our dilation + `sqrt(det_before/det_after)` compensation **is exactly** gsplat's "antialiased"
  mode / Mip-Splatting's 2D Mip filter (their s = 0.3 ≈ our 0.3). Nothing further to gain there;
  H3-as-2D-filter is already implemented.
* No screen-space additive filter shortens a needle's **major** axis — it only dims the splat
  (EWA-correct Vʰ = I with energy normalization would dim needles harder than 0.3 does, at the
  cost of dimming legitimate sub-pixel detail too). Screen-space filtering alone turns a bright
  ray into a faint ray, never into fuzz.
* The only filter in the literature that attacks needles in 3D is **Mip-Splatting's 3D smoothing
  filter** (arXiv:2311.16493, Eq. 9): Σ₃D ← Σ₃D + σ_f²·I with α ← α·√(det Σ₃D / det(Σ₃D+σ_f²I)),
  σ_f² = 0.2·(d/f)². It is a pure per-splat parameter transform — appliable post-hoc in a viewer
  (using the current view's d/f) or baked in the converter with a conservative sampling rate. It
  fattens the sub-pixel axes *before* projection and annihilates the opacity of truly degenerate
  splats. Known risk on scenes not trained with it (gsplat issue #840): legitimate fine detail
  (spokes, leaves, fur sparkle) dims — so it must be **gated by an anisotropy score**, making it
  a surgical needle-killer rather than a scene-wide blur.
* gsplat's **opacity-adaptive extent** is worth adopting regardless: quad radius =
  σ·min(cutoff, √(2·ln(α·255))) — the quad is cut where the splat's own contribution falls below
  1 LSB, which shortens faint needles' quads drastically before any hard clamp. (Our renormalized
  kernel shifts the formula slightly; derive against α_renorm < 1/255.)

## Finding 4 — sorting exonerated

Popping/sorting artifacts (StopThePop family, arXiv:2402.00525) redistribute color only *within*
the union of projected splat supports; reordering cannot emit energy into empty background. A
straight ray crossing the silhouette **is** a real needle's projected support. Sorting can at
most modulate a ray's brightness frame to frame. Deprioritized as a cause, as suspected.

## Finding 5 — why existing cleaning/pruning cannot catch our needles

* Contribution-based post-hoc pruners (PUP 3D-GS, LightGaussian, RadSplat, Mini-Splatting,
  TrimGS) rank splats by their contribution to *training views* — our needles are
  high-contribution at the offending angles and fine at training-like angles, so these keep them.
  (Most also need the training cameras/images, which viewers don't have.)
* Isolation/cluster/voxel floater filters (SOR, PlayCanvas splat-transform `--filter-floaters` /
  `--filter-cluster`, kNN-isolation scores) pass them: silhouette needles are *attached* to the
  surface. TIDI-GS (arXiv:2601.09291) even adds explicit whitelists to *protect* thin/anisotropic
  splats so wires survive — confirming isolation alone cannot separate bad needles from wires.
* Needle *detection* metrics are well developed and reusable for gating: effective rank
  (arXiv:2406.11672; erank < 1.02 ≈ ≥ 20:1 needle; needle population grows during training and
  causes "spiky artifacts" worst in views far from training cameras — our regime exactly),
  spectral entropy (Spectral-GS, arXiv:2409.12771, split when H < 0.5), Vis4GS's needle score
  (arXiv:2606.26985). Threshold survey: physics work clamps aspect at 3:1, geometry work calls
  ≥ 20:1 pathological; our `clamp_needle_scales` at 6×mid already sits at the aggressive end of
  the published spectrum — consistent with fur damage when tightened further; do not tighten.

## Finding 6 — the normal-aware filtering hypothesis vs prior art

Not previously done. Every ingredient exists separately — min-axis-as-normal proxies (SuGaR,
2DGS, Gaussian Surfels, GaussianShader), Mahalanobis mutual-kNN graphs over pretrained splat
clouds (arXiv:2502.17531 — best foundation for the neighborhood machinery), needle classifiers
(above) — but no published work fades or clamps splats whose **major axis disagrees with a
neighbor-estimated surface plane**, post-hoc. Two structural notes from the literature:

* For a needle, its own min axis is rotationally degenerate (useless as a normal), but its major
  axis is well-conditioned — so testing the needle's major axis against a *neighborhood* plane is
  the right formulation, as hypothesized.
* The design risk is fur/grass/whiskers: legitimately perpendicular to the surface. The remedy is
  gating by **neighborhood planarity confidence**: fur neighborhoods are needle-dominated and not
  planar, so a planarity gate exempts them automatically (precedent: effective-rank's adaptive
  preservation of elongated splats; TIDI-GS's detail guards).

Sketch for a v1 `splatconv` cleaning pass (view-independent, bakes an opacity multiplier, no pack
or shader change): kNN (k=16, grid hash over our existing Morton order) → weighted PCA over
neighbor centers (weights = opacity × neighbor diskness) → planarity confidence
C = (λ2−λ3)/λ1, act only when C ≥ ~0.6 → needle test on the splat (existing 6×mid ratio or
erank < 1.1) → misalignment θ = asin(|dot(major_axis, plane_normal)|) → fade
α *= 1 − C·smoothstep(20°, 45°, θ), floored at 0.1 for the first experiments; never touch splats
whose neighborhood is *linear* ((λ1−λ2)/λ1 > 0.7 — wires/branches). CPU-seconds for 1 M splats,
embarrassingly parallel.

## Root cause confirmed (2026-09-02, addendum)

Reading Finding 1 against our own numerics settles it: the guard we are missing is
**structural, not a threshold**. The web viewers in the table draw ellipse-oriented quads in the
eigenbasis of Σ₂D, pass quad-local σ-unit coordinates as an interpolated varying, evaluate
α = a·exp(−½·u·u), and **never invert the 2D covariance**; a splat whose minor eigenvalue comes
out non-positive has no valid quad and is dropped. They stay clean on our uncleaned scene with
widely different dilation, opacity-compensation and cut-off settings, so every one of our
threshold-level hypotheses (H1–H6) was secondary; the mechanism is the kernel parameterization
itself.

The failure mode in our conic path, spelled out: at silhouettes a needle's Σ₂D has λmax up to
10⁵–10⁷ px² and a λmin that is pure floating-point cancellation noise (fp32 relative error ~10⁻⁷
on covariance entries → absolute λmin error of order 0.1–1 px², the same order as our +0.3
dilation — which is why the dilation doesn't save us and why our post-dilation `det ≤ 0` reject
doesn't fire). The inverted conic is then indefinite-to-noise: the quadratic form sits ≈ 0 along
the ridge eigendirection across the whole (effectively uncapped) quad, `power > 0` and
`power < −4` discards never trigger there, and the kernel paints α ≈ a along a perfectly
straight line — brightness equal to the splat's own opacity. Which side of zero λmin lands on
flips with a few degrees of camera rotation, exactly matching the angle-dependent
appear/disappear behavior. In the eigenbasis-UV parameterization this failure mode is
*unrepresentable*: r² = u·u is exact regardless of Σ₂D conditioning, and the worst case for a
legitimate huge needle is a correctly-weighted sub-pixel-wide streak — the "soft fuzz" reference
viewers show.

Numerical corollary for our current code: our eigenvalue discriminant is computed as
`mid² − det`, which itself cancels catastrophically for extreme needles; the stable form is
`((a−c)/2)² + b²` (algebraically identical, no cancellation), with an axis-aligned fallback when
|b| is tiny. Constants adopted: per-axis half-extent cap min(√8·√λ, 2048 px) (the
mkkellogg/GaussianSplats3D value); λmin ≤ 0 → reject; √8σ support with the 1/255 floor.

## Root cause #2 — the actual spike mechanism (2026-09-02, final addendum)

The eigenbasis rewrite eliminated the fp32-indefinite-conic mechanism (real, but not the visible
artifact) — the bright spikes survived it, survived dilation on/off, cleaning on/off, SH on/off,
and FOV changes, on the raw uncleaned scene. A side-by-side at matched poses against an
independent reference renderer, on the same raw file, found it:

**The EWA Jacobian's y row had the wrong sign — the computed 2D covariance was the y-mirror of
the true one.** Our hosts negate the projection's y row for Vulkan's y-down NDC (correct, and
deliberately "handled in the projection, never in data"), and the pixel-space center comes from
that y-negated projection — but the camera UBO carries a *positive* focal_y and the preprocess
Jacobian used +fy. So J_used = diag(1,−1)·J_true and Σ_computed = D·Σ_true·D with D = diag(1,−1):
`a` and `c` unchanged, **b sign-flipped**. Consequences, which explain every observation:

* Eigenvalues unchanged → every λ-level instrument (degenerate reject, extent caps, s_cut,
  overdraw stats, the golden PSNR gate, the 360-pose sweep) was blind to it, and it survived the
  conic→oriented-quad rewrite because it sits upstream of both kernels.
* Eigenvectors mirrored about the horizontal axis → a splat trained to lean +37° drew leaning
  −37°. Near-axis-aligned splats are (almost) self-mirror-images, so the bulk of the image looked
  plausible; diagonal elongated splats — strands, edge splats, drapery folds, exactly what
  training lays *along* features — drew **across** them. At silhouettes they stood proud of the
  contour as the bright spikes; over the interior they crisscrossed into the soft veil that ate
  faces and folds. The spikes and the crispness gap were one bug.
* Self-consistent in every build → image-vs-self comparisons could never catch it; identical on
  NVIDIA and Adreno → correctly looked like "not a driver issue".
* The cure is structural: take the Jacobian's focal terms from the same (y-negated) projection
  that places the splat center, signs included, so the two are consistent by construction.

Fix: negate the Jacobian's y row in `splat_preprocess.comp` (shared by both hosts). Goldens
regenerated (the old ones were self-consistent images of the bug). New permanent gate:
`golden_chiral_orientation` (`scripts/chiral_check.py`) renders a synthetic splat elongated
along world +x+y and asserts the projected slope's handedness — it fails on the mirrored
covariance with correlation +0.99 and passes fixed; fully synthetic, never skips. Verified
against the reference at three matched poses on the raw uncleaned scene: spikes gone, veil gone,
detail parity. Quantization (fp16 log-scales, snorm8 quats) was assessed NOT GUILTY — the mirror
was a 2·angle orientation error, two orders of magnitude above quantization noise; the mobile
pack budget stands.

The silhouette-ray saga therefore had **two stacked root causes**: the fp32-indefinite conic
(fixed by the eigenbasis rewrite, ADR-0006 — kept, also the fill win) and this covariance
mirror. The "residue after four fixes" from S10 was the mirror all along. Remaining smaller
quality levers, ranked by the comparison: the always-on +0.3 dilation (consider a
no-dilation toggle), device internal resolution 0.6×, and possibly
relaxing `clamp_needle_scales` (`--needle-aspect 6`) — it was tuned against mirrored needles and
partly suppressed the symptom, so correct rendering may allow keeping more legitimate strand
detail.

## Ranked plan

1. **Oriented quads + per-axis extent clamp with falloff compression** (Finding 1). Clamp each
   eigen-axis half-extent to C ≈ min(1024, viewport_min) and evaluate the kernel in quad UV so
   the clamp shrinks σ instead of truncating (equivalently: λ_i ← min(λ_i, C²/8) and rebuild the
   conic). Identical output for every splat under the cap; kills the ray symptom by construction;
   and oriented quads were already queued as the Adreno fill-rate win (a square quad on a 100:1
   splat covers ~100× the pixels) — one change, two payoffs. Mobile-safe, vertex/preprocess ALU
   only.
2. **Opacity-adaptive quad extent** (gsplat) — near-free preprocess tweak, shortens faint
   needles' quads on principle; adopt alongside 1.
3. **Anisotropy-gated 3D smoothing filter** (Mip-Splatting Eq. 9, view-adaptive σ_f² = 0.2·(d/f)²
   in preprocess, applied only when a needle score fires) — the only mechanism that attacks
   degenerate splats in 3D; ~20 flops/splat behind a gate. Evaluate against goldens for detail
   loss.
4. **Tangent-plane (optimal) projection** (arXiv:2402.00752) — removes the *fictitious*
   elongation the affine Jacobian adds at eccentric positions (the angle-dependent component).
   Complements 1; performance-neutral per the paper.
5. **Normal-aware cleaning pass** (Finding 6) — the principled data-side fix for needles that are
   wrong at every angle; prototype after 1–2 land, since the residue they leave defines the
   actual target set.
6. Housekeeping candidates: min-pixel-size cull (2 px), Vis4GS/erank needle score as the shared
   gate for 3 and 5.

Items 1–4 are view-time and testable on the desktop host against the golden gate + the
`--dist-scale 0.6 --yaw-deg 60` museum1 repro before any device run.

## License ledger for this survey

Idea-safe to read (permissive): PlayCanvas engine / SuperSplat / splat-transform (MIT),
mkkellogg/GaussianSplats3D (MIT), antimatter15/splat (MIT), Spark (MIT), Babylon.js (Apache-2.0),
gsplat (Apache-2.0), Analytic-Splatting (MIT), Niantic spz (MIT, format only). Papers-only
(non-permissive or Inria-derived code): 3DGS reference, Mip-Splatting, StopThePop, VRSplat,
AAA-Gaussians, RaDe-GS, 2DGS, SuGaR, Gaussian Surfels, TrimGS, vkraygs (CC BY-SA), SA-GS (verify
before touching). Everything adopted gets implemented from the paper/spec per CLAUDE.md §2–3.

## Primary sources

Mip-Splatting arXiv:2311.16493 · gsplat docs + PR #117 + issue #840 · EWA Splatting (Zwicker et
al., TVCG'02) · Optimal Projection arXiv:2402.00752 · AAA-Gaussians arXiv:2504.12811 ·
Analytic-Splatting arXiv:2403.11056 · SA-GS arXiv:2403.19615 · AA-2DGS arXiv:2506.11252 ·
StopThePop arXiv:2402.00525 · effective rank arXiv:2406.11672 · Spectral-GS arXiv:2409.12771 ·
PhysGaussian arXiv:2311.12198 · PUP 3D-GS arXiv:2406.10219 · RadSplat arXiv:2403.13806 ·
Mini-Splatting arXiv:2403.14166 · TIDI-GS arXiv:2601.09291 · Laplace-Beltrami on GS
arXiv:2502.17531 · VoD-3DGS arXiv:2501.17978 · Vis4GS arXiv:2606.26985 · ArtifactWorld
arXiv:2604.12251 · viewer sources: playcanvas/engine `gsplatCorner.js` + `frag/gsplat.js`,
mkkellogg/GaussianSplats3D `SplatMaterial3D.js`, antimatter15/splat `main.js`, Babylon.js
`gaussianSplatting.fx`, sparkjsdev/spark `splatVertex.glsl`, nerfstudio-project/gsplat
`ProjectionEWA3DGSFused.cu`.
