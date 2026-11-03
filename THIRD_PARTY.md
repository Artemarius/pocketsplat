# Third-party dependencies (approved list)

All pulled via CMake FetchContent, pinned. Update this table whenever a dependency is added or bumped.
Licenses verified by reading each fetched LICENSE file on 2026-09-01; the "LICENSE first line" column
quotes it verbatim.

The shipped binaries carry these licence texts: `scripts/third_party_notices.py` builds them from the fetched
checkouts (the Windows zip at packaging time; the Android asset `android/app/src/main/assets/third_party_notices.txt`
is committed and must be regenerated when a pin or an app Gradle dependency changes). The APK's JVM libraries
(AndroidX, Kotlin stdlib, kotlinx.coroutines, JetBrains annotations, Guava ListenableFuture) are all Apache-2.0.

| Dependency | Use | License | LICENSE first line | Pinned tag | Scope |
|---|---|---|---|---|---|
| nianticlabs/spz | SPZ read/write | MIT | "MIT License" | `v3.0.0` | core |
| facebook/zstd | SPZ compression | BSD-3 (dual BSD OR GPLv2; used under BSD) | "BSD License" (LICENSE; COPYING is the GPLv2 alternative we do not use) | `1.5.6` via spz URL+SHA256 pin | core (transitive) |
| madler/zlib | SPZ gzip support | zlib | "Copyright notice:" | `1.3.1` via spz SHA256 pin; URL overridden to zlib.net/fossils (upstream 404) | core (transitive) |
| g-truc/glm | Math | MIT (dual Happy Bunny / MIT; used under MIT) | "GLM is licensed under The Happy Bunny License or MIT License" | `1.0.3` | core, render |
| google/googletest | Unit tests | BSD-3 | "Copyright 2008, Google Inc." | `v1.18.0` | tests |
| KhronosGroup/Vulkan-Headers | Vulkan headers for volk/VMA | Apache-2.0 | "Copyright 2015-2026 The Khronos Group Inc." | `vulkan-sdk-1.4.357.0` | render |
| zeux/volk | Vulkan loader | MIT | "Copyright (c) 2018-2026 Arseny Kapoulkine" | `vulkan-sdk-1.4.357.0` | render |
| GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | Allocations | MIT | "Copyright (c) 2017-2026 Advanced Micro Devices, Inc." | `v3.4.0` | render |
| charles-lunarg/vk-bootstrap | Instance/device/swapchain | MIT | "Copyright © 2020 Charles Giessen" | `v1.4.360` | render |
| ocornut/imgui | Desktop debug HUD | MIT | "The MIT License (MIT)" | `v1.92.9b` | desktop |
| glfw/glfw | Desktop window | zlib | "Copyright (c) 2002-2006 Marcus Geelnard" | `3.5.1` | desktop |
| KhronosGroup/glslang (via Vulkan SDK / NDK shaderc) | Build-time GLSL→SPIR-V | BSD-3/Apache-2.0 | SDK | SDK | build |
| jbeder/nanoflann *(optional)* | kNN fallback for SOR | BSD-2 | — | — | core (only if own grid is too slow) |

Benchmark fixtures — downloaded by `scripts/bench_web.py fetch` into the gitignored `.cache/`, pinned
by npm integrity hash, served only to the phone's browser during competitor measurements; never
linked, bundled or committed:

| Package | Use | License | Pinned version |
|---|---|---|---|
| @playcanvas/supersplat-viewer | Competitor baseline (WebGPU / WebGL2 / sort-free) | MIT | `1.41.0` |
| @sparkjsdev/spark | Competitor baseline (WebGL2) | MIT | `2.4.0` |
| three | Spark's host engine | MIT | `0.180.0` |

Reference-only (read for ideas, never copied or linked): NVIDIA `vk_gaussian_splatting` (Apache-2.0),
`jaesung-cs/vkgs` and `vulkan_radix_sort` (MIT — benchmark reference for the sort), PlayCanvas
`splat-transform`/SuperSplat (MIT — cleaning UX reference). **Never**: INRIA 3DGS /
diff-gaussian-rasterization (non-commercial), 3DGS.cpp (LGPL), any unreleased research code.
