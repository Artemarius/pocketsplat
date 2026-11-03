// pocketsplat-viewer: the end-user Windows viewer. Same renderer, import pipeline and navigation
// as the Android app (splatrender, splatcore::import_scene, the trackball of ADR-0012, the home view
// of ADR-0014): open a .ply/.spz/.pspk (command line, drag and drop, file dialog), orbit, pan, zoom,
// double-click a new pivot, Home. Frames are drawn on demand: an idle window does not use the GPU.
// The development host with the benchmark and image-test flags stays pocketsplat-desktop
// (main.cpp); this program has no console.

#include <volk.h>

#define GLFW_INCLUDE_NONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
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
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "platform.h"
#include "splatcore/coverage.h"
#include "splatcore/home_view.h"
#include "splatcore/pack.h"
#include "splatcore/scene_import.h"
#include "splatcore/scene_load.h"
#include "splatrender/splat_renderer.h"
#include "splatrender/vk_context.h"
#include "splatrender/vk_frame.h"
#include "splatrender/vk_swapchain.h"
#include "splatrender/vk_timer.h"
#include "trackball.h"

namespace fs = std::filesystem;
using pocketsplat::Trackball;
namespace platform = pocketsplat::platform;

namespace {

constexpr const char* kAppTitle = "PocketSplat";
constexpr float kFovLongSide = 1.0471976f;  // 60 degrees across the window's long side, as on the phone
constexpr int kSettleFrames = 3;            // frames in flight (2) + 1: timers and counts resolve
constexpr float kBackground[3] = {0.55f, 0.58f, 0.62f};  // the app's light gray (hides capture fuzz)
// The phone's interactive defaults (ADR-0008): >= 43.5 dB against exact rendering.
constexpr int kFastKeyBits = 16;
constexpr float kFastMinContribution = 0.1f;
// Imported .ply/.spz scenes are cached as packs; bump when the import pipeline's output changes.
constexpr int kImportCacheVersion = 1;
// The import's float expansion is ~240 B per splat at SH 3: refuse captures beyond ~3.8 GB of it.
constexpr std::uint64_t kRefuseAbove = 16'000'000;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string with_thousands(std::uint64_t value) {
    std::string digits = std::to_string(value), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

bool is_scene_file(const fs::path& path) {
    const std::string ext = lower(platform::utf8_from_path(path.extension()));
    return ext == ".ply" || ext == ".spz" || ext == ".pspk";
}

// Cache file for an imported scene: <stem>-<hash of path, size, mtime, cache version>.pspk.
fs::path cache_path_for(const fs::path& source, const fs::path& cache_dir) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(source, ec);
    const std::string key = platform::utf8_from_path(ec ? source : absolute) + "|" +
                            std::to_string(fs::file_size(source, ec)) + "|" +
                            std::to_string(fs::last_write_time(source, ec).time_since_epoch().count()) +
                            "|v" + std::to_string(kImportCacheVersion);
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : key) hash = (hash ^ c) * 1099511628211ull;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return cache_dir / (platform::utf8_from_path(source.stem()) + "-" + hex + ".pspk");
}

// Everything the GPU upload needs, prepared off the UI thread.
struct LoadedScene {
    splatcore::GpuPack pack;
    splatcore::HomeView home{};
    splatcore::SceneFraming framing{};
    std::optional<std::vector<std::uint32_t>> coverage;
    std::string name;
    std::string note;  // what the import did, for the HUD
};

// One background load at a time: import (.ply/.spz, cached) or read (.pspk), then the home view.
class SceneLoader {
public:
    ~SceneLoader() { join(); }

    void start(const fs::path& source, const fs::path& cache_dir, std::size_t max_splats) {
        join();
        m_done = false;
        m_running = true;
        m_stage = "Opening";
        m_thread = std::thread([this, source, cache_dir, max_splats] {
            splatcore::Result<LoadedScene> result = load(source, cache_dir, max_splats);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_result.emplace(std::move(result));
            }
            m_running = false;
            m_done = true;
        });
    }

    bool running() const { return m_running; }
    std::string stage() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stage;
    }
    // The finished load, once; nullopt while running or after it was taken.
    std::optional<splatcore::Result<LoadedScene>> take() {
        if (!m_done) return std::nullopt;
        join();
        std::lock_guard<std::mutex> lock(m_mutex);
        std::optional<splatcore::Result<LoadedScene>> out = std::move(m_result);
        m_result.reset();
        m_done = false;
        return out;
    }

private:
    void set_stage(const char* stage) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stage = stage;
    }
    void join() {
        if (m_thread.joinable()) m_thread.join();
    }

    splatcore::Result<LoadedScene> load(const fs::path& source, const fs::path& cache_dir,
                                        std::size_t max_splats) {
        using Out = splatcore::Result<LoadedScene>;
        LoadedScene scene;
        scene.name = platform::utf8_from_path(source.filename());
        fs::path pack_path = source;
        if (lower(platform::utf8_from_path(source.extension())) != ".pspk") {
            pack_path = cache_path_for(source, cache_dir);
            std::error_code ec;
            if (fs::exists(pack_path, ec)) {
                scene.note = "imported earlier (cached pack)";
            } else {
                splatcore::ImportOptions options;
                options.sh_target = 1;
                options.max_splats = max_splats;
                options.refuse_above = kRefuseAbove;
                options.progress = [this](splatcore::ImportStage stage) {
                    static const char* kNames[] = {"Reading", "Cleaning", "Removing floaters", "Packing",
                                                   "Writing"};
                    set_stage(kNames[static_cast<int>(stage)]);
                    return true;
                };
                splatcore::Result<splatcore::ImportReport> report =
                    splatcore::import_scene(source, pack_path, options);
                if (!report) return Out::failure(report.error());
                scene.note = "imported " + with_thousands(report.value().input_count) + " -> " +
                             with_thousands(report.value().count) + " splats (cleaned, SH " +
                             std::to_string(report.value().sh_degree) + ")";
            }
        }
        set_stage("Loading");
        splatcore::Result<splatcore::GpuPack> pack = splatcore::load_scene_pack(pack_path, 1);
        if (!pack) return Out::failure(pack.error());
        scene.pack = std::move(pack.value());
        // Coverage fade sidecar (ADR-0013): <stem>.pscov next to a .pspk.
        if (pack_path == source) {
            fs::path cov = source;
            cov.replace_extension(".pscov");
            std::ifstream file(cov, std::ios::binary);
            if (file) {
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                                      std::istreambuf_iterator<char>());
                splatcore::Result<std::vector<std::uint32_t>> masks =
                    splatcore::parse_coverage(bytes, scene.pack);
                if (masks) scene.coverage = std::move(masks.value());
            }
        }
        set_stage("Finding the home view");
        scene.home = splatcore::estimate_home_view(scene.pack);
        scene.framing = splatcore::robust_framing(scene.pack);
        return Out::success(std::move(scene));
    }

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_done{false};
    mutable std::mutex m_mutex;
    std::string m_stage;
    std::optional<splatcore::Result<LoadedScene>> m_result;
};

// Input gathered by the GLFW callbacks between frames (single-threaded: callbacks run inside
// glfwPollEvents / glfwWaitEvents on the main thread).
struct Input {
    int pending_frames = kSettleFrames;  // frames still owed to the last change
    double scroll = 0.0;
    std::vector<fs::path> dropped;
    bool open_dialog = false, home = false, toggle_level = false, toggle_hud = false;
    bool toggle_fullscreen = false, leave_fullscreen = false;
    std::optional<glm::dvec2> double_click;  // window coordinates
    double last_click_time = -1.0;
    glm::dvec2 last_click_pos{0.0};
};
Input g_input;

void bump(GLFWwindow*) {
    g_input.pending_frames = kSettleFrames;
}

void on_scroll(GLFWwindow* window, double, double y) {
    bump(window);
    if (!ImGui::GetCurrentContext() || !ImGui::GetIO().WantCaptureMouse) g_input.scroll += y;
}

void on_mouse_button(GLFWwindow* window, int button, int action, int) {
    bump(window);
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) return;
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    const double now = glfwGetTime();
    const glm::dvec2 pos(x, y);
    if (g_input.last_click_time >= 0.0 && now - g_input.last_click_time < 0.35 &&
        glm::length(pos - g_input.last_click_pos) < 5.0) {
        g_input.double_click = pos;
        g_input.last_click_time = -1.0;
    } else {
        g_input.last_click_time = now;
        g_input.last_click_pos = pos;
    }
}

void on_key(GLFWwindow* window, int key, int, int action, int) {
    bump(window);
    if (action != GLFW_PRESS) return;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard) return;
    switch (key) {
        case GLFW_KEY_H:
        case GLFW_KEY_HOME:
            g_input.home = true;
            break;
        case GLFW_KEY_L:
            g_input.toggle_level = true;
            break;
        case GLFW_KEY_TAB:
            g_input.toggle_hud = true;
            break;
        case GLFW_KEY_O:
            g_input.open_dialog = true;
            break;
        case GLFW_KEY_F11:
        case GLFW_KEY_F:
            g_input.toggle_fullscreen = true;
            break;
        case GLFW_KEY_ESCAPE:
            g_input.leave_fullscreen = true;
            break;
        default:
            break;
    }
}

void on_drop(GLFWwindow* window, int count, const char** paths) {
    bump(window);
    for (int i = 0; i < count; ++i) g_input.dropped.push_back(platform::path_from_utf8(paths[i]));
}

void on_cursor(GLFWwindow* window, double, double) {
    bump(window);
}
void on_size(GLFWwindow* window, int, int) {
    bump(window);
}
void on_refresh(GLFWwindow* window) {
    bump(window);
}

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

// Camera parameters as on the phone: the FOV spans the long side (one focal length in pixels).
splatrender::SplatCameraParams camera_params(const Trackball& camera, int width, int height) {
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const float focal = 0.5f * std::max(w, h) / std::tan(0.5f * kFovLongSide);
    glm::mat4 proj = glm::perspective(2.0f * std::atan(0.5f * h / focal), w / h, 0.05f, 1000.0f);
    proj[1][1] *= -1.0f;  // Vulkan NDC y down - in the projection, never in data
    const glm::mat4 view = camera.view();
    const glm::mat4 proj_view = proj * view;
    const glm::vec3 eye = camera.eye();
    splatrender::SplatCameraParams params;
    std::memcpy(params.view, glm::value_ptr(view), sizeof(params.view));
    std::memcpy(params.proj_view, glm::value_ptr(proj_view), sizeof(params.proj_view));
    std::memcpy(params.cam_pos, glm::value_ptr(eye), sizeof(params.cam_pos));
    params.focal_x = params.focal_y = focal;
    params.viewport_w = w;
    params.viewport_h = h;
    return params;
}

std::optional<fs::path> bundled_sample() {
    std::error_code ec;
    const fs::path dir = platform::executable_dir() / "samples";
    std::vector<fs::path> found;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && is_scene_file(entry.path())) found.push_back(entry.path());
    }
    if (found.empty()) return std::nullopt;
    std::sort(found.begin(), found.end());
    return found.front();
}

}  // namespace

int main(int argc, char** argv) {
    // [scene] [--gpu NAME] [--validation]
    const std::vector<fs::path> args = platform::command_line(argc, argv);
    std::optional<fs::path> scene_arg;
    std::string gpu;
    bool validation = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string arg = platform::utf8_from_path(args[i]);
        if (arg == "--gpu" && i + 1 < args.size()) {
            gpu = platform::utf8_from_path(args[++i]);
        } else if (arg == "--validation") {
            validation = true;
        } else if (!arg.empty() && arg[0] != '-' && !scene_arg) {
            scene_arg = args[i];
        }
    }

    if (!glfwInit()) {
        platform::show_error(kAppTitle, "The window system could not be initialized.");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // shown once Vulkan is up: an error box stands alone
    GLFWwindow* window = glfwCreateWindow(1600, 900, kAppTitle, nullptr, nullptr);
    if (window == nullptr) {
        platform::show_error(kAppTitle, "The window could not be created.");
        glfwTerminate();
        return 1;
    }
    // Ours first: the ImGui backend chains to callbacks installed before it.
    glfwSetScrollCallback(window, on_scroll);
    glfwSetMouseButtonCallback(window, on_mouse_button);
    glfwSetKeyCallback(window, on_key);
    glfwSetCursorPosCallback(window, on_cursor);
    glfwSetDropCallback(window, on_drop);
    glfwSetFramebufferSizeCallback(window, on_size);
    glfwSetWindowRefreshCallback(window, on_refresh);

    splatrender::Context context;
    splatrender::ContextCreateInfo context_info;
    context_info.app_name = "pocketsplat-viewer";
    context_info.enable_validation = validation;
    context_info.request_dynamic_rendering_extension = true;  // ImGui backend checks the string
    context_info.preferred_device = gpu.c_str();
    if (splatcore::Status s = context.init(context_info); !s.ok()) {
        platform::show_error("PocketSplat cannot start", s.error());
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(context.instance(), window, nullptr, &surface) != VK_SUCCESS) {
        platform::show_error(kAppTitle, "Vulkan could not draw into the window (surface creation failed).");
        context.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    int fb_width = 0, fb_height = 0;
    glfwGetFramebufferSize(window, &fb_width, &fb_height);
    splatrender::Swapchain swapchain;
    splatrender::FrameRing ring;
    splatrender::GpuTimer timer;
    splatrender::SplatRenderer splats;
    {
        splatcore::Status s = swapchain.init(context, surface, static_cast<std::uint32_t>(fb_width),
                                             static_cast<std::uint32_t>(fb_height), /*vsync=*/true);
        if (s.ok()) s = ring.init(context);
        if (s.ok()) s = timer.init(context);
        if (!s.ok()) {
            platform::show_error(kAppTitle, s.error());
            return 1;
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini next to the user's files
    ImGui::StyleColorsDark();
    {
        float scale_x = 1.0f, scale_y = 1.0f;
        glfwGetWindowContentScale(window, &scale_x, &scale_y);
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale_x);
        style.FontScaleDpi = scale_x;
        style.WindowRounding = 6.0f * scale_x;
        style.Colors[ImGuiCol_WindowBg].w = 0.85f;
    }
    ImGui_ImplGlfw_InitForVulkan(window, true);
    VkFormat color_format = swapchain.format();
    ImGui_ImplVulkan_InitInfo imgui_info{};
    imgui_info.ApiVersion = VK_API_VERSION_1_3;
    imgui_info.Instance = context.instance();
    imgui_info.PhysicalDevice = context.physical_device();
    imgui_info.Device = context.device();
    imgui_info.QueueFamily = context.queue_family();
    imgui_info.Queue = context.queue();
    imgui_info.DescriptorPoolSize = 8;
    imgui_info.MinImageCount = 2;
    imgui_info.ImageCount = swapchain.image_count();
    imgui_info.UseDynamicRendering = true;
    imgui_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    imgui_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    imgui_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    imgui_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    if (!ImGui_ImplVulkan_Init(&imgui_info)) {
        platform::show_error(kAppTitle, "The interface renderer could not be initialized.");
        return 1;
    }

    glfwShowWindow(window);

    // Viewer state.
    Trackball camera;
    SceneLoader loader;
    const fs::path cache_dir = platform::import_cache_dir();
    // Largest scene this GPU can hold: the draw records live in one storage buffer bound whole.
    const std::size_t max_splats = context.max_storage_buffer_range() / 32u;
    bool has_scene = false;
    std::string scene_name, scene_note, load_error;
    bool show_hud = true;
    bool exact = false;  // false = the phone's 16-bit keys + 0.1 px cull
    bool pick_pending = false;
    bool fullscreen = false;
    int windowed_pos[2] = {100, 100}, windowed_size[2] = {1600, 900};
    bool needs_recreate = false;
    double last_x = 0.0, last_y = 0.0;
    bool dragging = false;

    auto open_scene = [&](const fs::path& path) {
        load_error.clear();
        if (!is_scene_file(path)) {
            load_error = "Not a splat scene: " + platform::utf8_from_path(path.filename()) +
                         " (PocketSplat opens .ply, .spz and .pspk files)";
            return;
        }
        loader.start(path, cache_dir, max_splats);
    };
    if (scene_arg) {
        open_scene(*scene_arg);
    } else if (std::optional<fs::path> sample = bundled_sample()) {
        open_scene(*sample);
    }

    while (!glfwWindowShouldClose(window)) {
        const bool busy = g_input.pending_frames > 0 || pick_pending || loader.running();
        if (busy && !loader.running()) {
            glfwPollEvents();
        } else {
            // Idle: sleep until an event (or a slow tick that animates the import progress).
            glfwWaitEventsTimeout(loader.running() ? 0.1 : 0.5);
            if (loader.running()) g_input.pending_frames = std::max(g_input.pending_frames, 1);
        }
        const double now = glfwGetTime();

        // A finished load: swap the renderer over to it.
        if (std::optional<splatcore::Result<LoadedScene>> done = loader.take()) {
            g_input.pending_frames = kSettleFrames;
            if (!done->ok()) {
                load_error = done->error();
            } else {
                LoadedScene& scene = done->value();
                context.wait_idle();
                splats.destroy();
                has_scene = false;
                const std::vector<std::uint32_t>* coverage = scene.coverage ? &*scene.coverage : nullptr;
                splatcore::Status s = splats.init(context, scene.pack, swapchain.format(), coverage);
                if (s.ok()) {
                    splats.set_key_bits(exact ? 32 : kFastKeyBits);
                    splats.set_min_contribution(exact ? 0.0f : kFastMinContribution);
                    camera.set_home(scene.home, scene.framing.diagonal);
                    has_scene = true;
                    scene_name = scene.name;
                    scene_note = scene.note;
                    pick_pending = false;
                    glfwSetWindowTitle(window, (scene_name + " - " + kAppTitle).c_str());
                } else {
                    load_error = s.error();
                }
            }
        }

        // Input -> camera / actions.
        for (const fs::path& path : g_input.dropped) open_scene(path);
        g_input.dropped.clear();
        if (g_input.open_dialog) {
            g_input.open_dialog = false;
            if (std::optional<fs::path> path = platform::open_scene_dialog(window)) open_scene(*path);
        }
        if (g_input.toggle_hud) show_hud = !show_hud;
        if (g_input.toggle_level) camera.set_level(!camera.level());
        if (g_input.home && has_scene) camera.reset_home();
        if (g_input.toggle_fullscreen || (g_input.leave_fullscreen && fullscreen)) {
            if (!fullscreen) {
                glfwGetWindowPos(window, &windowed_pos[0], &windowed_pos[1]);
                glfwGetWindowSize(window, &windowed_size[0], &windowed_size[1]);
                GLFWmonitor* monitor = glfwGetPrimaryMonitor();
                const GLFWvidmode* mode = glfwGetVideoMode(monitor);
                glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
            } else {
                glfwSetWindowMonitor(window, nullptr, windowed_pos[0], windowed_pos[1], windowed_size[0],
                                     windowed_size[1], 0);
            }
            fullscreen = !fullscreen;
            needs_recreate = true;
        }
        g_input.toggle_hud = g_input.toggle_level = g_input.home = false;
        g_input.toggle_fullscreen = g_input.leave_fullscreen = false;

        int win_width = 1, win_height = 1;
        glfwGetWindowSize(window, &win_width, &win_height);
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        if (fb_width == 0 || fb_height == 0 || win_width == 0 || win_height == 0) {
            glfwWaitEvents();  // minimized
            continue;
        }
        const float px_per_window = static_cast<float>(fb_width) / static_cast<float>(win_width);
        const float long_side_px = static_cast<float>(std::max(fb_width, fb_height));
        {
            double x = 0.0, y = 0.0;
            glfwGetCursorPos(window, &x, &y);
            const bool want_mouse = ImGui::GetIO().WantCaptureMouse;
            const bool lmb = !want_mouse && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            const bool pan_button =
                !want_mouse && (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                                glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);
            const float dx = static_cast<float>(x - last_x) * px_per_window;
            const float dy = static_cast<float>(y - last_y) * px_per_window;
            if (has_scene && dragging && (dx != 0.0f || dy != 0.0f)) {
                if (lmb) camera.orbit(dx, dy);
                if (pan_button && !lmb) camera.pan(dx / long_side_px, dy / long_side_px, kFovLongSide);
            }
            dragging = lmb || pan_button;
            last_x = x;
            last_y = y;
            if (has_scene && g_input.scroll != 0.0)
                camera.zoom(std::pow(0.9f, static_cast<float>(g_input.scroll)));
            g_input.scroll = 0.0;
            if (has_scene && g_input.double_click) {
                splats.request_pick(static_cast<float>(g_input.double_click->x) * px_per_window,
                                    static_cast<float>(g_input.double_click->y) * px_per_window);
                pick_pending = true;
            }
            g_input.double_click.reset();
        }
        if (camera.step(now)) g_input.pending_frames = kSettleFrames;  // pivot animation

        if (g_input.pending_frames <= 0 && !pick_pending) continue;  // nothing changed: draw nothing

        if (needs_recreate || fb_width != static_cast<int>(swapchain.extent().width) ||
            fb_height != static_cast<int>(swapchain.extent().height)) {
            if (splatcore::Status s = swapchain.recreate(static_cast<std::uint32_t>(fb_width),
                                                         static_cast<std::uint32_t>(fb_height));
                !s.ok()) {
                platform::show_error(kAppTitle, s.error());
                break;
            }
            needs_recreate = false;
        }

        VkCommandBuffer cmd = ring.begin();
        if (cmd == VK_NULL_HANDLE) break;
        std::uint32_t image_index = 0;
        const VkResult acquire_result = swapchain.acquire(ring.image_available(), &image_index);
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            needs_recreate = true;
            vkEndCommandBuffer(cmd);  // abandon this frame's recording
            continue;
        }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) break;

        timer.begin_frame(cmd, ring.slot());
        if (has_scene) {
            splats.prepare_frame(ring.slot(), camera_params(camera, fb_width, fb_height));
            if (pick_pending) {
                float world[3];
                const splatrender::SplatRenderer::PickResult result = splats.take_pick(world);
                if (result == splatrender::SplatRenderer::PickResult::kHit) {
                    camera.start_pivot_animation(glm::vec3(world[0], world[1], world[2]), now);
                    pick_pending = false;
                } else if (result != splatrender::SplatRenderer::PickResult::kPending) {
                    pick_pending = false;  // a miss: under 10 % coverage at the cursor
                }
            }
            splats.record_compute(cmd, ring.slot(), &timer);
        } else {
            timer.stamp(cmd, ring.slot());
            timer.stamp(cmd, ring.slot());
        }
        timer.stamp(cmd, ring.slot());

        const VkImage target_image = swapchain.image(image_index);
        image_barrier(cmd, target_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = swapchain.view(image_index);
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{kBackground[0], kBackground[1], kBackground[2], 1.0f}};
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, swapchain.extent()};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color_attachment;
        vkCmdBeginRendering(cmd, &rendering);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(fb_width), static_cast<float>(fb_height),
                                  0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, swapchain.extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (has_scene) splats.record_draw(cmd, ring.slot());

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (show_hud) {
            const float scale = ImGui::GetStyle().FontScaleDpi;
            ImGui::SetNextWindowPos(ImVec2(12.0f * scale, 12.0f * scale), ImGuiCond_FirstUseEver);
            ImGui::Begin(kAppTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            if (has_scene) {
                ImGui::Text("%s", scene_name.c_str());
                ImGui::Text("%s splats (SH %u), %s drawn", with_thousands(splats.splat_count()).c_str(),
                            splats.sh_degree(), with_thousands(splats.last_visible_count()).c_str());
                if (!scene_note.empty()) ImGui::TextDisabled("%s", scene_note.c_str());
            } else if (!loader.running()) {
                ImGui::TextWrapped("Drop a .ply, .spz or .pspk file on this window, or press O to open one.");
            }
            if (loader.running()) ImGui::Text("%s...", loader.stage().c_str());
            if (!load_error.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", load_error.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Separator();
            if (ImGui::Button("Open... (O)")) g_input.open_dialog = true;
            if (has_scene) {
                ImGui::SameLine();
                if (ImGui::Button("Home view (H)")) camera.reset_home();
                ImGui::SameLine();
                bool level = camera.level();
                if (ImGui::Checkbox("Level orbit (L)", &level)) camera.set_level(level);
                if (ImGui::Checkbox("Exact rendering (32-bit sort, no culling)", &exact)) {
                    splats.set_key_bits(exact ? 32 : kFastKeyBits);
                    splats.set_min_contribution(exact ? 0.0f : kFastMinContribution);
                }
                if (splats.has_coverage()) {
                    float strength = splats.coverage_strength();
                    if (ImGui::SliderFloat("Coverage fade", &strength, 0.0f, 1.0f, "%.2f")) {
                        splats.set_coverage_strength(strength);
                    }
                }
                ImGui::TextDisabled("GPU %.2f ms per frame  |  %s", timer.last_gpu_ms(),
                                    context.device_name());
            } else {
                ImGui::TextDisabled("%s", context.device_name());
            }
            if (ImGui::CollapsingHeader("Controls")) {
                ImGui::BulletText("Left drag: orbit    Right/middle drag: pan    Wheel: zoom");
                ImGui::BulletText("Double-click: orbit around the point under the cursor");
                ImGui::BulletText("H: home view    L: level orbit    Tab: hide this panel");
                ImGui::BulletText("F11: full screen    O: open a scene (or drop a file)");
            }
            ImGui::End();
        }
        if (ImGui::IsAnyItemActive()) g_input.pending_frames = kSettleFrames;  // a slider being dragged
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vkCmdEndRendering(cmd);

        image_barrier(cmd, target_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                      VK_ACCESS_2_NONE);
        timer.end_frame(cmd, ring.slot());
        if (splatcore::Status s = ring.submit(swapchain.render_finished(image_index)); !s.ok()) {
            platform::show_error(kAppTitle, s.error());
            break;
        }
        const VkResult present_result = swapchain.present(context.queue(), image_index);
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
            needs_recreate = true;
        } else if (present_result != VK_SUCCESS) {
            break;
        }
        if (g_input.pending_frames > 0) --g_input.pending_frames;
    }

    context.wait_idle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    splats.destroy();
    timer.destroy();
    ring.destroy();
    swapchain.destroy();
    vkDestroySurfaceKHR(context.instance(), surface, nullptr);
    const bool clean = context.validation_message_count() == 0;
    context.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return validation && !clean ? 2 : 0;
}
