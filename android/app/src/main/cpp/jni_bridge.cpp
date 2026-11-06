// JNI surface for com.pocketsplat.app.SplatEngine. Thin translation layer only: no exceptions
// cross this boundary (splatcore Status/Result carries errors; failures log to logcat and return
// false/0). All calls except nativeGetStats arrive on the app's render thread.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "android_host.h"
#include "splatcore/capture.h"
#include "splatcore/scene_import.h"
#include "splatcore/scene_load.h"

namespace {

pocketsplat::AndroidHost* host_of(jlong handle) {
    return reinterpret_cast<pocketsplat::AndroidHost*>(handle);
}

std::string to_string(JNIEnv* env, jstring text) {
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(text, chars);
    return out;
}

void log_failure(const char* what, const splatcore::Status& status) {
    __android_log_print(ANDROID_LOG_ERROR, "PocketSplat", "%s: %s", what, status.error().c_str());
}

// Import runs synchronously on whatever background thread Kotlin calls it from (never the render
// thread); the UI polls the stage and requests cancellation through these. One import at a time.
std::atomic<int> g_import_stage{-1};  // ImportStage as int, -1 = no import running
std::atomic<bool> g_import_cancel{false};
std::atomic<bool> g_import_active{false};
std::string g_open_error;  // last nativeOpenScene failure, render thread only

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_com_pocketsplat_app_SplatEngine_nativeCreate(JNIEnv*, jobject) {
    auto* host = new pocketsplat::AndroidHost();
    const splatcore::Status status = host->init();
    if (!status.ok()) {
        log_failure("engine init", status);
        delete host;
        return 0;
    }
    return reinterpret_cast<jlong>(host);
}

JNIEXPORT void JNICALL Java_com_pocketsplat_app_SplatEngine_nativeDestroy(JNIEnv*, jobject, jlong handle) {
    delete host_of(handle);
}

JNIEXPORT jboolean JNICALL Java_com_pocketsplat_app_SplatEngine_nativeOpenScene(JNIEnv* env, jobject,
                                                                                jlong handle, jstring path) {
    const splatcore::Status status = host_of(handle)->open_scene(to_string(env, path));
    g_open_error = status.ok() ? "" : status.error();
    if (!status.ok()) log_failure("open scene", status);
    return status.ok() ? JNI_TRUE : JNI_FALSE;
}

// The error of the last failed nativeOpenScene, for the UI (render thread, like the open itself).
JNIEXPORT jstring JNICALL Java_com_pocketsplat_app_SplatEngine_nativeLastOpenError(JNIEnv* env, jobject) {
    return env->NewStringUTF(g_open_error.c_str());
}

// [splat count, SH degree, pivot x, y, z, framing diagonal, eye x, y, z, orientation w, x, y, z,
//  scene up x, y, z, has coverage, coverage source]: the home view (splatcore::estimate_home_view)
// as a trackball pose, the fitted ground normal a level orbit turns about, 1 when the scene has a
// coverage sidecar (the fade slider applies), where it came from (splatcore::CaptureSource:
// 0 capture cameras, 1 inferred room, 2 inferred object; -1 none), and the scene file's splat
// count (above the first entry when the GPU limit cut the scene at open).
JNIEXPORT jfloatArray JNICALL Java_com_pocketsplat_app_SplatEngine_nativeSceneInfo(JNIEnv* env, jobject,
                                                                                   jlong handle) {
    const pocketsplat::AndroidHost* host = host_of(handle);
    const splatcore::HomeView& home = host->home_view();
    float orientation[4];
    host->home_orientation(orientation);
    const float values[19] = {static_cast<float>(host->splat_count()),
                              static_cast<float>(host->sh_degree()),
                              home.focus[0],
                              home.focus[1],
                              home.focus[2],
                              host->framing().diagonal,
                              home.eye[0],
                              home.eye[1],
                              home.eye[2],
                              orientation[0],
                              orientation[1],
                              orientation[2],
                              orientation[3],
                              home.up[0],
                              home.up[1],
                              home.up[2],
                              host->has_coverage() ? 1.0f : 0.0f,
                              static_cast<float>(host->coverage_source()),
                              static_cast<float>(host->file_splat_count())};
    jfloatArray out = env->NewFloatArray(19);
    env->SetFloatArrayRegion(out, 0, 19, values);
    return out;
}

JNIEXPORT jboolean JNICALL Java_com_pocketsplat_app_SplatEngine_nativeSetSurface(JNIEnv* env, jobject,
                                                                                 jlong handle,
                                                                                 jobject surface) {
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    const splatcore::Status status = host_of(handle)->set_surface(window);
    if (!status.ok()) log_failure("set surface", status);
    return status.ok() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_pocketsplat_app_SplatEngine_nativeSetCameraPose(
    JNIEnv*, jobject, jlong handle, jfloat ex, jfloat ey, jfloat ez, jfloat qw, jfloat qx, jfloat qy,
    jfloat qz, jfloat fov_rad) {
    const float eye[3] = {ex, ey, ez};
    const float orientation[4] = {qw, qx, qy, qz};
    host_of(handle)->set_camera_pose(eye, orientation, fov_rad);
}

// Depth pick at a point of the view (0..1, y down) for the double-tap pivot.
JNIEXPORT void JNICALL Java_com_pocketsplat_app_SplatEngine_nativeRequestPick(JNIEnv*, jobject, jlong handle,
                                                                              jfloat u, jfloat v) {
    host_of(handle)->request_pick(u, v);
}

// null = no result yet (keep drawing frames) or nothing requested; [x, y, z] = hit; [] = miss.
JNIEXPORT jfloatArray JNICALL Java_com_pocketsplat_app_SplatEngine_nativeTakePick(JNIEnv* env, jobject,
                                                                                  jlong handle) {
    float world[3];
    const auto result = host_of(handle)->take_pick(world);
    if (result == splatrender::SplatRenderer::PickResult::kHit) {
        jfloatArray out = env->NewFloatArray(3);
        env->SetFloatArrayRegion(out, 0, 3, world);
        return out;
    }
    return result == splatrender::SplatRenderer::PickResult::kMiss ? env->NewFloatArray(0) : nullptr;
}

JNIEXPORT jboolean JNICALL Java_com_pocketsplat_app_SplatEngine_nativeRender(JNIEnv*, jobject, jlong handle) {
    return host_of(handle)->render() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_pocketsplat_app_SplatEngine_nativeSetAdaptive(JNIEnv*, jobject, jlong handle,
                                                                              jboolean on) {
    host_of(handle)->set_adaptive_quality(on == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_pocketsplat_app_SplatEngine_nativeSetCoverageStrength(JNIEnv*, jobject,
                                                                                      jlong handle,
                                                                                      jfloat strength) {
    host_of(handle)->set_coverage_strength(strength);
}

// About once a second while resumed; true = the quality rung changed (redraw).
JNIEXPORT jboolean JNICALL Java_com_pocketsplat_app_SplatEngine_nativeThermalTick(JNIEnv*, jobject,
                                                                                  jlong handle) {
    return host_of(handle)->thermal_tick() ? JNI_TRUE : JNI_FALSE;
}

// Rate interactive frames are paced at; 0 = free-running (bench).
JNIEXPORT jfloat JNICALL Java_com_pocketsplat_app_SplatEngine_nativeTargetFps(JNIEnv*, jobject,
                                                                              jlong handle) {
    return host_of(handle)->interactive_target_fps();
}

JNIEXPORT jboolean JNICALL Java_com_pocketsplat_app_SplatEngine_nativeStartTrace(JNIEnv* env, jobject,
                                                                                 jlong handle, jstring path) {
    const splatcore::Status status = host_of(handle)->start_trace(to_string(env, path));
    if (!status.ok()) log_failure("start trace", status);
    return status.ok() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_pocketsplat_app_SplatEngine_nativeFlushTrace(JNIEnv*, jobject, jlong handle) {
    host_of(handle)->flush_trace();
}

JNIEXPORT jboolean JNICALL Java_com_pocketsplat_app_SplatEngine_nativeStartBench(
    JNIEnv* env, jobject, jlong handle, jstring csv_path, jstring meta, jstring scene_name, jint key_bits,
    jfloat min_contribution, jfloat target_fps, jint draw_mode, jboolean controller, jboolean vote_fps,
    jboolean home_swing) {
    pocketsplat::AndroidHost::BenchConfig config;
    config.csv_path = to_string(env, csv_path);
    config.meta = to_string(env, meta);
    config.scene_name = to_string(env, scene_name);
    config.key_bits = key_bits;
    config.min_contribution = min_contribution;
    config.target_fps = target_fps;
    config.draw_mode = draw_mode;
    config.controller = controller == JNI_TRUE;
    config.vote_fps = vote_fps == JNI_TRUE;
    config.home_swing = home_swing == JNI_TRUE;
    const splatcore::Status status = host_of(handle)->start_bench(config);
    if (!status.ok()) log_failure("start bench", status);
    return status.ok() ? JNI_TRUE : JNI_FALSE;
}

// Writes the CSV; returns the one-line summary, or "" if nothing was recorded.
JNIEXPORT jstring JNICALL Java_com_pocketsplat_app_SplatEngine_nativeStopBench(JNIEnv* env, jobject,
                                                                               jlong handle) {
    return env->NewStringUTF(host_of(handle)->stop_bench().c_str());
}

// Header-only probe: [splat count, SH degree, file bytes], or null when the file is not a
// loadable scene (the reason goes to logcat). Safe from any thread; touches no AndroidHost.
JNIEXPORT jlongArray JNICALL Java_com_pocketsplat_app_SplatEngine_nativeProbeScene(JNIEnv* env, jobject,
                                                                                   jstring path) {
    const splatcore::Result<splatcore::SceneProbe> probe = splatcore::probe_scene(to_string(env, path));
    if (!probe.ok()) {
        __android_log_print(ANDROID_LOG_ERROR, "PocketSplat", "probe: %s", probe.error().c_str());
        return nullptr;
    }
    const jlong values[3] = {static_cast<jlong>(probe.value().count),
                             static_cast<jlong>(probe.value().sh_degree),
                             static_cast<jlong>(probe.value().file_bytes)};
    jlongArray out = env->NewLongArray(3);
    env->SetLongArrayRegion(out, 0, 3, values);
    return out;
}

// Full import pipeline (splatcore::import_scene), synchronous on the calling thread. Returns ""
// on success (the cleaning report goes to logcat) or the error message; "import cancelled" after
// nativeImportCancel. max_splats <= 0 disables the budget prune.
JNIEXPORT jstring JNICALL Java_com_pocketsplat_app_SplatEngine_nativeImportScene(
    JNIEnv* env, jobject, jstring src, jstring dst, jint sh_target, jboolean deep_clean, jlong max_splats) {
    bool expected = false;
    if (!g_import_active.compare_exchange_strong(expected, true)) {
        return env->NewStringUTF("an import is already running");
    }
    g_import_cancel.store(false);
    splatcore::ImportOptions options;
    options.sh_target = static_cast<int>(sh_target);
    options.deep_clean = deep_clean == JNI_TRUE;
    options.max_splats = max_splats > 0 ? static_cast<std::size_t>(max_splats) : 0;
    options.progress = [](splatcore::ImportStage stage) {
        g_import_stage.store(static_cast<int>(stage));
        return !g_import_cancel.load();
    };
    const splatcore::Result<splatcore::ImportReport> report =
        splatcore::import_scene(to_string(env, src), to_string(env, dst), options);
    g_import_stage.store(-1);
    g_import_active.store(false);
    if (!report.ok()) {
        __android_log_print(ANDROID_LOG_ERROR, "PocketSplat", "import: %s", report.error().c_str());
        return env->NewStringUTF(report.error().c_str());
    }
    for (const splatcore::CleanOpStats& op : report.value().ops) {
        __android_log_print(ANDROID_LOG_INFO, "PocketSplat", "import op %s: in %zu, affected %zu (%s)",
                            op.op.c_str(), op.input_count, op.removed, op.params.c_str());
    }
    __android_log_print(ANDROID_LOG_INFO, "PocketSplat", "import done: %llu -> %u splats, SH %d",
                        static_cast<unsigned long long>(report.value().input_count), report.value().count,
                        report.value().sh_degree);
    return env->NewStringUTF("");
}

// Backside info for a scene (ADR-0015): writes <scene stem>.pscov next to `scene` from capture
// camera files (source 0: cameras.json, transforms.json, or COLMAP cameras + images) or from
// virtual cameras for a scene type (1 room, 2 object; `camera_files` ignored). Runs on a
// background thread. Returns "ok: <summary>" or the error.
JNIEXPORT jstring JNICALL Java_com_pocketsplat_app_SplatEngine_nativeComputeCoverage(
    JNIEnv* env, jobject, jstring scene, jint source, jobjectArray camera_files) {
    const auto start = std::chrono::steady_clock::now();
    const std::string path = to_string(env, scene);
    splatcore::Result<splatcore::GpuPack> pack = splatcore::load_scene_pack(path, 0);
    if (!pack) return env->NewStringUTF(pack.error().c_str());
    std::vector<splatcore::CaptureFile> files;
    const jsize n = camera_files != nullptr ? env->GetArrayLength(camera_files) : 0;
    for (jsize i = 0; i < n; ++i) {
        auto* item = static_cast<jstring>(env->GetObjectArrayElement(camera_files, i));
        splatcore::CaptureFile f;
        const std::string file_path = to_string(env, item);
        env->DeleteLocalRef(item);
        f.name = file_path.substr(file_path.find_last_of('/') + 1);
        std::ifstream in(file_path, std::ios::binary);
        if (!in) return env->NewStringUTF(("cannot read " + f.name).c_str());
        f.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        files.push_back(std::move(f));
    }
    const auto kind = static_cast<splatcore::CaptureSource>(std::clamp(static_cast<int>(source), 0, 2));
    splatcore::Result<splatcore::CoverageSidecar> sidecar =
        splatcore::make_coverage_sidecar(pack.value(), kind, files);
    if (!sidecar) {
        __android_log_print(ANDROID_LOG_ERROR, "PocketSplat", "coverage: %s", sidecar.error().c_str());
        return env->NewStringUTF(sidecar.error().c_str());
    }
    const std::string out_path = path.substr(0, path.find_last_of('.')) + ".pscov";
    const std::string tmp = out_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(sidecar.value().bytes.data()),
                  static_cast<std::streamsize>(sidecar.value().bytes.size()));
        if (!out.flush()) return env->NewStringUTF("cannot write the backside file");
    }
    if (std::rename(tmp.c_str(), out_path.c_str()) != 0)
        return env->NewStringUTF("cannot write the backside file");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const char* what = kind == splatcore::CaptureSource::kCameras ? "capture cameras"
                       : kind == splatcore::CaptureSource::kRoom  ? "virtual cameras (room)"
                                                                  : "virtual cameras (object)";
    char summary[160];
    std::snprintf(summary, sizeof(summary), "ok: %zu %s, they see %.0f %% of the scene, %.1f s",
                  sidecar.value().cameras, what, 100.0 * sidecar.value().fit, seconds);
    __android_log_print(ANDROID_LOG_INFO, "PocketSplat", "coverage %s: %s", out_path.c_str(), summary);
    return env->NewStringUTF(summary);
}

// Stage of the running import as an ImportStage ordinal (0 read, 1 clean, 2 SOR, 3 pack,
// 4 write), or -1 when no import is running. Safe from any thread.
JNIEXPORT jint JNICALL Java_com_pocketsplat_app_SplatEngine_nativeImportStage(JNIEnv*, jobject) {
    return g_import_stage.load();
}

// Asks the running import to stop at its next stage boundary (the long ops themselves finish).
JNIEXPORT void JNICALL Java_com_pocketsplat_app_SplatEngine_nativeImportCancel(JNIEnv*, jobject) {
    g_import_cancel.store(true);
}

// [gpu ms, cull ms, preprocess ms, prepare+sort ms, draw ms, visible, visible chunks, frames,
//  contribution threshold px, adaptive-quality rung (-1 = off), visible splats not drawn (clipped),
//  clip guard floor px (0 = inactive)]
JNIEXPORT jfloatArray JNICALL Java_com_pocketsplat_app_SplatEngine_nativeGetStats(JNIEnv* env, jobject,
                                                                                  jlong handle) {
    const pocketsplat::AndroidHost::Stats& stats = host_of(handle)->stats();
    const float values[12] = {stats.gpu_ms.load(std::memory_order_relaxed),
                              stats.cull_ms.load(std::memory_order_relaxed),
                              stats.preprocess_ms.load(std::memory_order_relaxed),
                              stats.sort_ms.load(std::memory_order_relaxed),
                              stats.draw_ms.load(std::memory_order_relaxed),
                              static_cast<float>(stats.visible.load(std::memory_order_relaxed)),
                              static_cast<float>(stats.visible_chunks.load(std::memory_order_relaxed)),
                              static_cast<float>(stats.frames.load(std::memory_order_relaxed)),
                              stats.min_contribution.load(std::memory_order_relaxed),
                              static_cast<float>(stats.rung.load(std::memory_order_relaxed)),
                              static_cast<float>(stats.clipped.load(std::memory_order_relaxed)),
                              stats.clip_floor.load(std::memory_order_relaxed)};
    jfloatArray out = env->NewFloatArray(12);
    env->SetFloatArrayRegion(out, 0, 12, values);
    return out;
}

}  // extern "C"
