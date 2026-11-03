#include "splatcore/scene_import.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "splatcore/pack.h"
#include "splatcore/ply_io.h"
#include "splatcore/spz_io.h"

namespace splatcore {
namespace {

namespace fs = std::filesystem;

fs::path temp_file(const std::string& name) {
    return fs::temp_directory_path() / ("pocketsplat_import_" + name);
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.flush()) << path;
}

// Cloud whose splats all survive the default cleaning pipeline (finite fields, opacity well above
// the prune threshold, scales inside the scene-relative bounds, aspect below the needle cap).
SplatCloud make_survivable_cloud(std::size_t n, int degree) {
    SplatCloud cloud;
    cloud.resize(n, degree);
    const int rest = sh_rest_coeffs(degree);
    for (std::size_t i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i);
        for (int c = 0; c < 3; ++c) {
            cloud.positions[3 * i + c] = 2.0f * fi + static_cast<float>(c);
            cloud.log_scales[3 * i + c] = -2.0f - 0.1f * static_cast<float>(c);
            cloud.dc[3 * i + c] = 0.02f * fi - 0.5f;
        }
        cloud.opacities[i] = 2.0f;
        cloud.rotations[4 * i + 0] = 1.0f;
        cloud.rotations[4 * i + 1] = 0.0f;
        cloud.rotations[4 * i + 2] = 0.0f;
        cloud.rotations[4 * i + 3] = 0.0f;
        for (int k = 0; k < 3 * rest; ++k) {
            cloud.sh[3 * static_cast<std::size_t>(rest) * i + static_cast<std::size_t>(k)] = 0.1f;
        }
    }
    return cloud;
}

// INRIA-layout binary PLY bytes for make_survivable_cloud-style values at any degree 0-3 (the
// writer library only emits 0-1, and import must be tested against a real degree-2 source).
std::string ply_bytes(std::size_t n, int degree) {
    const int rest = sh_rest_coeffs(degree);
    std::string out = "ply\nformat binary_little_endian 1.0\n";
    out += "element vertex " + std::to_string(n) + "\n";
    for (const char* p : {"x", "y", "z"}) out += std::string("property float ") + p + "\n";
    for (int i = 0; i < 3; ++i) out += "property float f_dc_" + std::to_string(i) + "\n";
    for (int k = 0; k < 3 * rest; ++k) out += "property float f_rest_" + std::to_string(k) + "\n";
    out += "property float opacity\n";
    for (int i = 0; i < 3; ++i) out += "property float scale_" + std::to_string(i) + "\n";
    for (int i = 0; i < 4; ++i) out += "property float rot_" + std::to_string(i) + "\n";
    out += "end_header\n";
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<float> row;
        const float fi = static_cast<float>(i);
        for (int c = 0; c < 3; ++c) row.push_back(2.0f * fi + static_cast<float>(c));
        for (int c = 0; c < 3; ++c) row.push_back(0.02f * fi - 0.5f);
        for (int k = 0; k < 3 * rest; ++k) row.push_back(0.1f);
        row.push_back(2.0f);
        for (int c = 0; c < 3; ++c) row.push_back(-2.0f - 0.1f * static_cast<float>(c));
        row.push_back(1.0f);
        row.push_back(0.0f);
        row.push_back(0.0f);
        row.push_back(0.0f);
        out.append(reinterpret_cast<const char*>(row.data()), row.size() * sizeof(float));
    }
    return out;
}

TEST(SceneProbeTest, PlyHeaderOnly) {
    const fs::path path = temp_file("probe.ply");
    write_bytes(path, ply_bytes(37, 2));
    Result<SceneProbe> probe = probe_scene(path);
    fs::remove(path);
    ASSERT_TRUE(probe.ok()) << probe.error();
    EXPECT_EQ(probe.value().kind, "ply");
    EXPECT_EQ(probe.value().count, 37u);
    EXPECT_EQ(probe.value().sh_degree, 2);
    EXPECT_GT(probe.value().file_bytes, 0u);
}

TEST(SceneProbeTest, SpzNgspContainer) {
    // spz v3.0.0's saveSpz writes the NGSP v4 container (raw 32-byte header at the file start).
    const SplatCloud cloud = make_survivable_cloud(29, 1);
    const fs::path path = temp_file("probe_ngsp.spz");
    ASSERT_TRUE(write_spz(cloud, path).ok());
    Result<SceneProbe> probe = probe_scene(path);
    fs::remove(path);
    ASSERT_TRUE(probe.ok()) << probe.error();
    EXPECT_EQ(probe.value().kind, "spz");
    EXPECT_EQ(probe.value().count, 29u);
    EXPECT_EQ(probe.value().sh_degree, 1);
}

TEST(SceneProbeTest, SpzLegacyGzipContainer) {
    // Hand-built legacy container: gzip stream whose decompressed head is the 16-byte v2 header.
    std::vector<std::uint8_t> plain(16 + 64, 0xAB);  // header + dummy payload
    const std::uint32_t magic = 0x5053474e, version = 2, num_points = 123456;
    std::memcpy(plain.data() + 0, &magic, 4);
    std::memcpy(plain.data() + 4, &version, 4);
    std::memcpy(plain.data() + 8, &num_points, 4);
    plain[12] = 3;   // shDegree
    plain[13] = 12;  // fractionalBits
    plain[14] = 1;   // flags (antialiased)
    plain[15] = 0;

    z_stream stream{};
    ASSERT_EQ(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY),
              Z_OK);
    std::vector<std::uint8_t> packed(deflateBound(&stream, static_cast<uLong>(plain.size())) + 64);
    stream.next_in = plain.data();
    stream.avail_in = static_cast<uInt>(plain.size());
    stream.next_out = packed.data();
    stream.avail_out = static_cast<uInt>(packed.size());
    ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
    packed.resize(packed.size() - stream.avail_out);
    deflateEnd(&stream);

    const fs::path path = temp_file("probe_legacy.spz");
    write_bytes(path, std::string(reinterpret_cast<const char*>(packed.data()), packed.size()));
    Result<SceneProbe> probe = probe_scene(path);
    fs::remove(path);
    ASSERT_TRUE(probe.ok()) << probe.error();
    EXPECT_EQ(probe.value().kind, "spz");
    EXPECT_EQ(probe.value().count, 123456u);
    EXPECT_EQ(probe.value().sh_degree, 3);
}

TEST(SceneProbeTest, PackHeader) {
    SplatCloud cloud = make_survivable_cloud(41, 0);
    Result<GpuPack> pack = build_pack(cloud);
    ASSERT_TRUE(pack.ok()) << pack.error();
    const std::vector<std::uint8_t> bytes = pack.value().serialize();
    const fs::path path = temp_file("probe.pspk");
    write_bytes(path, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    Result<SceneProbe> probe = probe_scene(path);
    fs::remove(path);
    ASSERT_TRUE(probe.ok()) << probe.error();
    EXPECT_EQ(probe.value().kind, "pspk");
    EXPECT_EQ(probe.value().count, 41u);
    EXPECT_EQ(probe.value().sh_degree, 0);
}

TEST(SceneProbeTest, RejectsJunk) {
    for (const char* name : {"junk.ply", "junk.spz", "junk.pspk"}) {
        const fs::path path = temp_file(name);
        write_bytes(path, "this is not a splat scene, whatever the extension says");
        Result<SceneProbe> probe = probe_scene(path);
        fs::remove(path);
        EXPECT_FALSE(probe.ok()) << name;
    }
    EXPECT_FALSE(probe_scene(temp_file("missing_nonexistent.ply")).ok());
}

TEST(SceneImportTest, PlyDegree2EndToEnd) {
    const fs::path src = temp_file("import.ply");
    const fs::path dst = temp_file("import.pspk");
    write_bytes(src, ply_bytes(32, 2));

    std::vector<ImportStage> stages;
    ImportOptions options;
    options.progress = [&](ImportStage stage) {
        stages.push_back(stage);
        return true;
    };
    Result<ImportReport> report = import_scene(src, dst, options);
    fs::remove(src);
    ASSERT_TRUE(report.ok()) << report.error();
    EXPECT_EQ(report.value().input_count, 32u);
    EXPECT_EQ(report.value().count, 32u);  // survivable values: nothing pruned
    EXPECT_EQ(report.value().sh_degree, 1);
    const auto& ops = report.value().ops;
    EXPECT_TRUE(
        std::any_of(ops.begin(), ops.end(), [](const CleanOpStats& s) { return s.op == "truncate_sh"; }));
    EXPECT_FALSE(std::any_of(ops.begin(), ops.end(),
                             [](const CleanOpStats& s) { return s.op == "remove_floaters_sor"; }));
    EXPECT_EQ(stages, (std::vector<ImportStage>{ImportStage::kRead, ImportStage::kClean, ImportStage::kPack,
                                                ImportStage::kWrite}));

    // The written pack is a valid .pspk with the truncated degree.
    Result<SceneProbe> probe = probe_scene(dst);
    fs::remove(dst);
    ASSERT_TRUE(probe.ok()) << probe.error();
    EXPECT_EQ(probe.value().kind, "pspk");
    EXPECT_EQ(probe.value().count, 32u);
    EXPECT_EQ(probe.value().sh_degree, 1);
}

TEST(SceneImportTest, BudgetPrune) {
    const fs::path src = temp_file("budget.ply");
    const fs::path dst = temp_file("budget.pspk");
    write_bytes(src, ply_bytes(64, 0));
    ImportOptions options;
    options.max_splats = 16;
    Result<ImportReport> report = import_scene(src, dst, options);
    fs::remove(src);
    fs::remove(dst);
    ASSERT_TRUE(report.ok()) << report.error();
    EXPECT_EQ(report.value().count, 16u);
}

TEST(SceneImportTest, DeepCleanRunsSor) {
    const fs::path src = temp_file("deep.ply");
    const fs::path dst = temp_file("deep.pspk");
    write_bytes(src, ply_bytes(64, 0));
    std::vector<ImportStage> stages;
    ImportOptions options;
    options.deep_clean = true;
    options.progress = [&](ImportStage stage) {
        stages.push_back(stage);
        return true;
    };
    Result<ImportReport> report = import_scene(src, dst, options);
    fs::remove(src);
    fs::remove(dst);
    ASSERT_TRUE(report.ok()) << report.error();
    EXPECT_TRUE(std::any_of(report.value().ops.begin(), report.value().ops.end(),
                            [](const CleanOpStats& s) { return s.op == "remove_floaters_sor"; }));
    EXPECT_NE(std::find(stages.begin(), stages.end(), ImportStage::kSor), stages.end());
}

TEST(SceneImportTest, CancelLeavesNoOutput) {
    const fs::path src = temp_file("cancel.ply");
    const fs::path dst = temp_file("cancel.pspk");
    write_bytes(src, ply_bytes(8, 0));
    ImportOptions options;
    options.progress = [](ImportStage stage) { return stage != ImportStage::kPack; };
    Result<ImportReport> report = import_scene(src, dst, options);
    fs::remove(src);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.error(), "import cancelled");
    EXPECT_FALSE(fs::exists(dst));
}

TEST(SceneImportTest, RefusesOversizedInput) {
    const fs::path src = temp_file("huge.ply");
    write_bytes(src, ply_bytes(8, 0));
    ImportOptions options;
    options.refuse_above = 4;
    Result<ImportReport> report = import_scene(src, temp_file("huge.pspk"), options);
    fs::remove(src);
    ASSERT_FALSE(report.ok());
    EXPECT_NE(report.error().find("import limit"), std::string::npos);
}

TEST(SceneImportTest, RefusesPackInput) {
    SplatCloud cloud = make_survivable_cloud(8, 0);
    Result<GpuPack> pack = build_pack(cloud);
    ASSERT_TRUE(pack.ok());
    const std::vector<std::uint8_t> bytes = pack.value().serialize();
    const fs::path src = temp_file("already.pspk");
    write_bytes(src, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    Result<ImportReport> report = import_scene(src, temp_file("already_out.pspk"), ImportOptions{});
    fs::remove(src);
    ASSERT_FALSE(report.ok());
    EXPECT_NE(report.error().find("already a pack"), std::string::npos);
}

TEST(SceneImportTest, EmptyResultFails) {
    // All opacities far below the prune threshold: cleaning removes everything.
    SplatCloud cloud = make_survivable_cloud(8, 0);
    for (float& o : cloud.opacities) o = -10.0f;
    const fs::path src = temp_file("empty.spz");
    ASSERT_TRUE(write_spz(cloud, src).ok());
    Result<ImportReport> report = import_scene(src, temp_file("empty.pspk"), ImportOptions{});
    fs::remove(src);
    ASSERT_FALSE(report.ok());
    EXPECT_NE(report.error().find("survived"), std::string::npos);
}

}  // namespace
}  // namespace splatcore
