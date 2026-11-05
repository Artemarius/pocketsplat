#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "splatcore/capture.h"
#include "splatcore/cleaning.h"
#include "splatcore/coords.h"
#include "splatcore/coverage.h"
#include "splatcore/home_view.h"
#include "splatcore/pack.h"
#include "splatcore/ply_io.h"
#include "splatcore/quality_controller.h"
#include "splatcore/scene_load.h"
#include "splatcore/spz_io.h"
#include "splatcore/stats.h"

#ifdef _WIN32
#include <io.h>
#define PS_DUP _dup
#define PS_DUP2 _dup2
#define PS_FDOPEN _fdopen
#define PS_FILENO _fileno
#else
#include <unistd.h>
#define PS_DUP dup
#define PS_DUP2 dup2
#define PS_FDOPEN fdopen
#define PS_FILENO fileno
#endif

namespace {

// Machine-readable results (JSON) go here. The spz library prints progress lines to stdout, so
// main() moves the process's stdout to stderr and points this at a duplicate of the original.
FILE* g_json = stdout;

namespace fs = std::filesystem;
using splatcore::CoordSystem;
using splatcore::SplatCloud;

void print_usage() {
    std::fprintf(stderr,
                 "splatconv — PocketSplat converter/cleaner/inspector\n"
                 "\n"
                 "Usage:\n"
                 "  splatconv inspect <file.ply|file.spz>\n"
                 "      Print scene statistics as JSON to stdout (no coordinate conversion).\n"
                 "\n"
                 "  splatconv convert <in.ply|in.spz> <out.ply|out.spz> [options]\n"
                 "      --in-coords RDF|RUB   Coordinate system of an input PLY (default RDF).\n"
                 "      --out-coords RDF|RUB  Coordinate system of an output PLY (default RDF).\n"
                 "      --sh 0|1              Truncate SH to this degree. Required when the input\n"
                 "                            degree is 2-3: PocketSplat stores degree 0-1 only.\n"
                 "\n"
                 "  splatconv clean <in.ply|in.spz> <out.ply|out.spz> [options]\n"
                 "      Full pipeline per docs/cleaning.md: drop_invalid, prune_opacity,\n"
                 "      prune_scale, clamp_needle_scales, SOR floater removal, truncate_sh,\n"
                 "      prune_budget, Morton reorder. Prints a JSON report to stdout.\n"
                 "      --alpha V        prune_opacity threshold (default 0.02)\n"
                 "      --scale-frac V   prune_scale max fraction of scene diagonal (default 0.05)\n"
                 "      --min-scale-frac V  prune_scale min smallest-axis fraction of the diagonal\n"
                 "                       (default 0 = off, ADR-0007; 1e-6 was the pre-ADR value)\n"
                 "      --needle-frac V  clamp_needle_scales length cap as a fraction of the scene\n"
                 "                       diagonal (default 0.01)\n"
                 "      --needle-aspect V  clamp_needle_scales max/mid aspect cap (default 6)\n"
                 "      --no-needle-clamp  skip the needle clamp\n"
                 "      --sor-k N        SOR neighbour count (default 16)\n"
                 "      --sor-m V        SOR stddev multiplier (default 2.0)\n"
                 "      --sh 0|1         target SH degree (default 1)\n"
                 "      --budget N       max splats kept (default 1000000)\n"
                 "      --no-sor         skip the SOR pass\n"
                 "      --no-morton      skip the Morton reorder\n"
                 "      --report FILE    also write the JSON report to FILE\n"
                 "      --in-coords / --out-coords as for convert\n"
                 "\n"
                 "  splatconv pack <in.ply|in.spz> <out.pspk> [options]\n"
                 "      Build the GPU pack (docs/formats.md §3): Morton reorder (unless\n"
                 "      --no-morton), then the quantized SoA layout with 256-splat chunk AABBs.\n"
                 "      --sh 0|1 and coordinate flags as for convert.\n"
                 "\n"
                 "  splatconv pack --dump-header <file.pspk>\n"
                 "      Print the 64-byte pack header as JSON.\n"
                 "\n"
                 "  splatconv coverage <scene.pspk|.ply|.spz> <camera files...> <out.pscov>\n"
                 "  splatconv coverage <scene.pspk|.ply|.spz> <out.pscov> --infer room|object\n"
                 "      Per-splat observed-direction mask for the renderer's coverage fade (ADR-0013,\n"
                 "      ADR-0015); a sidecar bound to the scene's packed positions. Camera files: a\n"
                 "      3DGS cameras.json, a transforms.json, or COLMAP cameras + images (.bin or .txt),\n"
                 "      in the PLY's world; files that see under half the scene are rejected. Without\n"
                 "      cameras, --infer places virtual cameras for a room (walked through) or an\n"
                 "      object (orbited). Prints coverage statistics as JSON.\n"
                 "      --dump-cameras FILE  also write the cameras used as a cameras.json\n"
                 "      --room-positions N / --room-radius F / --room-clearance N\n"
                 "      --object-radii a,b / --object-elevations a,b,c   virtual capture tuning\n"
                 "\n"
                 "  splatconv replay <bench.csv> [options]\n"
                 "      Run the quality controller (ADR-0009) over a recorded Android bench CSV and\n"
                 "      print its decisions as JSON. On a controller run it reproduces the logged\n"
                 "      decisions; on a fixed-rung run it is open loop (the recording never saw the\n"
                 "      controller's steps). Defaults come from the CSV header where it has them.\n"
                 "      --target-fps V   frame-guard target (default: the run's frame cap)\n"
                 "      --light V        LIGHT headroom threshold (default: header, else 0.833)\n"
                 "      --max-mhz N      GPU clock maximum for the step-up gate (default: header,\n"
                 "                       else unknown = dwell only)\n"
                 "      --frame-guard N/M  step on N bad one-second windows of the last M (default:\n"
                 "                       header, else 4/6)\n"
                 "      --forecast-warmup S  ignore the platform forecaster's first S seconds\n"
                 "                       (default: header; controller runs without it ran with 0;\n"
                 "                       otherwise 20)\n"
                 "      --backoff-window S  a step down this soon after a step up doubles the dwell\n"
                 "                       (default: header; controller runs without it ran with\n"
                 "                       120; otherwise 600, ADR-0011)\n"
                 "      --step-up-current on|off  a step up also needs the current headroom below\n"
                 "                       the threshold (default: header; controller runs without\n"
                 "                       it ran without; otherwise on, ADR-0011)\n"
                 "      --frame-guard-clock F  a frame-guard window is bad only at a GPU clock of at\n"
                 "                       least F x the maximum (default: header; controller runs\n"
                 "                       without it ran with 0 = off; otherwise 0.9, ADR-0011)\n"
                 "\n"
                 "SPZ files are always read and written in RUB; coordinate flags apply to PLY.\n"
                 "Exit code is 0 on success, 1 on any failure.\n");
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

std::string json_float(float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value));
    return buf;
}

std::string json_vec3(const std::array<float, 3>& v) {
    return "[" + json_float(v[0]) + ", " + json_float(v[1]) + ", " + json_float(v[2]) + "]";
}

std::string json_histogram(const splatcore::Histogram& histogram) {
    std::string bins;
    for (std::size_t i = 0; i < histogram.bins.size(); ++i) {
        if (i > 0) bins += ", ";
        bins += std::to_string(histogram.bins[i]);
    }
    return "{\"min\": " + json_float(histogram.min_value) + ", \"max\": " + json_float(histogram.max_value) +
           ", \"bins\": [" + bins + "]}";
}

bool has_extension(const std::string& path, const char* ext) {
    std::string lower = fs::path(path).extension().string();
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == ext;
}

// --- tiny flag parser -------------------------------------------------------------------------

struct Options {
    std::vector<std::string> positional;
    CoordSystem in_coords = CoordSystem::kRdf;
    CoordSystem out_coords = CoordSystem::kRdf;
    std::optional<int> sh;
    float alpha = 0.02f;
    float scale_frac = 0.05f;
    float min_scale_frac = 0.0f;  // ADR-0007: flat splats are content, not noise
    float needle_frac = 0.01f;
    float needle_aspect = 6.0f;
    int sor_k = 16;
    float sor_m = 2.0f;
    std::size_t budget = 1000000;
    bool no_sor = false;
    bool no_needle_clamp = false;
    bool no_morton = false;
    bool dump_header = false;
    std::optional<float> target_fps;  // replay overrides of the CSV header
    std::optional<float> light;
    std::optional<int> max_mhz;
    std::optional<std::pair<int, int>> frame_guard;  // bad windows / span
    std::optional<double> forecast_warmup;
    std::optional<double> backoff_window;
    std::optional<bool> step_up_current;
    std::optional<float> frame_guard_clock;
    std::string report_file;
    std::string infer;  // coverage: room | object (no camera files)
    splatcore::VirtualCaptureOptions capture;
    std::string dump_cameras;  // coverage: write the cameras used as a 3DGS cameras.json
};

std::vector<float> parse_float_list(const std::string& text) {
    std::vector<float> out;
    std::stringstream s(text);
    std::string item;
    while (std::getline(s, item, ',')) out.push_back(std::stof(item));
    return out;
}

bool parse_coords(const std::string& text, CoordSystem& out) {
    if (text == "RDF") {
        out = CoordSystem::kRdf;
        return true;
    }
    if (text == "RUB") {
        out = CoordSystem::kRub;
        return true;
    }
    return false;
}

bool parse_options(int argc, char** argv, int start, Options& options) {
    for (int i = start; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "splatconv: %s needs a value\n", flag);
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };
        if (arg == "--no-sor") {
            options.no_sor = true;
        } else if (arg == "--no-morton") {
            options.no_morton = true;
        } else if (arg == "--dump-header") {
            options.dump_header = true;
        } else if (arg == "--in-coords" || arg == "--out-coords") {
            auto v = value(arg.c_str());
            if (!v) return false;
            CoordSystem& target = (arg == "--in-coords") ? options.in_coords : options.out_coords;
            if (!parse_coords(*v, target)) {
                std::fprintf(stderr, "splatconv: unknown coordinate system '%s' (RDF|RUB)\n", v->c_str());
                return false;
            }
        } else if (arg == "--sh") {
            auto v = value("--sh");
            if (!v) return false;
            options.sh = std::stoi(*v);
            if (*options.sh != 0 && *options.sh != 1) {
                std::fprintf(stderr, "splatconv: --sh must be 0 or 1\n");
                return false;
            }
        } else if (arg == "--alpha") {
            auto v = value("--alpha");
            if (!v) return false;
            options.alpha = std::stof(*v);
        } else if (arg == "--scale-frac") {
            auto v = value("--scale-frac");
            if (!v) return false;
            options.scale_frac = std::stof(*v);
        } else if (arg == "--min-scale-frac") {
            auto v = value("--min-scale-frac");
            if (!v) return false;
            options.min_scale_frac = std::stof(*v);
        } else if (arg == "--needle-frac") {
            auto v = value("--needle-frac");
            if (!v) return false;
            options.needle_frac = std::stof(*v);
        } else if (arg == "--needle-aspect") {
            auto v = value("--needle-aspect");
            if (!v) return false;
            options.needle_aspect = std::stof(*v);
        } else if (arg == "--no-needle-clamp") {
            options.no_needle_clamp = true;
        } else if (arg == "--sor-k") {
            auto v = value("--sor-k");
            if (!v) return false;
            options.sor_k = std::stoi(*v);
        } else if (arg == "--sor-m") {
            auto v = value("--sor-m");
            if (!v) return false;
            options.sor_m = std::stof(*v);
        } else if (arg == "--budget") {
            auto v = value("--budget");
            if (!v) return false;
            options.budget = static_cast<std::size_t>(std::stoull(*v));
        } else if (arg == "--target-fps") {
            auto v = value("--target-fps");
            if (!v) return false;
            options.target_fps = std::stof(*v);
        } else if (arg == "--light") {
            auto v = value("--light");
            if (!v) return false;
            options.light = std::stof(*v);
        } else if (arg == "--max-mhz") {
            auto v = value("--max-mhz");
            if (!v) return false;
            options.max_mhz = std::stoi(*v);
        } else if (arg == "--frame-guard") {
            auto v = value("--frame-guard");
            if (!v) return false;
            const std::size_t slash = v->find('/');
            if (slash == std::string::npos) {
                std::fprintf(stderr, "splatconv: --frame-guard takes N/M (bad windows / span)\n");
                return false;
            }
            options.frame_guard =
                std::make_pair(std::stoi(v->substr(0, slash)), std::stoi(v->substr(slash + 1)));
        } else if (arg == "--forecast-warmup") {
            auto v = value("--forecast-warmup");
            if (!v) return false;
            options.forecast_warmup = std::stod(*v);
        } else if (arg == "--backoff-window") {
            auto v = value("--backoff-window");
            if (!v) return false;
            options.backoff_window = std::stod(*v);
        } else if (arg == "--step-up-current") {
            auto v = value("--step-up-current");
            if (!v) return false;
            if (*v != "on" && *v != "off") {
                std::fprintf(stderr, "splatconv: --step-up-current takes on or off\n");
                return false;
            }
            options.step_up_current = *v == "on";
        } else if (arg == "--frame-guard-clock") {
            auto v = value("--frame-guard-clock");
            if (!v) return false;
            options.frame_guard_clock = std::stof(*v);
        } else if (arg == "--infer") {
            auto v = value("--infer");
            if (!v) return false;
            if (*v != "room" && *v != "object") {
                std::fprintf(stderr, "splatconv: --infer must be room or object\n");
                return false;
            }
            options.infer = *v;
        } else if (arg == "--room-positions") {
            auto v = value("--room-positions");
            if (!v) return false;
            options.capture.room_positions = std::stoi(*v);
        } else if (arg == "--room-radius") {
            auto v = value("--room-radius");
            if (!v) return false;
            options.capture.room_radius = std::stof(*v);
        } else if (arg == "--room-clearance") {
            auto v = value("--room-clearance");
            if (!v) return false;
            options.capture.room_clearance = std::stoi(*v);
        } else if (arg == "--room-enclosure") {
            auto v = value("--room-enclosure");
            if (!v) return false;
            options.capture.room_enclosure = std::stof(*v);
        } else if (arg == "--grid-half") {
            auto v = value("--grid-half");
            if (!v) return false;
            options.capture.grid_half = std::stof(*v);
        } else if (arg == "--object-radii") {
            auto v = value("--object-radii");
            if (!v) return false;
            options.capture.object_radii = parse_float_list(*v);
        } else if (arg == "--object-elevations") {
            auto v = value("--object-elevations");
            if (!v) return false;
            options.capture.object_elevations_deg = parse_float_list(*v);
        } else if (arg == "--dump-cameras") {
            auto v = value("--dump-cameras");
            if (!v) return false;
            options.dump_cameras = *v;
        } else if (arg == "--report") {
            auto v = value("--report");
            if (!v) return false;
            options.report_file = *v;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "splatconv: unknown option '%s'\n", arg.c_str());
            return false;
        } else {
            options.positional.push_back(arg);
        }
    }
    return true;
}

// --- IO in the internal RUB space -------------------------------------------------------------

// Reads PLY (converting from options.in_coords to RUB) or SPZ (already RUB).
splatcore::Result<SplatCloud> read_any(const std::string& path, const Options& options) {
    if (has_extension(path, ".ply")) {
        splatcore::Result<SplatCloud> cloud = splatcore::read_ply(fs::path(path));
        if (cloud) splatcore::convert_coordinates(cloud.value(), options.in_coords, CoordSystem::kRub);
        return cloud;
    }
    if (has_extension(path, ".spz")) return splatcore::read_spz(fs::path(path));
    return splatcore::Result<SplatCloud>::failure("unsupported input extension on '" + path +
                                                  "' (.ply or .spz)");
}

// Writes SPZ (RUB) or PLY (converted from RUB to options.out_coords). Mutates the cloud.
splatcore::Status write_any(SplatCloud& cloud, const std::string& path, const Options& options) {
    if (cloud.sh_degree > 1) {
        return splatcore::Status::failure(
            "scene has SH degree " + std::to_string(cloud.sh_degree) +
            "; PocketSplat stores degree 0-1 only - pass --sh 0 or --sh 1 to truncate explicitly");
    }
    if (has_extension(path, ".spz")) return splatcore::write_spz(cloud, fs::path(path));
    if (has_extension(path, ".ply")) {
        splatcore::convert_coordinates(cloud, CoordSystem::kRub, options.out_coords);
        return splatcore::write_ply(cloud, fs::path(path));
    }
    return splatcore::Status::failure("unsupported output extension on '" + path + "' (.ply or .spz)");
}

// --- commands ---------------------------------------------------------------------------------

int cmd_inspect(const std::string& path) {
    splatcore::Result<SplatCloud> cloud = has_extension(path, ".spz") ? splatcore::read_spz(fs::path(path))
                                                                      : splatcore::read_ply(fs::path(path));
    if (!cloud) {
        std::fprintf(stderr, "splatconv: %s\n", cloud.error().c_str());
        return 1;
    }
    const splatcore::CloudStats stats = splatcore::compute_stats(cloud.value());
    std::string json;
    json += "{\n";
    json += "  \"file\": \"" + json_escape(path) + "\",\n";
    json += "  \"count\": " + std::to_string(stats.count) + ",\n";
    json += "  \"sh_degree\": " + std::to_string(stats.sh_degree) + ",\n";
    json += "  \"antialiased\": " + std::string(cloud.value().antialiased ? "true" : "false") + ",\n";
    json += "  \"nan_count\": " + std::to_string(stats.nan_count) + ",\n";
    json += "  \"bbox_min\": " + json_vec3(stats.bbox_min) + ",\n";
    json += "  \"bbox_max\": " + json_vec3(stats.bbox_max) + ",\n";
    json += "  \"opacity_histogram\": " + json_histogram(stats.opacity) + ",\n";
    json += "  \"scale_histogram\": " + json_histogram(stats.scale) + "\n";
    json += "}\n";
    std::fputs(json.c_str(), g_json);
    return 0;
}

int cmd_convert(const Options& options) {
    const std::string& in = options.positional[1];
    const std::string& out = options.positional[2];
    splatcore::Result<SplatCloud> cloud = read_any(in, options);
    if (!cloud) {
        std::fprintf(stderr, "splatconv: %s\n", cloud.error().c_str());
        return 1;
    }
    if (options.sh) splatcore::truncate_sh(cloud.value(), *options.sh);
    splatcore::Status status = write_any(cloud.value(), out, options);
    if (!status.ok()) {
        std::fprintf(stderr, "splatconv: %s\n", status.error().c_str());
        return 1;
    }
    std::fprintf(g_json, "{\"input\": \"%s\", \"output\": \"%s\", \"count\": %zu, \"sh_degree\": %d}\n",
                 json_escape(in).c_str(), json_escape(out).c_str(), cloud.value().count,
                 cloud.value().sh_degree);
    return 0;
}

int cmd_clean(const Options& options) {
    const std::string& in = options.positional[1];
    const std::string& out = options.positional[2];
    splatcore::Result<SplatCloud> read = read_any(in, options);
    if (!read) {
        std::fprintf(stderr, "splatconv: %s\n", read.error().c_str());
        return 1;
    }
    SplatCloud& cloud = read.value();
    const std::size_t input_count = cloud.count;

    std::vector<std::pair<splatcore::CleanOpStats, double>> timed;
    const auto run = [&](auto&& op) {
        const auto t0 = std::chrono::steady_clock::now();
        splatcore::CleanOpStats stats = op();
        const auto t1 = std::chrono::steady_clock::now();
        timed.emplace_back(std::move(stats), std::chrono::duration<double, std::milli>(t1 - t0).count());
    };

    run([&] { return splatcore::drop_invalid(cloud); });
    run([&] { return splatcore::prune_opacity(cloud, options.alpha); });
    run([&] {
        return splatcore::prune_scale(cloud, splatcore::robust_aabb(cloud), options.scale_frac,
                                      options.min_scale_frac);
    });
    if (!options.no_needle_clamp) {
        run([&] {
            return splatcore::clamp_needle_scales(cloud, splatcore::robust_aabb(cloud), options.needle_frac,
                                                  options.needle_aspect);
        });
    }
    if (!options.no_sor) {
        run([&] { return splatcore::remove_floaters_sor(cloud, options.sor_k, options.sor_m); });
    }
    run([&] { return splatcore::truncate_sh(cloud, options.sh.value_or(1)); });
    run([&] { return splatcore::prune_budget(cloud, options.budget); });
    if (!options.no_morton) {
        run([&] { return splatcore::reorder_morton(cloud, splatcore::robust_aabb(cloud)); });
    }

    splatcore::Status status = write_any(cloud, out, options);
    if (!status.ok()) {
        std::fprintf(stderr, "splatconv: %s\n", status.error().c_str());
        return 1;
    }

    std::string json;
    json += "{\n";
    json += "  \"input\": \"" + json_escape(in) + "\",\n";
    json += "  \"output\": \"" + json_escape(out) + "\",\n";
    json += "  \"input_count\": " + std::to_string(input_count) + ",\n";
    json += "  \"output_count\": " + std::to_string(cloud.count) + ",\n";
    json += "  \"output_sh_degree\": " + std::to_string(cloud.sh_degree) + ",\n";
    std::error_code ec;
    const auto out_size = fs::file_size(fs::path(out), ec);
    json += "  \"output_bytes\": " + std::to_string(ec ? 0 : out_size) + ",\n";
    json += "  \"ops\": [\n";
    for (std::size_t i = 0; i < timed.size(); ++i) {
        const auto& [stats, ms] = timed[i];
        json += "    {\"op\": \"" + stats.op + "\", \"input_count\": " + std::to_string(stats.input_count) +
                ", \"removed\": " + std::to_string(stats.removed) + ", \"params\": \"" +
                json_escape(stats.params) + "\", \"ms\": " + json_float(static_cast<float>(ms)) + "}";
        json += (i + 1 < timed.size()) ? ",\n" : "\n";
    }
    json += "  ]\n";
    json += "}\n";
    std::fputs(json.c_str(), g_json);
    if (!options.report_file.empty()) {
        std::ofstream file(options.report_file, std::ios::binary);
        file << json;
        if (!file) {
            std::fprintf(stderr, "splatconv: cannot write report '%s'\n", options.report_file.c_str());
            return 1;
        }
    }
    return 0;
}

int cmd_pack_dump_header(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(sizeof(splatcore::PackHeader));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        std::fprintf(stderr, "splatconv: cannot read 64-byte header from '%s'\n", path.c_str());
        return 1;
    }
    splatcore::Result<splatcore::PackHeader> parsed = splatcore::parse_pack_header(bytes);
    if (!parsed) {
        std::fprintf(stderr, "splatconv: %s\n", parsed.error().c_str());
        return 1;
    }
    const splatcore::PackHeader& h = parsed.value();
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(h.source_hash));
    std::error_code ec;
    const auto file_size = fs::file_size(fs::path(path), ec);
    std::string json;
    json += "{\n";
    json += "  \"file\": \"" + json_escape(path) + "\",\n";
    json += "  \"version\": " + std::to_string(h.version) + ",\n";
    json += "  \"count\": " + std::to_string(h.count) + ",\n";
    json += "  \"chunk_size\": " + std::to_string(h.chunk_size) + ",\n";
    json += "  \"sh_degree\": " + std::to_string(h.sh_degree) + ",\n";
    json += "  \"coords\": \"" + std::string(h.coords == splatcore::kPackCoordsRub ? "RUB" : "?") + "\",\n";
    json += "  \"sh1_scale\": " + json_float(h.sh1_scale) + ",\n";
    json += "  \"aabb_min\": " + json_vec3({h.aabb_min[0], h.aabb_min[1], h.aabb_min[2]}) + ",\n";
    json += "  \"aabb_max\": " + json_vec3({h.aabb_max[0], h.aabb_max[1], h.aabb_max[2]}) + ",\n";
    json += "  \"source_hash\": \"" + std::string(hash) + "\",\n";
    json += "  \"file_bytes\": " + std::to_string(ec ? 0 : file_size) + "\n";
    json += "}\n";
    std::fputs(json.c_str(), g_json);
    return 0;
}

int cmd_pack(const Options& options) {
    const std::string& in = options.positional[1];
    const std::string& out = options.positional[2];
    splatcore::Result<SplatCloud> read = read_any(in, options);
    if (!read) {
        std::fprintf(stderr, "splatconv: %s\n", read.error().c_str());
        return 1;
    }
    SplatCloud& cloud = read.value();
    if (options.sh) splatcore::truncate_sh(cloud, *options.sh);
    if (cloud.sh_degree > 1) {
        std::fprintf(stderr,
                     "splatconv: scene has SH degree %d; the pack stores degree 0-1 only - pass "
                     "--sh 0 or --sh 1 to truncate explicitly\n",
                     cloud.sh_degree);
        return 1;
    }
    if (!options.no_morton) splatcore::reorder_morton(cloud, splatcore::robust_aabb(cloud));
    splatcore::Result<splatcore::GpuPack> pack = splatcore::build_pack(cloud);
    if (!pack) {
        std::fprintf(stderr, "splatconv: %s\n", pack.error().c_str());
        return 1;
    }
    const std::vector<std::uint8_t> bytes = pack.value().serialize();
    std::ofstream file(out, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file.flush()) {
        std::fprintf(stderr, "splatconv: cannot write '%s'\n", out.c_str());
        return 1;
    }
    std::fprintf(g_json,
                 "{\"input\": \"%s\", \"output\": \"%s\", \"count\": %u, \"sh_degree\": %u, "
                 "\"chunks\": %zu, \"bytes\": %zu}\n",
                 json_escape(in).c_str(), json_escape(out).c_str(), pack.value().header.count,
                 pack.value().header.sh_degree, pack.value().chunk_count(), bytes.size());
    return 0;
}

// --- quality-controller replay ----------------------------------------------------------------

std::string json_number(double value) {
    if (!std::isfinite(value)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    return buf;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, separator)) parts.push_back(part);
    return parts;
}

// The Android bench CSV (android_host.cpp stop_bench): '# key=value, ...' comment lines, one
// header line, then one numeric row per frame. Columns of older recordings are a subset.
struct BenchCsv {
    std::map<std::string, std::string> meta;
    std::map<std::string, std::size_t> column;
    std::vector<std::vector<double>> rows;
};

splatcore::Result<BenchCsv> read_bench_csv(const std::string& path) {
    std::ifstream file(path);
    if (!file) return splatcore::Result<BenchCsv>::failure("cannot open '" + path + "'");
    BenchCsv csv;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            for (std::string part : split(line.substr(1), ',')) {
                const std::size_t eq = part.find('=');
                if (eq == std::string::npos) continue;
                const std::size_t begin = part.find_first_not_of(' ');
                csv.meta[part.substr(begin, eq - begin)] = part.substr(eq + 1);
            }
        } else if (csv.column.empty()) {
            const std::vector<std::string> names = split(line, ',');
            for (std::size_t i = 0; i < names.size(); ++i) csv.column[names[i]] = i;
        } else {
            std::vector<double> row;
            for (const std::string& field : split(line, ','))
                row.push_back(std::strtod(field.c_str(), nullptr));
            csv.rows.push_back(std::move(row));
        }
    }
    for (const char* required : {"t_s", "cpu_ms", "gpu_ms", "thermal_headroom"}) {
        if (csv.column.count(required) == 0) {
            return splatcore::Result<BenchCsv>::failure("'" + path + "' has no " + required + " column");
        }
    }
    return splatcore::Result<BenchCsv>::success(std::move(csv));
}

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Writes cameras as a 3DGS cameras.json (PLY world, RDF), for inspection of inferred cameras.
bool write_cameras_json(const std::string& path, const std::vector<splatcore::CaptureCamera>& cameras) {
    std::ofstream out(path);
    if (!out) return false;
    const float flip[3] = {1.0f, -1.0f, -1.0f};
    out << "[\n";
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        const splatcore::CaptureCamera& c = cameras[i];
        out << "{\"id\": " << i << ", \"img_name\": \"v" << i << "\", \"width\": " << c.width
            << ", \"height\": " << c.height << ", \"position\": [" << c.position[0] * flip[0] << ", "
            << c.position[1] * flip[1] << ", " << c.position[2] * flip[2] << "], \"rotation\": [";
        for (int r = 0; r < 3; ++r) {
            out << (r > 0 ? ", " : "") << "[" << c.right[r] * flip[r] << ", " << c.down[r] * flip[r] << ", "
                << c.forward[r] * flip[r] << "]";
        }
        out << "], \"fx\": " << c.fx << ", \"fy\": " << c.fy << "}" << (i + 1 < cameras.size() ? "," : "")
            << "\n";
    }
    out << "]\n";
    return static_cast<bool>(out.flush());
}

// coverage <scene> <camera files...> <out.pscov>   or   coverage <scene> <out.pscov> --infer room|object
int cmd_coverage(const Options& options) {
    const std::string& scene = options.positional[1];
    const std::string& out = options.positional.back();
    const std::vector<std::string> camera_paths(options.positional.begin() + 2, options.positional.end() - 1);
    if (camera_paths.empty() == options.infer.empty()) {
        std::fprintf(stderr, "splatconv: coverage needs camera files or --infer room|object (not both)\n");
        return 1;
    }
    splatcore::Result<splatcore::GpuPack> pack = splatcore::load_scene_pack(scene, 0);
    if (!pack) {
        std::fprintf(stderr, "splatconv: %s\n", pack.error().c_str());
        return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<splatcore::CaptureFile> files;
    for (const std::string& path : camera_paths) {
        splatcore::CaptureFile f;
        f.name = fs::path(path).filename().string();
        if (!read_file(path, f.bytes)) {
            std::fprintf(stderr, "splatconv: cannot read '%s'\n", path.c_str());
            return 1;
        }
        files.push_back(std::move(f));
    }
    const splatcore::CaptureSource source = options.infer.empty()     ? splatcore::CaptureSource::kCameras
                                            : options.infer == "room" ? splatcore::CaptureSource::kRoom
                                                                      : splatcore::CaptureSource::kObject;
    splatcore::Result<splatcore::CoverageSidecar> sidecar =
        splatcore::make_coverage_sidecar(pack.value(), source, files, options.capture);
    if (!sidecar) {
        std::fprintf(stderr, "splatconv: %s\n", sidecar.error().c_str());
        return 1;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!options.dump_cameras.empty() && !write_cameras_json(options.dump_cameras, sidecar.value().used)) {
        std::fprintf(stderr, "splatconv: cannot write '%s'\n", options.dump_cameras.c_str());
        return 1;
    }
    const std::vector<std::uint8_t>& bytes = sidecar.value().bytes;
    std::ofstream file(out, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file.flush()) {
        std::fprintf(stderr, "splatconv: cannot write '%s'\n", out.c_str());
        return 1;
    }
    std::vector<std::uint32_t> masks(pack.value().header.count);
    std::memcpy(masks.data(), bytes.data() + sizeof(splatcore::CoverageHeader),
                masks.size() * sizeof(std::uint32_t));
    // Observed cells per splat: 0 = never observed by any camera.
    std::vector<std::size_t> cells(splatcore::kCoverageGrid * splatcore::kCoverageGrid + 1, 0);
    for (std::uint32_t m : masks) ++cells[static_cast<std::size_t>(std::popcount(m))];
    std::string histogram;
    for (std::size_t k = 0; k < cells.size(); ++k)
        histogram += (k > 0 ? ", " : "") + std::to_string(cells[k]);
    const splatcore::HomeView home = splatcore::estimate_home_view(pack.value());
    const char* source_name = source == splatcore::CaptureSource::kCameras ? "cameras"
                              : source == splatcore::CaptureSource::kRoom  ? "room"
                                                                           : "object";
    std::fprintf(g_json,
                 "{\"scene\": \"%s\", \"source\": \"%s\", \"interpretation\": \"%s\", \"cameras\": %zu, "
                 "\"fit\": %.3f, \"count\": %u, \"output\": \"%s\", \"bytes\": %zu, \"seconds\": %.2f, "
                 "\"never_observed\": %zu, \"cells_histogram\": [%s], \"home_focus\": [%.6g, %.6g, %.6g], "
                 "\"home_up\": [%.6g, %.6g, %.6g], \"home_full_distance\": %.6g}\n",
                 json_escape(scene).c_str(), source_name, json_escape(sidecar.value().interpretation).c_str(),
                 sidecar.value().cameras, sidecar.value().fit, pack.value().header.count,
                 json_escape(out).c_str(), bytes.size(), seconds, cells[0], histogram.c_str(), home.focus[0],
                 home.focus[1], home.focus[2], home.up[0], home.up[1], home.up[2], home.full_distance);
    return 0;
}

int cmd_replay(const Options& options) {
    const std::string& path = options.positional[1];
    splatcore::Result<BenchCsv> read = read_bench_csv(path);
    if (!read) {
        std::fprintf(stderr, "splatconv: %s\n", read.error().c_str());
        return 1;
    }
    const BenchCsv& csv = read.value();
    const auto meta_float = [&](const char* key, float fallback) {
        const auto it = csv.meta.find(key);
        return it != csv.meta.end() ? std::stof(it->second) : fallback;
    };

    splatcore::QualityControllerConfig config;
    config.target_fps = options.target_fps.value_or(meta_float("target_fps", 0.0f));
    config.light_headroom = options.light.value_or(meta_float("light_headroom", config.light_headroom));
    config.max_gpu_mhz = options.max_mhz.value_or(static_cast<int>(meta_float("max_gpu_mhz", 0.0f)));
    if (options.frame_guard) {
        config.bad_windows_to_step = options.frame_guard->first;
        config.bad_window_span = options.frame_guard->second;
    } else if (const auto it = csv.meta.find("frame_guard"); it != csv.meta.end()) {  // the rule that ran
        const std::vector<std::string> parts = split(it->second, '/');
        if (parts.size() == 2) {
            config.bad_windows_to_step = std::stoi(parts[0]);
            config.bad_window_span = std::stoi(parts[1]);
        }
    }
    if (options.forecast_warmup) {
        config.forecast_warmup_s = *options.forecast_warmup;
    } else if (const auto it = csv.meta.find("forecast_warmup_s"); it != csv.meta.end()) {
        config.forecast_warmup_s = std::stod(it->second);
    } else if (const auto on = csv.meta.find("controller"); on != csv.meta.end() && on->second == "on") {
        config.forecast_warmup_s = 0.0;  // recorded before S20: the policy that ran had no warm-up
    }
    // ADR-0011 settings; controller runs recorded before them ran with ADR-0009's.
    const auto logged_on = csv.meta.find("controller");
    const bool controller_run = logged_on != csv.meta.end() && logged_on->second == "on";
    if (options.backoff_window) {
        config.backoff_window_s = *options.backoff_window;
    } else if (const auto it = csv.meta.find("backoff_window_s"); it != csv.meta.end()) {
        config.backoff_window_s = std::stod(it->second);
    } else if (controller_run) {
        config.backoff_window_s = 120.0;
    }
    if (options.step_up_current) {
        config.step_up_needs_current = *options.step_up_current;
    } else if (const auto it = csv.meta.find("step_up_current"); it != csv.meta.end()) {
        config.step_up_needs_current = it->second == "on";
    } else if (controller_run) {
        config.step_up_needs_current = false;
    }
    if (options.frame_guard_clock) {
        config.frame_guard_clock_fraction = *options.frame_guard_clock;
    } else if (const auto it = csv.meta.find("frame_guard_clock"); it != csv.meta.end()) {
        config.frame_guard_clock_fraction = std::stof(it->second);
    } else if (controller_run) {
        config.frame_guard_clock_fraction = 0.0f;
    }
    if (const auto it = csv.meta.find("ladder"); it != csv.meta.end()) {
        config.ladder.clear();
        for (const std::string& rung : split(it->second, '/')) config.ladder.push_back(std::stof(rung));
    }
    splatcore::QualityController controller(config);

    // Newer recordings mark the frames that took a thermal reading (1 = 30 s forecast, 2 = current
    // headroom; both read the clock) and log the controller's own rung and reason; older ones get a
    // reading per second of their current-headroom column.
    const auto col = [&](const char* name) -> std::optional<std::size_t> {
        const auto it = csv.column.find(name);
        return it != csv.column.end() ? std::optional<std::size_t>(it->second) : std::nullopt;
    };
    const std::size_t c_t = *col("t_s"), c_cpu = *col("cpu_ms"), c_gpu = *col("gpu_ms");
    const std::optional<std::size_t> c_mhz = col("gpu_mhz");  // S18 and later
    const std::optional<std::size_t> c_sample = col("thermal_sample");
    const std::optional<std::size_t> c_rung = col("rung");
    const std::optional<std::size_t> c_reason = col("reason");
    const std::size_t c_current = *col("thermal_headroom");
    const std::size_t c_forecast = col("thermal_forecast").value_or(c_current);
    const bool logged = c_rung && c_reason && !csv.rows.empty() && csv.rows.front()[*c_rung] >= 0.0;

    std::vector<double> time_at_rung(static_cast<std::size_t>(controller.rung_count()), 0.0);
    std::string decisions;
    std::size_t decision_count = 0, logged_count = 0, mismatches = 0;
    double last_tick = -1e300;
    for (std::size_t i = 0; i < csv.rows.size(); ++i) {
        const std::vector<double>& row = csv.rows[i];
        if (row.size() < csv.column.size()) continue;  // a truncated last line
        splatcore::QualitySample sample;
        sample.t_s = static_cast<float>(row[c_t]);  // the app feeds the float it prints
        sample.frame_interval_ms = static_cast<float>(row[c_cpu]);
        sample.gpu_ms = static_cast<float>(row[c_gpu]);
        const int code =
            c_sample ? static_cast<int>(row[*c_sample]) : (sample.t_s - last_tick >= 1.0 ? 1 : 0);
        if (code != 0) {
            last_tick = sample.t_s;
            if (c_mhz) sample.gpu_mhz = static_cast<int>(row[*c_mhz]);
            if (code == 1) sample.headroom_forecast = static_cast<float>(row[c_forecast]);
            if (code == 2 && c_sample) sample.headroom_current = static_cast<float>(row[c_current]);
        }
        if (i + 1 < csv.rows.size()) {
            time_at_rung[static_cast<std::size_t>(controller.rung())] += csv.rows[i + 1][c_t] - sample.t_s;
        }
        const int rung_before = controller.rung();
        const splatcore::QualityDecision decision = controller.update(sample);
        if (logged) {
            const auto logged_reason =
                static_cast<splatcore::QualityReason>(static_cast<int>(row[*c_reason]));
            if (logged_reason != splatcore::QualityReason::kNone) ++logged_count;
            if (logged_reason != decision.reason || static_cast<int>(row[*c_rung]) != rung_before)
                ++mismatches;
        }
        if (decision.reason == splatcore::QualityReason::kNone) continue;
        decisions += decision_count++ == 0 ? "\n" : ",\n";
        decisions += "    {\"frame\": " + std::to_string(i) + ", \"t_s\": " + json_number(sample.t_s) +
                     ", \"reason\": \"" + splatcore::reason_name(decision.reason) +
                     "\", \"rung\": " + std::to_string(decision.rung) +
                     ", \"min_contribution\": " + json_number(controller.min_contribution()) +
                     ", \"forecast\": " + json_number(controller.forecast()) +
                     ", \"current\": " + json_number(controller.current()) +
                     ", \"slope_per_min\": " + json_number(controller.slope_per_min()) +
                     ", \"clock_mhz\": " + json_number(controller.clock_mhz()) + "}";
    }

    std::string ladder, rung_times;
    for (std::size_t i = 0; i < config.ladder.size(); ++i) {
        ladder += (i > 0 ? ", " : "") + json_number(config.ladder[i]);
        rung_times += (i > 0 ? ", " : "") + json_number(time_at_rung[i]);
    }
    std::string json;
    json += "{\n";
    json += "  \"file\": \"" + json_escape(path) + "\",\n";
    json += "  \"frames\": " + std::to_string(csv.rows.size()) + ",\n";
    json += "  \"seconds\": " + json_number(csv.rows.empty() ? 0.0 : csv.rows.back()[c_t]) + ",\n";
    json += "  \"config\": {\"target_fps\": " + json_number(config.target_fps) +
            ", \"light_headroom\": " + json_number(config.light_headroom) +
            ", \"max_gpu_mhz\": " + std::to_string(config.max_gpu_mhz) + ", \"frame_guard\": \"" +
            std::to_string(config.bad_windows_to_step) + "/" + std::to_string(config.bad_window_span) +
            "\", \"forecast_warmup_s\": " + json_number(config.forecast_warmup_s) +
            ", \"backoff_window_s\": " + json_number(config.backoff_window_s) +
            ", \"step_up_current\": " + (config.step_up_needs_current ? "true" : "false") +
            ", \"frame_guard_clock\": " + json_number(config.frame_guard_clock_fraction) + ", \"ladder\": [" +
            ladder + "]},\n";
    json += "  \"forecast_column\": \"" +
            std::string(col("thermal_forecast") ? "thermal_forecast" : "thermal_headroom") + "\",\n";
    json += "  \"open_loop\": " + std::string(logged ? "false" : "true") + ",\n";
    if (logged) {
        json += "  \"logged_decisions\": " + std::to_string(logged_count) + ",\n";
        json += "  \"reproduces_log\": " + std::string(mismatches == 0 ? "true" : "false") + ",\n";
    }
    json += "  \"time_at_rung_s\": [" + rung_times + "],\n";
    json += "  \"decisions\": [" + decisions + (decision_count > 0 ? "\n  " : "") + "]\n";
    json += "}\n";
    std::fputs(json.c_str(), g_json);
    return logged && mismatches > 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::fflush(stdout);
    const int json_fd = PS_DUP(PS_FILENO(stdout));
    if (json_fd >= 0 && PS_DUP2(PS_FILENO(stderr), PS_FILENO(stdout)) >= 0) {
        if (FILE* json = PS_FDOPEN(json_fd, "w")) g_json = json;
    }
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_usage();
        return 0;
    }
    Options options;
    if (argc < 2 || !parse_options(argc, argv, 1, options)) {
        if (argc < 2) print_usage();
        return 1;
    }
    if (options.positional.empty()) {
        print_usage();
        return 1;
    }
    const std::string& command = options.positional[0];
    if (command == "inspect" && options.positional.size() == 2) return cmd_inspect(options.positional[1]);
    if (command == "convert" && options.positional.size() == 3) return cmd_convert(options);
    if (command == "clean" && options.positional.size() == 3) return cmd_clean(options);
    if (command == "pack" && options.dump_header && options.positional.size() == 2) {
        return cmd_pack_dump_header(options.positional[1]);
    }
    if (command == "pack" && options.positional.size() == 3) return cmd_pack(options);
    if (command == "coverage" && options.positional.size() >= 3 && options.positional.size() <= 6)
        return cmd_coverage(options);
    if (command == "replay" && options.positional.size() == 2) return cmd_replay(options);
    print_usage();
    return 1;
}
