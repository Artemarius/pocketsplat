#include "splatcore/capture.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "splatcore/pack.h"
#include "splatcore/splat_cloud.h"

namespace splatcore {
namespace {

constexpr double kPi = 3.14159265358979;

// A camera in the PLY's RDF world: centre and camera-to-world rotation (columns = right, down,
// forward), as a 3DGS cameras.json stores it.
struct RdfCamera {
    double c[3];
    double c2w[3][3];
    double fx, fy;
    int w, h;
};

// Rotation about +y by `a` (camera-to-world).
RdfCamera camera_about_y(double a, double cx, double cy, double cz) {
    RdfCamera cam{{cx, cy, cz}, {{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}},
                  500.0,        510.0,
                  640,          480};
    return cam;
}

std::string cameras_json(const std::vector<RdfCamera>& cams) {
    std::string s = "[";
    for (std::size_t i = 0; i < cams.size(); ++i) {
        const RdfCamera& c = cams[i];
        s += (i ? "," : "") + std::string("{\"id\": ") + std::to_string(i) + ", \"position\": [" +
             std::to_string(c.c[0]) + "," + std::to_string(c.c[1]) + "," + std::to_string(c.c[2]) +
             "], \"rotation\": [";
        for (int r = 0; r < 3; ++r) {
            s += (r ? "," : "") + std::string("[") + std::to_string(c.c2w[r][0]) + "," +
                 std::to_string(c.c2w[r][1]) + "," + std::to_string(c.c2w[r][2]) + "]";
        }
        s += "], \"fx\": " + std::to_string(c.fx) + ", \"fy\": " + std::to_string(c.fy) +
             ", \"width\": " + std::to_string(c.w) + ", \"height\": " + std::to_string(c.h) + "}";
    }
    return s + "]";
}

// COLMAP stores world-to-camera: R = c2w^T, t = -R c, R as a quaternion (w, x, y, z).
void colmap_pose(const RdfCamera& c, double q[4], double t[3]) {
    double r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = c.c2w[j][i];
    const double tr = r[0][0] + r[1][1] + r[2][2];
    q[0] = 0.5 * std::sqrt(std::max(0.0, 1.0 + tr));
    q[1] = (r[2][1] - r[1][2]) / (4.0 * q[0]);
    q[2] = (r[0][2] - r[2][0]) / (4.0 * q[0]);
    q[3] = (r[1][0] - r[0][1]) / (4.0 * q[0]);
    for (int i = 0; i < 3; ++i) t[i] = -(r[i][0] * c.c[0] + r[i][1] * c.c[1] + r[i][2] * c.c[2]);
}

template <typename T>
void put(std::vector<std::uint8_t>& out, T v) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
}

std::vector<std::uint8_t> colmap_cameras_bin(const RdfCamera& c) {
    std::vector<std::uint8_t> b;
    put<std::uint64_t>(b, 1);
    put<std::uint32_t>(b, 7);  // camera id
    put<std::int32_t>(b, 1);   // PINHOLE
    put<std::uint64_t>(b, static_cast<std::uint64_t>(c.w));
    put<std::uint64_t>(b, static_cast<std::uint64_t>(c.h));
    for (double p : {c.fx, c.fy, c.w / 2.0, c.h / 2.0}) put<double>(b, p);
    return b;
}

std::vector<std::uint8_t> colmap_images_bin(const std::vector<RdfCamera>& cams) {
    std::vector<std::uint8_t> b;
    put<std::uint64_t>(b, cams.size());
    for (std::size_t i = 0; i < cams.size(); ++i) {
        double q[4], t[3];
        colmap_pose(cams[i], q, t);
        put<std::uint32_t>(b, static_cast<std::uint32_t>(i + 1));
        for (double v : q) put<double>(b, v);
        for (double v : t) put<double>(b, v);
        put<std::uint32_t>(b, 7);
        for (char ch : std::string("img.png")) put<char>(b, ch);
        put<char>(b, 0);
        put<std::uint64_t>(b, 2);  // two 2D points (x, y, point3D id) to skip
        for (int k = 0; k < 2; ++k) {
            put<double>(b, 1.5);
            put<double>(b, 2.5);
            put<std::int64_t>(b, -1);
        }
    }
    return b;
}

std::vector<std::uint8_t> bytes_of(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string colmap_cameras_txt(const RdfCamera& c) {
    return "# Camera list\n# CAMERA_ID, MODEL, WIDTH, HEIGHT, PARAMS[]\n7 PINHOLE " + std::to_string(c.w) +
           " " + std::to_string(c.h) + " " + std::to_string(c.fx) + " " + std::to_string(c.fy) + " 320 240\n";
}

std::string colmap_images_txt(const std::vector<RdfCamera>& cams) {
    std::string s = "# Image list\n#   IMAGE_ID, QW, QX, QY, QZ, TX, TY, TZ, CAMERA_ID, NAME\n";
    for (std::size_t i = 0; i < cams.size(); ++i) {
        double q[4], t[3];
        colmap_pose(cams[i], q, t);
        char line[512];
        std::snprintf(line, sizeof(line), "%zu %.12f %.12f %.12f %.12f %.12f %.12f %.12f 7 img%zu.png\n",
                      i + 1, q[0], q[1], q[2], q[3], t[0], t[1], t[2], i);
        s += line;
        s += i == 0 ? "10.0 20.0 -1 11.0 21.0 3\n" : "\n";  // the second image has no 2D points
    }
    return s;
}

std::string transforms_json(const std::vector<RdfCamera>& cams) {
    // OpenGL camera-to-world: columns right, up (= -down), back (= -forward), position.
    std::string s = "{\"fl_x\": 500, \"fl_y\": 510, \"w\": 640, \"h\": 480, \"frames\": [";
    for (std::size_t i = 0; i < cams.size(); ++i) {
        const RdfCamera& c = cams[i];
        s += (i ? "," : "") + std::string("{\"file_path\": \"images/") + std::to_string(i) +
             ".png\", \"transform_matrix\": [";
        for (int r = 0; r < 3; ++r) {
            s += "[" + std::to_string(c.c2w[r][0]) + "," + std::to_string(-c.c2w[r][1]) + "," +
                 std::to_string(-c.c2w[r][2]) + "," + std::to_string(c.c[r]) + "],";
        }
        s += "[0,0,0,1]]}";
    }
    return s + "]}";
}

void expect_same(const std::vector<CaptureCamera>& a, const std::vector<CaptureCamera>& b) {
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(a[i].position[k], b[i].position[k], 1e-4f) << "camera " << i;
            EXPECT_NEAR(a[i].right[k], b[i].right[k], 1e-4f) << "camera " << i;
            EXPECT_NEAR(a[i].down[k], b[i].down[k], 1e-4f) << "camera " << i;
            EXPECT_NEAR(a[i].forward[k], b[i].forward[k], 1e-4f) << "camera " << i;
        }
        EXPECT_NEAR(a[i].fx, b[i].fx, 1e-3f);
        EXPECT_NEAR(a[i].fy, b[i].fy, 1e-3f);
        EXPECT_EQ(a[i].width, b[i].width);
        EXPECT_EQ(a[i].height, b[i].height);
    }
}

std::vector<RdfCamera> two_cameras() {
    return {camera_about_y(0.0, 0.5, -1.0, -4.0), camera_about_y(kPi / 2.0, -4.0, -1.5, 0.25)};
}

TEST(Capture, ColmapBinaryTextAndCamerasJsonAgree) {
    const std::vector<RdfCamera> cams = two_cameras();
    const auto reference = parse_cameras_json(cameras_json(cams));
    ASSERT_TRUE(reference) << reference.error();
    const auto bin = parse_colmap(colmap_cameras_bin(cams[0]), colmap_images_bin(cams));
    ASSERT_TRUE(bin) << bin.error();
    expect_same(bin.value(), reference.value());
    const auto txt = parse_colmap(bytes_of(colmap_cameras_txt(cams[0])), bytes_of(colmap_images_txt(cams)));
    ASSERT_TRUE(txt) << txt.error();
    expect_same(txt.value(), reference.value());
    // RDF -> RUB: the first camera looks along RDF +z, which is RUB -z.
    EXPECT_NEAR(reference.value()[0].forward[2], -1.0f, 1e-6f);
    EXPECT_NEAR(reference.value()[0].position[1], 1.0f, 1e-6f);
}

TEST(Capture, TransformsJsonMatchesCamerasJson) {
    const std::vector<RdfCamera> cams = two_cameras();
    const auto reference = parse_cameras_json(cameras_json(cams));
    const auto transforms = parse_transforms_json(transforms_json(cams));
    ASSERT_TRUE(transforms) << transforms.error();
    expect_same(transforms.value(), reference.value());
    // camera_angle_x without a size: the Blender convention, 800 px wide.
    const auto blender = parse_transforms_json(
        "{\"camera_angle_x\": 0.6911112, \"frames\": [{\"transform_matrix\": "
        "[[1,0,0,0],[0,1,0,0],[0,0,1,4],[0,0,0,1]]}]}");
    ASSERT_TRUE(blender) << blender.error();
    EXPECT_EQ(blender.value()[0].width, 800u);
    EXPECT_NEAR(blender.value()[0].fx, 400.0f / std::tan(0.5f * 0.6911112f), 1e-2f);
}

TEST(Capture, TheFormatIsSniffedFromTheContent) {
    const std::vector<RdfCamera> cams = two_cameras();
    const auto reference = parse_cameras_json(cameras_json(cams)).value();
    const auto json = parse_capture_cameras({{"anything.json", bytes_of(cameras_json(cams))}});
    ASSERT_TRUE(json) << json.error();
    expect_same(json.value(), reference);
    const auto transforms = parse_capture_cameras({{"transforms.json", bytes_of(transforms_json(cams))}});
    ASSERT_TRUE(transforms) << transforms.error();
    expect_same(transforms.value(), reference);
    const auto named = parse_capture_cameras(
        {{"images.bin", colmap_images_bin(cams)}, {"cameras.bin", colmap_cameras_bin(cams[0])}});
    ASSERT_TRUE(named) << named.error();
    expect_same(named.value(), reference);
    // Unhelpful names: both orders are tried.
    const auto unnamed =
        parse_capture_cameras({{"a", colmap_images_bin(cams)}, {"b", colmap_cameras_bin(cams[0])}});
    ASSERT_TRUE(unnamed) << unnamed.error();
    expect_same(unnamed.value(), reference);
    EXPECT_FALSE(parse_capture_cameras({{"points3D.bin", colmap_images_bin(cams)}}));
    EXPECT_FALSE(parse_capture_cameras({}));
    // A truncated images.bin is an error, not a crash.
    std::vector<std::uint8_t> cut = colmap_images_bin(cams);
    cut.resize(cut.size() - 30);
    EXPECT_FALSE(parse_colmap(colmap_cameras_bin(cams[0]), cut));
}

// --- synthetic scenes for the fit check and the virtual capture ---

struct CloudBuilder {
    SplatCloud cloud;
    std::uint32_t rng = 987654321u;
    float uniform() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.0f;
    }
    // A flat disc (thin axis along `normal_axis` 0/1/2), opaque.
    void disc(float x, float y, float z, int normal_axis, float radius, float logit = 3.0f) {
        const std::size_t i = cloud.count;
        cloud.resize(i + 1, 0);
        cloud.positions[3 * i] = x, cloud.positions[3 * i + 1] = y, cloud.positions[3 * i + 2] = z;
        for (int k = 0; k < 3; ++k)
            cloud.log_scales[3 * i + k] = std::log(k == normal_axis ? 0.002f : radius);
        cloud.rotations[4 * i] = 1.0f;
        cloud.opacities[i] = logit;
    }
};

TEST(Capture, FitSeparatesMatchingFromForeignCameras) {
    CloudBuilder b;
    for (int k = 0; k < 2000; ++k)
        b.disc(b.uniform() - 0.5f, b.uniform() - 0.5f, -5.0f - b.uniform(), 2, 0.01f);
    const GpuPack pack = build_pack(b.cloud).value();
    CaptureCamera cam{};  // RUB: at the origin looking along -z
    cam.forward[2] = -1.0f, cam.right[0] = 1.0f, cam.down[1] = -1.0f;
    cam.fx = cam.fy = 500.0f, cam.width = 640, cam.height = 480;
    EXPECT_GT(capture_fit(pack, {cam}), 0.99f);
    cam.forward[2] = 1.0f, cam.down[1] = 1.0f;  // turned around
    EXPECT_LT(capture_fit(pack, {cam}), 0.01f);
}

// nerfstudio's ns-process-data re-orients the COLMAP world: x' = A x with A's rows (x, z, -y),
// recorded as `applied_transform`. A scene left in the COLMAP world needs A undone; the sidecar
// builder finds that by fit, and rejects the same cameras when nothing says how to undo it.
TEST(Capture, SidecarUndoesTheFrameThatFitsAndRejectsForeignCameras) {
    const RdfCamera cam = camera_about_y(0.3, 0.5, -1.0, -4.0);
    CloudBuilder b;  // splats 3-5 units in front of the camera, PLY world (RDF), stored RUB
    for (int k = 0; k < 3000; ++k) {
        const double z = 3.0 + 2.0 * b.uniform(), x = (b.uniform() - 0.5) * z * 0.8,
                     y = (b.uniform() - 0.5) * z * 0.6;
        double w[3];
        for (int r = 0; r < 3; ++r)
            w[r] = cam.c[r] + cam.c2w[r][0] * x + cam.c2w[r][1] * y + cam.c2w[r][2] * z;
        b.disc(static_cast<float>(w[0]), static_cast<float>(-w[1]), static_cast<float>(-w[2]), 2, 0.01f);
    }
    const GpuPack pack = build_pack(b.cloud).value();
    const double a[3][3] = {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
    const auto apply = [&](const double v[3], double sign, double out[3]) {
        for (int r = 0; r < 3; ++r) out[r] = sign * (a[r][0] * v[0] + a[r][1] * v[1] + a[r][2] * v[2]);
    };
    double right[3], down[3], fwd[3], pos[3], col[4][3];
    for (int r = 0; r < 3; ++r) right[r] = cam.c2w[r][0], down[r] = cam.c2w[r][1], fwd[r] = cam.c2w[r][2];
    apply(right, 1.0, col[0]);
    apply(down, -1.0, col[1]);  // OpenGL up
    apply(fwd, -1.0, col[2]);   // OpenGL back
    apply(cam.c, 1.0, pos);
    for (int r = 0; r < 3; ++r) col[3][r] = pos[r];
    std::string frame = "{\"transform_matrix\": [";
    for (int r = 0; r < 3; ++r) {
        frame += "[" + std::to_string(col[0][r]) + "," + std::to_string(col[1][r]) + "," +
                 std::to_string(col[2][r]) + "," + std::to_string(col[3][r]) + "],";
    }
    frame += "[0,0,0,1]]}";
    const std::string head =
        "{\"fl_x\": 500, \"fl_y\": 510, \"w\": 640, \"h\": 480, \"frames\": [" + frame + "]";
    const std::string with_applied = head + ", \"applied_transform\": [[1,0,0,0],[0,0,1,0],[0,-1,0,0]]}";
    const auto sidecar =
        make_coverage_sidecar(pack, CaptureSource::kCameras, {{"transforms.json", bytes_of(with_applied)}});
    ASSERT_TRUE(sidecar) << sidecar.error();
    EXPECT_EQ(sidecar.value().interpretation, "applied_transform undone");
    EXPECT_GT(sidecar.value().fit, 0.95f);
    EXPECT_EQ(sidecar.value().bytes.size(), 32u + 4u * pack.header.count);
    // Another scene's cameras (here: 50 units away) see none of it and are rejected.
    std::string far_frame = frame;
    far_frame.replace(far_frame.find(std::to_string(col[3][0])), std::to_string(col[3][0]).size(),
                      std::to_string(col[3][0] + 50.0));
    const std::string foreign_json =
        "{\"fl_x\": 500, \"fl_y\": 510, \"w\": 640, \"h\": 480, \"frames\": [" + far_frame + "]}";
    const auto foreign =
        make_coverage_sidecar(pack, CaptureSource::kCameras, {{"t.json", bytes_of(foreign_json)}});
    EXPECT_FALSE(foreign);
}

// A room: a 10 x 3 x 10 box of fine opaque wall, floor and ceiling discs (thin axis along the
// face normal), and detailed furniture inside. The capture walked through it.
GpuPack make_room() {
    CloudBuilder b;
    const auto face = [&](int axis, float at, int n) {
        for (int k = 0; k < n; ++k) {
            float p[3] = {10.0f * b.uniform() - 5.0f, 3.0f * b.uniform(), 10.0f * b.uniform() - 5.0f};
            p[axis] = at;
            b.disc(p[0], p[1], p[2], axis, 0.03f);
        }
    };
    for (float at : {-5.0f, 5.0f}) face(0, at, 15000), face(2, at, 15000);
    face(1, 0.0f, 25000);
    face(1, 3.0f, 25000);
    for (int k = 0; k < 15000; ++k) {  // two pieces of furniture
        const float side = k % 2 == 0 ? -2.0f : 2.0f;
        b.disc(side + b.uniform() - 0.5f, 0.8f * b.uniform(), side + b.uniform() - 0.5f, 1, 0.01f);
    }
    return build_pack(b.cloud).value();
}

TEST(Capture, RoomCamerasStayInsideAndWallBacksFade) {
    const GpuPack room = make_room();
    const std::vector<CaptureCamera> cams = infer_capture_cameras(room, CaptureSource::kRoom);
    ASSERT_FALSE(cams.empty());
    EXPECT_EQ(cams.size() % 6, 0u);  // omnidirectional: 6 faces per position
    for (const CaptureCamera& c : cams) {
        EXPECT_LT(std::fabs(c.position[0]), 5.0f);
        EXPECT_LT(std::fabs(c.position[2]), 5.0f);
        EXPECT_GT(c.position[1], 0.0f);
        EXPECT_LT(c.position[1], 3.0f);
    }
    // The masks: a wall disc is observed from the room side only, so a viewer behind the wall
    // (looking in from outside) fades it, and a viewer inside keeps it.
    const std::vector<std::uint32_t> masks = compute_coverage(room, cams);
    const float cos_full = std::cos(35.0f * 3.14159265f / 180.0f), cos_zero = 0.0f;
    std::size_t walls = 0, kept_inside = 0, faded_outside = 0;
    for (std::size_t i = 0; i < room.header.count; ++i) {
        const float* p = &room.positions[3 * i];
        if (std::fabs(std::fabs(p[0]) - 5.0f) > 1e-3f || std::fabs(p[2]) > 4.0f || p[1] < 0.5f || p[1] > 2.5f)
            continue;  // x walls, away from the corners
        ++walls;
        const float out[3] = {p[0] > 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
        const float in[3] = {-out[0], 0.0f, 0.0f};
        kept_inside += coverage_match(masks[i], in, cos_full, cos_zero) > 0.5f ? 1u : 0u;
        faded_outside += coverage_match(masks[i], out, cos_full, cos_zero) < 0.01f ? 1u : 0u;
    }
    ASSERT_GT(walls, 1000u);
    EXPECT_GT(kept_inside, walls * 9 / 10);
    EXPECT_GT(faded_outside, walls * 9 / 10);
}

TEST(Capture, ObjectRingSurroundsTheObjectAndLooksAtIt) {
    CloudBuilder b;
    for (int k = 0; k < 40000; ++k) {  // ground
        const float r = 6.0f * std::sqrt(b.uniform()), a = 6.2831853f * b.uniform();
        b.disc(r * std::cos(a), 0.0f, r * std::sin(a), 1, 0.05f);
    }
    for (int k = 0; k < 30000; ++k) {  // a ball of fine detail resting on it
        float x, y, z;
        do {
            x = 2.0f * b.uniform() - 1.0f, y = 2.0f * b.uniform() - 1.0f, z = 2.0f * b.uniform() - 1.0f;
        } while (x * x + y * y + z * z > 1.0f);
        b.disc(x, 1.0f + y, z, 0, 0.005f);
    }
    const GpuPack pack = build_pack(b.cloud).value();
    const std::vector<CaptureCamera> cams = infer_capture_cameras(pack, CaptureSource::kObject);
    ASSERT_GT(cams.size(), 32u);
    for (const CaptureCamera& c : cams) {
        const float to_ball[3] = {0.0f - c.position[0], 1.0f - c.position[1], 0.0f - c.position[2]};
        const float d =
            std::sqrt(to_ball[0] * to_ball[0] + to_ball[1] * to_ball[1] + to_ball[2] * to_ball[2]);
        EXPECT_GT(d, 1.0f);              // outside the ball
        EXPECT_GE(c.position[1], 0.0f);  // above the ground
        const float facing =
            (to_ball[0] * c.forward[0] + to_ball[1] * c.forward[1] + to_ball[2] * c.forward[2]) / d;
        EXPECT_GT(facing, 0.95f);
    }
}

}  // namespace
}  // namespace splatcore
