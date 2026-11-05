# ADR-0016 — Draw records sized to a visible capacity, not to the scene

**Status**: proposed (2026-11-06, S28). Implemented and checked on the desktop (Evidence). Phone
gates are open: sortbench on the S24U, phone memory and per-pass timings on a scene above the old
cap, the Adreno 830's `maxStorageBufferRange`, and a hand-held look at a clipped frame.

**Context**: every per-splat array lives in one storage buffer bound whole, so each is capped
by `maxStorageBufferRange` (Adreno 750: 128 MB). The widest was the draw records, 32 B per splat
and sized for every splat in the scene. That capped a scene at 128 MB / 32 B = 4,194,304 splats
(S27: larger packs open as their most important 4.19 M under a "partial scene" warning). The
preprocess writes a record only for a splat that passes the frustum, near and contribution
culls, so a frame fills a fraction of that buffer: bicycle-full (4.48 M) shows 569 K at the
app's home view. The owner asked for larger scenes in the next release (end of S27).

**Decision**:

1. **Visible capacity.** The draw records, the sort keys and values, and the sort's scratch
   (ping-pong keys/values, block histograms) hold `capacity = min(splat count, range / 32)`
   entries: 4,194,304 on the 750, the whole scene on desktop GPUs with a 4 GB range.
2. **The scene limit moves to the widest remaining per-splat array.** Positions, scale_rot and
   SH 1 take 12 B per splat, so `SplatRenderer::max_splats = range / 12`: 11,184,810 on the 750.
   `max_visible = range / 32` is the capacity bound. A pack above `max_splats` still opens as its
   most important splats under the partial-scene warning (S27 path, unchanged), now cut at
   11.18 M.
3. **Overflow is clipped and reported.** The preprocess still counts every visible splat
   (`atomicAdd`), and writes the record, key and value only when its slot is below the capacity.
   The prepare pass clamps the sort and draw count to `min(visible, capacity)`. The depth pick
   reads that clamped count (the sort arguments) instead of the raw counter.
   `last_clipped_count()` = visible − capacity is shown in the desktop HUD and viewer, the bench
   CSVs (`clipped`, the last column on desktop and Android) and the app (a red "CLIPPED: N splats
   in view not drawn" line, one line when the overlay is hidden).
4. **Test hook.** `SplatRenderer::init(..., visible_capacity)` and `pocketsplat-desktop
   --visible-cap N` force a smaller capacity, so the clip path runs on the RTX (4 GB range).
5. **Synthetic stress scenes.** `scripts/tile_scene.py` tiles a PLY N times on the ground plane
   (the copies do not overlap) for capacity and correctness runs. They are not captures: never
   use them for image-quality claims and never publish renders of them as scenes.

**Evidence** (desktop, RTX 3060 Laptop, 864 × 1872 offscreen, validation layers on, 0 messages
in every run):

* Raw INRIA bicycle, 6,131,954 splats (local only, non-commercial). It opened, and at
  `--visible-cap 4194304` it rendered the same image as uncapped, PSNR 90.7 dB (max difference 4
  levels; equal-depth ties in the atomic compaction order). On the bench orbit (360 frames,
  16-bit keys) the most splats ever visible were 2,543,725 (dist-scale 1) and 2,037,146 (dist-scale
  2). With the phone's 0.1 px contribution cull they were 1,501,515 and 939,581. A real
  6 M scene stays far below the 4.19 M capacity.
* Tiled bicycle-full (`tile_scene.py`): ×2 = 8,966,440 and ×4 = 17,932,880 splats. Over a
  120-frame orbit at 0.8 × the bench distance, the most splats visible at once were 4,322,492 (×2)
  and 5,125,958 (×4). At `--visible-cap 4194304` the clipped count is exactly the excess (at most
  128,188 and 931,654), and frames with nothing clipped are unchanged.
* What clipping looks like: raw bicycle at `--visible-cap 2000000` (2.52 M visible, 522,533
  clipped) loses whole regions, 13.4 dB against the full image. A workgroup is a 256-splat
  chunk, so the chunks that append last are the ones dropped. That is an arbitrary part of the
  view, near content included, not the least important splats.
* Depth pick at a pixel works with and without clipping (×4, cap 4194304, validation clean).
* Sort correctness (sortbench, validation): 4,194,304, 4,194,303 and 4,190,000 keys, 32- and
  16-bit: PASS. The sort code is unchanged; only its maximum size now comes from the capacity.
* ctest 111/111. The Android debug APK builds (NDK clang `-Werror`).

**Consequences**:

* Scenes up to 11.18 M splats open whole on the 750. GPU memory per splat is 28 B (SH 0) or
  40 B (SH 1), plus 4 B with a coverage sidecar. The capacity-sized buffers are fixed: records
  128 MB, plus 64 MB for keys, values and the sort's scratch. An 11.18 M SH 1 scene takes about
  450 MB of pack data plus 192 MB. Before, the largest scene took 168 MB plus the same 192 MB.
  The host keeps the pack in RAM as well (phone check pending).
* A frame with more than 4.19 M splats in view draws an arbitrary 4.19 M of them and says so.
  On real scenes this needs an overview of an 8 M+ capture without the contribution cull. If
  it shows up in use, the next step is a contribution floor: the renderer raises the cull
  threshold while frames clip, so the smallest contributions go first. That would get its own
  ADR (thresholds are never changed silently).
* Above 11.18 M (step 2): split the 12 B arrays over several bindings, or address them through
  buffer device address (1.3 core). Both need checking on the 750 and 830 drivers first.

**Alternatives considered**:

* *Keep sizing records to the scene, split the record buffer into several bindings.* That gains
  the same 4.19 M → 11.18 M step but keeps 32 B × count of memory nobody writes. It also adds a
  binding selection to the preprocess and the vertex shader, which is the hot path on the 750.
* *Raise the contribution threshold until the scene fits.* That drops splats permanently and
  changes the image at every view. Clipping costs nothing until a frame actually overflows.
