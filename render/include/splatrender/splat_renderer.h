#pragma once

#include <volk.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "splatcore/pack.h"
#include "splatcore/result.h"
#include "splatrender/radix_sort.h"
#include "splatrender/vk_buffer.h"
#include "splatrender/vk_context.h"
#include "splatrender/vk_descriptors.h"
#include "splatrender/vk_frame.h"
#include "splatrender/vk_pipeline.h"
#include "splatrender/vk_timer.h"

namespace splatrender {

// Pixel count that SplatRenderer::set_min_contribution's unit refers to: the phone protocol's
// 864x1872 internal image.
inline constexpr float kContributionReferencePixels = 864.0f * 1872.0f;

// Everything in RUB world space; matrices column-major.
struct SplatCameraParams {
    float view[16];       // world -> view (camera looks along -z)
    float proj_view[16];  // Vulkan NDC: y down, depth [0,1]
    float cam_pos[3];
    float focal_x = 0.0f, focal_y = 0.0f;  // pixels
    float viewport_w = 0.0f, viewport_h = 0.0f;
};

// Renders one GpuPack, fully GPU-driven per frame: a single-workgroup chunk cull frustum-tests
// the pack's 256-splat chunk AABBs and writes both a visible-chunk list and the preprocess's
// indirect dispatch, a compacting cull+preprocess writes records and depth keys for visible
// splats only, a prepare pass turns the visible count into sort and draw arguments, the radix
// sort orders back-to-front, and an indirect draw of quads composites with premultiplied "over"
// blending. The CPU never reads the visible counts (except a trailing mapped copy for the HUD).
// When the camera (and every setting that shapes the records) is unchanged since the last
// computed frame, record_compute records nothing and the draw re-uses the sorted buffers.
class SplatRenderer {
public:
    SplatRenderer() = default;
    ~SplatRenderer() { destroy(); }
    SplatRenderer(const SplatRenderer&) = delete;
    SplatRenderer& operator=(const SplatRenderer&) = delete;

    // Size limits. Every per-splat array lives in one storage buffer bound whole, so each is capped
    // by maxStorageBufferRange (Adreno 750: 128 MB). The widest per-splat input arrays (positions,
    // scale_rot, SH1) take 12 B per splat: the most splats a scene may have (750: 11,184,810).
    // The 32 B draw records, the sort keys/values and the sort's scratch hold only the splats
    // visible in a frame, up to the visible capacity: min(splat count, range / 32) (750:
    // 4,194,304). A frame with more visible splats draws the first `capacity` the preprocess
    // appends (an arbitrary subset) and reports the rest in last_clipped_count (ADR-0016).
    static constexpr std::uint32_t kRecordBytes = 32;
    static constexpr std::uint32_t kMaxBytesPerSplat = 12;  // widest per-splat input array
    static std::uint64_t max_splats(const Context& context) {
        return std::min<std::uint64_t>(context.max_storage_buffer_range() / kMaxBytesPerSplat, kMaxCount);
    }
    static std::uint64_t max_visible(const Context& context) {
        return context.max_storage_buffer_range() / kRecordBytes;
    }

    // `coverage`: optional per-splat observed-direction masks (splatcore/coverage.h, pack order,
    // one per splat) for the coverage fade; without them set_coverage_strength has no effect.
    // `visible_capacity`: 0 = min(splat count, max_visible); a smaller value forces the clip path
    // (testing and benching only).
    splatcore::Status init(Context& context, const splatcore::GpuPack& pack, VkFormat color_format,
                           const std::vector<std::uint32_t>* coverage = nullptr,
                           std::uint32_t visible_capacity = 0);
    void destroy();

    // 32 (exact float depth order) or 16 (2 sort passes, log depth quantized over the frame's
    // possible depth range; equal keys keep a non-deterministic relative order - ADR-0008).
    void set_key_bits(int bits) {
        m_key_bits = bits == 16 ? 16 : 32;
        m_compute_dirty = true;
    }
    int key_bits() const { return m_key_bits; }

    // Contribution culling: drop splats whose opacity x screen area (2*pi*sqrt(det Sigma2D)) is
    // below `pixels` of full coverage before they are sorted. 0 = off (ADR-0008). The unit is a
    // pixel of the 864x1872 phone-protocol image (kContributionReferencePixels): the threshold is
    // scaled by the viewport's pixel count every frame, so the same splats are culled at any
    // resolution - a fixed per-pixel threshold would cull physically larger splats at lower ones.
    void set_min_contribution(float pixels) {
        m_min_contribution = pixels > 0.0f ? pixels : 0.0f;
        m_compute_dirty = true;
    }
    float min_contribution() const { return m_min_contribution; }

    // Coverage fade (ADR-0013): a splat seen from a direction away from every direction the
    // capture cameras observed it from is drawn more transparent: alpha x (1 - r x (1 - match)),
    // match = smoothstep over the angle to the nearest observed cell (1 within `full_deg`, 0
    // beyond `zero_deg`), r = 1 - (1 - strength)^2 the opacity an unmatched splat loses. The
    // square makes the slider roughly perceptually even: floaters are stacks of near-opaque
    // splats, and with r = strength the stump test view changed by 29 % of its full change at
    // 0.5 and 78 % at 0.9. Strength 0 renders exactly as without masks, at no cost.
    void set_coverage_strength(float strength) {
        const float s = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
        if (s != m_coverage_strength) m_compute_dirty = true;
        m_coverage_strength = s;
    }
    float coverage_strength() const { return m_coverage_strength; }
    bool has_coverage() const { return m_has_coverage; }
    void set_coverage_angles(float full_deg, float zero_deg);
    static constexpr float kDefaultCoverageFullDeg = 35.0f;  // ~ the largest centre distance in a cell
    static constexpr float kDefaultCoverageZeroDeg = 90.0f;
    // Chunk-level contribution bound (with min_contribution > 0): chunk cull also drops chunks in
    // which no splat can pass the preprocess's contribution test - a conservative bound, so the
    // image is unchanged; on by default, off for A/B benching.
    void set_chunk_contribution_cull(bool enabled) {
        m_chunk_contribution_cull = enabled;
        m_compute_dirty = true;
    }

    // Chunk cull on by default; off = direct preprocess dispatch over all splats (A/B benching).
    void set_chunk_cull(bool enabled) {
        m_chunk_cull = enabled;
        m_compute_dirty = true;
    }
    bool chunk_cull() const { return m_chunk_cull; }

    // How the draw emits each splat (same image; the primitive and vertex work differ). The draw
    // of a heavy scene is primitive-bound on Adreno, so one triangle per splat is the default
    // (ADR-0010); the quad modes stay for A/B runs.
    enum class DrawMode : std::uint32_t {
        kVertex6 = 0,       // two triangles, 6 vertices per splat from gl_VertexIndex (ADR-0006 quads)
        kInstanced4 = 1,    // one instance per splat, 4-vertex triangle strip
        kIndexedBatch = 2,  // shared index buffer of kQuadsPerBatch quads, one instance per batch
        kTriangle = 3,      // one triangle per splat around the support circle (half the triangles)
    };
    static constexpr std::uint32_t kDrawModeCount = 4;
    static constexpr DrawMode kDefaultDrawMode = DrawMode::kTriangle;
    static constexpr std::uint32_t kQuadsPerBatch = 1024;
    void set_draw_mode(DrawMode mode) {
        m_draw_mode = mode;
        m_compute_dirty = true;  // the prepare pass writes the draw arguments for the mode
    }
    DrawMode draw_mode() const { return m_draw_mode; }

    // Timing-only variants of the triangle draw (wrong image by design; splat.vert "Probes"), to
    // split the draw's cost on a GPU into gather, vertex/primitive and fill work. Applies to
    // DrawMode::kTriangle only; other modes draw normally. kSortedCopy is the exception that
    // draws the right image: a compute pass after the sort copies the records into draw order
    // (splat_sorted_copy.comp, timed inside the sort segment), and the draw reads them at its own
    // position - the same blend order as kNone without the per-vertex order[] gather. It holds a
    // second record buffer (visible capacity x 32 B), allocated by the first set_draw_probe that asks
    // for it and kept until destroy.
    enum class DrawProbe : std::uint32_t {
        kNone = 0,
        kSequential = 1,  // records in compacted order, no order[] gather
        kTiny = 2,        // primitives shrunk to ~0 area: no fragments
        kClip = 3,        // primitives rejected by clipping: vertex work only
        kNoBlend = 4,     // blending off (fragments overwrite): removes the blend's read-modify-write
        kNoFragment = 5,  // every fragment discarded first: rasterization and launch, no shading
        kSortedCopy = 6,  // records copied into draw order after the sort, read sequentially
    };
    static constexpr std::uint32_t kDrawProbeCount = 7;
    // Fails (and keeps the previous probe) only when kSortedCopy's buffer cannot be allocated.
    splatcore::Status set_draw_probe(DrawProbe probe);
    DrawProbe draw_probe() const { return m_draw_probe; }

    // Uploads the camera UBO for the frame about to be recorded on `slot` (whose previous
    // submission has completed), refreshes the HUD's visible-count copies, and decides whether
    // the compute chain must re-run (camera or settings changed since the last computed frame).
    void prepare_frame(std::uint32_t slot, const SplatCameraParams& camera);

    // Records chunk cull + preprocess + prepare + sort; a no-op returning false when nothing
    // changed since the last computed frame (the sorted buffers stay valid for the draw). With a
    // timer, stamps two pass boundaries: after the chunk cull and after the preprocess.
    bool record_compute(VkCommandBuffer cmd, std::uint32_t slot,
                        GpuTimer* timer = nullptr);             // outside dynamic rendering
    void record_draw(VkCommandBuffer cmd, std::uint32_t slot);  // inside dynamic rendering

    std::uint32_t splat_count() const { return m_count; }
    std::uint32_t visible_capacity() const { return m_capacity; }
    std::uint32_t chunk_count() const { return m_chunk_count; }
    std::uint32_t sh_degree() const { return m_sh_degree; }
    // Visible counts of a recently completed frame (trail by the frames in flight); HUD only.
    std::uint32_t last_visible_count() const { return m_last_visible; }
    std::uint32_t last_visible_chunks() const { return m_last_visible_chunks; }
    // Visible splats of that frame beyond the visible capacity: passed the culls, not drawn.
    std::uint32_t last_clipped_count() const {
        return m_last_visible > m_capacity ? m_last_visible - m_capacity : 0;
    }

    // Depth pick: the world point under a pixel, as drawn (a double-tap pivot). request_pick marks
    // the next frame's compute chain dirty; that frame keeps the sorted keys and a pick pass
    // appends the (depth, alpha) of every splat covering the pixel. take_pick returns the result
    // once that frame's slot has completed (prepare_frame of the same slot, kFramesInFlight frames
    // later): the depth at which the composited opacity reaches half its total, along the pixel's
    // ray of that frame's camera. kMiss when the splats there cover less than 10 % of the pixel.
    enum class PickResult { kNone, kPending, kHit, kMiss };
    void request_pick(float x_px, float y_px);  // framebuffer pixels, y down
    PickResult take_pick(float world[3]);       // kHit fills world; kHit and kMiss consume the result

private:
    Context* m_context = nullptr;
    static constexpr std::uint64_t kMaxCount = 0x7fffffffu;  // splat indices and counts are uint32
    std::uint32_t m_count = 0;
    std::uint32_t m_capacity = 0;  // draw records / sort elements: visible splats drawn per frame
    std::uint32_t m_chunk_count = 0;
    std::uint32_t m_sh_degree = 0;
    float m_sh1_scale = 0.0f;
    float m_aabb_min[3] = {}, m_aabb_max[3] = {};  // pack bounds: per-frame depth range of the keys
    float m_log_depth_min = 0.0f, m_log_depth_inv_range = 1.0f;
    int m_key_bits = 32;
    float m_min_contribution = 0.0f;
    bool m_has_coverage = false;
    float m_coverage_strength = 0.0f;
    float m_coverage_cos_full = 0.0f, m_coverage_cos_zero = 0.0f;  // set in init
    bool m_chunk_cull = true;
    bool m_chunk_contribution_cull = true;
    DrawMode m_draw_mode = kDefaultDrawMode;
    DrawProbe m_draw_probe = DrawProbe::kNone;
    bool m_compute_dirty = true;  // camera/settings changed since the last computed frame
    std::uint32_t m_last_visible = 0;
    std::uint32_t m_last_visible_chunks = 0;

    Buffer m_positions, m_scale_rot, m_color, m_sh1, m_records;
    Buffer m_coverage;                 // per-splat masks, or a 4-byte dummy
    Buffer m_keys, m_values;           // sort key/index pairs for visible splats (m_capacity each)
    Buffer m_chunks;                   // pack chunk cull AABBs + importance_max (7 floats each)
    Buffer m_chunk_list;               // visible chunk indices, written by the chunk cull
    Buffer m_cull_dispatch;            // VkDispatchIndirectCommand for the preprocess
    Buffer m_counter;                  // {visible splats, visible chunks}, device-local
    Buffer m_sort_dispatch;            // VkDispatchIndirectCommand for the sort block passes
    Buffer m_draw_cmd;                 // VkDraw(Indexed)IndirectCommand for m_draw_mode (5 uints)
    Buffer m_quad_indices;             // kQuadsPerBatch quads of 16-bit indices (indexed-batch mode)
    Buffer m_camera[kFramesInFlight];  // host-visible, mapped; camera UBO
    // Per-slot host-visible snapshot of m_counter, copied after the preprocess. The live counter
    // is cleared and refilled by whichever frame the GPU is executing, so reading it from the host
    // races; a slot's snapshot is complete once that slot's previous submission has finished.
    Buffer m_counter_readback[kFramesInFlight];
    bool m_readback_pending[kFramesInFlight] = {};  // a snapshot copy was recorded on this slot
    DescriptorSetLayout m_compute_layout, m_prepare_layout, m_draw_layout;
    DescriptorPool m_pool;
    VkDescriptorSet m_compute_sets[kFramesInFlight] = {};
    VkDescriptorSet m_prepare_set = VK_NULL_HANDLE;
    VkDescriptorSet m_draw_set = VK_NULL_HANDLE;
    Pipeline m_chunk_cull_pipe, m_preprocess, m_prepare;
    Pipeline m_draw[kDrawModeCount];          // one per DrawMode (specialized vertex shader + topology)
    Pipeline m_draw_probes[kDrawProbeCount];  // triangle draw per DrawProbe (index 0 unused)
    RadixSort m_sort;

    // DrawProbe::kSortedCopy: records in draw order, the copy pass and its descriptor sets (the
    // draw set binds m_sorted_records at binding 0). The buffer exists once the probe was asked for.
    void record_sorted_copy(VkCommandBuffer cmd);
    Buffer m_sorted_records;
    DescriptorSetLayout m_copy_layout;
    VkDescriptorSet m_copy_set = VK_NULL_HANDLE;
    VkDescriptorSet m_draw_copy_set = VK_NULL_HANDLE;
    Pipeline m_copy_pipe;

    // Depth pick (request_pick / take_pick).
    void record_pick(VkCommandBuffer cmd, std::uint32_t slot);
    void resolve_pick(std::uint32_t slot);
    Buffer m_pick;                            // {count, pad, (depth, alpha) x kPickCapacity}
    Buffer m_pick_readback[kFramesInFlight];  // host-visible copy, read when the slot completes
    DescriptorSetLayout m_pick_layout;
    VkDescriptorSet m_pick_set = VK_NULL_HANDLE;
    Pipeline m_pick_pipe;
    bool m_pick_requested = false;  // record the pick on the next computed frame
    float m_pick_px[2] = {};
    int m_pick_slot = -1;                   // slot whose readback holds a recorded pick, -1 = none
    unsigned char m_pick_camera[160] = {};  // CameraUbo of the frame the pick was recorded on
    PickResult m_pick_result = PickResult::kNone;
    float m_pick_world[3] = {};

    float m_last_viewport[2] = {1.0f, 1.0f};
    unsigned char m_last_camera[160] = {};  // last uploaded CameraUbo bytes (dirty tracking)
    bool m_has_last_camera = false;
};

}  // namespace splatrender
