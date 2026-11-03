#pragma once

#include <android/native_window.h>
#include <android/thermal.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "splatcore/home_view.h"
#include "splatcore/quality_controller.h"
#include "splatcore/result.h"
#include "splatcore/scene_load.h"
#include "splatrender/splat_renderer.h"
#include "splatrender/vk_context.h"
#include "splatrender/vk_frame.h"
#include "splatrender/vk_swapchain.h"
#include "splatrender/vk_timer.h"

namespace pocketsplat {

// Interactive renderer settings on the phone (ADR-0008): 16-bit sort keys and contribution
// culling below 0.1 px of full coverage, each inside the measured image-quality budget. The bench
// sets its own values per run; the desktop host keeps exact keys and no culling (golden images).
inline constexpr int kDefaultKeyBits = 16;
inline constexpr float kDefaultMinContribution = 0.1f;

// Thermal-adaptive quality in interactive use (ADR-0009): the controller moves the threshold
// along its ladder (kDefaultMinContribution is its best rung) to hold kInteractiveTargetFps
// without reaching thermal throttling. Frames are drawn on demand, so the first frame after more
// than kIdleGapS without one starts a new interaction and its interval is not counted.
// Interactive frames are paced at kInteractiveTargetFps, not the display's rate: the ladder saves
// work per frame only at a fixed frame rate, and a drag on the S24U's 120 Hz panel rendered at
// 104 fps (S21). The surface asks the compositor for that rate too, and the render thread skips
// the vsyncs in between when the panel stays faster (interactive_target_fps()).
inline constexpr bool kDefaultAdaptiveQuality = true;
inline constexpr float kInteractiveTargetFps = 60.0f;
inline constexpr double kIdleGapS = 0.25;

// The Android render host: one Vulkan context for the process lifetime of the engine object, a
// swapchain bound to whatever ANativeWindow the SurfaceView currently provides, and the shared
// splat pipeline. Every method except the stats getters must be called on the app's single
// render thread (the Kotlin side owns that contract); stats are atomics readable from any
// thread. Scene and surface may arrive in either order - the renderer is created when both are
// present.
class AndroidHost {
public:
    AndroidHost() = default;
    ~AndroidHost() { destroy(); }
    AndroidHost(const AndroidHost&) = delete;
    AndroidHost& operator=(const AndroidHost&) = delete;

    splatcore::Status init();  // Vulkan context; no validation layers on device
    void destroy();

    splatcore::Status open_scene(const std::string& path);
    // Takes ownership of `window` (released on the next set_surface/destroy); null releases the
    // swapchain and surface (the Kotlin side blocks surfaceDestroyed on this call).
    splatcore::Status set_surface(ANativeWindow* window);

    // Interactive camera: eye position and camera-to-world orientation (unit quaternion w, x, y,
    // z; the camera looks down its -z), RUB world. The Kotlin trackball owns the pose; the bench
    // ignores it and runs its own orbit.
    // fov_rad spans the view's long side, so turning the phone keeps the scale and shows more
    // across (in portrait, the vertical FOV as before).
    void set_camera_pose(const float eye[3], const float orientation[4], float fov_rad);
    // Start view of the open scene (splatcore::estimate_home_view), computed at open_scene.
    const splatcore::HomeView& home_view() const { return m_home; }
    // Its camera-to-world orientation (w, x, y, z): looking at the focus, level with home.up.
    void home_orientation(float out[4]) const {
        for (int i = 0; i < 4; ++i) out[i] = m_home_orientation[i];
    }
    // Depth pick for the double-tap pivot: request_pick at a point of the view (0..1, y down);
    // take_pick reports kPending until the result is back (a few frames; frames must keep
    // being drawn), then kHit with the world point or kMiss once.
    void request_pick(float u, float v);
    splatrender::SplatRenderer::PickResult take_pick(float world[3]);
    void render();  // one frame; no-op without a surface (clears without a scene)

    // Interactive quality controller (ADR-0009), fed after every rendered frame and by
    // thermal_tick(), which the render thread calls about once a second whether or not it draws:
    // Android drops its headroom forecast history after 10 s without a call, and an idle phone
    // that cooled down should be back on a better rung before the next drag. thermal_tick()
    // returns true when the rung changed (the caller redraws). The bench pauses the interactive
    // controller and runs its own.
    void set_adaptive_quality(bool on);
    // Coverage fade (ADR-0013) strength, 0..1; kept across scenes, effective for scenes with a
    // <stem>.pscov sidecar next to the pack (has_coverage), bench runs included.
    void set_coverage_strength(float strength);
    bool has_coverage() const { return m_renderer_ready ? m_splats.has_coverage() : m_coverage.has_value(); }
    bool adaptive_quality() const { return m_adaptive.has_value(); }
    bool thermal_tick();
    // Rate the render thread paces interactive frames at; 0 = free-running (bench).
    float interactive_target_fps() const { return m_bench_active ? 0.0f : kInteractiveTargetFps; }

    // Interactive trace: every sample of interactive use - rendered frames and thermal ticks - as
    // rows of the bench CSV (column `drawn` tells them apart), so `splatconv replay` reproduces
    // the controller's decisions over a hand-held session. Starting a trace restarts the
    // interactive controller, so the file holds its whole history. Rows are appended to the file
    // by flush_trace(), which thermal_tick() calls every few seconds and the host calls on pause.
    splatcore::Status start_trace(const std::string& path);
    void flush_trace();
    void stop_trace();

    // Benchmark mode: deterministic auto-orbit (2*pi/600 per frame, the desktop --bench rate),
    // vsync off (MAILBOX on Android), per-frame rows recorded in memory and written as a CSV on
    // stop. Touch camera input is ignored while active. Requires an open scene and a surface.
    // With a frame cap the render thread sleeps to a fixed schedule, so an equal-work run against
    // a paced viewer renders the same frames per second instead of as many as the GPU allows.
    struct BenchConfig {
        std::string csv_path;    // final CSV path (written atomically via .tmp + rename)
        std::string meta;        // host-side metadata appended to the CSV header (model, soc, ...)
        std::string scene_name;  // scene basename recorded in the header
        int key_bits = 32;
        float min_contribution = 0.0f;  // contribution cull threshold, pixels (0 = off)
        float target_fps = 0.0f;        // frame cap (0 = uncapped)
        // splatrender::SplatRenderer::DrawMode (0..3) + 4 x DrawProbe (timing-only variants of the
        // triangle draw; bench_android.py --draw-probe). Carried in one value so the intent extra
        // and the JNI call stay as they were.
        int draw_mode = 0;
        // Thermal-adaptive quality controller (ADR-0009): starts on the ladder's best rung and
        // moves the contribution threshold; min_contribution is ignored while it runs.
        bool controller = false;
        // Ask the compositor for target_fps, as interactive frames do (the panel then runs at
        // that rate). Off, the panel stays at the compositor's choice: 120 Hz at every cap on
        // both phones, as in every run before S21.
        bool vote_fps = false;
    };
    splatcore::Status start_bench(const BenchConfig& config);
    std::string stop_bench();  // writes the CSV; returns a one-line summary ("" = nothing ran)
    bool bench_active() const { return m_bench_active; }

    bool has_scene() const { return m_pack.has_value() || m_renderer_ready; }
    const splatcore::SceneFraming& framing() const { return m_framing; }
    std::uint32_t splat_count() const { return m_splat_count; }
    std::uint32_t sh_degree() const { return m_sh_degree; }

    // Readable from any thread (HUD polling).
    struct Stats {
        std::atomic<float> gpu_ms{0.0f};
        std::atomic<float> cull_ms{0.0f};
        std::atomic<float> preprocess_ms{0.0f};
        std::atomic<float> sort_ms{0.0f};
        std::atomic<float> draw_ms{0.0f};
        std::atomic<std::uint32_t> visible{0};
        std::atomic<std::uint32_t> visible_chunks{0};
        std::atomic<std::uint64_t> frames{0};
        std::atomic<float> min_contribution{0.0f};  // threshold the last frame rendered with
        std::atomic<std::int32_t> rung{-1};         // interactive controller rung, -1 = off
    };
    const Stats& stats() const { return m_stats; }

private:
    splatcore::Status ensure_renderer();  // creates the splat renderer once scene + swapchain exist
    void release_surface();
    splatcore::QualityControllerConfig controller_config(float target_fps) const;
    // Reads thermal status, headroom and the kgsl clock at most once per second (AThermal returns
    // NaN when polled faster): even readings take the controller's 30 s forecast, odd ones the
    // current headroom that every earlier bench CSV logged. Returns the bench CSV's
    // thermal_sample code: 0 no reading, 1 forecast, 2 current, 3 NaN.
    std::uint8_t sample_platform(std::chrono::steady_clock::time_point now);
    // One interactive sample (a rendered frame, or a thermal tick with frame = false): feeds the
    // interactive controller and the trace. Returns true when the rung changed.
    bool interactive_update(std::chrono::steady_clock::time_point now, bool frame);
    void vote_frame_rate();  // asks the compositor for the interactive or the bench rate

    splatrender::Context m_context;
    bool m_context_ready = false;
    splatrender::Swapchain m_swapchain;
    splatrender::FrameRing m_ring;
    splatrender::GpuTimer m_timer;
    splatrender::SplatRenderer m_splats;
    bool m_ring_ready = false;
    bool m_swapchain_ready = false;
    bool m_renderer_ready = false;
    bool m_needs_recreate = false;

    ANativeWindow* m_window = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    std::optional<splatcore::GpuPack> m_pack;              // held until the renderer can be created
    std::optional<std::vector<std::uint32_t>> m_coverage;  // the pack's sidecar masks, likewise
    float m_coverage_strength = 0.0f;
    std::string m_scene_name;  // file name of the open scene (trace header)
    splatcore::SceneFraming m_framing{{0.0f, 0.0f, 0.0f}, 1.0f};
    std::uint32_t m_splat_count = 0;
    std::uint32_t m_sh_degree = 0;

    // Orbit camera state (RUB world, matching the desktop host).
    // Bench orbit (deterministic, the desktop --bench path).
    float m_yaw = 0.6f, m_pitch = 0.3f, m_distance = 5.0f;
    float m_target[3] = {0.0f, 0.0f, 0.0f};
    float m_fov = 1.0471976f;  // 60 deg across the long side (= vertical in portrait)
    // Interactive pose (set_camera_pose), the home view until the Kotlin side sends one.
    float m_eye[3] = {0.0f, 0.0f, 5.0f};
    float m_orientation[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    splatcore::HomeView m_home{};
    float m_home_orientation[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    // Benchmark recording (render-thread state; GPU columns trail by the frames in flight).
    struct BenchRow {
        float t_s, cpu_ms, gpu_ms, cull_ms, preprocess_ms, sort_ms, draw_ms;
        std::uint32_t visible, visible_chunks;
        std::uint8_t compute_ran, thermal_status;
        float headroom;               // current headroom at the last reading
        std::int32_t gpu_mhz;         // kgsl clock at the last 1 Hz sample, -1 when unreadable
        float min_contribution;       // the threshold this frame rendered with
        std::int8_t rung;             // controller rung this frame rendered with, -1 = no controller
        std::uint8_t reason;          // splatcore::QualityReason decided after this frame
        float forecast;               // 30 s headroom forecast at the last reading
        std::uint8_t thermal_sample;  // reading taken with this frame: 0 none, 1 forecast, 2 current, 3 NaN
        std::uint8_t drawn;           // 1 = a rendered frame, 0 = a thermal tick (interactive trace)
    };
    // Bench and trace CSVs share the controller header line, the notes, the columns and the row
    // format.
    void write_csv_controller_header(std::ostream& csv, const splatcore::QualityControllerConfig& config,
                                     bool on) const;
    static void write_csv_columns(std::ostream& csv);
    static void write_csv_rows(std::ostream& csv, const std::vector<BenchRow>& rows, std::size_t first_index);
    void write_trace_scene();  // '# scene=...' line whenever a renderer is created while tracing
    void bench_pace();         // frame cap: sleeps to the next deadline
    void bench_record_frame();
    bool m_bench_active = false;
    BenchConfig m_bench_config;
    int m_interactive_key_bits = kDefaultKeyBits;  // renderer settings restored when the bench stops
    splatrender::SplatRenderer::DrawMode m_interactive_draw_mode =
        splatrender::SplatRenderer::kDefaultDrawMode;
    // What the bench ran with, for the CSV header (written after the interactive mode is back).
    splatrender::SplatRenderer::DrawMode m_bench_draw_mode = splatrender::SplatRenderer::kDefaultDrawMode;
    splatrender::SplatRenderer::DrawProbe m_bench_draw_probe = splatrender::SplatRenderer::DrawProbe::kNone;
    float m_interactive_min_contribution = kDefaultMinContribution;
    std::vector<BenchRow> m_bench_rows;
    std::chrono::steady_clock::time_point m_bench_start, m_bench_last_frame, m_bench_next_frame;
    AThermalManager* m_thermal = nullptr;  // acquired for the host's lifetime
    std::chrono::steady_clock::time_point m_thermal_sampled_at;
    bool m_thermal_sampled = false;    // no reading yet (since init or the bench start)
    std::uint32_t m_thermal_tick = 0;  // even ticks read the forecast, odd ones the current value
    std::uint8_t m_thermal_status = 0;
    float m_thermal_headroom = 0.0f;
    float m_thermal_forecast = 0.0f;
    std::int32_t m_gpu_mhz = -1;  // sampled with the thermal state (Phase 4 controller input)
    std::optional<splatcore::QualityController> m_controller;  // bench runs with config.controller
    float m_light_headroom = 0.0f;  // controller LIGHT threshold and where it came from
    const char* m_light_source = "";
    std::int32_t m_max_gpu_mhz = 0;  // 0 = unreadable

    std::optional<splatcore::QualityController> m_adaptive;  // interactive controller, empty = off
    std::chrono::steady_clock::time_point m_epoch;           // its time base (init or trace start)
    std::chrono::steady_clock::time_point m_last_frame_at;
    bool m_has_last_frame = false;
    bool m_last_compute_ran = false;  // record_compute of the last rendered frame

    std::ofstream m_trace;               // interactive trace, open while recording
    std::vector<BenchRow> m_trace_rows;  // rows since the last flush
    std::size_t m_trace_written = 0;     // rows already in the file (the frame column continues)
    std::chrono::steady_clock::time_point m_trace_flushed_at;

    Stats m_stats;
};

}  // namespace pocketsplat
