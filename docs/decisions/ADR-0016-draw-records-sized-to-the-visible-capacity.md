# ADR-0016 — Draw records sized to a visible capacity, with a contribution floor when a view overflows it

**Status**: proposed (2026-11-06, S28). Implemented, measured on the desktop and on both phones
(Evidence). Waiting for the owner's hand-held look at a large scene on the S24U.

**Context**: every per-splat array lives in one storage buffer bound whole, so each is capped
by `maxStorageBufferRange`. The widest was the draw records, 32 B per splat and sized for every
splat in the scene. On the Adreno 750 (128 MB) that capped a scene at 4,194,304 splats; larger
packs opened as their most important 4.19 M under a "partial scene" warning (S27). The
preprocess writes a record only for a splat that passes the frustum, near and contribution
culls, so a frame fills a fraction of that buffer: bicycle-full (4.48 M) shows 569 K at the
app's home view. The owner asked for larger scenes in the next release (end of S27), and for the
contribution floor once the phone runs showed how clipping looks (S28).

**Decision**:

1. **Visible capacity.** The draw records, the sort keys and values, and the sort's scratch
   (ping-pong keys/values, block histograms) hold `capacity = min(splat count, range / 32)`
   entries.
2. **The scene limit moves to the widest remaining per-splat array.** Positions, scale_rot and
   SH 1 take 12 B per splat, so `SplatRenderer::max_splats = range / 12`; `max_visible = range /
   32` bounds the capacity. Both come from the device's Vulkan limits at run time (no per-GPU
   table). The app logs them when Vulkan starts, and the bench and trace CSVs record
   `visible_capacity` and `storage_buffer_range`:

   | GPU | maxStorageBufferRange | Scene limit | Visible capacity |
   |---|---|---|---|
   | Adreno 750 (S24U) | 128 MB | 11,184,810 | 4,194,304 |
   | Adreno 830 (S25U) | 2 GB − 1 | 178,956,970 | 67,108,863 |
   | RTX 3060 Laptop | 4 GB − 1 | 357,913,941 | 134,217,727 |

   On the 830 and the RTX the binding never limited a scene: memory does. A pack above the
   scene limit still opens as its most important splats under the partial-scene warning (the
   S27 path, unchanged), now cut at the new limit.
3. **Overflow is clipped and reported.** The preprocess still counts every visible splat
   (`atomicAdd`) and writes the record, key and value only for a slot below the capacity. The
   prepare pass clamps the sort and draw count to `min(visible, capacity)`. The depth pick reads
   that clamped count (the sort arguments) instead of the raw counter. `last_clipped_count()` =
   visible − capacity.
4. **Clip guard: a contribution floor while frames overflow** (on by default). A frame that
   clips raises a floor under the contribution threshold (ADR-0008). It starts at 0.1 px of the
   reference image, the phone's first rung, and then doubles, or quadruples when the count
   exceeds the capacity by more than 25 %. The threshold used is `max(min_contribution, floor)`,
   in the chunk cull's bound too. The floor falls by 1/1.5 (to 0 below 0.1 px) after 30 computed
   frames in which the visible count, grown by what the last raise removed (the count that
   triggered it divided by the first count after it), stays under 90 % of the capacity. Only
   frames computed at the current floor count, because the counts trail by the frames in flight.
   A changed floor marks the compute dirty, and `wants_frame()` keeps on-demand hosts drawing
   until its first count is in, so a still view settles too. `set_clip_floor_enabled(false)` and
   `pocketsplat-desktop --no-clip-floor` switch it off for A/B runs.
5. **What the user sees.** The app shows a yellow line while the floor is up ("Large view:
   splats under 0.1 px hidden"), or the clipped count during the transient frames ("CLIPPED: N
   splats in view not drawn"). Yellow: it depends on the view and goes away closer in; red stays
   the partial scene, cut at open. Desktop HUD and viewer likewise. The bench and trace CSVs
   carry `clipped` and `clip_floor`.
6. **Test tools.** `pocketsplat-desktop --visible-cap N` forces a smaller capacity, so the clip
   path runs on desktop GPUs. `scripts/tile_scene.py` tiles a PLY N times on the ground plane
   (no overlap) for capacity runs. Tiled scenes are not captures: never use them for
   image-quality claims, and never publish renders of them as scenes.

**Evidence** (`../benchmarks/cap-lift-2026-11-06.csv`; desktop runs at 864 × 1872 offscreen with
validation layers on, 0 messages in all of them):

* **Phones open the scenes.** Raw INRIA bicycle (6.13 M) and garden (5.83 M), uncleaned and
  local only (non-commercial), and tiled bicycle-full ×2 (8.97 M) open whole on both phones.
  ×3 (13.45 M) and ×4 (17.93 M) open whole on the S25U, and partially on the S24U at
  11,184,810. 60 s bench orbit, 16-bit keys, fps over the first 30 s:

  | Scene | S24U, 0.1 px | S25U, 0.1 px | S24U, no cull | S25U, no cull |
  |---|---|---|---|---|
  | bicycle-4m (control, 4.0 M) | 64.6 | 78.4 | – | – |
  | bicycle raw 6.13 M | 59.3 | 68.4 | 50.0 | 53.5 |
  | garden raw 5.83 M | 60.2 | 63.2 | 49.4 | 56.0 |
  | ×2, 8.97 M | 49.7 | 52.7 | 31.9 | 35.3 |
  | ×3, 13.45 M (S24U: 11.18 M) | 45.8 | 49.9 | 27.7 → **46.0** with the floor | 28.2 |
  | ×4, 17.93 M (S24U: 11.18 M) | 37.7 | 38.8 | 21.0 → **37.6** with the floor | 21.4 |

  The runs went back to back without cooling (thermal status 1–2), so read these as "it
  works, at about this speed", not as protocol numbers.
* **With the app's 0.1 px cull no frame clipped** on either phone, in any scene: at most
  2.55 M visible (×4). Real 6 M scenes stay far below 4.19 M even without the cull (at most
  2.54 M).
* **Clipping without the floor** (S24U, no cull): 45.6 % of ×3's frames and 99.8 % of ×4's, up
  to 1.83 M splats dropped. A workgroup is a 256-splat chunk, so the chunks that append last
  vanish: an arbitrary part of the view, near content included. On the desktop, raw bicycle at
  `--visible-cap 2000000` lost the ground in front of the camera, 13.4 dB against the full
  image.
* **With the floor**: S24U ×3 0.4 % and ×4 0.1 % clipped frames, and the floor stayed at
  0.1 px. On the desktop, the same still view converges in one step (floor 0.1, 1.48 M
  visible), 52.0 dB against the full image. The 18 M orbit at capacity 4,194,304 drops from
  70.3 % clipped frames to 0.7 %, with 4 floor changes in 600 frames.
* **Memory** (`dumpsys meminfo`, peak PSS, 5 s samples): S24U 0.8 GB at bicycle-4m, 1.05–1.18 GB
  for every larger scene (graphics 0.92–1.0 GB). S25U 1.2 GB at 6.1 M, 1.56 GB at 9 M, 2.0 GB at
  17.9 M.
* **Sort** (sortbench): S24U 4,190,000 / 4,194,303 / 4,194,304 keys, 32 and 16 bit: PASS
  (16-bit at the capacity: 11.0 ms unsorted). S25U up to 16 M keys: PASS (16-bit: 35.2 ms).
  RTX with validation: PASS. The sort code is unchanged; only its maximum size now comes from
  the capacity.
* ctest 111/111; the Android APKs build (NDK clang `-Werror`).

**Consequences**:

* Scenes up to 11.18 M splats open whole on the 750; the 830 is limited only by memory (17.9 M
  ran at 2.0 GB). GPU memory per splat is 28 B (SH 0) or 40 B (SH 1), plus 4 B with a coverage
  sidecar, plus the fixed capacity buffers (750: 128 MB records + 64 MB keys, values and sort
  scratch).
* A view with more visible splats than the capacity loses its smallest contributions instead of
  random regions. The threshold rises above the user's or the quality controller's setting only
  while that is needed, and the app says so in yellow. Two transient frames still clip when a
  view first overflows: the counts trail by the frames in flight.
* Above the scene limit (step 2): split the 12 B arrays over several bindings, or address them
  through buffer device address (1.3 core). That only matters for the 750, and only for scenes
  of 11 M or more: both drivers need checking first.

**Alternatives considered**:

* *Keep sizing records to the scene and split the record buffer into several bindings.* That
  gains the same 4.19 M → 11.18 M step, but keeps 32 B × count of memory nobody writes, and adds
  a binding selection to the preprocess and to the vertex shader, which is the hot path on the 750.
* *A fixed floor or a permanently higher threshold for large scenes.* It changes the image at
  every view, including the close ones that never overflow.
* *Clip without the floor.* That was step 1 of this ADR. It drops whole regions, often near ones
  (Evidence).
* *Lower the floor at a fixed 60 % of the capacity.* It oscillated between 0 and 0.1 px (26
  changes in 600 frames, 4.4 % clipped frames), because 0.1 px halves the visible count of a
  large view. See the negative-results log in `../benchmarks/README.md`.
