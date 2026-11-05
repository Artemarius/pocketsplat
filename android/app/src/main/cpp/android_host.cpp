#include "android_host.h"

#include <android/log.h>
#include <dlfcn.h>

#include "splatcore/coverage.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iterator>
#include <limits>
#include <thread>

#define LOG_TAG "PocketSplat"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace pocketsplat {
namespace {

// "x;y;z" for the bench CSV header (';' keeps the header's ", "-separated fields parseable).
std::string vec3_text(const float v[3]) {
    char text[96];
    std::snprintf(text, sizeof(text), "%.5f;%.5f;%.5f", v[0], v[1], v[2]);
    return text;
}

// Adreno GPU clock from kgsl sysfs, or -1 when the app's SELinux domain cannot read it. At a capped
// frame rate the governor holds busy % near a target and moves this clock with the work instead
// (docs/benchmarks/public-baseline.md, S17), so the clock is the work signal the controller watches.
std::int32_t read_gpu_mhz() {
    std::FILE* f = std::fopen("/sys/class/kgsl/kgsl-3d0/clock_mhz", "r");
    if (f == nullptr) return -1;
    int mhz = -1;
    if (std::fscanf(f, "%d", &mhz) != 1) mhz = -1;
    std::fclose(f);
    return mhz;
}

// Highest clock kgsl offers (MHz), the reference of the controller's step-up gate; 0 = unreadable.
std::int32_t read_max_gpu_mhz() {
    if (std::FILE* f = std::fopen("/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies", "r")) {
        long long hz = 0, best = 0;
        while (std::fscanf(f, "%lld", &hz) == 1) best = std::max(best, hz);
        std::fclose(f);
        if (best > 0) return static_cast<std::int32_t>(best / 1000000);
    }
    if (std::FILE* f = std::fopen("/sys/class/kgsl/kgsl-3d0/max_clock_mhz", "r")) {
        int mhz = 0;
        if (std::fscanf(f, "%d", &mhz) != 1) mhz = 0;
        std::fclose(f);
        return std::max(mhz, 0);
    }
    return 0;
}

// Pre-rotation (Swapchain::pre_transform): on a turned phone the swapchain image is the user's
// view turned by the display transform, in the panel's native orientation. A turned image of a
// pinhole camera (square pixels, centred principal point) is exactly the image of the same camera
// rolled about its axis by the opposite angle, so the host rolls the view and the renderer stays
// unaware. ROTATE_90 = the view turned 90 deg clockwise: image right = view up, image up = view left.
glm::mat4 pre_rotation_roll(VkSurfaceTransformFlagBitsKHR transform) {
    float angle = 0.0f;
    switch (transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            angle = -0.5f * glm::pi<float>();
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            angle = glm::pi<float>();
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            angle = 0.5f * glm::pi<float>();
            break;
        default:
            return glm::mat4(1.0f);
    }
    return glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 0.0f, 1.0f));
}

// A point of the user's view (u, v in 0..1, y down) in the turned swapchain image.
void view_to_image_uv(VkSurfaceTransformFlagBitsKHR transform, float& u, float& v) {
    const float vu = u, vv = v;
    switch (transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            u = 1.0f - vv;
            v = vu;
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            u = 1.0f - vu;
            v = 1.0f - vv;
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            u = vv;
            v = 1.0f - vu;
            break;
        default:
            break;
    }
}

const char* transform_name(VkSurfaceTransformFlagBitsKHR transform) {
    switch (transform) {
        case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
            return "identity";
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            return "rotate 90";
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            return "rotate 180";
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            return "rotate 270";
        default:
            return "other";
    }
}

// LIGHT headroom threshold from AThermal_getThermalHeadroomThresholds (API 35; looked up at run
// time because minSdk is 33), or NaN when the platform reports none.
float platform_light_headroom(AThermalManager* manager) {
    using Fn = int (*)(AThermalManager*, const AThermalHeadroomThreshold**, size_t*);
    const auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "AThermal_getThermalHeadroomThresholds"));
    const AThermalHeadroomThreshold* thresholds = nullptr;
    size_t count = 0;
    if (fn == nullptr || manager == nullptr || fn(manager, &thresholds, &count) != 0 ||
        thresholds == nullptr) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    for (size_t i = 0; i < count; ++i) {
        if (thresholds[i].thermalStatus == ATHERMAL_STATUS_LIGHT && std::isfinite(thresholds[i].headroom)) {
            return thresholds[i].headroom;
        }
    }
    return std::numeric_limits<float>::quiet_NaN();
}

// Bench values as the CSV prints them (%.3f). The controller is fed these quantized values, so
// `splatconv replay` on the CSV reproduces its decisions exactly (ties to even, like printf).
float csv3(double value) {
    return static_cast<float>(std::nearbyint(value * 1000.0) / 1000.0);
}

constexpr int kForecastSeconds = 30;  // ADR-0009: the thermal loop acts on the 30 s forecast

// Swapchain-image layout transition via synchronization2 (same shape as the desktop host's).
void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace

splatcore::Status AndroidHost::init() {
    splatrender::ContextCreateInfo info;
    info.app_name = "pocketsplat-android";
    info.enable_validation = false;  // no validation layers shipped in the APK
    splatcore::Status s = m_context.init(info);
    if (!s.ok()) return s;
    m_context_ready = true;
    s = m_ring.init(m_context);
    if (s.ok()) s = m_timer.init(m_context);
    if (!s.ok()) return s;
    m_ring_ready = true;
    LOGI("Vulkan ready: %s", m_context.device_name());

    // Controller inputs found at run time: the platform's LIGHT threshold, else ADR-0009's
    // (40 C - 15) / 30 for the Samsung skin sensor, and the highest kgsl clock.
    m_epoch = std::chrono::steady_clock::now();
    m_thermal = AThermal_acquireManager();
    m_max_gpu_mhz = read_max_gpu_mhz();
    const float platform_light = platform_light_headroom(m_thermal);
    m_light_source = std::isfinite(platform_light) ? "platform" : "default";
    m_light_headroom =
        std::isfinite(platform_light) ? platform_light : splatcore::QualityControllerConfig{}.light_headroom;
    set_adaptive_quality(kDefaultAdaptiveQuality);
    return splatcore::Status::success();
}

splatcore::QualityControllerConfig AndroidHost::controller_config(float target_fps) const {
    splatcore::QualityControllerConfig config;
    config.target_fps = target_fps;
    config.light_headroom = m_light_headroom;
    config.max_gpu_mhz = m_max_gpu_mhz;
    return config;
}

void AndroidHost::set_adaptive_quality(bool on) {
    if (on == m_adaptive.has_value()) return;
    if (on) {
        m_adaptive.emplace(controller_config(kInteractiveTargetFps));
    } else {
        m_adaptive.reset();
    }
    m_has_last_frame = false;
    const float threshold = m_adaptive ? m_adaptive->min_contribution() : kDefaultMinContribution;
    if (m_renderer_ready && !m_bench_active) m_splats.set_min_contribution(threshold);
    m_stats.min_contribution.store(threshold, std::memory_order_relaxed);
    m_stats.rung.store(m_adaptive ? m_adaptive->rung() : -1, std::memory_order_relaxed);
    LOGI("adaptive quality %s (light %.3f from %s, max_gpu_mhz %d)", on ? "on" : "off", m_light_headroom,
         m_light_source, m_max_gpu_mhz);
    if (m_trace.is_open()) {  // a new controller: a replay of the trace diverges from here on
        flush_trace();
        m_trace << "# adaptive quality " << (on ? "on" : "off") << " at "
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - m_epoch).count()
                << " s\n";
    }
}

bool AndroidHost::thermal_tick() {
    if (m_bench_active) return false;
    const auto now = std::chrono::steady_clock::now();
    const bool changed = interactive_update(now, false);
    if (m_trace.is_open() && now - m_trace_flushed_at >= std::chrono::seconds(10)) flush_trace();
    return changed;
}

bool AndroidHost::interactive_update(std::chrono::steady_clock::time_point now, bool frame) {
    if (!m_adaptive && !m_trace.is_open()) return false;
    // Values as the trace prints them, so a replay sees exactly what the controller saw.
    splatcore::QualitySample sample;
    sample.t_s = csv3(std::chrono::duration<double>(now - m_epoch).count());
    if (frame) {
        if (m_has_last_frame) {
            const double gap_s = std::chrono::duration<double>(now - m_last_frame_at).count();
            if (gap_s <= kIdleGapS) sample.frame_interval_ms = csv3(1000.0 * gap_s);
        }
        m_last_frame_at = now;
        m_has_last_frame = true;
        sample.gpu_ms = csv3(m_timer.last_gpu_ms());
    }
    const std::uint8_t reading = sample_platform(now);
    if (reading != 0) sample.gpu_mhz = m_gpu_mhz;
    if (reading == 1) sample.headroom_forecast = m_thermal_forecast;
    if (reading == 2) sample.headroom_current = m_thermal_headroom;

    const int rung = m_adaptive ? m_adaptive->rung() : -1;
    splatcore::QualityDecision decision{rung, splatcore::QualityReason::kNone, false};
    if (m_adaptive) {
        decision = m_adaptive->update(sample);
        if (decision.reason != splatcore::QualityReason::kNone) {
            LOGI(
                "adaptive quality: t=%.1f s %s -> rung %d (c %.2f), forecast %.3f, slope %+.4f/min, clock "
                "%.0f MHz",
                sample.t_s, splatcore::reason_name(decision.reason), decision.rung,
                m_adaptive->min_contribution(), m_adaptive->forecast(), m_adaptive->slope_per_min(),
                m_adaptive->clock_mhz());
        }
    }
    if (m_trace.is_open()) {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        BenchRow row{};
        row.t_s = static_cast<float>(sample.t_s);
        row.cpu_ms = sample.frame_interval_ms;  // the interval the controller counted, NaN = none
        row.gpu_ms = sample.gpu_ms;
        row.cull_ms = frame ? static_cast<float>(m_timer.segment_ms(0)) : nan;
        row.preprocess_ms = frame ? static_cast<float>(m_timer.segment_ms(1)) : nan;
        row.sort_ms = frame ? static_cast<float>(m_timer.segment_ms(2)) : nan;
        row.draw_ms = frame ? static_cast<float>(m_timer.segment_ms(3)) : nan;
        row.visible = frame ? m_splats.last_visible_count() : 0;
        row.visible_chunks = frame ? m_splats.last_visible_chunks() : 0;
        row.compute_ran = frame && m_last_compute_ran ? 1 : 0;
        row.thermal_status = m_thermal_status;
        row.headroom = m_thermal_headroom;
        row.gpu_mhz = m_gpu_mhz;
        row.min_contribution = m_splats.min_contribution();
        row.rung = static_cast<std::int8_t>(rung);
        row.reason = static_cast<std::uint8_t>(decision.reason);
        row.forecast = m_thermal_forecast;
        row.thermal_sample = reading;
        row.drawn = frame ? 1 : 0;
        m_trace_rows.push_back(row);
    }
    if (decision.changed) {
        if (m_renderer_ready) m_splats.set_min_contribution(m_adaptive->min_contribution());
        m_stats.min_contribution.store(m_adaptive->min_contribution(), std::memory_order_relaxed);
        m_stats.rung.store(m_adaptive->rung(), std::memory_order_relaxed);
    }
    return decision.changed;
}

void AndroidHost::vote_frame_rate() {
    if (m_window == nullptr) return;
    // 0 = no preference: the compositor's own choice, as in every bench run before S21.
    const float fps = !m_bench_active           ? kInteractiveTargetFps
                      : m_bench_config.vote_fps ? m_bench_config.target_fps
                                                : 0.0f;
    if (ANativeWindow_setFrameRate(m_window, fps, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT) != 0) {
        LOGE("ANativeWindow_setFrameRate(%.0f) failed", fps);
    }
}

splatcore::Status AndroidHost::start_trace(const std::string& path) {
    stop_trace();
    m_trace.open(path, std::ios::out | std::ios::trunc);
    if (!m_trace) return splatcore::Status::failure("cannot open trace file '" + path + "'");
    // The controller and its time base start with the trace, and so does the thermal sampling
    // (the first reading is a forecast, as in a bench run).
    const auto now = std::chrono::steady_clock::now();
    m_epoch = m_trace_flushed_at = now;
    if (m_adaptive) {
        m_adaptive.emplace(controller_config(kInteractiveTargetFps));
        if (m_renderer_ready && !m_bench_active)
            m_splats.set_min_contribution(m_adaptive->min_contribution());
    }
    m_has_last_frame = false;
    m_thermal_sampled = false;
    m_thermal_tick = 0;
    m_trace_rows.clear();
    m_trace_written = 0;

    const splatcore::QualityControllerConfig defaults;
    m_trace << "# pocketsplat-android interactive trace\n"
            << "# device=" << m_context.device_name() << ", key_bits=" << kDefaultKeyBits
            << ", target_fps=" << kInteractiveTargetFps << ", frame_rate_vote=" << kInteractiveTargetFps
            << ", present_mode=fifo\n";
    write_csv_controller_header(m_trace, m_adaptive ? m_adaptive->config() : defaults,
                                m_adaptive.has_value());
    if (m_renderer_ready) write_trace_scene();
    write_csv_columns(m_trace);
    m_trace.flush();
    LOGI("interactive trace: %s", path.c_str());
    return splatcore::Status::success();
}

void AndroidHost::flush_trace() {
    if (!m_trace.is_open()) return;
    write_csv_rows(m_trace, m_trace_rows, m_trace_written);
    m_trace_written += m_trace_rows.size();
    m_trace_rows.clear();
    m_trace.flush();
    m_trace_flushed_at = std::chrono::steady_clock::now();
}

void AndroidHost::stop_trace() {
    if (!m_trace.is_open()) return;
    flush_trace();
    m_trace.close();
    LOGI("interactive trace closed: %zu rows", m_trace_written);
}

std::uint8_t AndroidHost::sample_platform(std::chrono::steady_clock::time_point now) {
    if (m_thermal == nullptr) return 0;
    if (m_thermal_sampled && now - m_thermal_sampled_at < std::chrono::seconds(1)) return 0;
    m_thermal_status = static_cast<std::uint8_t>(AThermal_getCurrentThermalStatus(m_thermal));
    const bool forecast_tick = m_thermal_tick++ % 2 == 0;
    const float headroom = AThermal_getThermalHeadroom(m_thermal, forecast_tick ? kForecastSeconds : 0);
    std::uint8_t reading = 0;
    if (std::isnan(headroom)) {
        reading = 3;
    } else if (forecast_tick) {
        m_thermal_forecast = csv3(headroom);
        reading = 1;
    } else {
        m_thermal_headroom = csv3(headroom);  // a controller input since ADR-0011, as printed
        reading = 2;
    }
    m_gpu_mhz = read_gpu_mhz();
    m_thermal_sampled_at = now;
    m_thermal_sampled = true;
    return reading;
}

splatcore::Status AndroidHost::open_scene(const std::string& path) {
    splatcore::Result<splatcore::GpuPack> pack = splatcore::load_scene_pack(path, 1);
    if (!pack) return splatcore::Status::failure(pack.error());
    if (m_renderer_ready) {  // replacing a scene: tear the old renderer down first
        m_context.wait_idle();
        m_splats.destroy();
        m_renderer_ready = false;
    }
    m_framing = splatcore::robust_framing(pack.value());
    m_splat_count = pack.value().header.count;
    m_sh_degree = pack.value().header.sh_degree;
    m_yaw = 0.6f;
    m_pitch = 0.3f;
    m_distance = 1.2f * m_framing.diagonal;
    std::memcpy(m_target, m_framing.target, sizeof(m_target));
    // Interactive start: near the capture, looking at the main object (not the whole scene).
    const auto home_start = std::chrono::steady_clock::now();
    m_home = splatcore::estimate_home_view(pack.value());
    std::memcpy(m_eye, m_home.eye, sizeof(m_eye));
    {
        const glm::vec3 back =
            glm::normalize(glm::vec3(m_home.eye[0] - m_home.focus[0], m_home.eye[1] - m_home.focus[1],
                                     m_home.eye[2] - m_home.focus[2]));
        const glm::vec3 right =
            glm::normalize(glm::cross(glm::vec3(m_home.up[0], m_home.up[1], m_home.up[2]), back));
        const glm::quat q = glm::quat_cast(glm::mat3(right, glm::cross(back, right), back));
        m_home_orientation[0] = q.w, m_home_orientation[1] = q.x, m_home_orientation[2] = q.y,
        m_home_orientation[3] = q.z;
        std::memcpy(m_orientation, m_home_orientation, sizeof(m_orientation));
    }
    LOGI("home view in %.0f ms: focus %.3f %.3f %.3f, distance %.3f, plane %s",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - home_start).count(),
         m_home.focus[0], m_home.focus[1], m_home.focus[2], m_home.distance,
         m_home.plane_found ? "yes" : "no");
    // Coverage sidecar (ADR-0013): <stem>.pscov next to the scene. A file that does not belong to
    // this scene is logged and ignored - the scene still opens, without the fade.
    m_coverage.reset();
    {
        const std::string cov_path = path.substr(0, path.find_last_of('.')) + ".pscov";
        std::ifstream file(cov_path, std::ios::binary);
        if (file) {
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                                  std::istreambuf_iterator<char>());
            splatcore::Result<std::vector<std::uint32_t>> masks =
                splatcore::parse_coverage(bytes, pack.value());
            if (masks) {
                m_coverage = std::move(masks.value());
                splatcore::CoverageHeader header;
                std::memcpy(&header, bytes.data(), sizeof(header));  // parse_coverage checked the size
                m_coverage_source = static_cast<int>(header.source);
                LOGI("coverage: %s (source %d, %u cameras)", cov_path.c_str(), m_coverage_source,
                     header.camera_count);
            } else {
                LOGE("coverage %s ignored: %s", cov_path.c_str(), masks.error().c_str());
            }
        }
    }
    m_pack = std::move(pack.value());
    m_scene_name = path.substr(path.find_last_of('/') + 1);
    LOGI("scene: %s, %u splats, SH %u", path.c_str(), m_splat_count, m_sh_degree);
    return ensure_renderer();
}

splatcore::Status AndroidHost::ensure_renderer() {
    using splatcore::Status;
    if (m_renderer_ready || !m_pack.has_value() || !m_swapchain_ready) return Status::success();
    Status s = m_splats.init(m_context, *m_pack, m_swapchain.format(), m_coverage ? &*m_coverage : nullptr);
    if (!s.ok()) return s;
    m_splats.set_coverage_strength(m_coverage_strength);
    if (!m_bench_active) {
        m_splats.set_key_bits(kDefaultKeyBits);
        m_splats.set_min_contribution(m_adaptive ? m_adaptive->min_contribution() : kDefaultMinContribution);
    }
    m_renderer_ready = true;
    m_pack.reset();  // renderer holds device copies; free the CPU arrays
    m_coverage.reset();
    if (m_trace.is_open()) write_trace_scene();
    return Status::success();
}

void AndroidHost::set_coverage_strength(float strength) {
    m_coverage_strength = std::clamp(strength, 0.0f, 1.0f);
    if (m_renderer_ready) m_splats.set_coverage_strength(m_coverage_strength);
}

void AndroidHost::write_trace_scene() {
    flush_trace();  // the line goes after the rows recorded before it
    m_trace << "# scene=" << m_scene_name << ", splats=" << m_splats.splat_count()
            << ", sh=" << m_splats.sh_degree() << ", resolution=" << m_swapchain.extent().width << "x"
            << m_swapchain.extent().height << "\n";
}

void AndroidHost::release_surface() {
    if (m_swapchain_ready) {
        m_context.wait_idle();
        m_swapchain.destroy();
        m_swapchain_ready = false;
    }
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_context.instance(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_window != nullptr) {
        ANativeWindow_release(m_window);
        m_window = nullptr;
    }
}

splatcore::Status AndroidHost::set_surface(ANativeWindow* window) {
    using splatcore::Status;
    if (!m_context_ready) return Status::failure("engine not initialized");
    release_surface();
    if (window == nullptr) return Status::success();
    m_window = window;

    VkAndroidSurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = m_window;
    if (vkCreateAndroidSurfaceKHR(m_context.instance(), &surface_info, nullptr, &m_surface) != VK_SUCCESS) {
        return Status::failure("Android surface creation failed");
    }
    const auto width = static_cast<std::uint32_t>(ANativeWindow_getWidth(m_window));
    const auto height = static_cast<std::uint32_t>(ANativeWindow_getHeight(m_window));
    Status s = m_swapchain.init(m_context, m_surface, width, height);
    if (!s.ok()) return s;
    m_swapchain_ready = true;
    m_needs_recreate = false;
    vote_frame_rate();
    LOGI("swapchain %ux%u, %s", m_swapchain.extent().width, m_swapchain.extent().height,
         transform_name(m_swapchain.pre_transform()));
    return ensure_renderer();
}

void AndroidHost::set_camera_pose(const float eye[3], const float orientation[4], float fov_rad) {
    if (m_bench_active) return;  // the bench owns the camera; touch input is ignored
    std::memcpy(m_eye, eye, sizeof(m_eye));
    std::memcpy(m_orientation, orientation, sizeof(m_orientation));
    m_fov = fov_rad;
}

void AndroidHost::request_pick(float u, float v) {
    if (!m_renderer_ready || !m_swapchain_ready || m_bench_active) return;
    view_to_image_uv(m_swapchain.pre_transform(), u, v);
    m_splats.request_pick(u * static_cast<float>(m_swapchain.extent().width),
                          v * static_cast<float>(m_swapchain.extent().height));
}

splatrender::SplatRenderer::PickResult AndroidHost::take_pick(float world[3]) {
    using PickResult = splatrender::SplatRenderer::PickResult;
    if (!m_renderer_ready) return PickResult::kNone;
    const PickResult result = m_splats.take_pick(world);
    if (result == PickResult::kHit) {
        const float d[3] = {world[0] - m_eye[0], world[1] - m_eye[1], world[2] - m_eye[2]};
        LOGI("pick: hit %.3f %.3f %.3f, %.3f from the eye", world[0], world[1], world[2],
             std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    } else if (result == PickResult::kMiss) {
        LOGI("pick: miss (less than 10 %% coverage under the finger)");
    }
    return result;
}

void AndroidHost::render() {
    if (!m_swapchain_ready) return;
    if (m_bench_active && m_bench_config.target_fps > 0.0f) bench_pace();
    // Deterministic full orbit every 600 frames - the desktop --bench rate, so per-frame GPU
    // work is comparable across devices regardless of achieved fps.
    if (m_bench_active) {
        m_yaw += 6.2831853f / 600.0f;
        ++m_bench_frame;
    }
    if (m_needs_recreate) {
        const auto width = static_cast<std::uint32_t>(ANativeWindow_getWidth(m_window));
        const auto height = static_cast<std::uint32_t>(ANativeWindow_getHeight(m_window));
        if (width == 0 || height == 0) return;
        if (!m_swapchain.recreate(width, height).ok()) return;
        m_needs_recreate = false;
        LOGI("swapchain recreated %ux%u, %s", m_swapchain.extent().width, m_swapchain.extent().height,
             transform_name(m_swapchain.pre_transform()));
    }

    VkCommandBuffer cmd = m_ring.begin();
    if (cmd == VK_NULL_HANDLE) return;
    std::uint32_t image_index = 0;
    const VkResult acquire = m_swapchain.acquire(m_ring.image_available(), &image_index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        m_needs_recreate = true;
        vkEndCommandBuffer(cmd);  // abandon this frame's recording
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) return;

    const float width = static_cast<float>(m_swapchain.extent().width);
    const float height = static_cast<float>(m_swapchain.extent().height);

    m_timer.begin_frame(cmd, m_ring.slot());
    if (m_renderer_ready) {
        // Camera -> the shared camera params (RUB world, Vulkan NDC in the projection): the bench
        // orbit, else the interactive pose (world -> view = the inverse of the camera's rigid pose).
        glm::vec3 eye;
        glm::mat4 view;
        if (m_bench_active && m_bench_config.home_swing) {
            const glm::vec3 focus(m_home.focus[0], m_home.focus[1], m_home.focus[2]);
            const glm::vec3 up(m_home.up[0], m_home.up[1], m_home.up[2]);
            const float u = static_cast<float>(m_bench_frame % 600) / 600.0f;
            const float tri = u < 0.25f ? 4.0f * u : (u < 0.75f ? 2.0f - 4.0f * u : 4.0f * u - 4.0f);
            const float angle = glm::radians(kHomeSwingDeg) * tri;
            const glm::vec3 arm = glm::vec3(m_home.eye[0], m_home.eye[1], m_home.eye[2]) - focus;
            eye = focus + glm::angleAxis(angle, up) * arm;
            view = glm::lookAt(eye, focus, up);
        } else if (m_bench_active) {
            const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
            const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
            const glm::vec3 target(m_target[0], m_target[1], m_target[2]);
            eye = target + m_distance * glm::vec3(cp * sy, sp, cp * cy);
            view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
        } else {
            eye = glm::vec3(m_eye[0], m_eye[1], m_eye[2]);
            const glm::mat3 rotation = glm::mat3_cast(glm::normalize(
                glm::quat(m_orientation[0], m_orientation[1], m_orientation[2], m_orientation[3])));
            const glm::mat3 inverse = glm::transpose(rotation);
            view = glm::mat4(inverse);
            view[3] = glm::vec4(-(inverse * eye), 1.0f);
        }
        // Turned phone: render the view rolled into the native-orientation image (pre_rotation_roll).
        view = pre_rotation_roll(m_swapchain.pre_transform()) * view;
        // The FOV spans the long side, which a quarter turn keeps: one focal length (pixels) for
        // both orientations, and the image's vertical FOV follows from it.
        const float focal = 0.5f * std::max(width, height) / std::tan(0.5f * m_fov);
        glm::mat4 proj =
            glm::perspective(2.0f * std::atan(0.5f * height / focal), width / height, 0.05f, 1000.0f);
        proj[1][1] *= -1.0f;  // Vulkan NDC y down - in the projection, never in data
        const glm::mat4 proj_view = proj * view;

        splatrender::SplatCameraParams params;
        std::memcpy(params.view, glm::value_ptr(view), sizeof(params.view));
        std::memcpy(params.proj_view, glm::value_ptr(proj_view), sizeof(params.proj_view));
        std::memcpy(params.cam_pos, glm::value_ptr(eye), sizeof(params.cam_pos));
        params.focal_y = focal;
        params.focal_x = focal;
        params.viewport_w = width;
        params.viewport_h = height;
        m_splats.prepare_frame(m_ring.slot(), params);
        m_last_compute_ran = m_splats.record_compute(cmd, m_ring.slot(), &m_timer);
    }
    m_timer.stamp(cmd, m_ring.slot());  // boundary: prepare+sort / draw

    image_barrier(cmd, m_swapchain.image(image_index), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = m_swapchain.view(image_index);
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // Light neutral background (PlayCanvas-viewer-like): captures carry faint white fuzz at
    // silhouettes that a dark background spotlights (docs/benchmarks/README.md, S10). The
    // desktop host keeps its dark default (golden refs) and compares via --bg.
    color_attachment.clearValue.color = {{0.55f, 0.58f, 0.62f, 1.0f}};
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, m_swapchain.extent()};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color_attachment;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, m_swapchain.extent()};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (m_renderer_ready) m_splats.record_draw(cmd, m_ring.slot());
    vkCmdEndRendering(cmd);

    image_barrier(cmd, m_swapchain.image(image_index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    m_timer.end_frame(cmd, m_ring.slot());

    if (!m_ring.submit(m_swapchain.render_finished(image_index)).ok()) return;
    const VkResult present = m_swapchain.present(m_context.queue(), image_index);
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) m_needs_recreate = true;

    // Segment layout matches the desktop host: cull / preprocess / prepare+sort / draw.
    m_stats.gpu_ms.store(static_cast<float>(m_timer.last_gpu_ms()), std::memory_order_relaxed);
    m_stats.cull_ms.store(static_cast<float>(m_timer.segment_ms(0)), std::memory_order_relaxed);
    m_stats.preprocess_ms.store(static_cast<float>(m_timer.segment_ms(1)), std::memory_order_relaxed);
    m_stats.sort_ms.store(static_cast<float>(m_timer.segment_ms(2)), std::memory_order_relaxed);
    m_stats.draw_ms.store(static_cast<float>(m_timer.segment_ms(3)), std::memory_order_relaxed);
    m_stats.visible.store(m_splats.last_visible_count(), std::memory_order_relaxed);
    m_stats.visible_chunks.store(m_splats.last_visible_chunks(), std::memory_order_relaxed);
    m_stats.frames.fetch_add(1, std::memory_order_relaxed);

    if (m_bench_active) {
        bench_record_frame();
    } else {
        interactive_update(std::chrono::steady_clock::now(), true);
    }
    m_stats.min_contribution.store(m_splats.min_contribution(), std::memory_order_relaxed);
    m_stats.rung.store(m_adaptive && !m_bench_active ? m_adaptive->rung() : -1, std::memory_order_relaxed);
}

splatcore::Status AndroidHost::start_bench(const BenchConfig& config) {
    using splatcore::Status;
    if (m_bench_active) return Status::failure("bench already running");
    if (!m_renderer_ready || !m_swapchain_ready) return Status::failure("bench needs a scene and a surface");
    m_bench_config = config;
    m_interactive_key_bits = m_splats.key_bits();
    m_interactive_min_contribution = m_splats.min_contribution();
    m_interactive_draw_mode = m_splats.draw_mode();
    m_splats.set_key_bits(config.key_bits);
    m_splats.set_min_contribution(config.min_contribution);
    // draw_mode = DrawMode + kDrawModeCount x DrawProbe (the probe is a diagnostic, BenchConfig).
    using Renderer = splatrender::SplatRenderer;
    const int draw_value = std::max(config.draw_mode, 0);
    m_bench_draw_mode =
        static_cast<Renderer::DrawMode>(draw_value % static_cast<int>(Renderer::kDrawModeCount));
    m_bench_draw_probe =
        static_cast<Renderer::DrawProbe>(std::min(draw_value / static_cast<int>(Renderer::kDrawModeCount),
                                                  static_cast<int>(Renderer::kDrawProbeCount) - 1));
    m_splats.set_draw_mode(m_bench_draw_mode);
    if (Status probe = m_splats.set_draw_probe(m_bench_draw_probe); !probe.ok()) return probe;

    // Vsync off (MAILBOX on Adreno - IMMEDIATE is absent) so present never paces the GPU.
    m_swapchain.set_vsync(false);
    Status s = m_swapchain.recreate(m_swapchain.extent().width, m_swapchain.extent().height);
    if (!s.ok()) return s;

    // Deterministic start pose: the framing default the desktop --bench orbit starts from.
    m_yaw = 0.6f;
    m_pitch = 0.3f;
    m_distance = 1.2f * m_framing.diagonal;
    std::memcpy(m_target, m_framing.target, sizeof(m_target));
    m_fov = 1.0471976f;  // across the long side: the protocol's portrait 864x1872 -> vertical 60 deg
    m_bench_frame = 0;

    m_bench_rows.clear();
    m_bench_rows.reserve(1 << 16);
    m_bench_start = m_bench_last_frame = m_bench_next_frame = std::chrono::steady_clock::now();
    // Every bench CSV starts with a forecast reading on its first frame.
    m_thermal_sampled = false;
    m_thermal_tick = 0;
    m_thermal_status = 0;
    m_thermal_headroom = 0.0f;
    m_thermal_forecast = 0.0f;
    m_gpu_mhz = read_gpu_mhz();

    // The controller settings (LIGHT, clock maximum) are recorded for fixed runs too, so
    // `splatconv replay` can use them.
    m_controller.reset();
    if (config.controller) {
        m_controller.emplace(controller_config(config.target_fps));
        m_splats.set_min_contribution(m_controller->min_contribution());
    }

    m_bench_active = true;
    vote_frame_rate();
    LOGI(
        "bench start: scene=%s key_bits=%d min_contribution=%.3f target_fps=%.1f %ux%u gpu_mhz=%d "
        "controller=%s light=%.3f (%s) max_gpu_mhz=%d",
        config.scene_name.c_str(), m_splats.key_bits(), m_splats.min_contribution(), config.target_fps,
        m_swapchain.extent().width, m_swapchain.extent().height, m_gpu_mhz, config.controller ? "on" : "off",
        m_light_headroom, m_light_source, m_max_gpu_mhz);
    return Status::success();
}

void AndroidHost::bench_pace() {
    // Fixed schedule on the steady clock. A late frame moves the schedule rather than banking the
    // time, so a slow stretch (thermal throttling) never turns into a catch-up burst afterwards.
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(m_bench_config.target_fps)));
    std::this_thread::sleep_until(m_bench_next_frame);
    m_bench_next_frame = std::max(m_bench_next_frame + period, Clock::now());
}

void AndroidHost::bench_record_frame() {
    const auto now = std::chrono::steady_clock::now();
    const double t_s = std::chrono::duration<double>(now - m_bench_start).count();
    const std::uint8_t thermal_sample = sample_platform(now);  // each column updates every 2 s

    BenchRow row;
    row.t_s = csv3(t_s);
    row.cpu_ms = csv3(std::chrono::duration<double, std::milli>(now - m_bench_last_frame).count());
    m_bench_last_frame = now;
    row.gpu_ms = csv3(m_timer.last_gpu_ms());
    row.cull_ms = static_cast<float>(m_timer.segment_ms(0));
    row.preprocess_ms = static_cast<float>(m_timer.segment_ms(1));
    row.sort_ms = static_cast<float>(m_timer.segment_ms(2));
    row.draw_ms = static_cast<float>(m_timer.segment_ms(3));
    row.visible = m_splats.last_visible_count();
    row.visible_chunks = m_splats.last_visible_chunks();
    row.compute_ran = 1;  // the orbit moves every frame, so the compute chain never skips
    row.thermal_status = m_thermal_status;
    row.headroom = m_thermal_headroom;
    row.gpu_mhz = m_gpu_mhz;
    row.min_contribution = m_splats.min_contribution();
    row.rung = -1;
    row.reason = 0;
    row.forecast = m_thermal_forecast;
    row.thermal_sample = thermal_sample;
    row.drawn = 1;

    if (m_controller) {
        // The row's own values, so a replay of the CSV sees exactly what the controller saw.
        splatcore::QualitySample sample;
        sample.t_s = row.t_s;
        sample.frame_interval_ms = row.cpu_ms;
        sample.gpu_ms = row.gpu_ms;
        if (thermal_sample != 0) sample.gpu_mhz = row.gpu_mhz;
        if (thermal_sample == 1) sample.headroom_forecast = row.forecast;
        if (thermal_sample == 2) sample.headroom_current = row.headroom;
        row.rung = static_cast<std::int8_t>(m_controller->rung());
        const splatcore::QualityDecision decision = m_controller->update(sample);
        row.reason = static_cast<std::uint8_t>(decision.reason);
        if (decision.changed) m_splats.set_min_contribution(m_controller->min_contribution());
        if (decision.reason != splatcore::QualityReason::kNone) {
            LOGI(
                "controller: t=%.1f s %s -> rung %d (c %.2f), forecast %.3f, slope %+.4f/min, clock %.0f MHz",
                t_s, splatcore::reason_name(decision.reason), decision.rung, m_controller->min_contribution(),
                m_controller->forecast(), m_controller->slope_per_min(), m_controller->clock_mhz());
        }
    }
    if (m_bench_rows.size() < 1000000) m_bench_rows.push_back(row);  // ~2.5 h; bounded if left running
}

void AndroidHost::write_csv_controller_header(std::ostream& csv,
                                              const splatcore::QualityControllerConfig& config,
                                              bool on) const {
    std::string ladder;
    for (std::size_t i = 0; i < config.ladder.size(); ++i) {
        char rung[16];
        std::snprintf(rung, sizeof(rung), "%s%g", i > 0 ? "/" : "", static_cast<double>(config.ladder[i]));
        ladder += rung;
    }
    char light[24];  // every digit of the float, so a replay compares against the same threshold
    std::snprintf(light, sizeof(light), "%.9g", static_cast<double>(m_light_headroom));
    csv << "# controller=" << (on ? "on" : "off") << ", ladder=" << ladder << ", light_headroom=" << light
        << ", light_source=" << m_light_source << ", max_gpu_mhz=" << m_max_gpu_mhz
        << ", frame_guard=" << config.bad_windows_to_step << "/" << config.bad_window_span
        << ", forecast_s=" << kForecastSeconds << ", forecast_warmup_s=" << config.forecast_warmup_s
        << ", backoff_window_s=" << config.backoff_window_s
        << ", step_up_current=" << (config.step_up_needs_current ? "on" : "off")
        << ", frame_guard_clock=" << config.frame_guard_clock_fraction << "\n";
}

void AndroidHost::write_csv_columns(std::ostream& csv) {
    // Free text: no '=' here, the CSV readers take key=value pairs from every comment line.
    csv << "# gpu columns trail the frame index by 2 frames in flight; thermal sampled at 1 Hz, "
           "alternating the 30 s forecast and the current headroom (thermal_sample 1 or 2; 3: NaN); "
           "rung: controller rung of the frame (-1: none); reason: decision after it "
           "(0 none, 1 frame_miss, 2 thermal_high, 3 thermal_low, 4 ladder_exhausted); "
           "drawn 0: a thermal tick without a frame (interactive trace; cpu_ms is the frame interval "
           "the controller counted, nan after an idle gap)\n"
        << "frame,t_s,cpu_ms,gpu_ms,cull_ms,preprocess_ms,sort_ms,draw_ms,visible,visible_chunks,"
           "compute_ran,thermal_status,thermal_headroom,gpu_mhz,min_contribution,rung,reason,"
           "thermal_forecast,thermal_sample,drawn\n";
}

void AndroidHost::write_csv_rows(std::ostream& csv, const std::vector<BenchRow>& rows,
                                 std::size_t first_index) {
    char line[320];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const BenchRow& row = rows[i];
        std::snprintf(line, sizeof(line),
                      "%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%.3f,%d,%g,%d,%u,%.3f,%u,%u\n",
                      first_index + i, row.t_s, row.cpu_ms, row.gpu_ms, row.cull_ms, row.preprocess_ms,
                      row.sort_ms, row.draw_ms, row.visible, row.visible_chunks, row.compute_ran,
                      row.thermal_status, row.headroom, row.gpu_mhz,
                      static_cast<double>(row.min_contribution), row.rung, row.reason, row.forecast,
                      row.thermal_sample, row.drawn);
        csv << line;
    }
}

std::string AndroidHost::stop_bench() {
    if (!m_bench_active) return "";
    m_bench_active = false;
    m_has_last_frame = false;  // the interactive controller resumes with an uncounted frame
    vote_frame_rate();
    const char* present_mode = m_swapchain.present_mode() == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
                               : m_swapchain.present_mode() == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                                                             : "fifo";
    const std::string present_mode_used = present_mode;  // read before the vsync-on recreate

    // Restore the interactive settings (vsync on, the renderer settings from before the bench).
    m_splats.set_key_bits(m_interactive_key_bits);
    m_splats.set_min_contribution(m_adaptive ? m_adaptive->min_contribution()
                                             : m_interactive_min_contribution);
    m_splats.set_draw_mode(m_interactive_draw_mode);
    m_splats.set_draw_probe(splatrender::SplatRenderer::DrawProbe::kNone);
    if (m_swapchain_ready) {
        m_swapchain.set_vsync(true);
        m_swapchain.recreate(m_swapchain.extent().width, m_swapchain.extent().height);
    }
    if (m_bench_rows.empty()) return "";

    const std::string tmp_path = m_bench_config.csv_path + ".tmp";
    std::ofstream csv(tmp_path);
    if (!csv) {
        LOGE("bench csv open failed: %s", tmp_path.c_str());
        return "";
    }
    csv << "# pocketsplat-android bench\n"
        << "# device=" << m_context.device_name() << ", " << m_bench_config.meta << "\n"
        << "# scene=" << m_bench_config.scene_name << ", splats=" << m_splats.splat_count()
        << ", sh=" << m_splats.sh_degree() << ", resolution=" << m_swapchain.extent().width << "x"
        << m_swapchain.extent().height << ", key_bits=" << m_bench_config.key_bits
        << ", min_contribution=" << m_bench_config.min_contribution
        << ", chunk_cull=" << (m_splats.chunk_cull() ? "on" : "off") << ", present_mode=" << present_mode_used
        << ", target_fps=" << m_bench_config.target_fps
        << ", draw_mode=" << static_cast<std::uint32_t>(m_bench_draw_mode)
        << ", draw_probe=" << static_cast<std::uint32_t>(m_bench_draw_probe)
        << ", frame_rate_vote=" << (m_bench_config.vote_fps ? m_bench_config.target_fps : 0.0f)
        << ", coverage_strength=" << (m_splats.has_coverage() ? m_splats.coverage_strength() : 0.0f)
        << (m_bench_config.home_swing ? ", orbit=home-swing-30deg-tri/600f" : ", orbit=2pi/600f");
    if (m_bench_config.home_swing) {
        csv << ", home_focus=" << vec3_text(m_home.focus) << ", home_eye=" << vec3_text(m_home.eye)
            << ", home_up=" << vec3_text(m_home.up);
    }
    csv << "\n";
    // Controller settings, written for fixed runs too so that `splatconv replay` can evaluate
    // the policy on them.
    const splatcore::QualityControllerConfig defaults;
    write_csv_controller_header(csv, m_controller ? m_controller->config() : defaults,
                                m_controller.has_value());
    write_csv_columns(csv);
    write_csv_rows(csv, m_bench_rows, 0);
    std::size_t changes = 0;
    for (const BenchRow& row : m_bench_rows) {
        const auto reason = static_cast<splatcore::QualityReason>(row.reason);
        if (reason != splatcore::QualityReason::kNone &&
            reason != splatcore::QualityReason::kLadderExhausted) {
            ++changes;
        }
    }
    csv.close();
    char line[320];
    std::remove(m_bench_config.csv_path.c_str());
    if (std::rename(tmp_path.c_str(), m_bench_config.csv_path.c_str()) != 0) {
        LOGE("bench csv rename failed: %s", m_bench_config.csv_path.c_str());
        return "";
    }

    // Whole-run summary (the cold/sustained split is the benchmark script's job).
    const std::size_t warmup = std::min<std::size_t>(10, m_bench_rows.size() - 1);
    std::vector<float> gpu_sorted;
    double sum_gpu = 0.0, sum_cull = 0.0, sum_pre = 0.0, sum_sort = 0.0, sum_draw = 0.0;
    for (std::size_t i = warmup; i < m_bench_rows.size(); ++i) {
        sum_gpu += m_bench_rows[i].gpu_ms;
        sum_cull += m_bench_rows[i].cull_ms;
        sum_pre += m_bench_rows[i].preprocess_ms;
        sum_sort += m_bench_rows[i].sort_ms;
        sum_draw += m_bench_rows[i].draw_ms;
        gpu_sorted.push_back(m_bench_rows[i].gpu_ms);
    }
    std::sort(gpu_sorted.begin(), gpu_sorted.end());
    const double n = static_cast<double>(gpu_sorted.size());
    const double elapsed_s = m_bench_rows.back().t_s;
    const double fps = elapsed_s > 0.0 ? static_cast<double>(m_bench_rows.size()) / elapsed_s : 0.0;
    std::snprintf(line, sizeof(line),
                  "%zu frames in %.0f s (%.1f fps): gpu %.2f / p50 %.2f / p95 %.2f ms; cull %.2f, pre %.2f, "
                  "sort %.2f, draw %.2f; thermal %u",
                  m_bench_rows.size(), elapsed_s, fps, sum_gpu / n, gpu_sorted[gpu_sorted.size() / 2],
                  gpu_sorted[gpu_sorted.size() * 95 / 100], sum_cull / n, sum_pre / n, sum_sort / n,
                  sum_draw / n, m_bench_rows.back().thermal_status);
    std::string summary = line;
    if (m_controller) {
        std::snprintf(line, sizeof(line), "; controller: %zu changes, final c %.2f", changes,
                      m_controller->min_contribution());
        summary += line;
    }
    m_bench_rows.clear();
    m_bench_rows.shrink_to_fit();
    LOGI("bench csv written: %s", m_bench_config.csv_path.c_str());
    LOGI("bench summary: %s", summary.c_str());
    return summary;
}

void AndroidHost::destroy() {
    if (!m_context_ready) return;
    m_bench_active = false;  // recording (if any) is dropped, not flushed
    stop_trace();            // the trace is flushed: its rows were sampled as they happened
    if (m_thermal != nullptr) {
        AThermal_releaseManager(m_thermal);
        m_thermal = nullptr;
    }
    m_context.wait_idle();
    m_splats.destroy();
    m_renderer_ready = false;
    m_timer.destroy();
    if (m_ring_ready) {
        m_ring.destroy();
        m_ring_ready = false;
    }
    release_surface();
    m_context.destroy();
    m_context_ready = false;
}

}  // namespace pocketsplat
