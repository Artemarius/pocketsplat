# Formats: PLY in, SPZ on disk, GPU pack at runtime

## 1. Input: 3DGS PLY (INRIA layout)

Binary little-endian PLY, one `vertex` element. Required properties and their meaning:

| Property | Type | Meaning |
|---|---|---|
| `x y z` | float | Center |
| `nx ny nz` | float | Present in INRIA output, always zero, ignored on read, not written |
| `f_dc_0..2` | float | Base color as SH degree-0 coefficient: `rgb = 0.5 + 0.28209479177 · f_dc`, then clamp to [0,1] |
| `f_rest_0..N` | float | Higher SH coefficients, **planar per channel**: for degree *d* with *m = (d+1)²−1* coefficients per channel, `f_rest_[0..m)` are R, `[m..2m)` G, `[2m..3m)` B. N ∈ {0, 9, 24, 45} ⇒ degree {0, 1, 2, 3}. Anything else is rejected |
| `opacity` | float | Logit; `α = sigmoid(opacity)` |
| `scale_0..2` | float | **Log** of the ellipsoid radii; `s = exp(scale)` |
| `rot_0..3` | float | Quaternion `(w, x, y, z)`, not guaranteed normalized — normalize on read |

Covariance: `Σ = R S Sᵀ Rᵀ` with `R` from the quaternion and `S = diag(s)`.

Coordinate system: PLYs from COLMAP-based pipelines are in COLMAP camera convention, typically
**RDF** (X right, Y down, Z forward). There is no metadata to tell you — `splatconv` takes an explicit
`--in-coords RDF|RUB|…` flag (default RDF) and we test with a scene whose orientation is known.

## 2. On-disk: SPZ

Niantic's SPZ (MIT): quantized attributes (24-bit fixed-point positions with a header-declared
fractional bit count, 8-bit log-scale, 8-bit rotation, 8-bit alpha, 8-bit color, 5–8-bit SH) plus
zstd (SPZ 4; earlier versions gzip). ~10× smaller than PLY. Reasons it won over a home-grown format:
permissive license, widely adopted (Scaniverse, Adobe, PlayCanvas import), preserves splat order (so
our Morton ordering survives), explicit coordinate-system handling in the library (default **RUB**),
and it stores exactly what we need at SH 0–1.

Rules:
* We always write SPZ in **RUB** (the library default and our renderer's system). Reading a PLY in RDF
  therefore rotates data *and* rotates SH1 coefficients (the library does this; SH1 rotation is just the
  3×3 rotation applied to the linear band with the SH basis' axis permutation).
* SH degree written is 0 or 1. Higher input degrees are truncated *before* writing (drop the bands —
  DC is already the mean color; no re-fit needed).
* Chunk AABBs and cleaning provenance: recompute at load (O(n), sub-millisecond per 100 K on a phone)
  unless the SPZ extension mechanism turns out to be a clean fit (open question in PROJECT.md).
* PLY export is provided for interop; it re-expands to floats (degree 0 or 1, no normals).

## 3. Runtime: GPU pack (structure of arrays)

Built by `splatcore::build_pack()` from a cleaned, Morton-ordered `SplatCloud`. All arrays are
`std430`-friendly flat scalar arrays; the renderer uploads them into separate SSBOs so each pass reads
only what it needs (cull: positions + chunk AABBs; preprocess: everything; draw: only the compact
`VisibleSplat` records produced by preprocess).

| Array | Per splat | Encoding | Notes |
|---|---|---|---|
| `pos` | 12 B | 3 × float32 | Scene units, RUB. fp16 would break large scenes; chunk-relative fp16 is a possible later optimization |
| `logscale` | 6 B | 3 × float16 | Log-scale keeps dynamic range; `exp` in the preprocess shader |
| `rot` | 4 B | 4 × snorm8 `(w,x,y,z)` | Renormalized in shader; smallest-three packing is a possible later 3→10-bit upgrade |
| *(pad)* | 2 B | | `logscale`+`rot` packed as 3 × uint32 |
| `color` | 4 B | RGBA8 unorm | `rgb` = DC color already converted to [0,1]; `a` = sigmoid(opacity) |
| `sh1` (degree 1 only) | 12 B | 9 × snorm8 + 3 B pad, as 3 × uint32 | Coefficients scaled by a per-pack `sh1_scale` (max-abs over the scene) stored in the header |
| **Total** | **28 B (SH 0) / 40 B (SH 1)** | | 1 M splats ⇒ 28 / 40 MB resident |

Plus per 256-splat chunk (Morton order makes chunks spatially compact): `aabb_min`, `aabb_max` (6 ×
float32 = 24 B) and an `importance_max` float for budget culling — ~110 KB per 1 M splats.
The chunk AABBs are **cull bounds**: each splat contributes its center ± 3·exp(max log-scale)
(isotropic upper bound on the 3σ ellipsoid), so frustum-culling whole chunks can never drop a splat
whose 3σ footprint the renderer would draw. Center-only AABBs (pack v1) were unsafe for culling.

Header (`PackHeader`, 64 B): magic `PSPK`, version, count, chunk size, sh_degree, `sh1_scale`, scene
AABB, coordinate system tag, source hash. The pack is an in-memory/cached artifact, not an interchange
format — SPZ is what leaves the app.

### Pinned bit layout (pack version 2, implemented in `splatcore/pack.h|cpp`)

Version history: v1 stored center-only chunk AABBs; v2 (2026-09-01) stores the conservative cull
AABBs described above. The byte layout is unchanged, but v1 packs are rejected at load so stale
caches cannot silently render with unsafe cull bounds — re-run `splatconv pack`.

* **Header (64 B)**, little-endian, offsets: 0 `magic` u32 (`PSPK` bytes), 4 `version` u32, 8 `count`
  u32, 12 `chunk_size` u32 (=256), 16 `sh_degree` u32, 20 `coords` u32 (1 = RUB), 24 `sh1_scale` f32,
  28 reserved u32, 32 `aabb_min` 3×f32, 44 `aabb_max` 3×f32 (raw min/max of packed positions),
  56 `source_hash` u64 (FNV-1a 64 over the source SplatCloud arrays).
* **`scale_rot`** (3 × u32 per splat): word0 = `h(sx) | h(sy)<<16`; word1 = `h(sz) | snorm8(w)<<16 |
  snorm8(x)<<24`; word2 = `snorm8(y) | snorm8(z)<<8`, high 16 bits zero. `h()` is IEEE fp16 of the
  log-scale; quaternions are sign-canonicalized (`w ≥ 0`) before snorm8.
* **`color`** (u32 per splat): `r | g<<8 | b<<16 | a<<24`, rgb = `clamp(0.5 + 0.28209479·f_dc, 0, 1)`
  unorm8, a = `sigmoid(opacity)` unorm8.
* **`sh1`** (3 × u32 per splat, degree 1 only): 9 snorm8 of `sh[k]/sh1_scale` in coefficient-major
  order (c0.rgb, c1.rgb, c2.rgb) packed little-endian, bytes 9–11 zero. `sh1_scale` = max-abs
  coefficient over the scene (1.0 if all zero).
* **`chunks`** (7 × f32 per 256-splat chunk): cull-bound `min.xyz`, `max.xyz` (see above) and
  `importance_max` where importance = α·(s_x s_y + s_y s_z + s_z s_x). Last chunk may be partial.
* **Serialized order** (`.pspk`, `splatconv pack`): header, `pos`, `scale_rot`, `color`, `sh1`,
  `chunks`, each tightly packed.

### Coverage sidecar (`.pscov`, ADR-0013, implemented in `splatcore/coverage.h|cpp`)

Optional per-splat observed-direction masks for the coverage ("backside") fade, written by
`splatconv coverage` or the app (import, or later from the viewer) and read by both hosts from
`<scene stem>.pscov` next to the scene. A separate file so packs stay unchanged and the fade
stays optional. The masks come from the capture cameras when the scene ships them, else from
virtual cameras for a scene type the user picks (ADR-0015).

* **Header** (32 B, little-endian): `magic` u32 = `"PSCV"`, `version` u32 = 1, `count` u32 (the
  pack's splat count), `grid` u32 = 5, `positions_hash` u64 (FNV-1a 64 over the packed `pos`
  floats: binds the file to the splat order and positions, independent of the SH degree the
  scene was loaded at), `camera_count` u32 (real or virtual), `source` u32 (was `reserved` = 0
  before ADR-0015, so older files read as source 0): 0 = capture cameras, 1 = virtual cameras for
  a room, 2 = virtual cameras for an orbited object. Readers that predate it ignore it.
* **Masks** (`count` × u32, pack order): bit `c` set = the splat was observed from octahedral cell
  `c` of the direction splat → camera. Cell of a direction `d`: `p = d / |d|₁`; for `p.z < 0` fold
  `(x, y) = ((1 − |p.y|)·sgn p.x, (1 − |p.x|)·sgn p.y)` with sgn 0 = +1; then
  `ix = floor((x + 1)/2 · 5)`, `iy` likewise, clamped to 0..4, `c = 5·iy + ix`. Bits 25–31 are zero.
* A loader rejects a file whose count, size, grid or positions hash does not match the scene.
* Observed (`compute_coverage`): the splat centre projects inside the camera image, in front of
  it, and not behind the camera's 1/8-resolution depth buffer of opaque (α ≥ 0.5) splat centres
  (3 % tolerance). Cameras (`splatcore/capture.h`), all in the PLY's world (RDF, converted to RUB
  like the splats): a 3DGS `cameras.json`; a nerfstudio / instant-ngp / Brush `transforms.json`
  (OpenGL camera-to-world; tried as given, with its `applied_transform` undone, and with a
  `dataparser_transforms.json` applied - the one whose cameras see the scene wins); or a COLMAP
  sparse model (`cameras` + `images`, `.bin` or `.txt`; distortion ignored). Camera files whose
  cameras see under 90 % of the scene's opaque splats are rejected (real captures: 99.9-100 %).

## 4. Per-frame transient records (renderer-owned)

`VisibleSplat` (32 B): `ndc_center` (2 × f32), `conic` (3 × f16 + pad), `radius_px` (f16), `color`
(RGBA8), `depth_key` (u32 or u16 + pad). Written by the preprocess pass for visible splats only;
sorted by key; consumed by the vertex shader. Sized for the full splat count once; count is indirect.

Precision decisions (fp16 log-scale, snorm8 rotation, int8 SH1) are gated by the golden-image PSNR
test in Phase 2; the fallback is a pack version with fp32 scales/rotations at 48/60 B per splat.
