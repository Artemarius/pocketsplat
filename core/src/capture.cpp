#include "splatcore/capture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <limits>
#include <sstream>

#include "json_reader.h"
#include "splatcore/home_view.h"

namespace splatcore {
namespace {

using detail::JsonReader;
using detail::JsonValue;

constexpr float kRdfToRub[3] = {1.0f, -1.0f, -1.0f};  // negate y and z, as the PLY importer does

float dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void cross3(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalize3(float* v) {
    const float len = std::sqrt(dot3(v, v));
    if (!(len > 0.0f) || !std::isfinite(len)) return false;
    for (int c = 0; c < 3; ++c) v[c] /= len;
    return true;
}

// Camera from a position and camera-to-world axes given in the source (RDF) world.
CaptureCamera rdf_camera(const double pos[3], const double right[3], const double down[3],
                         const double fwd[3], double fx, double fy, double width, double height) {
    CaptureCamera cam;
    for (int k = 0; k < 3; ++k) {
        cam.position[k] = static_cast<float>(pos[k]) * kRdfToRub[k];
        cam.right[k] = static_cast<float>(right[k]) * kRdfToRub[k];
        cam.down[k] = static_cast<float>(down[k]) * kRdfToRub[k];
        cam.forward[k] = static_cast<float>(fwd[k]) * kRdfToRub[k];
    }
    cam.fx = static_cast<float>(fx);
    cam.fy = static_cast<float>(fy);
    cam.width = static_cast<std::uint32_t>(width);
    cam.height = static_cast<std::uint32_t>(height);
    return cam;
}

bool valid_intrinsics(double fx, double fy, double w, double h) {
    return fx > 0.0 && fy > 0.0 && w >= 1.0 && h >= 1.0 && w <= 1e6 && h <= 1e6 && std::isfinite(fx) &&
           std::isfinite(fy);
}

// Row-major 3x4 affine [R | t] with a uniform scale applied after it: x' = scale * (R x + t).
struct Affine {
    double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    double scale = 1.0;
};

bool read_affine(const JsonValue* v, Affine& out) {
    if (v == nullptr || v->kind != JsonValue::Kind::kArray || v->items.size() < 3) return false;
    for (int r = 0; r < 3; ++r) {
        const JsonValue& row = v->items[static_cast<std::size_t>(r)];
        if (row.kind != JsonValue::Kind::kArray || row.items.size() < 4) return false;
        for (int c = 0; c < 4; ++c) {
            if (row.items[static_cast<std::size_t>(c)].kind != JsonValue::Kind::kNumber) return false;
            out.m[r][c] = row.items[static_cast<std::size_t>(c)].number;
        }
    }
    return true;
}

// Inverse of a rigid [R | t] (R orthonormal up to scale: the inverse uses R^T / |row|^2).
Affine inverse_rigid(const Affine& a) {
    Affine inv;
    for (int r = 0; r < 3; ++r) {
        const double n2 = a.m[r][0] * a.m[r][0] + a.m[r][1] * a.m[r][1] + a.m[r][2] * a.m[r][2];
        for (int c = 0; c < 3; ++c) inv.m[c][r] = n2 > 0.0 ? a.m[r][c] / n2 : 0.0;
    }
    for (int r = 0; r < 3; ++r) {
        inv.m[r][3] = -(inv.m[r][0] * a.m[0][3] + inv.m[r][1] * a.m[1][3] + inv.m[r][2] * a.m[2][3]);
    }
    return inv;
}

// One camera of a transforms.json in its own world, before any interpretation.
struct GlCamera {
    double c2w[3][4];  // OpenGL camera axes: x right, y up, z back
    double fx, fy, w, h;
};

std::vector<CaptureCamera> transforms_cameras(const std::vector<GlCamera>& gl, const Affine& world) {
    std::vector<CaptureCamera> out;
    out.reserve(gl.size());
    for (const GlCamera& g : gl) {
        double pos[3], axis[3][3];  // axis 0 right, 1 down, 2 forward (OpenCV), source world
        for (int r = 0; r < 3; ++r) {
            pos[r] = g.c2w[r][3];
            axis[0][r] = g.c2w[r][0];
            axis[1][r] = -g.c2w[r][1];
            axis[2][r] = -g.c2w[r][2];
        }
        double p2[3], a2[3][3];
        for (int r = 0; r < 3; ++r) {
            p2[r] = world.scale * (world.m[r][0] * pos[0] + world.m[r][1] * pos[1] + world.m[r][2] * pos[2] +
                                   world.m[r][3]);
            for (int k = 0; k < 3; ++k)
                a2[k][r] =
                    world.m[r][0] * axis[k][0] + world.m[r][1] * axis[k][1] + world.m[r][2] * axis[k][2];
        }
        for (auto& a : a2) {
            const double len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (len > 0.0)
                for (double& x : a) x /= len;
        }
        out.push_back(rdf_camera(p2, a2[0], a2[1], a2[2], g.fx, g.fy, g.w, g.h));
    }
    return out;
}

Result<std::vector<GlCamera>> parse_gl_cameras(const JsonValue& root) {
    using R = Result<std::vector<GlCamera>>;
    const JsonValue* frames = root.field("frames");
    if (frames == nullptr || frames->kind != JsonValue::Kind::kArray)
        return R::failure("transforms.json: no \"frames\" array");
    const auto number = [](const JsonValue& o, const char* key, double fallback) {
        double v = fallback;
        detail::read_number(o, key, v);
        return v;
    };
    // Global intrinsics; frames may override them.
    const double g_w = number(root, "w", 0.0), g_h = number(root, "h", 0.0);
    const double g_angle = number(root, "camera_angle_x", 0.0);
    const double g_fx = number(root, "fl_x", 0.0), g_fy = number(root, "fl_y", 0.0);
    std::vector<GlCamera> out;
    for (std::size_t i = 0; i < frames->items.size(); ++i) {
        const JsonValue& f = frames->items[i];
        const std::string where = "transforms.json: frame " + std::to_string(i) + ": ";
        if (f.kind != JsonValue::Kind::kObject) return R::failure(where + "not an object");
        const JsonValue* m = f.field("transform_matrix");
        if (m == nullptr || m->kind != JsonValue::Kind::kArray || m->items.size() < 3)
            return R::failure(where + "missing transform_matrix");
        GlCamera g;
        for (int r = 0; r < 3; ++r) {
            const JsonValue& row = m->items[static_cast<std::size_t>(r)];
            if (row.kind != JsonValue::Kind::kArray || row.items.size() < 4)
                return R::failure(where + "transform_matrix rows must hold 4 numbers");
            for (int c = 0; c < 4; ++c) {
                const JsonValue& e = row.items[static_cast<std::size_t>(c)];
                if (e.kind != JsonValue::Kind::kNumber)
                    return R::failure(where + "non-numeric transform_matrix");
                g.c2w[r][c] = e.number;
            }
        }
        // Without a size the Blender synthetic convention applies (800 x 800 images).
        g.w = number(f, "w", g_w > 0.0 ? g_w : 800.0);
        g.h = number(f, "h", g_h > 0.0 ? g_h : g.w);
        const double angle = number(f, "camera_angle_x", g_angle);
        double fx = number(f, "fl_x", g_fx);
        if (!(fx > 0.0) && angle > 0.0) fx = 0.5 * g.w / std::tan(0.5 * angle);
        double fy = number(f, "fl_y", g_fy > 0.0 ? g_fy : fx);
        if (!(fy > 0.0)) fy = fx;
        g.fx = fx;
        g.fy = fy;
        if (!valid_intrinsics(g.fx, g.fy, g.w, g.h))
            return R::failure(where + "missing or invalid intrinsics (fl_x/fl_y or camera_angle_x, w, h)");
        out.push_back(g);
    }
    return R::success(std::move(out));
}

// --- COLMAP sparse model (https://colmap.github.io/format.html) ---

int colmap_param_count(int model) {
    // SIMPLE_PINHOLE, PINHOLE, SIMPLE_RADIAL, RADIAL, OPENCV, OPENCV_FISHEYE, FULL_OPENCV, FOV,
    // SIMPLE_RADIAL_FISHEYE, RADIAL_FISHEYE, THIN_PRISM_FISHEYE
    static constexpr int kCounts[] = {3, 4, 4, 5, 8, 8, 12, 5, 4, 5, 12};
    return model >= 0 && model < 11 ? kCounts[model] : -1;
}

bool colmap_single_focal(int model) {
    return model == 0 || model == 2 || model == 3 || model == 8 || model == 9;
}

int colmap_model_id(const std::string& name) {
    static const char* kNames[] = {"SIMPLE_PINHOLE",
                                   "PINHOLE",
                                   "SIMPLE_RADIAL",
                                   "RADIAL",
                                   "OPENCV",
                                   "OPENCV_FISHEYE",
                                   "FULL_OPENCV",
                                   "FOV",
                                   "SIMPLE_RADIAL_FISHEYE",
                                   "RADIAL_FISHEYE",
                                   "THIN_PRISM_FISHEYE"};
    for (int i = 0; i < 11; ++i)
        if (name == kNames[i]) return i;
    return -1;
}

struct ColmapIntrinsics {
    std::uint32_t id;
    double fx, fy, w, h;
};

struct ColmapImage {
    double q[4];  // world-to-camera rotation (w, x, y, z)
    double t[3];
    std::uint32_t camera_id;
};

class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& bytes) : m_bytes(bytes) {}
    template <typename T>
    bool read(T& out) {
        if (m_pos + sizeof(T) > m_bytes.size()) return false;
        std::memcpy(&out, m_bytes.data() + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return true;
    }
    bool skip(std::uint64_t n) {
        if (n > m_bytes.size() - m_pos) return false;
        m_pos += static_cast<std::size_t>(n);
        return true;
    }
    bool skip_cstring() {
        while (m_pos < m_bytes.size()) {
            if (m_bytes[m_pos++] == 0) return true;
        }
        return false;
    }

private:
    const std::vector<std::uint8_t>& m_bytes;
    std::size_t m_pos = 0;
};

bool looks_binary(const std::vector<std::uint8_t>& bytes) {
    const std::size_t n = std::min<std::size_t>(bytes.size(), 64);
    for (std::size_t i = 0; i < n; ++i) {
        if (bytes[i] == 0) return true;
    }
    return false;
}

Result<std::vector<ColmapIntrinsics>> colmap_cameras_bin(const std::vector<std::uint8_t>& bytes) {
    using R = Result<std::vector<ColmapIntrinsics>>;
    ByteReader in(bytes);
    std::uint64_t count = 0;
    if (!in.read(count) || count > 1000000) return R::failure("cameras.bin: bad header");
    std::vector<ColmapIntrinsics> out;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint32_t id = 0;
        std::int32_t model = 0;
        std::uint64_t w = 0, h = 0;
        if (!in.read(id) || !in.read(model) || !in.read(w) || !in.read(h))
            return R::failure("cameras.bin: truncated");
        const int np = colmap_param_count(model);
        if (np < 0) return R::failure("cameras.bin: unknown camera model " + std::to_string(model));
        double params[12] = {};
        for (int k = 0; k < np; ++k) {
            if (!in.read(params[k])) return R::failure("cameras.bin: truncated");
        }
        const double fx = params[0], fy = colmap_single_focal(model) ? params[0] : params[1];
        out.push_back({id, fx, fy, static_cast<double>(w), static_cast<double>(h)});
    }
    return R::success(std::move(out));
}

Result<std::vector<ColmapImage>> colmap_images_bin(const std::vector<std::uint8_t>& bytes) {
    using R = Result<std::vector<ColmapImage>>;
    ByteReader in(bytes);
    std::uint64_t count = 0;
    if (!in.read(count) || count > 10000000) return R::failure("images.bin: bad header");
    std::vector<ColmapImage> out;
    out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, 100000)));
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint32_t id = 0;
        ColmapImage img;
        if (!in.read(id)) return R::failure("images.bin: truncated");
        for (double& q : img.q) {
            if (!in.read(q)) return R::failure("images.bin: truncated");
        }
        for (double& t : img.t) {
            if (!in.read(t)) return R::failure("images.bin: truncated");
        }
        std::uint64_t points = 0;
        if (!in.read(img.camera_id) || !in.skip_cstring() || !in.read(points) ||
            points > std::numeric_limits<std::uint64_t>::max() / 24 || !in.skip(points * 24)) {
            return R::failure("images.bin: truncated");
        }
        out.push_back(img);
    }
    return R::success(std::move(out));
}

std::vector<std::string> text_lines(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::string> lines;
    std::string line;
    for (std::uint8_t b : bytes) {
        if (b == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(static_cast<char>(b));
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

Result<std::vector<ColmapIntrinsics>> colmap_cameras_txt(const std::vector<std::uint8_t>& bytes) {
    using R = Result<std::vector<ColmapIntrinsics>>;
    std::vector<ColmapIntrinsics> out;
    for (const std::string& line : text_lines(bytes)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::uint32_t id = 0;
        std::string model;
        double w = 0.0, h = 0.0;
        if (!(s >> id >> model >> w >> h)) return R::failure("cameras.txt: malformed line '" + line + "'");
        const int m = colmap_model_id(model);
        if (m < 0) return R::failure("cameras.txt: unknown camera model " + model);
        double params[12] = {};
        for (int k = 0; k < colmap_param_count(m); ++k) {
            if (!(s >> params[k])) return R::failure("cameras.txt: too few parameters for " + model);
        }
        out.push_back({id, params[0], colmap_single_focal(m) ? params[0] : params[1], w, h});
    }
    return R::success(std::move(out));
}

Result<std::vector<ColmapImage>> colmap_images_txt(const std::vector<std::uint8_t>& bytes) {
    using R = Result<std::vector<ColmapImage>>;
    std::vector<ColmapImage> out;
    const std::vector<std::string> lines = text_lines(bytes);
    // Each image is two lines: the pose line and its (possibly empty) 2D points line.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::uint32_t id = 0;
        ColmapImage img;
        if (!(s >> id >> img.q[0] >> img.q[1] >> img.q[2] >> img.q[3] >> img.t[0] >> img.t[1] >> img.t[2] >>
              img.camera_id)) {
            return R::failure("images.txt: malformed image line '" + line.substr(0, 80) + "'");
        }
        out.push_back(img);
        ++i;  // skip the points line
    }
    return R::success(std::move(out));
}

std::vector<CaptureCamera> colmap_to_cameras(const std::vector<ColmapIntrinsics>& intrinsics,
                                             const std::vector<ColmapImage>& images) {
    std::vector<CaptureCamera> out;
    for (const ColmapImage& img : images) {
        const auto it = std::find_if(intrinsics.begin(), intrinsics.end(),
                                     [&](const ColmapIntrinsics& c) { return c.id == img.camera_id; });
        if (it == intrinsics.end() || !valid_intrinsics(it->fx, it->fy, it->w, it->h)) continue;
        double qw = img.q[0], qx = img.q[1], qy = img.q[2], qz = img.q[3];
        const double qn = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
        if (!(qn > 0.0)) continue;
        qw /= qn, qx /= qn, qy /= qn, qz /= qn;
        // World-to-camera rotation; its rows are the camera axes (right, down, forward) in world.
        const double r[3][3] = {
            {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy)},
            {2 * (qx * qy + qw * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qw * qx)},
            {2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), 1 - 2 * (qx * qx + qy * qy)}};
        double pos[3];
        for (int k = 0; k < 3; ++k) pos[k] = -(r[0][k] * img.t[0] + r[1][k] * img.t[1] + r[2][k] * img.t[2]);
        out.push_back(rdf_camera(pos, r[0], r[1], r[2], it->fx, it->fy, it->w, it->h));
    }
    return out;
}

bool name_has(const std::string& name, const char* word) {
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find(word) != std::string::npos;
}

// --- Virtual capture ---

struct Occupancy {
    int g = 0;
    float lo[3] = {0.0f, 0.0f, 0.0f};
    float vox = 1.0f;
    std::vector<std::uint8_t> cells;

    std::size_t index(int x, int y, int z) const {
        return (static_cast<std::size_t>(x) * static_cast<std::size_t>(g) + static_cast<std::size_t>(y)) *
                   static_cast<std::size_t>(g) +
               static_cast<std::size_t>(z);
    }
    bool cell_of(const float* p, int out[3]) const {
        for (int c = 0; c < 3; ++c) {
            const float t = std::floor((p[c] - lo[c]) / vox);
            if (!(t >= 0.0f && t < static_cast<float>(g))) return false;
            out[c] = static_cast<int>(t);
        }
        return true;
    }
    void centre(int x, int y, int z, float out[3]) const {
        const int i[3] = {x, y, z};
        for (int c = 0; c < 3; ++c) out[c] = lo[c] + (static_cast<float>(i[c]) + 0.5f) * vox;
    }
};

void decode_shape(const GpuPack& pack, std::size_t i, float scale[3], float axes[3][3]) {
    const std::uint32_t w0 = pack.scale_rot[3 * i], w1 = pack.scale_rot[3 * i + 1],
                        w2 = pack.scale_rot[3 * i + 2];
    scale[0] = std::exp(glm::unpackHalf1x16(static_cast<std::uint16_t>(w0 & 0xFFFFu)));
    scale[1] = std::exp(glm::unpackHalf1x16(static_cast<std::uint16_t>(w0 >> 16)));
    scale[2] = std::exp(glm::unpackHalf1x16(static_cast<std::uint16_t>(w1 & 0xFFFFu)));
    const auto snorm = [](std::uint32_t b) {
        return std::max(static_cast<float>(static_cast<std::int8_t>(b & 0xFFu)) / 127.0f, -1.0f);
    };
    float qw = snorm(w1 >> 16), qx = snorm(w1 >> 24), qy = snorm(w2), qz = snorm(w2 >> 8);
    const float qn = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
    if (qn > 0.0f)
        qw /= qn, qx /= qn, qy /= qn, qz /= qn;
    else
        qw = 1.0f;
    // Columns of the rotation = the splat's local axes in world.
    const float m[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy)},
                           {2 * (qx * qy + qw * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qw * qx)},
                           {2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), 1 - 2 * (qx * qx + qy * qy)}};
    for (int a = 0; a < 3; ++a) {
        for (int r = 0; r < 3; ++r) axes[a][r] = m[r][a];
    }
}

// Occupancy of the opaque splats (alpha >= 0.3): 3 x 3 points on each footprint disc (the two
// largest axes at -1, 0, +1 sigma), on a g^3 cube grid of half size `half` around `centre`. The
// grid follows the capture, not the scene's box: a far background (stump's 1-99 % box is 40 ring
// radii wide) would otherwise put the whole capture into a few voxels.
Occupancy build_occupancy(const GpuPack& pack, int g, const float centre[3], float half) {
    Occupancy occ;
    occ.g = g;
    const std::size_t n = pack.header.count;
    half = std::max(half, 1e-3f);
    for (int c = 0; c < 3; ++c) occ.lo[c] = centre[c] - half;
    occ.vox = 2.0f * half / static_cast<float>(g);
    occ.cells.assign(static_cast<std::size_t>(g) * static_cast<std::size_t>(g) * static_cast<std::size_t>(g),
                     0);
    for (std::size_t i = 0; i < n; ++i) {
        if (static_cast<float>(pack.color[i] >> 24) < 0.3f * 255.0f) continue;
        float scale[3], axes[3][3];
        decode_shape(pack, i, scale, axes);
        int big = 0;
        for (int a = 1; a < 3; ++a)
            if (scale[a] > scale[big]) big = a;
        int mid = big == 0 ? 1 : 0;
        for (int a = 0; a < 3; ++a)
            if (a != big && scale[a] > scale[mid]) mid = a;
        const float* p = &pack.positions[3 * i];
        for (int u = -1; u <= 1; ++u) {
            for (int v = -1; v <= 1; ++v) {
                float q[3];
                for (int c = 0; c < 3; ++c)
                    q[c] = p[c] + static_cast<float>(u) * scale[big] * axes[big][c] +
                           static_cast<float>(v) * scale[mid] * axes[mid][c];
                int cell[3];
                if (occ.cell_of(q, cell)) occ.cells[occ.index(cell[0], cell[1], cell[2])] = 1;
            }
        }
    }
    return occ;
}

// The cube around the per-axis 1-99 % box of the splat centres: centre and half size.
void scene_cube(const GpuPack& pack, float centre[3], float& half) {
    const std::size_t n = pack.header.count;
    const std::size_t stride = std::max<std::size_t>(1, n / 200000);
    std::vector<float> axis;
    half = 1e-3f;
    for (int c = 0; c < 3; ++c) {
        axis.clear();
        for (std::size_t i = 0; i < n; i += stride) {
            const float v = pack.positions[3 * i + static_cast<std::size_t>(c)];
            if (std::isfinite(v)) axis.push_back(v);
        }
        if (axis.empty()) {
            centre[c] = 0.0f;
            continue;
        }
        const std::size_t a = axis.size() / 100, b = axis.size() - 1 - axis.size() / 100;
        std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(a), axis.end());
        const float lo = axis[a];
        std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(b), axis.end());
        const float hi = axis[b];
        centre[c] = 0.5f * (lo + hi);
        half = std::max(half, 0.5f * (hi - lo) * 1.05f);
    }
}

// Does a ray from fine-grid voxel (x, y, z) along the integer step `d` hit occupied space? It
// walks the fine grid around the focus, then continues in the coarse grid over the whole scene:
// a wall beyond the fine grid still encloses the point.
bool ray_hits(const Occupancy& fine, const Occupancy& coarse, int x, int y, int z, const int d[3]) {
    int c[3] = {x, y, z};
    while (true) {
        for (int k = 0; k < 3; ++k) c[k] += d[k];
        if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[0] >= fine.g || c[1] >= fine.g || c[2] >= fine.g) break;
        if (fine.cells[fine.index(c[0], c[1], c[2])] != 0) return true;
    }
    float p[3];
    fine.centre(c[0], c[1], c[2], p);
    int q[3];
    if (!coarse.cell_of(p, q)) return false;
    while (true) {
        if (coarse.cells[coarse.index(q[0], q[1], q[2])] != 0) return true;
        for (int k = 0; k < 3; ++k) q[k] += d[k];
        if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= coarse.g || q[1] >= coarse.g || q[2] >= coarse.g)
            return false;
    }
}

// Box dilation by `r` voxels (separable max filter).
std::vector<std::uint8_t> dilate(const Occupancy& occ, int r) {
    std::vector<std::uint8_t> cur = occ.cells, next(cur.size());
    const int g = occ.g;
    for (int axis = 0; axis < 3; ++axis) {
        for (int x = 0; x < g; ++x) {
            for (int y = 0; y < g; ++y) {
                for (int z = 0; z < g; ++z) {
                    const int i[3] = {x, y, z};
                    std::uint8_t v = 0;
                    for (int d = -r; d <= r && v == 0; ++d) {
                        int j[3] = {x, y, z};
                        j[axis] = i[axis] + d;
                        if (j[axis] < 0 || j[axis] >= g) continue;
                        v = cur[occ.index(j[0], j[1], j[2])];
                    }
                    next[occ.index(x, y, z)] = v;
                }
            }
        }
        std::swap(cur, next);
    }
    return cur;
}

CaptureCamera look_camera(const float eye[3], const float fwd_in[3], const float up_hint[3], std::uint32_t w,
                          std::uint32_t h, float fx) {
    CaptureCamera cam;
    float fwd[3] = {fwd_in[0], fwd_in[1], fwd_in[2]};
    normalize3(fwd);
    float right[3];
    cross3(fwd, up_hint, right);
    if (!normalize3(right)) {
        const float alt[3] = {1.0f, 0.0f, 0.0f};
        cross3(fwd, alt, right);
        normalize3(right);
    }
    float down[3];
    cross3(fwd, right, down);
    for (int c = 0; c < 3; ++c) {
        cam.position[c] = eye[c];
        cam.right[c] = right[c];
        cam.down[c] = down[c];
        cam.forward[c] = fwd[c];
    }
    cam.fx = cam.fy = fx;
    cam.width = w;
    cam.height = h;
    return cam;
}

}  // namespace

Result<std::vector<CaptureCamera>> parse_transforms_json(const std::string& text) {
    using R = Result<std::vector<CaptureCamera>>;
    JsonValue root;
    JsonReader reader(text);
    if (!reader.parse(root))
        return R::failure("transforms.json: malformed JSON near byte " + std::to_string(reader.position()));
    if (root.kind != JsonValue::Kind::kObject) return R::failure("transforms.json: expected an object");
    Result<std::vector<GlCamera>> gl = parse_gl_cameras(root);
    if (!gl) return R::failure(gl.error());
    return R::success(transforms_cameras(gl.value(), Affine{}));
}

Result<std::vector<CaptureCamera>> parse_colmap(const std::vector<std::uint8_t>& cameras_file,
                                                const std::vector<std::uint8_t>& images_file) {
    using R = Result<std::vector<CaptureCamera>>;
    Result<std::vector<ColmapIntrinsics>> intr =
        looks_binary(cameras_file) ? colmap_cameras_bin(cameras_file) : colmap_cameras_txt(cameras_file);
    if (!intr) return R::failure(intr.error());
    Result<std::vector<ColmapImage>> imgs =
        looks_binary(images_file) ? colmap_images_bin(images_file) : colmap_images_txt(images_file);
    if (!imgs) return R::failure(imgs.error());
    std::vector<CaptureCamera> cams = colmap_to_cameras(intr.value(), imgs.value());
    if (cams.empty()) return R::failure("COLMAP model: no image has a usable camera");
    return R::success(std::move(cams));
}

// Every plausible world of a camera file set, most literal first. A transforms.json may sit in
// a re-oriented frame (nerfstudio's ns-process-data records the COLMAP -> transforms rotation as
// `applied_transform`), and a splatfacto export may live in the normalized frame its
// `dataparser_transforms.json` describes; which one the PLY uses depends on the tool and version,
// so the caller picks the interpretation whose cameras see the scene (capture_fit).
static Result<std::vector<std::vector<CaptureCamera>>> capture_interpretations(
    const std::vector<CaptureFile>& files, std::vector<std::string>& names) {
    using R = Result<std::vector<std::vector<CaptureCamera>>>;
    names.clear();
    const CaptureFile* transforms = nullptr;
    const CaptureFile* dataparser = nullptr;
    for (const CaptureFile& f : files) {
        std::size_t k = 0;
        while (k < f.bytes.size() && std::isspace(f.bytes[k])) ++k;
        if (k >= f.bytes.size() || f.bytes[k] != '{') continue;
        if (name_has(f.name, "dataparser"))
            dataparser = &f;
        else
            transforms = &f;
    }
    if (transforms == nullptr) {
        Result<std::vector<CaptureCamera>> cams = parse_capture_cameras(files);
        if (!cams) return R::failure(cams.error());
        names.push_back("as given");
        return R::success({std::move(cams.value())});
    }
    JsonValue root;
    const std::string text(transforms->bytes.begin(),
                           transforms->bytes.end());  // JsonReader keeps a reference
    JsonReader reader(text);
    if (!reader.parse(root) || root.kind != JsonValue::Kind::kObject)
        return R::failure("transforms.json: malformed JSON near byte " + std::to_string(reader.position()));
    Result<std::vector<GlCamera>> gl = parse_gl_cameras(root);
    if (!gl) return R::failure(gl.error());
    std::vector<std::vector<CaptureCamera>> out;
    out.push_back(transforms_cameras(gl.value(), Affine{}));
    names.push_back("as given");
    Affine applied;
    if (read_affine(root.field("applied_transform"), applied)) {
        out.push_back(transforms_cameras(gl.value(), inverse_rigid(applied)));
        names.push_back("applied_transform undone");
    }
    if (dataparser != nullptr) {
        JsonValue dp;
        const std::string dp_text(dataparser->bytes.begin(), dataparser->bytes.end());
        JsonReader dp_reader(dp_text);
        Affine normalize;
        if (dp_reader.parse(dp) && read_affine(dp.field("transform"), normalize)) {
            detail::read_number(dp, "scale", normalize.scale);
            out.push_back(transforms_cameras(gl.value(), normalize));
            names.push_back("dataparser transform applied");
        }
    }
    return R::success(std::move(out));
}

Result<std::vector<CaptureCamera>> parse_capture_cameras(const std::vector<CaptureFile>& files) {
    using R = Result<std::vector<CaptureCamera>>;
    if (files.empty()) return R::failure("no camera files");
    // JSON: cameras.json (array) or transforms.json (object with frames).
    for (const CaptureFile& f : files) {
        std::size_t k = 0;
        while (k < f.bytes.size() && std::isspace(f.bytes[k])) ++k;
        if (k >= f.bytes.size() || (f.bytes[k] != '[' && f.bytes[k] != '{')) continue;
        const std::string text(f.bytes.begin(), f.bytes.end());
        if (f.bytes[k] == '[') return parse_cameras_json(text);
        if (name_has(f.name, "dataparser")) continue;  // a nerfstudio normalization, not cameras
        return parse_transforms_json(text);
    }
    // COLMAP: a cameras file and an images file (by name; else by trying both orders).
    if (files.size() >= 2) {
        const CaptureFile* cams = nullptr;
        const CaptureFile* imgs = nullptr;
        for (const CaptureFile& f : files) {
            if (name_has(f.name, "cameras"))
                cams = &f;
            else if (name_has(f.name, "images"))
                imgs = &f;
        }
        if (cams != nullptr && imgs != nullptr) return parse_colmap(cams->bytes, imgs->bytes);
        R first = parse_colmap(files[0].bytes, files[1].bytes);
        if (first) return first;
        return parse_colmap(files[1].bytes, files[0].bytes);
    }
    return R::failure("unrecognized camera file '" + files[0].name +
                      "' (expected cameras.json, transforms.json, or COLMAP cameras + images)");
}

float capture_fit(const GpuPack& pack, const std::vector<CaptureCamera>& cameras) {
    const std::size_t n = pack.header.count;
    if (n == 0 || cameras.empty()) return 0.0f;
    const std::size_t stride = std::max<std::size_t>(1, n / 200000);
    std::size_t total = 0, seen = 0;
    for (std::size_t i = 0; i < n; i += stride) {
        if ((pack.color[i] >> 24) < 128u) continue;
        ++total;
        const float* p = &pack.positions[3 * i];
        for (const CaptureCamera& cam : cameras) {
            const float rel[3] = {p[0] - cam.position[0], p[1] - cam.position[1], p[2] - cam.position[2]};
            const float z = dot3(rel, cam.forward);
            if (!(z > 1e-6f)) continue;
            const float u = dot3(rel, cam.right) / z * cam.fx;
            const float v = dot3(rel, cam.down) / z * cam.fy;
            if (std::fabs(u) <= 0.5f * static_cast<float>(cam.width) &&
                std::fabs(v) <= 0.5f * static_cast<float>(cam.height)) {
                ++seen;
                break;
            }
        }
    }
    return total > 0 ? static_cast<float>(seen) / static_cast<float>(total) : 0.0f;
}

std::vector<CaptureCamera> infer_capture_cameras(const GpuPack& pack, CaptureSource kind,
                                                 const VirtualCaptureOptions& options) {
    std::vector<CaptureCamera> cameras;
    if (pack.header.count == 0 || kind == CaptureSource::kCameras) return cameras;
    const HomeView home = estimate_home_view(pack);
    const float full = home.full_distance;
    const Occupancy occ =
        build_occupancy(pack, std::max(options.grid, 8), home.focus, options.grid_half * full);

    if (kind == CaptureSource::kObject) {
        const std::vector<std::uint8_t> blocked = dilate(occ, 1);
        float h0[3] = {0.0f, 0.0f, 1.0f};
        if (std::fabs(dot3(h0, home.up)) > 0.9f) h0[0] = 1.0f, h0[2] = 0.0f;
        const float along = dot3(h0, home.up);
        for (int c = 0; c < 3; ++c) h0[c] -= along * home.up[c];
        normalize3(h0);
        float h1[3];
        cross3(home.up, h0, h1);
        const std::uint32_t w = 1024, h = 768;
        const float fx = 0.5f * static_cast<float>(w) / std::tan(0.5f * options.object_fov_deg * 0.01745329f);
        for (const float radius : options.object_radii) {
            for (const float elevation_deg : options.object_elevations_deg) {
                const float e = elevation_deg * 0.01745329f;
                for (int k = 0; k < options.object_azimuths; ++k) {
                    const float yaw =
                        6.2831853f * static_cast<float>(k) / static_cast<float>(options.object_azimuths);
                    float eye[3], fwd[3];
                    for (int c = 0; c < 3; ++c) {
                        eye[c] = home.focus[c] +
                                 radius * full *
                                     (std::cos(e) * (std::cos(yaw) * h0[c] + std::sin(yaw) * h1[c]) +
                                      std::sin(e) * home.up[c]);
                        fwd[c] = home.focus[c] - eye[c];
                    }
                    int cell[3];
                    if (occ.cell_of(eye, cell) && blocked[occ.index(cell[0], cell[1], cell[2])] != 0)
                        continue;
                    cameras.push_back(look_camera(eye, fwd, home.up, w, h, fx));
                }
            }
        }
        return cameras;
    }

    // Room: omnidirectional cameras at enclosed free points near the focus. Enclosed = most of 26
    // rays from the point hit geometry (the fine grid, then a coarse grid over the whole scene):
    // inside a room the walls surround the point; outside them (where a plain distance rule put
    // half the cameras) rays escape.
    float scene_centre[3], scene_half = 1.0f;
    scene_cube(pack, scene_centre, scene_half);
    const Occupancy coarse = build_occupancy(pack, 64, scene_centre, scene_half);
    const std::vector<std::uint8_t> blocked = dilate(occ, std::max(options.room_clearance, 0));
    const float reach = options.room_radius * full;
    std::vector<std::array<int, 3>> candidates;  // voxel indices: the sampling below is exact integer math
    std::vector<float> enclosure;
    for (int x = 0; x < occ.g; ++x) {
        for (int y = 0; y < occ.g; ++y) {
            for (int z = 0; z < occ.g; ++z) {
                if (blocked[occ.index(x, y, z)] != 0) continue;
                float p[3];
                occ.centre(x, y, z, p);
                const float d[3] = {p[0] - home.focus[0], p[1] - home.focus[1], p[2] - home.focus[2]};
                if (dot3(d, d) > reach * reach) continue;
                int hits = 0;
                for (int dx = -1; dx <= 1; ++dx) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dz = -1; dz <= 1; ++dz) {
                            const int step[3] = {dx, dy, dz};
                            if (dx != 0 || dy != 0 || dz != 0)
                                hits += ray_hits(occ, coarse, x, y, z, step) ? 1 : 0;
                        }
                    }
                }
                candidates.push_back({x, y, z});
                enclosure.push_back(static_cast<float>(hits) / 26.0f);
            }
        }
    }
    if (!candidates.empty()) {
        // Keep the enclosed ones; only if none qualifies, the most enclosed quarter.
        std::vector<float> sorted = enclosure;
        std::sort(sorted.begin(), sorted.end());
        const float threshold =
            sorted.back() >= options.room_enclosure ? options.room_enclosure : sorted[sorted.size() * 3 / 4];
        std::size_t k = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (enclosure[i] >= threshold) candidates[k++] = candidates[i];
        }
        candidates.resize(k);
    }
    std::vector<std::array<float, 3>> positions;
    if (candidates.empty()) {
        positions.push_back({home.eye[0], home.eye[1], home.eye[2]});
    } else {
        // Farthest-point sampling from the candidate nearest the home eye, in integer voxel
        // units: no float rounding can reorder two candidates, so the same candidate set gives
        // the same cameras on every platform.
        const auto voxel_of = [&](float v, int c) {
            return static_cast<int>(std::floor((v - occ.lo[c]) / occ.vox));
        };
        const std::array<int, 3> eye_cell = {voxel_of(home.eye[0], 0), voxel_of(home.eye[1], 1),
                                             voxel_of(home.eye[2], 2)};
        const auto dist2i = [](const std::array<int, 3>& a, const std::array<int, 3>& b) {
            const std::int64_t dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        };
        std::vector<std::int64_t> nearest(candidates.size(), std::numeric_limits<std::int64_t>::max());
        std::size_t pick = 0;
        for (std::size_t i = 1; i < candidates.size(); ++i) {
            if (dist2i(candidates[i], eye_cell) < dist2i(candidates[pick], eye_cell)) pick = i;
        }
        const std::size_t want = std::min<std::size_t>(
            static_cast<std::size_t>(std::max(options.room_positions, 1)), candidates.size());
        while (positions.size() < want) {
            float p[3];
            occ.centre(candidates[pick][0], candidates[pick][1], candidates[pick][2], p);
            positions.push_back({p[0], p[1], p[2]});
            std::int64_t far = -1;
            std::size_t next = pick;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                nearest[i] = std::min(nearest[i], dist2i(candidates[i], candidates[pick]));
                if (nearest[i] > far) far = nearest[i], next = i;
            }
            pick = next;
        }
    }
    static constexpr float kFaces[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                           {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const auto& p : positions) {
        for (const auto& f : kFaces) {
            const float up_hint[3] = {0.0f, std::fabs(f[1]) > 0.5f ? 0.0f : 1.0f,
                                      std::fabs(f[1]) > 0.5f ? 1.0f : 0.0f};
            cameras.push_back(look_camera(p.data(), f, up_hint, 1024, 1024, 512.0f));
        }
    }
    return cameras;
}

Result<CoverageSidecar> make_coverage_sidecar(const GpuPack& pack, CaptureSource source,
                                              const std::vector<CaptureFile>& files,
                                              const VirtualCaptureOptions& options) {
    using R = Result<CoverageSidecar>;
    CoverageSidecar out;
    out.source = source;
    std::vector<CaptureCamera> cameras;
    if (source == CaptureSource::kCameras) {
        std::vector<std::string> names;
        Result<std::vector<std::vector<CaptureCamera>>> sets = capture_interpretations(files, names);
        if (!sets) return R::failure(sets.error());
        float best = -1.0f;
        for (std::size_t i = 0; i < sets.value().size(); ++i) {
            const float fit = capture_fit(pack, sets.value()[i]);
            if (fit > best) {
                best = fit;
                cameras = sets.value()[i];
                out.interpretation = names[i];
            }
        }
    } else {
        cameras = infer_capture_cameras(pack, source, options);
        out.interpretation = source == CaptureSource::kRoom ? "virtual: room" : "virtual: object";
    }
    if (cameras.empty()) return R::failure("no cameras");
    out.cameras = cameras.size();
    out.fit = capture_fit(pack, cameras);
    if (source == CaptureSource::kCameras && out.fit < kMinCaptureFit) {
        return R::failure("the cameras see only " + std::to_string(static_cast<int>(100.0f * out.fit)) +
                          " % of the scene: they belong to another scene or coordinate frame (a nerfstudio "
                          "export may need its dataparser_transforms.json too)");
    }
    CoverageHeader header;
    header.count = pack.header.count;
    header.positions_hash = coverage_positions_hash(pack);
    header.camera_count = static_cast<std::uint32_t>(cameras.size());
    header.source = static_cast<std::uint32_t>(source);
    out.bytes = serialize_coverage(header, compute_coverage(pack, cameras));
    out.used = std::move(cameras);
    return R::success(std::move(out));
}

}  // namespace splatcore
