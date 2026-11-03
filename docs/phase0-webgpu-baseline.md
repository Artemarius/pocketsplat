# Phase 0 — WebGPU baseline on the Galaxy S24 Ultra (measurement only)

Purpose: get real numbers for this exact phone before writing Vulkan, using third-party viewers
(PlayCanvas SuperSplat viewer / engine examples in Chrome with WebGPU enabled; WebSplatter demo if
hosted). No code is written or kept from this phase.

## Steps
1. Prepare three SH-0 scenes at ~300 K, ~1 M and ~2 M splats (export from SuperSplat with SH removed
   until `splatconv` exists) and one SH-1 scene at ~1 M.
2. Chrome (stable) on the S24U, `chrome://flags` WebGPU confirmed, desktop-site off, 120 Hz off.
3. For each scene: load, orbit continuously (auto-rotate if the viewer offers it), record fps at
   0, 1, 2, 5, 8, 10 min. Use the viewer stats overlay and, if possible, a Chrome performance trace
   (`chrome://inspect` remote debugging) for GPU vs JS time.
4. Repeat the 1 M SH-0 run at the lowest internal-resolution setting the viewer offers.
5. Independently record the thermal curve of the phone with a GPU stress benchmark (3DMark Wild Life
   Extreme stress) — this separates "the viewer is slow" from "the phone throttled".

## Record
`docs/benchmarks/phase0-webgpu-s24u.csv`: `viewer, viewer_version, scene, splats, sh_degree,
resolution_setting, t_min, fps, notes`. Plus a paragraph in `docs/benchmarks/README.md`:
fps at 1 M SH 0 cold and at minute 10, and the internal-resolution decision for Phase 3.

## Expectation (from published Adreno 750 data, to be confirmed)
Roughly 25–35 fps at ~1 M splats cold in WebGPU; native Vulkan at ~1080p-class internal resolution
should exceed this. If the WebGPU number is far lower, investigate resolution first (fill-rate), then
thermal, before concluding anything about splat count.
