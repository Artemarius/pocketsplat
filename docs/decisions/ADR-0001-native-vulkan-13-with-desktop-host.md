# ADR-0001 — Native Vulkan 1.3 renderer, hosted on desktop and Android

**Status**: accepted (2026-08-31)

**Context**: Mobile 3DGS viewers exist as WebGPU/WebGL pages or engine plugins; nothing native and
permissive targets Android. The author has Vulkan compute experience (NDK). Targets are S24U/S25U,
both Vulkan 1.3.

**Decision**: One Vulkan 1.3 renderer library (`splatrender`) using only core 1.3 features (dynamic
rendering, synchronization2, timeline semaphores) and no vendor extensions. Two hosts: a Windows
GLFW window for the development loop (RTX 3060, RenderDoc/Nsight) and the Android app. GLSL compiled
to SPIR-V at build time.

**Consequences**: 10× faster iteration than deploy-to-phone, but Adreno-specific behaviour (subgroup
size, shared memory, tile-based rendering) must be validated on device at least weekly from Phase 2.
No WebGPU build exists (Phase 0 uses third-party web viewers only for measurement). No OpenGL ES.
