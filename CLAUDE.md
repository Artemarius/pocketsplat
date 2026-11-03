# CLAUDE.md — PocketSplat technical configuration

Read this fully before touching code. `PROJECT.md` (gitignored, may be absent) holds the roadmap and
current phase; `docs/` holds the design notes that this file only summarizes.

## 1. What this repository is

Native Android Vulkan 1.3 viewer + editor for 3D Gaussian Splatting scenes, plus a portable C++20 core
library (`splatcore`) and a desktop CLI (`splatconv`) for conversion and cleaning. The renderer
(`splatrender`) is hosted both by a desktop GLFW window (development loop on an RTX 3060) and by the
Android app (NDK). **Training is not part of this repo.**

## 2. Hard constraints — never violate

1. **SH degree 0 and 1 only** at runtime and in our storage. Input PLY may carry degree 2–3; the
   converter truncates. Do not add SH 2/3 code paths to the renderer.
2. **Permissive licenses only**: MIT, BSD, Apache-2.0, zlib. No GPL, LGPL, MPL, CC-NC, or
   "research/non-commercial" code anywhere — including transitive deps, shader snippets, and "just this
   one function". The INRIA 3DGS reference code and `diff-gaussian-rasterization`, and 3DGS.cpp (LGPL),
   are **read-only references for ideas** — never copy code from them. When in doubt, implement from
   the paper and note the source in a comment.
3. **Clean-room**: no code, shader text, file layouts or identifiers from any employer or closed
   codebase. Everything here is derived from public papers, public specs (SPZ, PLY, Vulkan) and
   permissive open source.
4. **No test data in git** (`*.ply`, `*.spz`, etc. are ignored). `data/manifest.json` records sources.
5. **Mobile-safe GPU code**: no inter-workgroup spin-waiting (no decoupled look-back / single-pass scan),
   no reliance on a specific subgroup size, no 64-bit atomics, no `VK_NV_*`/vendor-only extensions.
   Everything must run on Adreno 7xx with Vulkan 1.3 core features.

## 3. Hardware and toolchain

| | |
|---|---|
| Dev machine | Windows 10 Pro, RTX 3060 Laptop (GA106, sm_86, 6 GB), CUDA 12.8 (unused here), MSVC 2022, CMake ≥ 3.24, Vulkan SDK ≥ 1.3 |
| WSL2 Ubuntu 24.04 | Available for `splatcore`/`splatconv` builds and tests only — **no usable Vulkan in WSL2**, the renderer is built natively on Windows |
| Test device 1 | Samsung Galaxy S24 Ultra — Snapdragon 8 Gen 3, Adreno 750, Vulkan 1.3, 12 GB RAM, 3120×1440 |
| Test device 2 (later) | Samsung Galaxy S25 Ultra — Snapdragon 8 Elite, Adreno 830 |
| Android | Android Studio (latest stable), NDK LTS, minSdk 33, Kotlin + Jetpack Compose UI, C++ core via JNI |
| Shaders | GLSL 4.60 (`#version 460`) compiled to SPIR-V **at build time** with `glslangValidator`; SPIR-V embedded in the binary. No runtime shader compilation. |

## 4. Repository layout

```
core/            splatcore — portable C++20, zero GPU deps. PLY/SPZ IO, cleaning, SH truncation,
                 Morton ordering, chunk AABBs, GPU-pack builder, stats, quality-controller policy (ADR-0009).
                 GoogleTest unit tests in core/tests.
tools/splatconv/ Desktop CLI over splatcore: inspect | convert | clean | pack | coverage (per-splat
                 observed-direction sidecar from a 3DGS cameras.json, ADR-0013) | replay (quality
                 controller over a recorded bench CSV). Exit code ≠ 0 on any failure.
render/          splatrender — Vulkan 1.3 renderer library (volk + VMA + vk-bootstrap + thin own wrappers).
                 render/shaders/*.comp|.vert|.frag → SPIR-V via CMake.
desktop/         GLFW hosts: pocketsplat-desktop (dev/bench, ImGui debug HUD) and pocketsplat-viewer
                 (end-user Windows viewer: phone navigation ported in trackball.h, platform.cpp).
android/         Gradle project: Compose UI, SurfaceView, NDK bridge to splatrender + splatcore.
docs/            Design notes (formats, sorting, cleaning), ADRs in docs/decisions, curated benchmark CSVs.
scripts/         Benchmark drivers, adb helpers, data manifest tooling (Python 3, stdlib + numpy only;
                 docs figure scripts may also use matplotlib).
third_party/     Empty by design; deps come via FetchContent pinned to tags. See THIRD_PARTY.md.
```

## 5. Build and test commands

```
# core + tools (Windows or WSL2)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# desktop renderer host (Windows only)
cmake -B build -DPOCKETSPLAT_BUILD_DESKTOP=ON
cmake --build build --config Release
build/Release/pocketsplat-desktop <scene.spz> [--bench --frames 600 --csv out.csv] [--gpu intel]
build/desktop/Release/pocketsplat-viewer [scene]   # end-user viewer (WIN32 subsystem, on-demand frames)
python scripts/package_windows.py   # build-release/ with POCKETSPLAT_STATIC_CRT=ON -> dist/*.zip
# phone-sized offscreen renders + image-quality / visible-count A/B of renderer variants
python scripts/quality_ab.py --exe build/desktop/Release/pocketsplat-desktop.exe \
      --scene data/derived/bench/stump-full.sh0.pspk --width 864 --height 1872 \
      --variant ref="--key-bits 32" --variant fast="--key-bits 16 --min-contribution 0.1" --reference ref

# android (from android/)
./gradlew assembleDebug && adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s PocketSplat:V

# headless renderer tools on the phone (sortbench): NDK toolchain + the SDK's CMake/Ninja
cmake -S . -B build-android-tools -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
      -DPOCKETSPLAT_BUILD_RENDER=ON -DPOCKETSPLAT_BUILD_TOOLS=OFF -DPOCKETSPLAT_BUILD_TESTS=OFF
cmake --build build-android-tools --target sortbench
adb push build-android-tools/render/sortbench /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/sortbench
adb shell /data/local/tmp/sortbench --stamp-probe

# like-for-like baselines (docs/benchmarks/README.md protocol)
python scripts/prepare_bench_scenes.py truck stump      # identical splats: PLY for web viewers, packs for the app
python scripts/bench_web.py fetch                       # pinned competitor viewers into .cache/
python scripts/bench_web.py run --viewer supersplat --scene data/derived/bench/truck-full.sh0.ply
python scripts/bench_android.py --scenes truck-full.sh0.pspk --width 540 --height 975
```
A change is not done until `ctest` passes and, for renderer changes, the desktop host renders the
reference scene without validation-layer errors (`--validation` flag).

Environment facts learned the hard way:
* Git Bash rewrites device paths (`/data/local/tmp` → `C:/Program Files/Git/data/...`) in adb
  arguments: `export MSYS_NO_PATHCONV=1` before adb commands in bash.
* Drivers recycle `VkBuffer` handle values — never key a cache on them (`Buffer::id()` exists for that).
* The visible counter must be read from the per-slot snapshot, never the live buffer (both frames in
  flight write it).
* Browser baselines on Samsung phones: Chrome paces pages at 60 Hz in the panel's adaptive mode; force
  120 Hz for the run (`adb shell settings put system min_refresh_rate 120.0` and `peak_refresh_rate
  120.0`, `settings delete` both afterwards). The app keeps drawing after a bench run; force-stop it
  before cooling (bench_android.py does).
* Windows' 260-character path limit breaks deeply nested FetchContent builds under long checkout paths;
  keep third-party Android checkouts at a short path.
* The Adreno 750 driver miscompiles compute shaders that carry per-invocation arrays (`uint x[16]`)
  across barriers — wrong results, no error. Keep such state in scalars, and run `sortbench` on the
  phone (not just the RTX) before any sort shader change merges (`docs/sorting.md` §8).
  Constant lookup tables indexed per invocation are a trap too: three `const vec2[]` corner tables
  in the splat vertex shader made every vertex ~10× slower on the 750 (correct image, RTX faster).
  Compute such values arithmetically (bit masks), and time any shader change on the phone.
* On the Samsung phones `/sys/class/power_supply/battery/*` is not readable from `adb shell`; current,
  voltage and the charge counter come from `dumpsys battery` (the bench scripts parse it). The S24U
  gauge refreshes `current now` only about every 10 s: power on runs shorter than ~10 min is ±10–15 %,
  so compare work with GPU cycles per frame (kgsl busy × clock ÷ fps), which the app can read itself
  (`gpu_mhz` column). Unplugged at 100 %, the S25U's charge counter stays pinned at full for ~10 min
  (S21: no change until 622 s of a 2.4 W run): take energy from current × voltage, or start below
  ~98 %.
* `AThermal_getThermalHeadroom(30)` is garbage for ~20 s after the first call and after any 10 s
  without a call: the framework fits its slope to as few as 3 one-second skin samples, so a 1 °C
  sensor step reads as +15 °C (S24U: 1.066 at 2 s from 31 °C skin). The controller ignores that
  window (ADR-0009 S20 amendment); anything else that reads the forecast must too.
* The S25U's skin reading (behind its thermal status and headroom) jumps +1.4–2.5 °C within ~20 s
  when it rises through ~38.7 °C while the AP temperature stays flat or falls (7 of 8 warm runs;
  never on the S24U), and in the S22 run fell back ~1.2 °C each time it cooled to ~38.9 °C. Read
  such steps as the sensor model, not heat: on the S25U no step down lands before LIGHT, and the
  S22 controller cycle was these steps (`docs/benchmarks/README.md`, negative results).
* SuperSplat's default camera (85° FOV, full-bbox distance) is ~35–45 % cheaper per frame than our
  bench pose: cost comparisons against it use `bench_web.py --pose ours`.

## 6. Coding conventions

* C++20, no exceptions across the JNI boundary (translate to error codes/`std::expected`-style results),
  no RTTI requirements, warnings as errors on MSVC and clang.
* `clang-format` (`.clang-format` in root) before every commit. `snake_case` functions/variables,
  `PascalCase` types, `kConstant`, `m_member`.
* Headers under `<lib>/include/<lib>/…`, one public header per module; keep `splatcore` free of
  Vulkan, Android and platform includes.
* Splat math conventions (see `docs/formats.md`): quaternion `(w, x, y, z)`; scale stored as log; opacity
  stored as logit in PLY, as linear `[0,1]` in the GPU pack; DC color `c = 0.5 + 0.28209479 · f_dc`;
  SH planar-per-channel in PLY, coefficient-major in our pack; coordinate system of the renderer is
  RUB (right-up-back, OpenGL-style); Vulkan NDC has +Y down and depth in `[0,1]` — handle in the
  projection matrix, never by flipping data.
* GLSL: `layout(std430)` SSBOs as flat `float`/`uint` arrays (never `vec3` arrays), explicit
  `local_size_x`, workgroup size as a specialization constant, all push constants in one struct per
  pipeline. Every compute pass has a `// Pass: …` header comment stating inputs, outputs and the
  synchronization it expects.
* Vulkan: dynamic rendering + synchronization2 + timeline semaphores (all 1.3 core). No render-pass
  objects. Frames in flight = 2. GPU timestamps around every pass in bench mode.

## 7. Testing and validation rules

* `splatcore`: unit tests for every IO path (PLY degree 0/1/2/3 read, degree 0/1 write, SPZ round trip),
  every cleaning op (synthetic inputs with known outcome), Morton ordering and pack layout (byte-exact).
* Renderer: (a) validation layers clean, (b) golden-image comparison of the desktop host against a
  stored reference PNG per test scene (PSNR ≥ 40 dB after quantization changes are accepted explicitly),
  (c) sort correctness test on random keys (1e5–4e6) before any sort optimization is merged.
* Device benchmarks are recorded, not remembered: `scripts/bench_android.py` → CSV → committed under
  `docs/benchmarks/` with device, build hash, scene, resolution, splat count, per-pass GPU time,
  thermal status. Negative results (things that were slower) are documented in `docs/benchmarks/README.md`.

## 8. Workflow for a session

1. Read `PROJECT.md` (if present) → current phase, open tasks, definition of done.
2. State the plan for the session in 3–6 bullets before editing. Small, compiling commits.
3. Prefer implementing from a paper/spec over adding a dependency. Adding a dependency requires: a
   license check, a pinned tag, an entry in `THIRD_PARTY.md`, and one sentence of justification.
4. When something doesn't work (slower, artifacts, driver bug): record it in the relevant doc with the
   measurement and the diagnosis. Negative results are first-class output.
5. End of session: update `PROJECT.md` status/session log, ensure `ctest` green, commit with a
   conventional prefix (`core:`, `render:`, `android:`, `tools:`, `docs:`, `bench:`).

## 9. Things not to do

* Do not add SH 2/3, LOD, streaming, sort-free rendering, training, iOS, or a WebGPU build "for later".
* Do not use `vulkan.hpp`, OpenGL ES, Unity, Filament, Kompute or any engine layer — thin wrappers only.
* Do not silently change quantization, thresholds or default cleaning parameters; every such change is an
  ADR or a documented benchmark.
* Do not store or log absolute file paths, device identifiers, or anything personal in committed files.
