# Device capability table

Recorded from `adb shell cmd gpu vkjson` and `dumpsys thermalservice`. Phase 0 deliverable; the
sort and renderer designs must hold within these limits (CLAUDE.md §2.5).

| Capability | Galaxy S24 Ultra (SM-S928B) | Galaxy S25 Ultra (SM-S938B) |
|---|---|---|
| Recorded | 2026-09-01, Android 16 (patch 2026-06-05) | 2026-09-01, Android 16 |
| SoC / GPU | Snapdragon 8 Gen 3 (SM8650) / Adreno 750 | Snapdragon 8 Elite (SM8750) / Adreno 830 |
| Vulkan API | **1.3.128** | **1.4.0** |
| Driver version (raw) | 0x802fa029 | 0x80320040 |
| Subgroup size | **64** default; min 64 / max 128 | **64** fixed (min = max = 64) |
| Subgroup size control | yes, compute stage (`requiredSubgroupSizeStages` = COMPUTE); `computeFullSubgroups` yes | yes, compute stage; `computeFullSubgroups` yes |
| Subgroup ops (compute+fragment) | BASIC, VOTE, **ARITHMETIC**, BALLOT, SHUFFLE, SHUFFLE_RELATIVE, QUAD — no CLUSTERED | BASIC, VOTE, **ARITHMETIC**, BALLOT, SHUFFLE, SHUFFLE_RELATIVE, QUAD, ROTATE — no CLUSTERED |
| maxComputeSharedMemorySize | **32 768 B** | **32 768 B** |
| maxComputeWorkGroupInvocations | 1024 (max WG size 1024³) | 1024 |
| maxComputeWorkgroupSubgroups | 16 | 16 |
| Timestamps | supported in compute+graphics, period **52.083 ns** | supported in compute+graphics, period **52.083 ns** (but see per-pass caveat in `phase3-android.md`) |
| maxStorageBufferRange | 128 MB | 2 GB (0x7FFFFFFF) |
| maxPushConstantsSize | 256 B | 256 B |
| maxBoundDescriptorSets | 7 | 7 |
| Timeline semaphore / sync2 / dynamic rendering | all present (1.3 core) | all present |
| Thermal API | ThermalHAL **AIDL 3**; `dumpsys thermalservice` exposes AP/SKIN/BAT/PA temps + status; status listeners available | ThermalHAL **AIDL 3**, same surface |

### Desktop GPUs (Windows hosts, 2026-11-01)

The development laptop exposes two Vulkan devices; `pocketsplat-desktop --list-gpus` prints them and
`--gpu <name>` picks one (default: the discrete GPU).

| Capability | NVIDIA GeForce RTX 3060 Laptop (GA106) | Intel UHD Graphics (Tiger Lake, 0x9A68) |
|---|---|---|
| Vulkan API / driver | 1.4.341 / 610.47 | **1.3.212** / 101.2079 |
| Subgroup size | 32 fixed | **8–32** (default 32); `requiredSubgroupSizeStages` = COMPUTE |
| maxComputeSharedMemorySize | 48 KB | 32 KB |
| maxStorageBufferRange | 4 GB | 1 GB (33.5 M splats of 32 B records) |
| Sort, 1 M keys, 32 / 16 bits (`sortbench`) | 2.09 / 0.55 ms | 15.2 / 7.35 ms (15.2 / 7.4 ns per key) |
| Goldens (museum1, sphere; gate 40 dB) | 63.6 / 64.1 dB | 55.6 / 58.3 dB |

* The Intel GPU's 8-lane minimum subgroup tripped the sort's ≥ 16-lane check. The downsweep now
  requires 16-lane, full subgroups on any GPU whose size can vary below 16 (1.3 core subgroup size
  control, mandatory in 1.3 and enabled on every device). Fixed-size GPUs (Adreno 64, NVIDIA 32)
  build exactly the pipelines they built before. `sortbench --gpu intel --validation` passes every
  size from 100 K to 4 M keys at 32 and 16 bits.

## Consequences for the sort/renderer design

* Subgroup 64 (not 32) with `subgroupExclusiveAdd` available — the reduce-then-scan sort can use
  subgroup arithmetic with the shared-memory fallback reserved for other vendors (Mali 16-wide).
* 32 KB workgroup memory comfortably holds the 256-bin histograms (1 KB) and rank scratch.
* GPU per-pass timing works on device (timestamps in compute), so `bench_android.py` can reuse the
  desktop GpuTimer approach.
* `maxStorageBufferRange` 128 MB caps any single SSBO at ~4.5 M splats of positions — irrelevant
  under the 1 M budget, relevant only if someone disables the budget prune.
