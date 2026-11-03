# ADR-0003 — Thin Vulkan helpers (volk, VMA, vk-bootstrap) + own ~1.5 k-line wrapper; no engine

**Status**: accepted (2026-08-31)

**Context**: Raw Vulkan is verbose; the risk is thousands of lines of unmaintainable setup code.
Alternatives: Vulkan-Hpp RAII (Apache), engines (Filament/Diligent/bgfx — permissive but large
abstractions that hide indirect compute/sort control), Kompute (compute-only), or thin helpers.

**Decision**: `volk` (MIT, loader), `VulkanMemoryAllocator` (MIT, allocations), `vk-bootstrap` (MIT,
instance/device/swapchain selection), `glm` (MIT), `Dear ImGui` (MIT, desktop debug HUD), `GLFW`
(zlib, desktop host only). On top: a small owned `vkutil` layer — `Context`, `Buffer`, `Image`,
`ComputePass`, `GraphicsPipeline`, `FrameRing`, `GpuTimer`, descriptor helpers — modelled on how
small Vulkan splat viewers (e.g. `jaesung-cs/vkgs`, MIT) are structured. Plain C API, no `vulkan.hpp`
(NDK header version lag, compile-time cost, no benefit for a ~10-pipeline renderer).

**Consequences**: ~1.5 k lines of wrapper we own and understand; every Vulkan object creation goes
through it; validation layers in debug builds; the same code compiles for Windows and Android.
