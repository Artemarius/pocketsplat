#include "splatcore/home_view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <limits>
#include <vector>

namespace splatcore {
namespace {

constexpr std::size_t kMaxSamples = 500000;  // Morton order: a stride keeps the samples uniform
constexpr int kGrid = 32;
constexpr float kDistancePerExtent = 2.3f;       // eye distance / half-detail radius (13 INRIA scenes)
constexpr float kElevationRad = 0.2617994f;      // 15 deg above the fitted plane
constexpr float kHighElevationRad = 0.6108652f;  // 35 deg: the second choice
constexpr int kEyeCandidates = 8;
// View score raster (ADR-0014): resolution, sampled splats, "fine detail" radius, field of view.
constexpr int kScoreRes = 48;
constexpr std::size_t kScoreSamples = 100000;
constexpr float kSharpPx = 1.0f;
constexpr float kScoreFovRad = 1.0471976f;  // 60 deg, the app's FOV across the long side

struct Sample {
    float p[3];
    float alpha;
    float detail;  // alpha / mean scale^2
    float reach;   // 3 x the largest scale: how far from its center the splat still covers
};

float dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float dist2(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return dot3(d, d);
}

void normalize3(float* v) {
    const float len = std::sqrt(dot3(v, v));
    if (len > 0.0f) {
        for (int c = 0; c < 3; ++c) v[c] /= len;
    }
}

// xorshift32: a fixed seed keeps the RANSAC (and so the view) deterministic.
struct Random {
    std::uint32_t state = 0x9E3779B9u;
    std::uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};

std::vector<Sample> take_samples(const GpuPack& pack) {
    const std::size_t n = pack.header.count;
    const std::size_t stride = std::max<std::size_t>(1, (n + kMaxSamples - 1) / kMaxSamples);
    std::vector<Sample> samples;
    samples.reserve(n / stride + 1);
    for (std::size_t i = 0; i < n; i += stride) {
        Sample s;
        for (int c = 0; c < 3; ++c) s.p[c] = pack.positions[3 * i + static_cast<std::size_t>(c)];
        const std::uint32_t w0 = pack.scale_rot[3 * i + 0], w1 = pack.scale_rot[3 * i + 1];
        const float sx = std::exp(glm::unpackHalf1x16(static_cast<std::uint16_t>(w0 & 0xFFFFu)));
        const float sy = std::exp(glm::unpackHalf1x16(static_cast<std::uint16_t>(w0 >> 16)));
        const float sz = std::exp(glm::unpackHalf1x16(static_cast<std::uint16_t>(w1 & 0xFFFFu)));
        const float mean_scale = (sx + sy + sz) / 3.0f;
        s.alpha = static_cast<float>(pack.color[i] >> 24) / 255.0f;
        s.detail = mean_scale > 0.0f ? s.alpha / (mean_scale * mean_scale) : 0.0f;
        s.reach = 3.0f * std::max({sx, sy, sz});
        if (std::isfinite(s.detail) && std::isfinite(s.p[0] + s.p[1] + s.p[2])) samples.push_back(s);
    }
    return samples;
}

// What a view of `focus` from `eye` would show (ADR-0014): the sampled splats rastered nearest-first
// into a kScoreRes^2 image as squares of their 1-sigma radius, scored as the share of pixels whose
// front splat is fine detail (1-sigma radius <= kSharpPx). Large near splats (an eye inside or
// against geometry, floaters in front) and empty background both score 0; a focus hidden behind
// something (the median depth of the 4x4 centre block under 0.6 x the distance) halves the score.
struct ScoreSample {
    float p[3];
    float sigma;  // largest scale
};

float view_score(const std::vector<ScoreSample>& pts, const float* eye, const float* focus, const float* up,
                 std::vector<float>& depth, std::vector<float>& front) {
    float back[3] = {eye[0] - focus[0], eye[1] - focus[1], eye[2] - focus[2]};
    const float dist = std::sqrt(dot3(back, back));
    if (!(dist > 0.0f)) return 0.0f;
    for (float& b : back) b /= dist;
    float right[3] = {up[1] * back[2] - up[2] * back[1], up[2] * back[0] - up[0] * back[2],
                      up[0] * back[1] - up[1] * back[0]};
    if (dot3(right, right) < 1e-12f) right[0] = 1.0f, right[1] = right[2] = 0.0f;
    normalize3(right);
    const float cam_up[3] = {back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                             back[0] * right[1] - back[1] * right[0]};
    const float res = static_cast<float>(kScoreRes);
    const float f = 0.5f * res / std::tan(0.5f * kScoreFovRad);
    depth.assign(static_cast<std::size_t>(kScoreRes * kScoreRes), std::numeric_limits<float>::infinity());
    front.assign(depth.size(), std::numeric_limits<float>::infinity());
    const auto plot = [&](int x, int y, float z, float r) {
        const std::size_t k = static_cast<std::size_t>(y * kScoreRes + x);
        if (z < depth[k]) {
            depth[k] = z;
            front[k] = r;
        }
    };
    for (const ScoreSample& s : pts) {
        const float rel[3] = {s.p[0] - eye[0], s.p[1] - eye[1], s.p[2] - eye[2]};
        const float z = -dot3(rel, back);
        if (!(z > 1e-3f * dist)) continue;
        const float u = dot3(rel, right) / z * f + 0.5f * res;
        const float v = -dot3(rel, cam_up) / z * f + 0.5f * res;
        const float r = s.sigma / z * f;
        if (r <= 1.5f) {
            if (u >= 0.0f && u < res && v >= 0.0f && v < res)
                plot(static_cast<int>(u), static_cast<int>(v), z, r);
            continue;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(u - r)));
        const int x1 = std::min(kScoreRes - 1, static_cast<int>(std::floor(u + r)));
        const int y0 = std::max(0, static_cast<int>(std::floor(v - r)));
        const int y1 = std::min(kScoreRes - 1, static_cast<int>(std::floor(v + r)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) plot(x, y, z, r);
        }
    }
    std::size_t sharp = 0;
    for (std::size_t k = 0; k < depth.size(); ++k)
        sharp += std::isfinite(depth[k]) && front[k] <= kSharpPx ? 1u : 0u;
    float score = static_cast<float>(sharp) / static_cast<float>(depth.size());
    float centre[16];
    int n = 0;
    for (int y = kScoreRes / 2 - 2; y < kScoreRes / 2 + 2; ++y) {
        for (int x = kScoreRes / 2 - 2; x < kScoreRes / 2 + 2; ++x)
            centre[n++] = depth[static_cast<std::size_t>(y * kScoreRes + x)];
    }
    std::sort(centre, centre + 16);
    if (0.5f * (centre[7] + centre[8]) < 0.6f * dist) score *= 0.5f;
    return score;
}

// Dominant plane through opaque samples within `radius` of `center`: RANSAC on inlier count.
// Returns false when no plane holds at least 5 % of them.
bool dominant_plane(const std::vector<Sample>& samples, const float* center, float radius, float* normal) {
    std::vector<const Sample*> pts;
    for (const Sample& s : samples) {
        if (s.alpha > 0.5f && dist2(s.p, center) < radius * radius) pts.push_back(&s);
    }
    if (pts.size() < 64) return false;
    Random rng;
    const float tol = 0.01f * radius;
    std::size_t best = 0;
    float best_normal[3] = {0.0f, 1.0f, 0.0f};
    for (int it = 0; it < 400; ++it) {
        const float* a = pts[rng.next() % pts.size()]->p;
        const float* b = pts[rng.next() % pts.size()]->p;
        const float* c = pts[rng.next() % pts.size()]->p;
        const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                      ab[0] * ac[1] - ab[1] * ac[0]};
        if (dot3(n, n) < 1e-20f) continue;
        normalize3(n);
        const float offset = dot3(n, a);
        std::size_t inliers = 0;
        for (const Sample* s : pts) inliers += std::fabs(dot3(n, s->p) - offset) < tol ? 1u : 0u;
        if (inliers > best) {
            best = inliers;
            for (int k = 0; k < 3; ++k) best_normal[k] = n[k];
        }
    }
    if (best * 20 < pts.size()) return false;
    // The inputs' +y is up to within 90 deg (the viewer always assumed +y up): right on all 13
    // INRIA scenes, where "most splats above the plane" picks the wrong side on kitchen.
    const float sign = best_normal[1] < 0.0f ? -1.0f : 1.0f;
    for (int k = 0; k < 3; ++k) normal[k] = sign * best_normal[k];
    return true;
}

}  // namespace

HomeView estimate_home_view(const GpuPack& pack) {
    HomeView view{{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 1.0f, false};
    const std::vector<Sample> samples = take_samples(pack);
    if (samples.empty()) return view;

    // Robust box: per-axis 5th-95th percentile, as robust_framing.
    float lo[3], hi[3];
    std::vector<float> axis(samples.size());
    for (int c = 0; c < 3; ++c) {
        for (std::size_t i = 0; i < samples.size(); ++i) axis[i] = samples[i].p[c];
        const std::size_t p_lo = axis.size() / 20, p_hi = axis.size() - 1 - axis.size() / 20;
        std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(p_lo), axis.end());
        lo[c] = axis[p_lo];
        std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(p_hi), axis.end());
        hi[c] = axis[p_hi];
        if (hi[c] <= lo[c]) hi[c] = lo[c] + 1e-3f;
    }
    const float span[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    const float diagonal = std::max(std::sqrt(dot3(span, span)), 1e-3f);
    const auto inside = [&](const Sample& s) {
        return s.p[0] >= lo[0] && s.p[0] <= hi[0] && s.p[1] >= lo[1] && s.p[1] <= hi[1] && s.p[2] >= lo[2] &&
               s.p[2] <= hi[2];
    };

    // Focus: densest detail cell of a 32^3 grid (3^3 box-smoothed), then mean shift.
    std::vector<double> grid(static_cast<std::size_t>(kGrid * kGrid * kGrid), 0.0);
    const auto cell = [&](const float* p, int c) {
        return std::clamp(static_cast<int>((p[c] - lo[c]) / span[c] * kGrid), 0, kGrid - 1);
    };
    for (const Sample& s : samples) {
        if (!inside(s)) continue;
        grid[static_cast<std::size_t>((cell(s.p, 0) * kGrid + cell(s.p, 1)) * kGrid + cell(s.p, 2))] +=
            s.detail;
    }
    double best = -1.0;
    int peak[3] = {kGrid / 2, kGrid / 2, kGrid / 2};
    for (int x = 0; x < kGrid; ++x) {
        for (int y = 0; y < kGrid; ++y) {
            for (int z = 0; z < kGrid; ++z) {
                double sum = 0.0;
                for (int dx = -1; dx <= 1; ++dx) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dz = -1; dz <= 1; ++dz) {
                            const int gx = x + dx, gy = y + dy, gz = z + dz;
                            if (gx < 0 || gy < 0 || gz < 0 || gx >= kGrid || gy >= kGrid || gz >= kGrid)
                                continue;
                            sum += grid[static_cast<std::size_t>((gx * kGrid + gy) * kGrid + gz)];
                        }
                    }
                }
                if (sum > best) {
                    best = sum;
                    peak[0] = x, peak[1] = y, peak[2] = z;
                }
            }
        }
    }
    float focus[3];
    for (int c = 0; c < 3; ++c) focus[c] = lo[c] + (static_cast<float>(peak[c]) + 0.5f) / kGrid * span[c];
    const float bandwidth = 0.1f * diagonal;
    for (int it = 0; it < 20; ++it) {
        double sum[3] = {0.0, 0.0, 0.0}, weight = 0.0;
        for (const Sample& s : samples) {
            if (!inside(s) || dist2(s.p, focus) >= bandwidth * bandwidth) continue;
            for (int c = 0; c < 3; ++c) sum[c] += static_cast<double>(s.detail) * s.p[c];
            weight += s.detail;
        }
        if (weight <= 0.0) break;
        for (int c = 0; c < 3; ++c) focus[c] = static_cast<float>(sum[c] / weight);
    }

    // Half-detail radius around the focus.
    std::vector<std::pair<float, float>> by_distance;  // (distance^2, detail)
    double total = 0.0;
    for (const Sample& s : samples) {
        if (!inside(s)) continue;
        by_distance.emplace_back(dist2(s.p, focus), s.detail);
        total += s.detail;
    }
    std::sort(by_distance.begin(), by_distance.end());
    float extent = 0.1f * diagonal;
    double cumulative = 0.0;
    for (const auto& [d2, detail] : by_distance) {
        cumulative += detail;
        if (cumulative >= 0.5 * total) {
            extent = std::max(std::sqrt(d2), 1e-4f * diagonal);
            break;
        }
    }

    float up[3] = {0.0f, 1.0f, 0.0f};
    view.plane_found = dominant_plane(samples, focus, 2.0f * extent, up);
    const float full_distance = std::min(kDistancePerExtent * extent, diagonal);

    // Eye (ADR-0014): of 96 candidates looking at the focus - 8 sides x 15 / 35 deg above the plane
    // x 1, 0.7, 0.5, 0.35, 0.25, 0.18 of the full distance - the one whose view shows the most fine detail
    // (view_score); the first best in this order on a tie, so the result is deterministic.
    float h0[3] = {0.0f, 0.0f, 1.0f};
    if (std::fabs(dot3(h0, up)) > 0.9f) h0[0] = 1.0f, h0[2] = 0.0f;
    const float along = dot3(h0, up);
    for (int c = 0; c < 3; ++c) h0[c] -= along * up[c];
    normalize3(h0);
    const float h1[3] = {up[1] * h0[2] - up[2] * h0[1], up[2] * h0[0] - up[0] * h0[2],
                         up[0] * h0[1] - up[1] * h0[0]};
    std::vector<ScoreSample> pts;
    {
        const std::size_t stride = std::max<std::size_t>(1, samples.size() / kScoreSamples);
        for (std::size_t i = 0; i < samples.size(); i += stride) {
            const Sample& s = samples[i];
            if (s.alpha > 0.3f) pts.push_back({{s.p[0], s.p[1], s.p[2]}, s.reach / 3.0f});
        }
    }
    std::vector<float> depth, front;
    float best_eye[3] = {focus[0], focus[1] + full_distance, focus[2]};
    float best_distance = full_distance;
    float best_score = -1.0f;
    for (const float scale : {1.0f, 0.7f, 0.5f, 0.35f, 0.25f, 0.18f}) {
        const float distance = scale * full_distance;
        for (const float elevation : {kElevationRad, kHighElevationRad}) {
            const float ce = std::cos(elevation), se = std::sin(elevation);
            for (int k = 0; k < kEyeCandidates; ++k) {
                const float yaw = 6.2831853f * static_cast<float>(k) / kEyeCandidates;
                float eye[3];
                for (int c = 0; c < 3; ++c)
                    eye[c] = focus[c] +
                             distance * (ce * (std::cos(yaw) * h0[c] + std::sin(yaw) * h1[c]) + se * up[c]);
                const float score = view_score(pts, eye, focus, up, depth, front);
                if (score > best_score) {
                    best_score = score;
                    best_distance = distance;
                    std::memcpy(best_eye, eye, sizeof(eye));
                }
            }
        }
    }
    for (int c = 0; c < 3; ++c) {
        view.focus[c] = focus[c];
        view.up[c] = up[c];
        view.eye[c] = best_eye[c];
    }
    view.distance = best_distance;
    view.full_distance = full_distance;
    return view;
}

}  // namespace splatcore
