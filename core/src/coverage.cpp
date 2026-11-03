#include "splatcore/coverage.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <thread>

namespace splatcore {
namespace {

// --- Minimal JSON reader: enough for cameras.json (objects, arrays, numbers, strings, literals). ---
struct JsonValue {
    enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = Kind::kNull;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> items;
    std::map<std::string, JsonValue> fields;

    const JsonValue* field(const std::string& key) const {
        const auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : m_text(text) {}

    bool parse(JsonValue& out) {
        if (!value(out, 0)) return false;
        skip_space();
        return m_pos == m_text.size();
    }
    std::size_t position() const { return m_pos; }

private:
    static constexpr int kMaxDepth = 64;

    void skip_space() {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos]))) ++m_pos;
    }
    bool literal(const char* word) {
        const std::size_t n = std::strlen(word);
        if (m_text.compare(m_pos, n, word) != 0) return false;
        m_pos += n;
        return true;
    }
    bool string(std::string& out) {
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') return false;
        ++m_pos;
        while (m_pos < m_text.size() && m_text[m_pos] != '"') {
            char c = m_text[m_pos++];
            if (c == '\\') {
                if (m_pos >= m_text.size()) return false;
                const char e = m_text[m_pos++];
                if (e == 'u') {  // keep \uXXXX as '?': names are never needed verbatim here
                    if (m_pos + 4 > m_text.size()) return false;
                    m_pos += 4;
                    c = '?';
                } else {
                    c = e == 'n'   ? '\n'
                        : e == 't' ? '\t'
                        : e == 'r' ? '\r'
                        : e == 'b' ? '\b'
                        : e == 'f' ? '\f'
                                   : e;
                }
            }
            out.push_back(c);
        }
        if (m_pos >= m_text.size()) return false;
        ++m_pos;
        return true;
    }
    bool value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip_space();
        if (m_pos >= m_text.size()) return false;
        const char c = m_text[m_pos];
        if (c == '{') {
            out.kind = JsonValue::Kind::kObject;
            ++m_pos;
            skip_space();
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            while (true) {
                skip_space();
                std::string key;
                if (!string(key)) return false;
                skip_space();
                if (m_pos >= m_text.size() || m_text[m_pos] != ':') return false;
                ++m_pos;
                if (!value(out.fields[key], depth + 1)) return false;
                skip_space();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                    ++m_pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            out.kind = JsonValue::Kind::kArray;
            ++m_pos;
            skip_space();
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            while (true) {
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1)) return false;
                skip_space();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                    ++m_pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.kind = JsonValue::Kind::kString;
            return string(out.text);
        }
        if (literal("true") || literal("false")) {
            out.kind = JsonValue::Kind::kBool;
            out.number = m_text[m_pos - 1] == 'e' && m_text[m_pos - 2] == 'u' ? 1.0 : 0.0;
            return true;
        }
        if (literal("null")) {
            out.kind = JsonValue::Kind::kNull;
            return true;
        }
        // Number: strtod on a bounded copy (the text is not NUL-terminated at the number's end).
        std::size_t end = m_pos;
        while (end < m_text.size() && std::strchr("+-0123456789.eE", m_text[end]) != nullptr) ++end;
        if (end == m_pos) return false;
        const std::string token = m_text.substr(m_pos, end - m_pos);
        char* stop = nullptr;
        out.number = std::strtod(token.c_str(), &stop);
        if (stop != token.c_str() + token.size()) return false;
        out.kind = JsonValue::Kind::kNumber;
        m_pos = end;
        return true;
    }

    const std::string& m_text;
    std::size_t m_pos = 0;
};

bool read_number(const JsonValue& object, const char* key, double& out) {
    const JsonValue* v = object.field(key);
    if (v == nullptr || v->kind != JsonValue::Kind::kNumber) return false;
    out = v->number;
    return true;
}

float sign_nonzero(float x) {
    return x < 0.0f ? -1.0f : 1.0f;
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

Result<std::vector<CaptureCamera>> parse_cameras_json(const std::string& text) {
    using R = Result<std::vector<CaptureCamera>>;
    JsonValue root;
    JsonReader reader(text);
    if (!reader.parse(root))
        return R::failure("cameras.json: malformed JSON near byte " + std::to_string(reader.position()));
    if (root.kind != JsonValue::Kind::kArray) return R::failure("cameras.json: expected an array of cameras");
    std::vector<CaptureCamera> cameras;
    cameras.reserve(root.items.size());
    // RDF (PLY/COLMAP world) -> RUB: negate y and z, for positions and axis vectors alike.
    const float flip[3] = {1.0f, -1.0f, -1.0f};
    for (std::size_t i = 0; i < root.items.size(); ++i) {
        const JsonValue& c = root.items[i];
        const std::string where = "cameras.json: camera " + std::to_string(i) + ": ";
        if (c.kind != JsonValue::Kind::kObject) return R::failure(where + "not an object");
        double fx = 0.0, fy = 0.0, width = 0.0, height = 0.0;
        if (!read_number(c, "fx", fx) || !read_number(c, "fy", fy) || !read_number(c, "width", width) ||
            !read_number(c, "height", height)) {
            return R::failure(where + "missing fx, fy, width or height");
        }
        if (!(fx > 0.0) || !(fy > 0.0) || !(width >= 1.0) || !(height >= 1.0) || width > 1e6 ||
            height > 1e6) {
            return R::failure(where + "invalid intrinsics");
        }
        const JsonValue* pos = c.field("position");
        const JsonValue* rot = c.field("rotation");
        if (pos == nullptr || pos->kind != JsonValue::Kind::kArray || pos->items.size() != 3 ||
            rot == nullptr || rot->kind != JsonValue::Kind::kArray || rot->items.size() != 3) {
            return R::failure(where + "missing position[3] or rotation[3][3]");
        }
        CaptureCamera cam;
        float m[3][3];  // camera-to-world rows; column k is camera axis k in world
        for (int r = 0; r < 3; ++r) {
            const JsonValue& p = pos->items[static_cast<std::size_t>(r)];
            const JsonValue& row = rot->items[static_cast<std::size_t>(r)];
            if (p.kind != JsonValue::Kind::kNumber || row.kind != JsonValue::Kind::kArray ||
                row.items.size() != 3) {
                return R::failure(where + "position/rotation entries must be numbers");
            }
            cam.position[r] = static_cast<float>(p.number) * flip[r];
            for (int k = 0; k < 3; ++k) {
                const JsonValue& e = row.items[static_cast<std::size_t>(k)];
                if (e.kind != JsonValue::Kind::kNumber)
                    return R::failure(where + "rotation entries must be numbers");
                m[r][k] = static_cast<float>(e.number);
            }
        }
        for (int r = 0; r < 3; ++r) {
            cam.right[r] = m[r][0] * flip[r];
            cam.down[r] = m[r][1] * flip[r];
            cam.forward[r] = m[r][2] * flip[r];
        }
        cam.fx = static_cast<float>(fx);
        cam.fy = static_cast<float>(fy);
        cam.width = static_cast<std::uint32_t>(width);
        cam.height = static_cast<std::uint32_t>(height);
        cameras.push_back(cam);
    }
    return R::success(std::move(cameras));
}

std::uint32_t coverage_cell(const float dir[3]) {
    const float l1 = std::abs(dir[0]) + std::abs(dir[1]) + std::abs(dir[2]);
    if (!(l1 > 0.0f)) return 0;
    float x = dir[0] / l1, y = dir[1] / l1;
    if (dir[2] < 0.0f) {
        const float fx = (1.0f - std::abs(y)) * sign_nonzero(x);
        const float fy = (1.0f - std::abs(x)) * sign_nonzero(y);
        x = fx;
        y = fy;
    }
    const auto bin = [](float t) {
        return std::clamp(static_cast<int>((t + 1.0f) * 0.5f * static_cast<float>(kCoverageGrid)), 0,
                          kCoverageGrid - 1);
    };
    return static_cast<std::uint32_t>(bin(y) * kCoverageGrid + bin(x));
}

void coverage_cell_center(std::uint32_t cell, float out[3]) {
    const float g = static_cast<float>(kCoverageGrid);
    float x = (static_cast<float>(cell % kCoverageGrid) + 0.5f) / g * 2.0f - 1.0f;
    float y = (static_cast<float>(cell / kCoverageGrid) + 0.5f) / g * 2.0f - 1.0f;
    const float z = 1.0f - std::abs(x) - std::abs(y);
    if (z < 0.0f) {
        const float fx = (1.0f - std::abs(y)) * sign_nonzero(x);
        const float fy = (1.0f - std::abs(x)) * sign_nonzero(y);
        x = fx;
        y = fy;
    }
    const float len = std::sqrt(x * x + y * y + z * z);
    out[0] = x / len;
    out[1] = y / len;
    out[2] = z / len;
}

float coverage_match(std::uint32_t mask, const float dir[3], float cos_full, float cos_zero) {
    float best = 0.0f;
    while (mask != 0u) {
        const auto cell = static_cast<std::uint32_t>(std::countr_zero(mask));
        mask &= mask - 1u;
        float c[3];
        coverage_cell_center(cell, c);
        best = std::max(best, smoothstep(cos_zero, cos_full, dir[0] * c[0] + dir[1] * c[1] + dir[2] * c[2]));
    }
    return best;
}

std::vector<std::uint32_t> compute_coverage(const GpuPack& pack, const std::vector<CaptureCamera>& cameras,
                                            const CoverageOptions& options) {
    const std::size_t n = pack.header.count;
    std::vector<std::uint32_t> mask(n, 0u);
    if (n == 0 || cameras.empty()) return mask;
    const int ds = std::max(options.depth_downscale, 1);

    std::vector<std::uint8_t> opaque(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float alpha = static_cast<float>(pack.color[i] >> 24) / 255.0f;
        opaque[i] = alpha >= options.opaque_alpha ? 1 : 0;
    }

    // One camera at a time per thread; each thread ORs into its own mask copy (no races, and the
    // union does not depend on which thread took which camera).
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned want = options.threads > 0 ? static_cast<unsigned>(options.threads) : std::min(hw, 16u);
    const unsigned workers = std::max(1u, std::min<unsigned>(want, static_cast<unsigned>(cameras.size())));
    std::vector<std::vector<std::uint32_t>> partial(workers);

    const auto run = [&](unsigned worker) {
        std::vector<std::uint32_t>& local = partial[worker];
        local.assign(n, 0u);
        std::vector<float> zbuf;
        std::vector<std::int32_t> pixel(n);
        std::vector<float> depth(n);
        for (std::size_t k = worker; k < cameras.size(); k += workers) {
            const CaptureCamera& cam = cameras[k];
            const int w = std::max(1, static_cast<int>(cam.width) / ds);
            const int h = std::max(1, static_cast<int>(cam.height) / ds);
            const float fx = cam.fx / static_cast<float>(ds), fy = cam.fy / static_cast<float>(ds);
            zbuf.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                        std::numeric_limits<float>::infinity());
            for (std::size_t i = 0; i < n; ++i) {
                const float* p = &pack.positions[3 * i];
                const float rel[3] = {p[0] - cam.position[0], p[1] - cam.position[1], p[2] - cam.position[2]};
                const float z = rel[0] * cam.forward[0] + rel[1] * cam.forward[1] + rel[2] * cam.forward[2];
                pixel[i] = -1;
                if (!(z > 1e-3f)) continue;
                const float u =
                    (rel[0] * cam.right[0] + rel[1] * cam.right[1] + rel[2] * cam.right[2]) / z * fx +
                    0.5f * static_cast<float>(w);
                const float v =
                    (rel[0] * cam.down[0] + rel[1] * cam.down[1] + rel[2] * cam.down[2]) / z * fy +
                    0.5f * static_cast<float>(h);
                if (!(u >= 0.0f && u < static_cast<float>(w) && v >= 0.0f && v < static_cast<float>(h)))
                    continue;
                const std::int32_t px = static_cast<std::int32_t>(v) * w + static_cast<std::int32_t>(u);
                pixel[i] = px;
                depth[i] = z;
                if (opaque[i] != 0)
                    zbuf[static_cast<std::size_t>(px)] = std::min(zbuf[static_cast<std::size_t>(px)], z);
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (pixel[i] < 0) continue;
                if (depth[i] > zbuf[static_cast<std::size_t>(pixel[i])] * (1.0f + options.depth_tolerance))
                    continue;
                const float* p = &pack.positions[3 * i];
                const float dir[3] = {cam.position[0] - p[0], cam.position[1] - p[1], cam.position[2] - p[2]};
                local[i] |= 1u << coverage_cell(dir);
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < workers; ++t) threads.emplace_back(run, t);
    run(0);
    for (std::thread& t : threads) t.join();
    for (const std::vector<std::uint32_t>& local : partial) {
        for (std::size_t i = 0; i < n; ++i) mask[i] |= local[i];
    }
    return mask;
}

std::uint64_t coverage_positions_hash(const GpuPack& pack) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(pack.positions.data());
    const std::size_t size = pack.positions.size() * sizeof(float);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::vector<std::uint8_t> serialize_coverage(const CoverageHeader& header,
                                             const std::vector<std::uint32_t>& masks) {
    std::vector<std::uint8_t> bytes(sizeof(CoverageHeader) + masks.size() * sizeof(std::uint32_t));
    std::memcpy(bytes.data(), &header, sizeof(CoverageHeader));
    if (!masks.empty())
        std::memcpy(bytes.data() + sizeof(CoverageHeader), masks.data(),
                    masks.size() * sizeof(std::uint32_t));
    return bytes;
}

Result<std::vector<std::uint32_t>> parse_coverage(const std::vector<std::uint8_t>& bytes,
                                                  const GpuPack& pack) {
    using R = Result<std::vector<std::uint32_t>>;
    if (bytes.size() < sizeof(CoverageHeader)) return R::failure("coverage file shorter than its header");
    CoverageHeader header;
    std::memcpy(&header, bytes.data(), sizeof(CoverageHeader));
    if (header.magic != kCoverageMagic) return R::failure("not a coverage file (bad magic)");
    if (header.version != kCoverageVersion) {
        return R::failure("coverage file version " + std::to_string(header.version) + ", expected " +
                          std::to_string(kCoverageVersion));
    }
    if (header.grid != static_cast<std::uint32_t>(kCoverageGrid))
        return R::failure("coverage grid size mismatch");
    if (header.count != pack.header.count) {
        return R::failure("coverage file has " + std::to_string(header.count) + " splats, the scene " +
                          std::to_string(pack.header.count));
    }
    if (bytes.size() != sizeof(CoverageHeader) + std::size_t{header.count} * sizeof(std::uint32_t)) {
        return R::failure("coverage file size does not match its header");
    }
    if (header.positions_hash != coverage_positions_hash(pack)) {
        return R::failure("coverage file was computed for a different scene (positions hash mismatch)");
    }
    std::vector<std::uint32_t> masks(header.count);
    if (header.count > 0)
        std::memcpy(masks.data(), bytes.data() + sizeof(CoverageHeader),
                    masks.size() * sizeof(std::uint32_t));
    return R::success(std::move(masks));
}

}  // namespace splatcore
