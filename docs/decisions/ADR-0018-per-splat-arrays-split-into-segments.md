# ADR-0018 — Lifting the Adreno 750's scene limit: per-splat arrays split into segments, one preprocess dispatch each

**Status**: proposed (2026-11-24, S34). The owner chose option (b), segments, and the memory policy
in Decision 3 (S34). It is implemented and checked on the desktop ("Desktop evidence"). It stays
proposed until the S24U runs under "What decides it" are in.

**Context**: every per-splat input array lives in one storage buffer bound whole, so each is
capped by `maxStorageBufferRange`. On the Adreno 750 (128 MB) the widest of them, 12 B per
splat, stops a scene at 11,184,810 splats (ADR-0016). Larger packs open as their most important
11.18 M splats under the red partial-scene banner. On the Adreno 830 (2 GB) and the RTX 3060
(4 GB) memory runs out long before the binding. The arrays and their sizes:

| Array | Per splat | Limit on the 750 | Read by |
|---|---|---|---|
| `pos` | 12 B (3 × f32) | 11,184,810 | preprocess |
| `scale_rot` | 12 B (3 × fp16 log-scale, 4 × snorm8 quaternion, palette index) | 11,184,810 | preprocess |
| `sh1`, direct | 12 B (9 × snorm8 + pad) | 11,184,810 | preprocess |
| `sh1`, palette (the app's default since v0.4.0, ADR-0017) | 0 B (≤ 65,536 entries × 12 B, shared) | – | preprocess |
| `color` | 4 B | 33,554,432 | preprocess |
| coverage masks (`.pscov`, optional) | 4 B | 33,554,432 | preprocess |
| draw records, sort keys/values | 32 B / 4 B, sized to the visible capacity | (visible capacity 4,194,304) | preprocess, sort, draw |

Only the preprocess (`splat_preprocess.comp`) reads per-splat inputs. With the chunk cull on
(the default everywhere), the preprocess runs one 256-thread workgroup per visible 256-splat
chunk, and the chunk cull writes the visible chunks in ascending order. Shrinking `scale_rot`
alone moves nothing: `pos` and direct `sh1` hold the same limit.

Facts the options depend on. `maxStorageBufferRange` is queried at run time and logged (750:
134,217,728; 830: 2,147,483,647). `../benchmarks/devices.md` also records 256 B of push
constants and 7 bound sets on both phones. Vulkan 1.3 guarantees `maxBufferSize` and
`maxMemoryAllocationSize` ≥ 1 GiB, `maxComputeWorkGroupCount[0]` ≥ 65,535, and
`bufferDeviceAddress`. The compute descriptor set binds 12 storage buffers today, so the 750
supports at least that many per stage. Not on record for either phone:
`maxPerStageDescriptorStorageBuffers`, `maxBufferSize`, `maxMemoryAllocationSize` and
`maxComputeWorkGroupCount`. Phase 0's `vkjson` dump was not kept, and the public database renders
its tables in script, so it could not be read. The next phone run logs them.

`maxComputeWorkGroupCount[0]` matters beyond the binding. The preprocess runs one workgroup per
visible chunk in a single dispatch, so the spec minimum, 65,535 workgroups, covers 16.78 M
splats. The 750 never reached it (its 11.18 M limit is 43,690 chunks). The 830 ran 17.93 M
whole in S28 (70,039 chunks), so its real limit is higher, or its driver tolerated the excess.
Every option that keeps a single preprocess dispatch must check this limit as well.

**Options**:

(a) **Windows: more bindings, selected in the shader.** Each 12-byte array is bound as two (or
K) storage buffers, each holding a chunk-aligned window of at most `range` bytes. The preprocess
picks the window from the chunk index. That choice is uniform per workgroup, because windows
hold whole chunks. Cost: +1 binding per 12-byte array per extra window (+3 at two windows, 15
storage buffers per stage), every load site duplicated behind a branch, and a per-invocation
path that changes even for scenes that never need a second window. The 750's compiler has
punished such changes before (CLAUDE.md: per-invocation arrays, constant tables). A variant
binds the windows as a descriptor array (`buffer Pos { float v[]; } pos[2]`) indexed by a
dynamically uniform value. That needs `shaderStorageBufferArrayDynamicIndexing`, which is not
on record for the 750.

(b) **Segments: one preprocess dispatch per segment.** The scene is split into segments of
`S` splats, a multiple of 65,536 (256 chunks), with `S · 12 B ≤ range` (750: 11,141,120 splats,
170 × 65,536). Each segment owns its own buffers for every per-splat array (`pos`, `scale_rot`,
`color`, direct `sh1`, coverage). Each frame slot gets one descriptor set per segment, at the
same binding numbers as today. A palette is bound whole in every set. The chunk cull keeps its
single workgroup. Because a segment holds whole 256-chunk blocks of its loop, it starts each
segment's compaction at a fixed offset in `chunk_list`, at segment × S / 256, and writes one
indirect dispatch triplet per segment. The preprocess is dispatched once per segment, and two
push constants carry the segment's list offset and splat base. Per invocation it reads
`chunk_list[list_base + workgroup]` and subtracts `splat_base` before every load. Nothing else
in the shader changes. Records, keys, values and the visible counter are shared by all
segments, so the prepare pass, sort and draw do not change. No barrier is needed between the
segment dispatches, because they only append through the atomic counter. A scene of up to `S`
splats has one segment: today's dispatch plus two push-constant values and one subtraction, so
the "no slower at 4 M" gate should hold by construction (the S24U run below confirms it). No new bindings, features or format
changes. Because every per-splat array, `color` and coverage included, is split, no binding
limits the scene any more. Segments are also capped at `maxComputeWorkGroupCount[0]` × 256
splats, so no dispatch can exceed that limit on any GPU. What remains is memory and the 32-bit
splat index (2^31). One allocation per segment and array also keeps each one well under 1 GiB.

(c) **Planar arrays.** Split `pos` into x[], y[], z[] and `scale_rot` and direct `sh1` into
their three words, 4 B per array, at upload. Limit 33.5 M with no index split, +4 bindings at
SH 0 palette packs and +6 with direct SH 1. One 12-byte stream per splat becomes three 4-byte
streams; whether the 750 reads those as fast is unknown. The upload de-interleaves in memory at
open, and the cap stays fixed at 33.5 M.

(d) **Buffer device address.** Core in Vulkan 1.3 and mandatory, so no range limit: one 64-bit
address per array in push constants, read with `GL_EXT_buffer_reference` and `uvec2` address
arithmetic (no `shaderInt64`). Every load path in the preprocess changes, robustness and
validation coverage are weaker than for descriptors, and this project has no measurement of
BDA loads on the Adreno 750 driver, which has miscompiled ordinary compute shaders before.
Kept as the fallback if (b)'s extra dispatches cost.

(e) **Smaller arrays: quantized positions and scales.** `pos` becomes 3 × 16 bit inside each
chunk's tight centre bounds plus a 16-bit pad that takes the palette index. `scale_rot` becomes
3 × 8-bit log-scales over the scene's log-scale range plus a smallest-three quaternion in 32
bits (2 + 3 × 10). That gives 8-byte arrays and 20 B per splat instead of 28 B. Limit 16.7 M,
and direct SH 1 packs keep 11.18 M unless their `sh1` is split too. It needs pack v4 (a format
change: `formats.md`, byte-exact tests, the cache and sidecar hash rules, the app's and
viewer's importers) and a quantization change, which needs explicit acceptance. Image price
measured by simulation below. Its real payoff is memory and bandwidth (−29 % per splat), not
the limit.

**Decision** (owner, S34): (b), segments with one preprocess dispatch each. On acceptance this
supersedes ADR-0016's Decision 2 (scene limit = range / 12); ADR-0016's visible capacity, clip
reporting and clip guard stay as they are.

1. `splatcore` gets a pure function `plan_segments(count, max_storage_buffer_range,
   max_workgroup_count_x, bytes_per_splat = 12)`. It returns the segment size, a multiple of
   65,536 splats no larger than `range / 12` or `max_workgroup_count_x × 256`, and the segment
   count. It is unit-tested at the edges: one splat, exactly `S`, `S + 1`, the 750 and 830
   ranges, a workgroup limit of 65,535, and a range below 65,536 × 12 B, which is rejected.
2. The renderer splits every per-splat array at upload into per-segment buffers, writes one
   compute descriptor set per frame slot and segment, and records one indirect dispatch per
   segment. Without the chunk cull it records one direct dispatch per segment.
3. **Above memory: a partial open under a memory budget** (owner, S34). Without the binding
   limit, a pack that does not fit would fail an allocation, or the OS would kill the app.
   Instead, `SplatRenderer::max_splats(context, bytes_per_splat)` is a memory bound. It takes the
   largest device-local heap's budget minus its usage. VMA reports that from
   `VK_EXT_memory_budget`, enabled where the device has it; without the extension VMA counts 80 %
   of the heap minus its own allocations. From that budget it subtracts 32 MB of slack (palette,
   chunk lists, pick buffers, UBOs) and 56 B per visible-capacity slot (record, key, value, the
   sort's ping-pong pair, margin). It then divides by the pack's resident bytes per splat
   (`splatcore::gpu_bytes_per_splat`: 29 B for SH 0 and palette packs, 41 B for direct SH 1,
   +4 B with coverage masks). The app asks it at open, after the previous scene is freed, and
   opens the most important splats that fit under the same red partial-scene banner (the S27
   path). The Windows viewer uses it, at the widest 45 B, as its import budget. The desktop dev
   host does not cut: an oversized pack fails with an allocation error. The 32 MB, 56 B and
   80 % are estimates; the S24U runs in "What decides it" give the first real numbers (heap
   size, budget, peak PSS against splat count). The app now logs them at Vulkan start.
4. The chunk cull's `Push` gets the segment size in chunks. At each segment boundary in its
   block loop it writes the finished segment's dispatch triplet and restarts compaction at the
   next segment's fixed list offset. Its `// Pass:` header and the preprocess's document the new
   contract.
5. `pocketsplat-desktop --segment-splats N` (N a multiple of 65,536) forces small segments, so
   the split path runs on the RTX under validation, as `--visible-cap` does for the clip path
   (ADR-0016).

**Desktop evidence (segments, f5fc198)**:
`../benchmarks/segments-rtx3060-f5fc198-2026-11-24.csv`, summarized in `../benchmarks/README.md`
("Segments"). `--segment-splats` forced 5–62 segments of 65,536 splats on the six gate-A packs,
and 2 (the 750's split) or 4 segments on tiled bicycle ×3 (13.45 M). Every split drew the
reference's visible count to the splat over the 600-frame orbit, with PSNR within the renderer's
run-to-run repeat noise. Validation reported 0 messages with and without the chunk cull, with
palette, direct SH 1 and coverage masks. `ctest` 146/146 (4 new `PackSegments` tests, 4 goldens).
The debug APK builds (NDK clang, `-Werror`). The chunk cull closes a segment inside its existing
end-of-block step, so its barriers are the v0.4.0 ones. The RTX's memory bound: heap 5,994 MB,
5,222 MB available (`VK_EXT_memory_budget`) → 64.0 M splats at 29 B per splat.

**Evidence (desktop, simulation of (e), 2026-11-24)**: `../benchmarks/quantize-sim-fb66214-2026-11-24.csv`
and `quantize-steps-fb66214-2026-11-24.csv`, summarized in `../benchmarks/README.md` ("Smaller
position and scale arrays"). `scripts/quantize_sim.py` quantizes and dequantizes the six gate-A
packs into ordinary v3 packs and renders them against the originals. The protocol is gate A's
(864 × 1872, 32-bit keys, no cull) plus two poses at 0.35 × the distance. A bounds-only control
lands inside the renderer's repeat noise. Its lowest value, beetle at 75.1 dB, equals gate A's
`@repeat` minimum for the same pack and pose (`palette-quality-c6f0a7f-2026-11-19.csv`).

* Positions at 16 bits inside tight chunk bounds: 49.3–63.0 dB at the worst pose. Half a step
  is a median 0.0002–0.0006 σ of the splat's largest axis, and more than 0.1 σ on at most 0.03 %
  of splats.
* Log-scales at 8 bits over the scene's range: 41.2–47.9 dB, moving up to 39 % of the pixels by
  more than 2 LSB. Invisible splats stretch the range (truck: log-scale −32.1 to 1.5, ±6.8 %
  per step). A range floor or per-chunk ranges would improve it; neither was tried.
* So (e) passes the 40 dB bar, but by 1.2 dB on truck. It is a real quantization change, which
  under CLAUDE.md §9 needs its own ADR, not a side effect of lifting the limit.

None of this decides between (a)–(d). That is a question of S24U time, and the laptop's GPU
times do not count (CLAUDE.md §7).

**What decides it** (S24U first, the owner's go for each run; debuggable release build
`-PdebuggableRelease` over the installed 0.4.0, which keeps the scenes):

0. First, before any scene opens: the app's "Vulkan ready" logcat line (heap size, available
   budget, `VK_EXT_memory_budget`, `maxComputeWorkGroupCount.x`, scene limit). A scene limit
   below 11,184,810 at 29 B per splat would be a regression against v0.4.0. If it is, stop the
   run and fix the memory policy first.
1. Gate: on bicycle-4m, preprocess and chunk-cull ms, the v0.4.0 build against the segment
   build, interleaved runs (`scripts/bench_android.py`, 540 × 975, 16-bit keys, 0.1 px cull).
   One segment must be no slower than today.
1a. Correctness of two or more segments on the 750's compiler, which has miscompiled silently
   before. On bicycle-4m, the same build with `bench_android.py --segment-splats 2097152` (two
   segments) against one segment: the per-frame `visible` column must be identical. That is
   the same GPU and the same arithmetic, so it should be exact. ×3 on the phone against the
   desktop's `--segment-splats 11141120` orbit at the phone's settings must agree within the
   culls' float noise. The CSV header records `segments` and `segment_splats`.
2. bicycle ×2 (8.97 M, one segment) against ×3 (13.45 M) and ×4 (17.93 M) (two segments on the
   750; `scripts/tile_scene.py`, `data/derived/stress`). Each must open whole with no
   partial-scene banner. Record per-pass times, chunk-cull ms (its loop grows to 274 blocks at
   17.9 M) and peak PSS (`dumpsys meminfo`; S28: ≤ 1.18 GB at 11.18 M).
3. The device limits not on record, from `adb shell cmd gpu vkjson`, kept this time:
   `maxPerStageDescriptorStorageBuffers`, `maxBufferSize`, `maxMemoryAllocationSize`,
   `maxComputeWorkGroupCount`, and whether the device has `VK_EXT_memory_budget`.
4. S25U: its 2 GB binding never splits a scene; its dispatch limit (logged in 0) does if it is
   the spec minimum (×4 would then be two segments). A regression check of 1, 1a and 2.

Desktop gates before any phone run: `ctest` green; validation layers clean with and without
`--segment-splats`; the goldens unchanged. Forced segments against one segment on the six
gate-A scenes and the tiled packs must land at the renderer's repeat noise. They cannot be
bit-identical: atomic slot order already differs run to run (`quality_ab.py` `@repeat`).

**Consequences**:

* The 750 opens scenes up to its memory: 29 B per splat on the GPU for palette and SH 0 packs,
  41 B for direct SH 1 (`gpu_bytes_per_splat`). The visible capacity (4,194,304) and the clip guard (ADR-0016) are
  unchanged, so wide views of very large scenes still meet the contribution floor.
* Importing such a scene on the phone is a separate limit, not measured above 6.13 M (S28). The
  tiled test packs are built on the desktop.
* Tiled packs are not captures: no image-quality claims or published renders from them
  (ADR-0016).
* (e) stays open as a memory and bandwidth optimization on its own merits, judged by S24U
  preprocess time and the image price below. It would be its own ADR with pack v4.
