// palettebench - headless SH1 palette trainer bench (Phase 8 B, docs/sh1-palette-research.md §4).
//
// Trains palettes with every combination of the given trainers, sizes, epochs, seeds and caps
// over one scene's SH1 vectors and writes one CSV row per run: loss (MSE in SH1 space), dead
// entries, used-code entropy, distance pairs evaluated, and wall times. The same binary runs on
// the desktop (losses; wall times there are indicative only) and on the phones (wall times: pushed
// to /data/local/tmp like sortbench).
//
// Input: a scene (.ply, RDF -> RUB as splatconv does; .spz), truncated to SH 1 and put in Morton
// order exactly like `splatconv pack`, so the vectors are the ones an import trains on; or a raw
// .sh1 dump of those vectors (--write-sh1), which is what goes to the phone.
//
// .sh1 layout (little-endian): "PSH1", u32 version = 1, u32 count, u32 dims = 9, then count x 9
// float32, coefficient-major per splat (SplatCloud::sh at degree 1).

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "splatcore/cleaning.h"
#include "splatcore/coords.h"
#include "splatcore/palette.h"
#include "splatcore/ply_io.h"
#include "splatcore/spz_io.h"

namespace {

namespace fs = std::filesystem;
using splatcore::PaletteTrainer;

constexpr char kMagic[4] = {'P', 'S', 'H', '1'};
constexpr std::uint32_t kVersion = 1;

void usage() {
    std::fprintf(stderr,
                 "palettebench - SH1 palette trainer bench (Phase 8 B)\n"
                 "\n"
                 "  palettebench <scene.ply|scene.spz|vectors.sh1> [options]\n"
                 "\n"
                 "  --write-sh1 FILE      write the SH1 vectors (pack order) to FILE and exit\n"
                 "  --trainer LIST        lloyd,hamerly,emeans (default emeans)\n"
                 "  --k LIST              palette sizes (default 4096)\n"
                 "  --epochs LIST         epochs per run (default 3)\n"
                 "  --seed LIST           seeds (default 1)\n"
                 "  --cap LIST            training cap in points per palette entry, 0 = all (default 0)\n"
                 "  --threads N           training threads (default 0 = all hardware threads)\n"
                 "  --batch B             e-means batch size (default 0 = scaling law)\n"
                 "  --alpha0 A            e-means initial step (default 0 = scaling law)\n"
                 "  --repeat R            run each configuration R times (default 1)\n"
                 "  --csv FILE            append rows to FILE (header written if new); default stdout\n"
                 "  --label TEXT          free text for the 'label' column (machine, build, ...)\n"
                 "\n"
                 "LIST is comma separated. The CSV row of each run is also printed to stderr.\n");
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t comma = s.find(',', start);
        const std::size_t end = comma == std::string::npos ? s.size() : comma;
        if (end > start) out.push_back(s.substr(start, end - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (s.empty() || end == nullptr || *end != '\0') return false;
    out = v;
    return true;
}

bool parse_list(const std::string& s, std::vector<std::uint64_t>& out) {
    out.clear();
    for (const std::string& item : split(s)) {
        std::uint64_t v = 0;
        if (!parse_u64(item, v)) return false;
        out.push_back(v);
    }
    return !out.empty();
}

bool has_ext(const std::string& path, const char* ext) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ext;
}

// SH1 vectors of a scene in pack order (see the file comment).
bool load_scene_sh1(const std::string& path, std::vector<float>& sh1, std::string& error) {
    splatcore::Result<splatcore::SplatCloud> read =
        has_ext(path, ".ply")   ? splatcore::read_ply(fs::path(path))
        : has_ext(path, ".spz") ? splatcore::read_spz(fs::path(path))
                                : splatcore::Result<splatcore::SplatCloud>::failure(
                                      "unsupported input extension (.ply, .spz, .sh1)");
    if (!read) {
        error = read.error();
        return false;
    }
    splatcore::SplatCloud& cloud = read.value();
    if (has_ext(path, ".ply")) {
        splatcore::convert_coordinates(cloud, splatcore::CoordSystem::kRdf, splatcore::CoordSystem::kRub);
    }
    if (cloud.sh_degree < 1) {
        error = "scene has no SH1 coefficients (degree 0)";
        return false;
    }
    if (cloud.sh_degree > 1) splatcore::truncate_sh(cloud, 1);
    splatcore::reorder_morton(cloud, splatcore::robust_aabb(cloud));
    sh1 = std::move(cloud.sh);
    return true;
}

bool read_sh1(const std::string& path, std::vector<float>& sh1, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    std::uint32_t header[3] = {};
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || std::memcmp(magic, kMagic, 4) != 0 || header[0] != kVersion ||
        header[2] != splatcore::kSh1Dims) {
        error = "not a version-1 .sh1 file: " + path;
        return false;
    }
    sh1.resize(static_cast<std::size_t>(header[1]) * splatcore::kSh1Dims);
    in.read(reinterpret_cast<char*>(sh1.data()), static_cast<std::streamsize>(sh1.size() * sizeof(float)));
    if (!in) {
        error = "truncated .sh1 file: " + path;
        return false;
    }
    return true;
}

bool write_sh1(const std::string& path, const std::vector<float>& sh1) {
    std::ofstream out(path, std::ios::binary);
    const std::uint32_t header[3] = {kVersion, static_cast<std::uint32_t>(sh1.size() / splatcore::kSh1Dims),
                                     static_cast<std::uint32_t>(splatcore::kSh1Dims)};
    out.write(kMagic, 4);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(sh1.data()),
              static_cast<std::streamsize>(sh1.size() * sizeof(float)));
    return static_cast<bool>(out.flush());
}

const char* kCsvHeader =
    "label,input,count,trainer,k,epochs,seed,cap_per_entry,train_count,threads,batch,alpha0,updates,revived,"
    "epochs_run,pairs,mse,dead,used_entropy_bits,train_ms,wall_ms\n";

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 1 : 0;
    }
    const std::string input = argv[1];
    std::string write_path, csv_path, label;
    std::vector<PaletteTrainer> trainers = {PaletteTrainer::kEMeans};
    std::vector<std::uint64_t> ks = {4096}, epochs = {3}, seeds = {1}, caps = {0};
    std::uint64_t threads = 0, batch = 0, repeat = 1;
    double alpha0 = 0.0;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            std::fprintf(stderr, "palettebench: %s needs a value\n", arg.c_str());
            return 1;
        }
        const std::string value = argv[++i];
        bool ok = true;
        if (arg == "--write-sh1") {
            write_path = value;
        } else if (arg == "--csv") {
            csv_path = value;
        } else if (arg == "--label") {
            label = value;
        } else if (arg == "--trainer") {
            trainers.clear();
            for (const std::string& t : split(value)) {
                if (t == "lloyd")
                    trainers.push_back(PaletteTrainer::kLloyd);
                else if (t == "hamerly")
                    trainers.push_back(PaletteTrainer::kHamerly);
                else if (t == "emeans")
                    trainers.push_back(PaletteTrainer::kEMeans);
                else
                    ok = false;
            }
            ok = ok && !trainers.empty();
        } else if (arg == "--k") {
            ok = parse_list(value, ks);
            for (std::uint64_t k : ks) ok = ok && k >= 1 && k <= splatcore::kMaxPaletteSize;
        } else if (arg == "--epochs") {
            ok = parse_list(value, epochs);
        } else if (arg == "--seed") {
            ok = parse_list(value, seeds);
        } else if (arg == "--cap") {
            ok = parse_list(value, caps);
        } else if (arg == "--threads") {
            ok = parse_u64(value, threads);
        } else if (arg == "--batch") {
            ok = parse_u64(value, batch);
        } else if (arg == "--repeat") {
            ok = parse_u64(value, repeat) && repeat >= 1;
        } else if (arg == "--alpha0") {
            char* end = nullptr;
            alpha0 = std::strtod(value.c_str(), &end);
            ok = end != nullptr && *end == '\0' && alpha0 >= 0.0;
        } else {
            std::fprintf(stderr, "palettebench: unknown option %s\n", arg.c_str());
            return 1;
        }
        if (!ok) {
            std::fprintf(stderr, "palettebench: bad value for %s: %s\n", arg.c_str(), value.c_str());
            return 1;
        }
    }

    std::vector<float> sh1;
    std::string error;
    const bool loaded =
        has_ext(input, ".sh1") ? read_sh1(input, sh1, error) : load_scene_sh1(input, sh1, error);
    if (!loaded) {
        std::fprintf(stderr, "palettebench: %s\n", error.c_str());
        return 1;
    }
    const std::size_t count = sh1.size() / splatcore::kSh1Dims;
    if (!write_path.empty()) {
        if (!write_sh1(write_path, sh1)) {
            std::fprintf(stderr, "palettebench: cannot write %s\n", write_path.c_str());
            return 1;
        }
        std::fprintf(stderr, "palettebench: wrote %zu SH1 vectors to %s\n", count, write_path.c_str());
        return 0;
    }

    std::ofstream csv_file;
    if (!csv_path.empty()) {
        const bool fresh = !fs::exists(csv_path) || fs::file_size(csv_path) == 0;
        csv_file.open(csv_path, std::ios::binary | std::ios::app);
        if (!csv_file) {
            std::fprintf(stderr, "palettebench: cannot open %s\n", csv_path.c_str());
            return 1;
        }
        if (fresh) csv_file << kCsvHeader << std::flush;
    } else {
        std::fputs(kCsvHeader, stdout);
    }
    const std::string input_name = fs::path(input).filename().string();
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    int status = 0;
    for (const std::uint64_t k : ks) {
        for (const std::uint64_t cap : caps) {
            for (const PaletteTrainer trainer : trainers) {
                for (const std::uint64_t e : epochs) {
                    for (const std::uint64_t seed : seeds) {
                        for (std::uint64_t r = 0; r < repeat; ++r) {
                            splatcore::PaletteParams params;
                            params.k = static_cast<std::uint32_t>(k);
                            params.epochs = static_cast<std::uint32_t>(e);
                            params.trainer = trainer;
                            params.seed = seed;
                            params.threads = static_cast<std::uint32_t>(threads);
                            params.batch = static_cast<std::uint32_t>(batch);
                            params.alpha0 = alpha0;
                            const std::uint64_t cap_points = cap * k;
                            params.train_cap =
                                cap == 0 || cap_points >= count ? 0u : static_cast<std::uint32_t>(cap_points);
                            const splatcore::Result<splatcore::Sh1Palette> p =
                                splatcore::train_sh1_palette(sh1.data(), count, params);
                            if (!p) {
                                std::fprintf(stderr, "palettebench: %s\n", p.error().c_str());
                                status = 1;
                                continue;
                            }
                            const splatcore::PaletteStats& s = p.value().stats;
                            char row[1024];
                            std::snprintf(row, sizeof(row),
                                          "%s,%s,%zu,%s,%u,%u,%" PRIu64 ",%" PRIu64
                                          ",%zu,%u,%u,%.6g,%u,%u,%u,%" PRIu64 ",%.9g,%u,%.6g,%.1f,%.1f\n",
                                          label.c_str(), input_name.c_str(), count,
                                          splatcore::palette_trainer_name(trainer), p.value().size(),
                                          params.epochs, seed, cap,
                                          params.train_cap != 0 ? std::size_t{params.train_cap} : count,
                                          params.threads != 0 ? params.threads : hw, s.batch, s.alpha0,
                                          s.updates, s.revived, s.epochs_run, s.pairs, s.mse, s.dead,
                                          s.used_entropy, s.train_ms, s.wall_ms);
                            if (csv_file.is_open()) {
                                csv_file << row << std::flush;
                                std::fputs(row, stderr);
                            } else {
                                std::fputs(row, stdout);
                                std::fflush(stdout);
                            }
                        }
                    }
                }
            }
        }
    }
    return status;
}
