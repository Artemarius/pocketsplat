#include "splatcore/home_view.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "splatcore/pack.h"
#include "splatcore/splat_cloud.h"

namespace splatcore {
namespace {

constexpr float kPi = 3.14159265f;

// A capture-like scene in a tilted frame: a ground plane of flat opaque discs, a small dense
// object of tiny splats standing on it, and a far shell of large background splats - the shape
// of the INRIA 360 captures the estimator was validated on (docs/navigation.md).
struct Scene {
    float up[3];      // the ground's normal (the frame's +y)
    float object[3];  // object center
    GpuPack pack;
};

Scene make_scene(bool with_ground, bool with_wall = false) {
    const float tilt = 30.0f * kPi / 180.0f;  // frame rotated about +x
    const float c = std::cos(tilt), s = std::sin(tilt);
    const float offset[3] = {5.0f, -2.0f, 3.0f};
    const auto to_world = [&](float x, float y, float z, float* out) {
        out[0] = x + offset[0];
        out[1] = c * y - s * z + offset[1];
        out[2] = s * y + c * z + offset[2];
    };
    std::uint32_t rng = 12345u;
    const auto uniform = [&rng]() {  // [0, 1)
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.0f;
    };

    const std::size_t ground = with_ground ? 60000 : 0, object = 40000, shell = 30000;
    const std::size_t wall = with_wall ? 20000 : 0;
    SplatCloud cloud;
    cloud.resize(ground + object + shell + wall, 0);
    std::size_t i = 0;
    const auto put = [&](float x, float y, float z, float sx, float sy, float sz, float logit) {
        to_world(x, y, z, &cloud.positions[3 * i]);
        cloud.log_scales[3 * i + 0] = std::log(sx);
        cloud.log_scales[3 * i + 1] = std::log(sy);
        cloud.log_scales[3 * i + 2] = std::log(sz);
        // The frame's rotation about +x as (w, x, y, z): local y (the thin axis) -> the ground normal.
        cloud.rotations[4 * i + 0] = std::cos(0.5f * tilt);
        cloud.rotations[4 * i + 1] = std::sin(0.5f * tilt);
        cloud.opacities[i] = logit;
        ++i;
    };
    for (std::size_t k = 0; k < ground; ++k) {
        const float r = 4.0f * std::sqrt(uniform()), a = 2.0f * kPi * uniform();  // dense near the object
        put(r * std::cos(a), 0.0f, r * std::sin(a), 0.05f, 0.002f, 0.05f, 3.0f);
    }
    for (std::size_t k = 0; k < object; ++k) {  // a ball of radius 1 resting on the ground
        float x, y, z;
        do {
            x = 2.0f * uniform() - 1.0f, y = 2.0f * uniform() - 1.0f, z = 2.0f * uniform() - 1.0f;
        } while (x * x + y * y + z * z > 1.0f);
        put(x, 1.0f + y, z, 0.005f, 0.005f, 0.005f, 3.0f);
    }
    for (std::size_t k = 0; k < shell; ++k) {
        const float zc = 2.0f * uniform() - 1.0f, a = 2.0f * kPi * uniform(), rr = std::sqrt(1.0f - zc * zc);
        put(40.0f * rr * std::cos(a), 40.0f * zc, 40.0f * rr * std::sin(a), 0.3f, 0.3f, 0.3f, 1.5f);
    }

    for (std::size_t k = 0; k < wall; ++k) {  // a slab of large opaque blobs beside the object (+z)
        put(6.0f * uniform() - 3.0f, 3.0f * uniform(), 1.05f + 1.15f * uniform(), 0.4f, 0.4f, 0.4f, 3.0f);
    }

    Scene scene;
    to_world(0.0f, 1.0f, 0.0f, scene.object);
    scene.up[0] = 0.0f, scene.up[1] = c, scene.up[2] = s;
    scene.pack = build_pack(cloud).value();
    return scene;
}

float angle_deg(const float* a, const float* b) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    return std::acos(std::clamp(d / (la * lb), -1.0f, 1.0f)) * 180.0f / kPi;
}

TEST(HomeView, FramesTheObjectNotTheWholeScene) {
    const Scene scene = make_scene(true);
    const HomeView view = estimate_home_view(scene.pack);
    const float off[3] = {view.focus[0] - scene.object[0], view.focus[1] - scene.object[1],
                          view.focus[2] - scene.object[2]};
    EXPECT_LT(std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]), 0.3f);  // object radius 1
    // A whole-scene framing would start ~1.2 x the 80-unit shell diagonal out; this one sits at
    // object scale (2.3 x the half-detail radius of a unit ball, ~0.8).
    EXPECT_GT(view.distance, 1.0f);
    EXPECT_LT(view.distance, 4.0f);
}

TEST(HomeView, StartsLevelWithTheGroundAndAboveIt) {
    const Scene scene = make_scene(true);
    const HomeView view = estimate_home_view(scene.pack);
    ASSERT_TRUE(view.plane_found);
    EXPECT_LT(angle_deg(view.up, scene.up), 3.0f);
    const float dir[3] = {view.eye[0] - view.focus[0], view.eye[1] - view.focus[1],
                          view.eye[2] - view.focus[2]};
    EXPECT_NEAR(90.0f - angle_deg(dir, scene.up), 15.0f, 3.0f);  // elevation above the plane
    EXPECT_NEAR(std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]), view.distance, 1e-3f);
}

// ADR-0014: the eye is chosen by what its view shows. A thick slab of large blobs right beside the
// object on the frame's +z side holds or hides every candidate eye on that side (the closer ones
// are inside the object); the eye goes at least 60 deg away from it.
TEST(HomeView, PicksTheSideWhoseViewShowsDetail) {
    const float tilt = 30.0f * kPi / 180.0f;
    const float local_z[3] = {0.0f, -std::sin(tilt), std::cos(tilt)};  // the frame's +z in world
    const auto side = [&](const HomeView& view) {  // cos of the angle between the eye's side and +z
        const float d[3] = {view.eye[0] - view.focus[0], view.eye[1] - view.focus[1],
                            view.eye[2] - view.focus[2]};
        const float along = d[0] * local_z[0] + d[1] * local_z[1] + d[2] * local_z[2];
        const float up_part = d[1] * std::cos(tilt) + d[2] * std::sin(tilt);
        const float level =
            std::sqrt(std::max(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - up_part * up_part, 1e-12f));
        return along / level;
    };
    const Scene walled = make_scene(true, true);
    const HomeView view = estimate_home_view(walled.pack);
    const float off[3] = {view.focus[0] - walled.object[0], view.focus[1] - walled.object[1],
                          view.focus[2] - walled.object[2]};
    EXPECT_LT(std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]), 0.3f);
    EXPECT_LT(side(view), 0.5f);
}

TEST(HomeView, WithoutAPlaneUpIsPlusY) {
    const Scene scene = make_scene(false);
    const HomeView view = estimate_home_view(scene.pack);
    EXPECT_FALSE(view.plane_found);
    EXPECT_FLOAT_EQ(view.up[1], 1.0f);
}

TEST(HomeView, IsDeterministic) {
    const Scene scene = make_scene(true);
    const HomeView a = estimate_home_view(scene.pack), b = estimate_home_view(scene.pack);
    for (int c = 0; c < 3; ++c) {
        EXPECT_EQ(a.focus[c], b.focus[c]);
        EXPECT_EQ(a.eye[c], b.eye[c]);
        EXPECT_EQ(a.up[c], b.up[c]);
    }
}

TEST(HomeView, EmptyPackGivesTheDefault) {
    const HomeView view = estimate_home_view(GpuPack{});
    EXPECT_FALSE(view.plane_found);
    EXPECT_GT(view.distance, 0.0f);
}

}  // namespace
}  // namespace splatcore
