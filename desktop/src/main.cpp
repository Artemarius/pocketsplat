#include <volk.h>

#define GLFW_INCLUDE_NONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "splatcore/coverage.h"
#include "splatcore/home_view.h"
#include "splatcore/pack.h"
#include "splatcore/scene_load.h"
#include "splatrender/splat_renderer.h"
#include "splatrender/vk_buffer.h"
#include "splatrender/vk_context.h"
#include "splatrender/vk_frame.h"
#include "splatrender/vk_pipeline.h"
#include "splatrender/vk_swapchain.h"
#include "splatrender/vk_timer.h"
#include "triangle_frag.spv.h"
#include "triangle_vert.spv.h"

namespace {

struct Args {
    std::string scene;         // empty = triangle demo
    std::string screenshot;    // PPM path written after the last frame (requires --frames)
    std::string csv;           // per-frame benchmark CSV (implies --bench semantics for output)
    std::string framing_from;  // take the camera framing from this scene (A/B image comparisons)
    std::string gpu;           // --gpu: substring of the GPU name to use (default: discrete first)
    bool list_gpus = false;    // --list-gpus: print every GPU Vulkan reports and exit
    bool validation = false;
    bool bench = false;  // deterministic orbit, vsync off, per-frame stats
    bool chunk_cull = true;
    bool chunk_contribution = true;  // chunk-level contribution bound (with --min-contribution)
    bool hud = true;                 // --no-hud for clean screenshots (golden images)
    bool offscreen = false;          // render into a --width x --height image instead of the window
    std::uint64_t frames = 0;        // 0 = run until the window closes
    int width = 1600;
    int height = 900;
    int sh = 1;                     // SH truncation target for .ply/.spz inputs above it
    int key_bits = 32;              // depth sort key width (16 = faster, quantized)
    float min_contribution = 0.0f;  // contribution cull threshold in pixels (0 = off)
    splatrender::SplatRenderer::DrawMode draw_mode = splatrender::SplatRenderer::kDefaultDrawMode;
    splatrender::SplatRenderer::DrawProbe draw_probe = splatrender::SplatRenderer::DrawProbe::kNone;
    float dist_scale = 1.0f;                  // camera distance multiplier (<1 = zoomed in)
    float fov_deg = 60.0f;                    // vertical FOV; distance auto-compensates to keep framing
    std::optional<float> yaw_deg, pitch_deg;  // initial camera override (degrees)
    float bg[3] = {0.05f, 0.06f, 0.08f};      // clear color (--bg r,g,b for viewer comparisons)
    bool home_view = false;                   // start at splatcore::estimate_home_view (the app's view)
    std::optional<glm::vec3> look_from, look_at, look_up;  // explicit view (RUB), e.g. a training camera
    std::optional<glm::vec2> pick;  // depth pick at this pixel (SplatRenderer::request_pick)
    std::string coverage;           // coverage sidecar (.pscov); default: <scene stem>.pscov beside it
    bool no_coverage = false;
    float coverage_strength = 0.0f;  // coverage fade strength (ADR-0013), 0 = off
    float coverage_full_deg = splatrender::SplatRenderer::kDefaultCoverageFullDeg;
    float coverage_zero_deg = splatrender::SplatRenderer::kDefaultCoverageZeroDeg;
};

// "x,y,z" -> vec3; false on a malformed value.
bool parse_vec3(const char* text, glm::vec3& out) {
    char* cursor = const_cast<char*>(text);
    for (int c = 0; c < 3; ++c) {
        out[c] = std::strtof(cursor, &cursor);
        if (c < 2 && *cursor++ != ',') return false;
    }
    return *cursor == '\0';
}

bool parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--validation") {
            args.validation = true;
        } else if (arg == "--gpu" && i + 1 < argc) {
            args.gpu = argv[++i];
        } else if (arg == "--list-gpus") {
            args.list_gpus = true;
        } else if (arg == "--bench") {
            args.bench = true;
        } else if (arg == "--no-chunk-cull") {
            args.chunk_cull = false;
        } else if (arg == "--no-chunk-contribution") {
            args.chunk_contribution = false;
        } else if (arg == "--no-hud") {
            args.hud = false;
        } else if (arg == "--offscreen") {
            args.offscreen = true;
        } else if (arg == "--csv" && i + 1 < argc) {
            args.csv = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            args.frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--width" && i + 1 < argc) {
            args.width = std::atoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            args.height = std::atoi(argv[++i]);
        } else if (arg == "--sh" && i + 1 < argc) {
            args.sh = std::atoi(argv[++i]);
        } else if (arg == "--key-bits" && i + 1 < argc) {
            args.key_bits = std::atoi(argv[++i]);
        } else if (arg == "--min-contribution" && i + 1 < argc) {
            args.min_contribution = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--draw-mode" && i + 1 < argc) {
            const std::string mode = argv[++i];
            using DrawMode = splatrender::SplatRenderer::DrawMode;
            if (mode == "vertex6") {
                args.draw_mode = DrawMode::kVertex6;
            } else if (mode == "instanced") {
                args.draw_mode = DrawMode::kInstanced4;
            } else if (mode == "indexed") {
                args.draw_mode = DrawMode::kIndexedBatch;
            } else if (mode == "triangle") {
                args.draw_mode = DrawMode::kTriangle;
            } else {
                std::fprintf(stderr, "--draw-mode expects vertex6, instanced, indexed or triangle\n");
                return false;
            }
        } else if (arg == "--draw-probe" && i + 1 < argc) {
            const std::string probe = argv[++i];
            using DrawProbe = splatrender::SplatRenderer::DrawProbe;
            if (probe == "none") {
                args.draw_probe = DrawProbe::kNone;
            } else if (probe == "seq") {
                args.draw_probe = DrawProbe::kSequential;
            } else if (probe == "tiny") {
                args.draw_probe = DrawProbe::kTiny;
            } else if (probe == "clip") {
                args.draw_probe = DrawProbe::kClip;
            } else if (probe == "noblend") {
                args.draw_probe = DrawProbe::kNoBlend;
            } else if (probe == "nofrag") {
                args.draw_probe = DrawProbe::kNoFragment;
            } else if (probe == "copy") {
                args.draw_probe = DrawProbe::kSortedCopy;
            } else {
                std::fprintf(stderr, "--draw-probe expects none, seq, tiny, clip, noblend, nofrag or copy\n");
                return false;
            }
        } else if (arg == "--screenshot" && i + 1 < argc) {
            args.screenshot = argv[++i];
        } else if (arg == "--dist-scale" && i + 1 < argc) {
            args.dist_scale = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--framing-from" && i + 1 < argc) {
            args.framing_from = argv[++i];
        } else if (arg == "--fov-deg" && i + 1 < argc) {
            args.fov_deg = glm::clamp(static_cast<float>(std::atof(argv[++i])), 5.0f, 120.0f);
        } else if (arg == "--bg" && i + 1 < argc) {
            char* cursor = argv[++i];
            for (int channel = 0; channel < 3; ++channel) {
                args.bg[channel] = std::strtof(cursor, &cursor);
                if (channel < 2 && *cursor++ != ',') {
                    std::fprintf(stderr, "--bg expects r,g,b in [0,1]\n");
                    return false;
                }
            }
        } else if (arg == "--yaw-deg" && i + 1 < argc) {
            args.yaw_deg = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--pitch-deg" && i + 1 < argc) {
            args.pitch_deg = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--home-view") {
            args.home_view = true;
        } else if (arg == "--coverage" && i + 1 < argc) {
            args.coverage = argv[++i];
        } else if (arg == "--no-coverage") {
            args.no_coverage = true;
        } else if (arg == "--coverage-strength" && i + 1 < argc) {
            args.coverage_strength = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 1.0f);
        } else if (arg == "--coverage-angles" && i + 1 < argc) {
            glm::vec3 v;  // full,zero degrees, the third value ignored
            const std::string text = std::string(argv[++i]) + ",0";
            if (!parse_vec3(text.c_str(), v)) {
                std::fprintf(stderr, "--coverage-angles expects full,zero (degrees)\n");
                return false;
            }
            args.coverage_full_deg = v.x;
            args.coverage_zero_deg = v.y;
        } else if (arg == "--pick" && i + 1 < argc) {
            glm::vec3 v;  // x,y pixels, the third value ignored
            const std::string text = std::string(argv[++i]) + ",0";
            if (!parse_vec3(text.c_str(), v)) {
                std::fprintf(stderr, "--pick expects x,y (pixels)\n");
                return false;
            }
            args.pick = glm::vec2(v.x, v.y);
        } else if ((arg == "--look-from" || arg == "--look-at" || arg == "--up") && i + 1 < argc) {
            glm::vec3 v;
            if (!parse_vec3(argv[++i], v)) {
                std::fprintf(stderr, "%s expects x,y,z\n", arg.c_str());
                return false;
            }
            (arg == "--look-from" ? args.look_from : arg == "--look-at" ? args.look_at : args.look_up) = v;
        } else if (!arg.empty() && arg[0] != '-' && args.scene.empty()) {
            args.scene = arg;
        } else {
            std::fprintf(
                stderr,
                "usage: pocketsplat-desktop [scene.ply|scene.spz|scene.pspk] [--validation]\n"
                "       [--gpu NAME] [--list-gpus]\n"
                "       [--frames N] [--width W] [--height H] [--sh 0|1] [--screenshot out.ppm]\n"
                "       [--yaw-deg A] [--pitch-deg A] [--key-bits 16|32] [--no-chunk-cull]\n"
                "       [--min-contribution PX] [--dist-scale F] [--fov-deg A] [--bench]\n"
                "       [--csv out.csv] [--no-hud] [--offscreen] [--bg r,g,b] [--framing-from scene]\n"
                "       [--no-chunk-contribution] [--draw-mode vertex6|instanced|indexed|triangle]\n"
                "       [--draw-probe none|seq|tiny|clip|noblend|nofrag|copy]\n"
                "       [--home-view] [--look-from x,y,z --look-at x,y,z [--up x,y,z]] [--pick x,y]\n"
                "       [--coverage file.pscov | --no-coverage] [--coverage-strength S]\n"
                "       [--coverage-angles full,zero]\n");
            return false;
        }
    }
    return true;
}

// Loads a scene as a GpuPack via the shared host load path (splatcore::load_scene_pack).
std::optional<splatcore::GpuPack> load_scene(const Args& args) {
    splatcore::Result<splatcore::GpuPack> pack = splatcore::load_scene_pack(args.scene, args.sh);
    if (!pack) {
        std::fprintf(stderr, "%s\n", pack.error().c_str());
        return std::nullopt;
    }
    return std::move(pack.value());
}

// Coverage masks for the coverage fade (ADR-0013): --coverage, else <scene stem>.pscov next to the
// scene when present. A mask file that does not belong to the scene is an error.
std::optional<std::vector<std::uint32_t>> load_coverage(const Args& args, const splatcore::GpuPack& pack,
                                                        bool& failed) {
    failed = false;
    if (args.no_coverage) return std::nullopt;
    std::filesystem::path path = args.coverage;
    if (path.empty()) {
        path = std::filesystem::path(args.scene).replace_extension(".pscov");
        if (!std::filesystem::exists(path)) return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) {
        std::fprintf(stderr, "cannot read coverage file %s\n", path.string().c_str());
        failed = true;
        return std::nullopt;
    }
    splatcore::Result<std::vector<std::uint32_t>> masks = splatcore::parse_coverage(bytes, pack);
    if (!masks) {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), masks.error().c_str());
        failed = true;
        return std::nullopt;
    }
    std::printf("coverage: %s\n", path.string().c_str());
    return std::move(masks.value());
}

// Orbit camera around a target point, RUB world space.
struct OrbitCamera {
    glm::vec3 target{0.0f};
    float distance = 5.0f;
    float yaw = 0.6f;    // radians around +y
    float pitch = 0.3f;  // radians above the horizon
    float fov_y = glm::radians(60.0f);
    // A fixed look-at view (--home-view, --look-from) until the mouse first moves the camera.
    std::optional<glm::vec3> fixed_eye;
    glm::vec3 fixed_up{0.0f, 1.0f, 0.0f};

    glm::vec3 position() const {
        if (fixed_eye) return *fixed_eye;
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        return target + distance * glm::vec3(cp * sy, sp, cp * cy);
    }
    glm::mat4 view() const {
        return glm::lookAt(position(), target, fixed_eye ? fixed_up : glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

double g_scroll = 0.0;
void scroll_callback(GLFWwindow*, double, double y) {
    g_scroll += y;
}

void update_camera(GLFWwindow* window, OrbitCamera& camera, float scene_diag) {
    static double last_x = 0.0, last_y = 0.0;
    static bool dragging = false;
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    const bool want_mouse = ImGui::GetIO().WantCaptureMouse;
    const bool lmb = !want_mouse && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool rmb = !want_mouse && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const float dx = static_cast<float>(x - last_x);
    const float dy = static_cast<float>(y - last_y);
    if ((dragging && (lmb || rmb)) || (!want_mouse && g_scroll != 0.0)) {
        camera.fixed_eye.reset();  // back to the orbit (yaw/pitch/distance around the target)
    }
    if (dragging && lmb) {
        camera.yaw -= dx * 0.005f;
        camera.pitch = glm::clamp(camera.pitch + dy * 0.005f, -1.55f, 1.55f);
    } else if (dragging && rmb) {
        // Pan in the camera plane.
        const glm::mat4 view = camera.view();
        const glm::vec3 right{view[0][0], view[1][0], view[2][0]};
        const glm::vec3 up{view[0][1], view[1][1], view[2][1]};
        camera.target += (-right * dx + up * dy) * (0.001f * camera.distance);
    }
    dragging = lmb || rmb;
    last_x = x;
    last_y = y;
    if (!want_mouse && g_scroll != 0.0) {
        camera.distance = glm::clamp(camera.distance * std::pow(0.9f, static_cast<float>(g_scroll)),
                                     0.01f * scene_diag, 10.0f * scene_diag);
    }
    g_scroll = 0.0;
}

splatrender::SplatCameraParams camera_params(const OrbitCamera& camera, int width, int height) {
    splatrender::SplatCameraParams params;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const glm::mat4 view = camera.view();
    glm::mat4 proj = glm::perspective(camera.fov_y, aspect, 0.05f, 1000.0f);
    proj[1][1] *= -1.0f;  // Vulkan NDC y down - handled in the projection, never in data
    const glm::mat4 proj_view = proj * view;
    std::memcpy(params.view, glm::value_ptr(view), sizeof(params.view));
    std::memcpy(params.proj_view, glm::value_ptr(proj_view), sizeof(params.proj_view));
    const glm::vec3 pos = camera.position();
    std::memcpy(params.cam_pos, glm::value_ptr(pos), sizeof(params.cam_pos));
    params.focal_y = 0.5f * static_cast<float>(height) / std::tan(0.5f * camera.fov_y);
    params.focal_x = params.focal_y;
    params.viewport_w = static_cast<float>(width);
    params.viewport_h = static_cast<float>(height);
    return params;
}

// Swapchain-image layout transition via synchronization2.
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

// One frame of benchmark data. GPU columns come from the double-buffered timer, so they describe
// the frame submitted kFramesInFlight earlier - irrelevant for steady-state averages.
struct BenchRow {
    std::uint64_t frame = 0;
    double cpu_ms = 0.0, gpu_ms = 0.0;
    double cull_ms = 0.0, preprocess_ms = 0.0, sort_ms = 0.0, draw_ms = 0.0;
    std::uint32_t visible = 0, visible_chunks = 0;
    bool compute_ran = false;
};

// --offscreen color target: any size (a window is capped by the monitor - the phone protocol's
// 864x1872 does not fit a 1080p laptop panel), never presented. Same format as the swapchain, so
// pipelines and screenshots are unchanged.
struct OffscreenTarget {
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};

    bool init(splatrender::Context& context, std::uint32_t width, std::uint32_t height, VkFormat format) {
        allocator = context.allocator();
        device = context.device();
        extent = {width, height};
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(allocator, &image_info, &alloc_info, &image, &allocation, nullptr) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(device, &view_info, nullptr, &view) == VK_SUCCESS;
    }
    void destroy() {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        if (image != VK_NULL_HANDLE) vmaDestroyImage(allocator, image, allocation);
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
    }
};

std::string basename_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// BGRA8 pixels -> binary PPM (no dependencies; convert externally if PNG is needed).
void write_ppm(const std::string& path, const std::uint8_t* bgra, std::uint32_t width, std::uint32_t height,
               VkFormat format) {
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << " " << height << "\n255\n";
    const bool bgr = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<std::uint8_t> row(3 * width);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* src = bgra + std::size_t{4} * width * y;
        for (std::uint32_t x = 0; x < width; ++x) {
            row[3 * x + 0] = src[4 * x + (bgr ? 2 : 0)];
            row[3 * x + 1] = src[4 * x + 1];
            row[3 * x + 2] = src[4 * x + (bgr ? 0 : 2)];
        }
        file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    std::printf("screenshot written: %s (%ux%u)\n", path.c_str(), width, height);
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;
    if (args.sh != 0 && args.sh != 1) {
        std::fprintf(stderr, "--sh must be 0 or 1\n");
        return 1;
    }
    if (args.bench && args.frames == 0) args.frames = 600;
    if (args.offscreen) args.hud = false;  // the HUD needs window input

    std::optional<splatcore::GpuPack> pack;
    if (!args.scene.empty()) {
        pack = load_scene(args);
        if (!pack) return 1;
        std::printf("scene: %s, %u splats, SH %u\n", args.scene.c_str(), pack->header.count,
                    pack->header.sh_degree);
    }
    std::optional<std::vector<std::uint32_t>> coverage;
    if (pack) {
        bool coverage_failed = false;
        coverage = load_coverage(args, *pack, coverage_failed);
        if (coverage_failed) return 1;
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "GLFW init failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if (args.offscreen) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(args.width, args.height, "PocketSplat", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "window creation failed\n");
        glfwTerminate();
        return 1;
    }
    glfwSetScrollCallback(window, scroll_callback);

    splatrender::Context context;
    splatrender::ContextCreateInfo context_info;
    context_info.app_name = "pocketsplat-desktop";
    context_info.enable_validation = args.validation;
    context_info.request_dynamic_rendering_extension = true;  // ImGui backend checks the string
    context_info.preferred_device = args.gpu.c_str();
    const splatcore::Status context_status = context.init(context_info);
    if (args.list_gpus) {
        std::printf("%s", context.devices_found().c_str());
        if (context_status.ok()) std::printf("selected: %s\n", context.device_name());
    }
    if (!context_status.ok()) {
        std::fprintf(stderr, "pocketsplat-desktop: %s\n", context_status.error().c_str());
        return 1;
    }
    if (args.list_gpus) return 0;
    std::printf("device: %s%s\n", context.device_name(), args.validation ? " (validation on)" : "");

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(context.instance(), window, nullptr, &surface) != VK_SUCCESS) {
        std::fprintf(stderr, "surface creation failed\n");
        return 1;
    }

    int fb_width = 0, fb_height = 0;
    glfwGetFramebufferSize(window, &fb_width, &fb_height);
    splatrender::Swapchain swapchain;
    splatrender::FrameRing ring;
    splatrender::GpuTimer timer;
    splatrender::Pipeline triangle;
    splatrender::SplatRenderer splats;
    {
        splatcore::Status s = swapchain.init(context, surface, static_cast<std::uint32_t>(fb_width),
                                             static_cast<std::uint32_t>(fb_height), !args.bench);
        if (s.ok()) s = ring.init(context);
        if (s.ok()) s = timer.init(context);
        if (s.ok() && !pack) {
            splatrender::GraphicsPipelineCreateInfo pipeline_info;
            pipeline_info.vertex_spirv = std::span(k_triangle_vert_spv);
            pipeline_info.fragment_spirv = std::span(k_triangle_frag_spv);
            pipeline_info.color_format = swapchain.format();
            s = triangle.init_graphics(context.device(), pipeline_info);
        }
        if (s.ok() && pack) {
            s = splats.init(context, *pack, swapchain.format(), coverage ? &*coverage : nullptr);
            splats.set_coverage_angles(args.coverage_full_deg, args.coverage_zero_deg);
            splats.set_coverage_strength(args.coverage_strength);
            splats.set_key_bits(args.key_bits);
            splats.set_chunk_cull(args.chunk_cull);
            splats.set_chunk_contribution_cull(args.chunk_contribution);
            splats.set_min_contribution(args.min_contribution);
            splats.set_draw_mode(args.draw_mode);
            if (s.ok()) s = splats.set_draw_probe(args.draw_probe);
        }
        if (!s.ok()) {
            std::fprintf(stderr, "pocketsplat-desktop: %s\n", s.error().c_str());
            return 1;
        }
    }

    OrbitCamera camera;
    float scene_diag = 1.0f;
    if (pack) {
        // Frame the bulk of the content, not the raw AABB - captures often carry background
        // splats hundreds of units out (per-axis 5th-95th percentile box).
        splatcore::SceneFraming framing = splatcore::robust_framing(*pack);
        if (!args.framing_from.empty()) {
            // Same camera for two variants of a scene (e.g. cleaning on/off): their own percentile
            // boxes differ slightly, which would misalign any image comparison.
            splatcore::Result<splatcore::GpuPack> other =
                splatcore::load_scene_pack(args.framing_from, args.sh);
            if (!other) {
                std::fprintf(stderr, "pocketsplat-desktop: --framing-from: %s\n", other.error().c_str());
                return 1;
            }
            framing = splatcore::robust_framing(other.value());
        }
        camera.target = glm::vec3(framing.target[0], framing.target[1], framing.target[2]);
        scene_diag = framing.diagonal;
        // Distance compensates the FOV so --fov-deg changes the lens, not the framing
        // (normalized to the 60-degree default: narrower FOV backs the camera up).
        camera.fov_y = glm::radians(args.fov_deg);
        const float fov_comp = std::tan(glm::radians(30.0f)) / std::tan(0.5f * camera.fov_y);
        camera.distance = 1.2f * scene_diag * args.dist_scale * fov_comp;
        if (args.yaw_deg) camera.yaw = glm::radians(*args.yaw_deg);
        if (args.pitch_deg) camera.pitch = glm::radians(*args.pitch_deg);
        if (args.home_view) {
            const splatcore::HomeView home = splatcore::estimate_home_view(*pack);
            camera.target = glm::vec3(home.focus[0], home.focus[1], home.focus[2]);
            camera.fixed_eye = glm::vec3(home.eye[0], home.eye[1], home.eye[2]);
            camera.fixed_up = glm::vec3(home.up[0], home.up[1], home.up[2]);
            camera.distance = home.distance;
            std::printf(
                "home view: focus %.4f,%.4f,%.4f eye %.4f,%.4f,%.4f up %.4f,%.4f,%.4f distance %.4f plane "
                "%s\n",
                home.focus[0], home.focus[1], home.focus[2], home.eye[0], home.eye[1], home.eye[2],
                home.up[0], home.up[1], home.up[2], home.distance, home.plane_found ? "yes" : "no");
        }
        if (args.look_from && args.look_at) {
            camera.target = *args.look_at;
            camera.fixed_eye = *args.look_from;
            camera.fixed_up = args.look_up.value_or(glm::vec3(0.0f, 1.0f, 0.0f));
            camera.distance = glm::length(*args.look_from - *args.look_at);
        }
        pack->positions.clear();  // renderer holds its own copies; free the big arrays
        pack->scale_rot.clear();
        pack->color.clear();
        pack->sh1.clear();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForVulkan(window, true);
    VkFormat color_format = swapchain.format();
    ImGui_ImplVulkan_InitInfo imgui_info{};
    imgui_info.ApiVersion = VK_API_VERSION_1_3;
    imgui_info.Instance = context.instance();
    imgui_info.PhysicalDevice = context.physical_device();
    imgui_info.Device = context.device();
    imgui_info.QueueFamily = context.queue_family();
    imgui_info.Queue = context.queue();
    imgui_info.DescriptorPoolSize = 8;  // backend creates its own pool
    imgui_info.MinImageCount = 2;
    imgui_info.ImageCount = swapchain.image_count();
    imgui_info.UseDynamicRendering = true;
    // Since ImGui 1.92 the per-viewport pipeline settings live in PipelineInfoMain.
    imgui_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    imgui_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    imgui_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    imgui_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    if (!ImGui_ImplVulkan_Init(&imgui_info)) {
        std::fprintf(stderr, "ImGui Vulkan backend init failed\n");
        return 1;
    }

    splatrender::Buffer screenshot_buffer;
    OffscreenTarget offscreen;
    if (args.offscreen && !offscreen.init(context, static_cast<std::uint32_t>(args.width),
                                          static_cast<std::uint32_t>(args.height), swapchain.format())) {
        std::fprintf(stderr, "pocketsplat-desktop: offscreen target creation failed\n");
        return 1;
    }
    VkExtent2D target_extent = args.offscreen ? offscreen.extent : swapchain.extent();

    std::uint64_t frame_count = 0;
    double cpu_ms_accum = 0.0;
    double gpu_ms_accum = 0.0;
    std::uint64_t timed_frames = 0;
    bool needs_recreate = false;
    bool screenshot_pending = false;
    bool compute_ran = false;
    std::vector<BenchRow> rows;
    if (args.bench || !args.csv.empty()) rows.reserve(args.frames);

    while (!glfwWindowShouldClose(window) && (args.frames == 0 || frame_count < args.frames)) {
        const auto cpu_start = std::chrono::steady_clock::now();
        glfwPollEvents();
        if (args.offscreen) {
            fb_width = static_cast<int>(offscreen.extent.width);
            fb_height = static_cast<int>(offscreen.extent.height);
        } else {
            glfwGetFramebufferSize(window, &fb_width, &fb_height);
            if (fb_width == 0 || fb_height == 0) {
                glfwWaitEvents();  // minimized
                continue;
            }
            if (needs_recreate || fb_width != static_cast<int>(swapchain.extent().width) ||
                fb_height != static_cast<int>(swapchain.extent().height)) {
                if (splatcore::Status s = swapchain.recreate(static_cast<std::uint32_t>(fb_width),
                                                             static_cast<std::uint32_t>(fb_height));
                    !s.ok()) {
                    std::fprintf(stderr, "pocketsplat-desktop: %s\n", s.error().c_str());
                    break;
                }
                needs_recreate = false;
            }
        }

        if (pack) {
            if (args.bench) {
                // Deterministic full orbit over the run; also keeps the compute chain dirty so
                // skip-when-static never idles the passes being measured.
                camera.yaw += 6.2831853f / static_cast<float>(args.frames);
            } else {
                update_camera(window, camera, scene_diag);
            }
        }

        VkCommandBuffer cmd = ring.begin();
        if (cmd == VK_NULL_HANDLE) break;
        std::uint32_t image_index = 0;
        if (!args.offscreen) {
            const VkResult acquire_result = swapchain.acquire(ring.image_available(), &image_index);
            if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
                needs_recreate = true;
                vkEndCommandBuffer(cmd);  // abandon this frame's recording
                continue;
            }
            if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) break;
        }
        const VkImage target_image = args.offscreen ? offscreen.image : swapchain.image(image_index);
        const VkImageView target_view = args.offscreen ? offscreen.view : swapchain.view(image_index);
        target_extent = args.offscreen ? offscreen.extent : swapchain.extent();

        timer.begin_frame(cmd, ring.slot());
        if (pack) {
            splats.prepare_frame(ring.slot(), camera_params(camera, fb_width, fb_height));
            if (args.pick) {
                // --pick: request on the first frame, report once the result is back.
                if (frame_count == 0) splats.request_pick(args.pick->x, args.pick->y);
                float world[3];
                const auto result = splats.take_pick(world);
                if (result == splatrender::SplatRenderer::PickResult::kHit) {
                    std::printf("pick %.1f,%.1f: hit %.5f,%.5f,%.5f (distance %.4f from the camera)\n",
                                args.pick->x, args.pick->y, world[0], world[1], world[2],
                                glm::length(glm::vec3(world[0], world[1], world[2]) - camera.position()));
                } else if (result == splatrender::SplatRenderer::PickResult::kMiss) {
                    std::printf("pick %.1f,%.1f: miss\n", args.pick->x, args.pick->y);
                }
            }
            // Stamps two boundaries inside: after the chunk cull and after the preprocess.
            compute_ran = splats.record_compute(cmd, ring.slot(), &timer);
        }
        timer.stamp(cmd, ring.slot());  // boundary: prepare+sort / draw+ui

        // A swapchain image is ordered after its last use by the acquire semaphore; the single
        // offscreen image is shared by both frames in flight, so wait for the previous frame's
        // writes (and screenshot copy) explicitly.
        image_barrier(
            cmd, target_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            args.offscreen ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT
                           : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            args.offscreen ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = target_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{args.bg[0], args.bg[1], args.bg[2], 1.0f}};
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, target_extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color_attachment;
        vkCmdBeginRendering(cmd, &rendering);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(fb_width), static_cast<float>(fb_height),
                            0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, target_extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (pack) {
            splats.record_draw(cmd, ring.slot());
        } else {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, triangle.handle());
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        if (args.hud) {
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("PocketSplat", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("%s", context.device_name());
            if (pack) {
                ImGui::Text("splats %u (SH %u), visible %u, %d-bit keys, min contribution %.2f px",
                            splats.splat_count(), splats.sh_degree(), splats.last_visible_count(),
                            splats.key_bits(), splats.min_contribution());
                ImGui::Text("chunks %u, visible %u (cull %s), compute %s", splats.chunk_count(),
                            splats.last_visible_chunks(), splats.chunk_cull() ? "on" : "off",
                            compute_ran ? "ran" : "skipped");
                ImGui::Text("gpu: cull %.2f, preprocess %.2f, sort %.2f, draw+ui %.2f ms",
                            timer.segment_ms(0), timer.segment_ms(1), timer.segment_ms(2),
                            timer.segment_ms(3));
                if (splats.has_coverage()) {
                    float strength = splats.coverage_strength();
                    if (ImGui::SliderFloat("coverage fade", &strength, 0.0f, 1.0f, "%.2f")) {
                        splats.set_coverage_strength(strength);
                    }
                }
            }
            ImGui::Text("cpu %.2f ms  gpu %.2f ms",
                        timed_frames > 0 ? cpu_ms_accum / static_cast<double>(timed_frames) : 0.0,
                        timer.last_gpu_ms());
            ImGui::Text("validation messages: %llu",
                        static_cast<unsigned long long>(context.validation_message_count()));
            ImGui::End();
            ImGui::Render();
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        }

        vkCmdEndRendering(cmd);

        const bool last_frame = args.frames > 0 && frame_count + 1 == args.frames;
        if (last_frame && !args.screenshot.empty()) {
            const VkDeviceSize bytes = VkDeviceSize{4} * fb_width * fb_height;
            if (screenshot_buffer
                    .init(context.allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
                    .ok()) {
                image_barrier(cmd, target_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                              VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {static_cast<std::uint32_t>(fb_width),
                                      static_cast<std::uint32_t>(fb_height), 1};
                vkCmdCopyImageToBuffer(cmd, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       screenshot_buffer.handle(), 1, &region);
                if (!args.offscreen) {
                    image_barrier(cmd, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT,
                                  VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
                }
                screenshot_pending = true;
            }
        } else if (!args.offscreen) {
            image_barrier(cmd, target_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        }
        timer.end_frame(cmd, ring.slot());

        if (splatcore::Status s =
                ring.submit(args.offscreen ? VK_NULL_HANDLE : swapchain.render_finished(image_index));
            !s.ok()) {
            std::fprintf(stderr, "pocketsplat-desktop: %s\n", s.error().c_str());
            break;
        }
        if (!args.offscreen) {
            const VkResult present_result = swapchain.present(context.queue(), image_index);
            if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
                needs_recreate = true;
            } else if (present_result != VK_SUCCESS) {
                break;
            }
        }

        ++frame_count;
        const auto cpu_end = std::chrono::steady_clock::now();
        const double cpu_ms = std::chrono::duration<double, std::milli>(cpu_end - cpu_start).count();
        cpu_ms_accum += cpu_ms;
        gpu_ms_accum += timer.last_gpu_ms();
        ++timed_frames;
        if (pack && (args.bench || !args.csv.empty())) {
            BenchRow row;
            row.frame = frame_count - 1;
            row.cpu_ms = cpu_ms;
            row.gpu_ms = timer.last_gpu_ms();
            row.cull_ms = timer.segment_ms(0);
            row.preprocess_ms = timer.segment_ms(1);
            row.sort_ms = timer.segment_ms(2);
            row.draw_ms = timer.segment_ms(3);
            row.visible = splats.last_visible_count();
            row.visible_chunks = splats.last_visible_chunks();
            row.compute_ran = compute_ran;
            rows.push_back(row);
        }
    }

    context.wait_idle();
    if (screenshot_pending) {
        write_ppm(args.screenshot, static_cast<const std::uint8_t*>(screenshot_buffer.mapped()),
                  target_extent.width, target_extent.height, swapchain.format());
    }

    // Benchmark output: per-frame CSV plus a steady-state summary (warmup frames excluded).
    if (pack && !rows.empty()) {
        const std::size_t warmup = std::min<std::size_t>(10, rows.size() - 1);
        double sum_gpu = 0.0, sum_cull = 0.0, sum_pre = 0.0, sum_sort = 0.0, sum_draw = 0.0, sum_cpu = 0.0;
        std::vector<double> gpu_sorted;
        for (std::size_t i = warmup; i < rows.size(); ++i) {
            sum_gpu += rows[i].gpu_ms;
            sum_cull += rows[i].cull_ms;
            sum_pre += rows[i].preprocess_ms;
            sum_sort += rows[i].sort_ms;
            sum_draw += rows[i].draw_ms;
            sum_cpu += rows[i].cpu_ms;
            gpu_sorted.push_back(rows[i].gpu_ms);
        }
        const double n = static_cast<double>(gpu_sorted.size());
        std::sort(gpu_sorted.begin(), gpu_sorted.end());
        const double p50 = gpu_sorted[gpu_sorted.size() / 2];
        const double p95 = gpu_sorted[gpu_sorted.size() * 95 / 100];
        std::printf(
            "bench (%zu frames after %zu warmup): gpu avg %.2f / p50 %.2f / p95 %.2f ms\n"
            "  cull %.3f, preprocess %.3f, prepare+sort %.3f, draw+ui %.3f ms avg; cpu %.2f ms\n",
            gpu_sorted.size(), warmup, sum_gpu / n, p50, p95, sum_cull / n, sum_pre / n, sum_sort / n,
            sum_draw / n, sum_cpu / n);
        if (!args.csv.empty()) {
            std::ofstream csv(args.csv);
            csv << "# pocketsplat-desktop bench\n"
                << "# device=" << context.device_name() << ", scene=" << basename_of(args.scene)
                << ", splats=" << splats.splat_count() << ", sh=" << splats.sh_degree()
                << ", resolution=" << target_extent.width << "x" << target_extent.height
                << ", key_bits=" << splats.key_bits() << ", min_contribution=" << splats.min_contribution()
                << ", chunk_cull=" << (splats.chunk_cull() ? "on" : "off")
                << ", draw_mode=" << static_cast<std::uint32_t>(splats.draw_mode())
                << ", draw_probe=" << static_cast<std::uint32_t>(splats.draw_probe()) << ", vsync="
                << (args.offscreen ? "none (offscreen)"
                    : args.bench   ? "off"
                                   : "on")
                << "\n"
                << "# gpu columns trail the frame index by 2 frames in flight\n"
                << "frame,cpu_ms,gpu_ms,cull_ms,preprocess_ms,sort_ms,draw_ms,visible,visible_chunks,"
                   "compute_ran\n";
            char line[256];
            for (const BenchRow& row : rows) {
                std::snprintf(line, sizeof(line), "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%d\n",
                              static_cast<unsigned long long>(row.frame), row.cpu_ms, row.gpu_ms, row.cull_ms,
                              row.preprocess_ms, row.sort_ms, row.draw_ms, row.visible, row.visible_chunks,
                              row.compute_ran ? 1 : 0);
                csv << line;
            }
            std::printf("bench csv written: %s (%zu rows)\n", args.csv.c_str(), rows.size());
        }
    }

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    screenshot_buffer.destroy();
    offscreen.destroy();
    splats.destroy();
    triangle.destroy();
    timer.destroy();
    ring.destroy();
    swapchain.destroy();
    vkDestroySurfaceKHR(context.instance(), surface, nullptr);

    const std::uint64_t validation_messages = context.validation_message_count();
    std::printf("frames: %llu, avg cpu %.2f ms, avg gpu %.2f ms, validation messages: %llu\n",
                static_cast<unsigned long long>(frame_count),
                timed_frames > 0 ? cpu_ms_accum / static_cast<double>(timed_frames) : 0.0,
                timed_frames > 0 ? gpu_ms_accum / static_cast<double>(timed_frames) : 0.0,
                static_cast<unsigned long long>(validation_messages));

    context.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return validation_messages > 0 ? 2 : 0;
}
