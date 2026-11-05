#include "splatcore/pack.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <limits>

#include "splatcore/cleaning.h"

namespace splatcore {
namespace {

// FNV-1a 64 over raw bytes; chained across arrays to fingerprint the source cloud.
std::uint64_t fnv1a(const void* data, std::size_t bytes, std::uint64_t hash) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t content_hash(const SplatCloud& cloud) {
    std::uint64_t hash = 14695981039346656037ull;
    const std::uint64_t meta[2] = {cloud.count, static_cast<std::uint64_t>(cloud.sh_degree)};
    hash = fnv1a(meta, sizeof(meta), hash);
    hash = fnv1a(cloud.positions.data(), cloud.positions.size() * sizeof(float), hash);
    hash = fnv1a(cloud.log_scales.data(), cloud.log_scales.size() * sizeof(float), hash);
    hash = fnv1a(cloud.rotations.data(), cloud.rotations.size() * sizeof(float), hash);
    hash = fnv1a(cloud.opacities.data(), cloud.opacities.size() * sizeof(float), hash);
    hash = fnv1a(cloud.dc.data(), cloud.dc.size() * sizeof(float), hash);
    hash = fnv1a(cloud.sh.data(), cloud.sh.size() * sizeof(float), hash);
    return hash;
}

std::uint32_t snorm8(float v) {
    const long q = std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f);
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(static_cast<std::int8_t>(q)));
}

std::uint32_t unorm8(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

}  // namespace

std::vector<std::uint8_t> GpuPack::serialize() const {
    std::vector<std::uint8_t> out(sizeof(PackHeader) + positions.size() * sizeof(float) +
                                  scale_rot.size() * sizeof(std::uint32_t) +
                                  color.size() * sizeof(std::uint32_t) + sh1.size() * sizeof(std::uint32_t) +
                                  chunks.size() * sizeof(float));
    std::uint8_t* p = out.data();
    const auto append = [&p](const void* data, std::size_t bytes) {
        std::memcpy(p, data, bytes);
        p += bytes;
    };
    append(&header, sizeof(header));
    append(positions.data(), positions.size() * sizeof(float));
    append(scale_rot.data(), scale_rot.size() * sizeof(std::uint32_t));
    append(color.data(), color.size() * sizeof(std::uint32_t));
    append(sh1.data(), sh1.size() * sizeof(std::uint32_t));
    append(chunks.data(), chunks.size() * sizeof(float));
    return out;
}

Result<GpuPack> build_pack(const SplatCloud& cloud) {
    using R = Result<GpuPack>;
    if (cloud.sh_degree != 0 && cloud.sh_degree != 1) {
        return R::failure("GPU pack supports SH degree 0 or 1 only, got degree " +
                          std::to_string(cloud.sh_degree) + " (truncate first)");
    }
    if (!cloud.valid_sizes()) return R::failure("SplatCloud has inconsistent array sizes");
    if (cloud.count > std::numeric_limits<std::uint32_t>::max()) {
        return R::failure("splat count exceeds the pack's 32-bit limit");
    }
    const std::size_t n = cloud.count;

    GpuPack pack;
    pack.header.count = static_cast<std::uint32_t>(n);
    pack.header.sh_degree = static_cast<std::uint32_t>(cloud.sh_degree);
    pack.header.source_hash = content_hash(cloud);

    pack.positions = cloud.positions;
    for (int c = 0; c < 3; ++c) {
        pack.header.aabb_min[c] = std::numeric_limits<float>::max();
        pack.header.aabb_max[c] = std::numeric_limits<float>::lowest();
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = cloud.positions[3 * i + static_cast<std::size_t>(c)];
            pack.header.aabb_min[c] = std::min(pack.header.aabb_min[c], v);
            pack.header.aabb_max[c] = std::max(pack.header.aabb_max[c], v);
        }
    }
    if (n == 0) {
        for (int c = 0; c < 3; ++c) pack.header.aabb_min[c] = pack.header.aabb_max[c] = 0.0f;
    }

    // scale_rot lanes (docs/formats.md §3): word 0 = h(sx) | h(sy)<<16; word 1 = h(sz) |
    // snorm8(w)<<16 | snorm8(x)<<24; word 2 = snorm8(y) | snorm8(z)<<8, high 16 bits zero.
    // Quaternions are sign-canonicalized (w >= 0) so packing is deterministic; q and -q are the
    // same rotation.
    pack.scale_rot.resize(3 * n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t hx = glm::packHalf1x16(cloud.log_scales[3 * i + 0]);
        const std::uint32_t hy = glm::packHalf1x16(cloud.log_scales[3 * i + 1]);
        const std::uint32_t hz = glm::packHalf1x16(cloud.log_scales[3 * i + 2]);
        float q[4] = {cloud.rotations[4 * i + 0], cloud.rotations[4 * i + 1], cloud.rotations[4 * i + 2],
                      cloud.rotations[4 * i + 3]};
        if (q[0] < 0.0f) {
            for (float& v : q) v = -v;
        }
        pack.scale_rot[3 * i + 0] = hx | (hy << 16);
        pack.scale_rot[3 * i + 1] = hz | (snorm8(q[0]) << 16) | (snorm8(q[1]) << 24);
        pack.scale_rot[3 * i + 2] = snorm8(q[2]) | (snorm8(q[3]) << 8);
    }

    // RGBA8: rgb is the displayed base color 0.5 + C0 * f_dc clamped to [0,1]; a = sigmoid(logit).
    pack.color.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t r = unorm8(0.5f + kShC0 * cloud.dc[3 * i + 0]);
        const std::uint32_t g = unorm8(0.5f + kShC0 * cloud.dc[3 * i + 1]);
        const std::uint32_t b = unorm8(0.5f + kShC0 * cloud.dc[3 * i + 2]);
        const std::uint32_t a = unorm8(sigmoid(cloud.opacities[i]));
        pack.color[i] = r | (g << 8) | (b << 16) | (a << 24);
    }

    // SH1: 9 snorm8 coefficients (coefficient-major, channel fastest) scaled by the per-pack
    // max-abs, plus 3 bytes of padding, as 3 uint32 lanes.
    if (cloud.sh_degree == 1) {
        float max_abs = 0.0f;
        for (float v : cloud.sh) max_abs = std::max(max_abs, std::abs(v));
        pack.header.sh1_scale = max_abs > 0.0f ? max_abs : 1.0f;
        const float inv_scale = 1.0f / pack.header.sh1_scale;
        pack.sh1.resize(3 * n);
        for (std::size_t i = 0; i < n; ++i) {
            const float* sh = cloud.sh.data() + 9 * i;
            std::uint32_t bytes[12] = {};
            for (int k = 0; k < 9; ++k) bytes[k] = snorm8(sh[k] * inv_scale);
            pack.sh1[3 * i + 0] = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
            pack.sh1[3 * i + 1] = bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
            pack.sh1[3 * i + 2] = bytes[8];  // bytes 9-11 are padding, zero
        }
    }

    // Morton order (the caller's responsibility) makes chunks spatially compact; each chunk gets
    // a conservative cull AABB and the max of the importance proxy for budget culling. The AABB
    // covers each splat's 3-sigma extent (center +/- 3 * largest scale, an isotropic upper bound
    // on the ellipsoid), so a frustum cull on chunk AABBs never drops a splat whose footprint the
    // renderer would draw (the draw radius is also the 3-sigma cutoff).
    const std::size_t chunk_count = pack.chunk_count();
    pack.chunks.resize(kChunkFloats * chunk_count);
    for (std::size_t chunk = 0; chunk < chunk_count; ++chunk) {
        const std::size_t begin = chunk * kPackChunkSize;
        const std::size_t end = std::min(n, begin + kPackChunkSize);
        float mn[3], mx[3];
        std::fill_n(mn, 3, std::numeric_limits<float>::max());
        std::fill_n(mx, 3, std::numeric_limits<float>::lowest());
        float importance_max = 0.0f;
        for (std::size_t i = begin; i < end; ++i) {
            const float max_log_scale =
                std::max({cloud.log_scales[3 * i], cloud.log_scales[3 * i + 1], cloud.log_scales[3 * i + 2]});
            const float r = 3.0f * std::exp(max_log_scale);
            for (int c = 0; c < 3; ++c) {
                const float v = cloud.positions[3 * i + static_cast<std::size_t>(c)];
                mn[c] = std::min(mn[c], v - r);
                mx[c] = std::max(mx[c], v + r);
            }
            importance_max = std::max(importance_max, importance(cloud, i));
        }
        float* out = pack.chunks.data() + kChunkFloats * chunk;
        for (int c = 0; c < 3; ++c) {
            out[c] = mn[c];
            out[3 + c] = mx[c];
        }
        out[6] = importance_max;
    }
    return R::success(std::move(pack));
}

Result<GpuPack> parse_pack(const std::vector<std::uint8_t>& bytes) {
    using R = Result<GpuPack>;
    Result<PackHeader> header = parse_pack_header(bytes);
    if (!header) return R::failure(header.error());

    GpuPack pack;
    pack.header = header.value();
    const std::size_t n = pack.header.count;
    pack.positions.resize(3 * n);
    pack.scale_rot.resize(3 * n);
    pack.color.resize(n);
    pack.sh1.resize(pack.header.sh_degree == 1 ? 3 * n : 0);
    pack.chunks.resize(static_cast<std::size_t>(kChunkFloats) * pack.chunk_count());

    const std::size_t expected = sizeof(PackHeader) + pack.positions.size() * sizeof(float) +
                                 pack.scale_rot.size() * sizeof(std::uint32_t) +
                                 pack.color.size() * sizeof(std::uint32_t) +
                                 pack.sh1.size() * sizeof(std::uint32_t) + pack.chunks.size() * sizeof(float);
    if (bytes.size() != expected) {
        return R::failure("pack size mismatch: " + std::to_string(bytes.size()) + " bytes, header implies " +
                          std::to_string(expected));
    }
    const std::uint8_t* p = bytes.data() + sizeof(PackHeader);
    const auto take = [&p](void* dst, std::size_t count_bytes) {
        std::memcpy(dst, p, count_bytes);
        p += count_bytes;
    };
    take(pack.positions.data(), pack.positions.size() * sizeof(float));
    take(pack.scale_rot.data(), pack.scale_rot.size() * sizeof(std::uint32_t));
    take(pack.color.data(), pack.color.size() * sizeof(std::uint32_t));
    take(pack.sh1.data(), pack.sh1.size() * sizeof(std::uint32_t));
    take(pack.chunks.data(), pack.chunks.size() * sizeof(float));
    return R::success(std::move(pack));
}

Result<PackHeader> parse_pack_header(const std::vector<std::uint8_t>& bytes) {
    using R = Result<PackHeader>;
    if (bytes.size() < sizeof(PackHeader)) return R::failure("pack data is smaller than the 64-byte header");
    PackHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kPackMagic) return R::failure("not a PocketSplat pack (bad magic)");
    if (header.version != kPackVersion) {
        return R::failure("unsupported pack version " + std::to_string(header.version));
    }
    if (header.sh_degree > 1) return R::failure("pack header carries invalid SH degree");
    return R::success(header);
}

GpuPack subset_pack(const GpuPack& pack, std::size_t max_count, std::vector<std::uint32_t>* kept) {
    const std::size_t n = pack.header.count;
    const auto log_scale = [&](std::size_t i, int axis) {
        const std::uint32_t w = pack.scale_rot[3 * i + (axis == 2 ? 1u : 0u)];
        const auto bits = static_cast<std::uint16_t>(axis == 1 ? (w >> 16) : (w & 0xFFFFu));
        return glm::unpackHalf1x16(bits);
    };
    std::vector<std::uint32_t> index;
    if (n <= max_count) {
        index.resize(n);
        for (std::size_t i = 0; i < n; ++i) index[i] = static_cast<std::uint32_t>(i);
        if (kept != nullptr) *kept = std::move(index);
        return pack;
    }
    std::vector<float> imp(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float alpha = static_cast<float>(pack.color[i] >> 24) / 255.0f;
        const float sx = std::exp(log_scale(i, 0)), sy = std::exp(log_scale(i, 1)),
                    sz = std::exp(log_scale(i, 2));
        imp[i] = alpha * (sx * sy + sy * sz + sz * sx);
    }
    std::vector<float> sorted = imp;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(max_count - 1),
                     sorted.end(), std::greater<float>());
    const float threshold = sorted[max_count - 1];
    std::vector<char> keep(n, 0);
    std::size_t count = 0;
    for (std::size_t i = 0; i < n && count < max_count; ++i) {  // strictly above first
        if (imp[i] > threshold) keep[i] = 1, ++count;
    }
    for (std::size_t i = 0; i < n && count < max_count; ++i) {  // fill ties in pack order
        if (keep[i] == 0 && imp[i] == threshold) keep[i] = 1, ++count;
    }
    index.reserve(count);
    for (std::size_t i = 0; i < n; ++i) {
        if (keep[i] != 0) index.push_back(static_cast<std::uint32_t>(i));
    }

    GpuPack out;
    out.header = pack.header;
    out.header.count = static_cast<std::uint32_t>(index.size());
    const bool sh1 = !pack.sh1.empty();
    out.positions.resize(3 * index.size());
    out.scale_rot.resize(3 * index.size());
    out.color.resize(index.size());
    if (sh1) out.sh1.resize(3 * index.size());
    for (int c = 0; c < 3; ++c) {
        out.header.aabb_min[c] = std::numeric_limits<float>::max();
        out.header.aabb_max[c] = std::numeric_limits<float>::lowest();
    }
    for (std::size_t k = 0; k < index.size(); ++k) {
        const std::size_t i = index[k];
        for (std::size_t c = 0; c < 3; ++c) {
            out.positions[3 * k + c] = pack.positions[3 * i + c];
            out.scale_rot[3 * k + c] = pack.scale_rot[3 * i + c];
            if (sh1) out.sh1[3 * k + c] = pack.sh1[3 * i + c];
            out.header.aabb_min[c] = std::min(out.header.aabb_min[c], out.positions[3 * k + c]);
            out.header.aabb_max[c] = std::max(out.header.aabb_max[c], out.positions[3 * k + c]);
        }
        out.color[k] = pack.color[i];
    }
    // Chunk cull bounds as build_pack: center +/- 3 x the largest scale, and the importance max.
    out.chunks.resize(static_cast<std::size_t>(kChunkFloats) * out.chunk_count());
    for (std::size_t chunk = 0; chunk < out.chunk_count(); ++chunk) {
        const std::size_t begin = chunk * kPackChunkSize;
        const std::size_t end = std::min(index.size(), begin + kPackChunkSize);
        float mn[3], mx[3];
        std::fill_n(mn, 3, std::numeric_limits<float>::max());
        std::fill_n(mx, 3, std::numeric_limits<float>::lowest());
        float importance_max = 0.0f;
        for (std::size_t k = begin; k < end; ++k) {
            const std::size_t i = index[k];
            const float r = 3.0f * std::exp(std::max({log_scale(i, 0), log_scale(i, 1), log_scale(i, 2)}));
            for (std::size_t c = 0; c < 3; ++c) {
                mn[c] = std::min(mn[c], out.positions[3 * k + c] - r);
                mx[c] = std::max(mx[c], out.positions[3 * k + c] + r);
            }
            importance_max = std::max(importance_max, imp[i]);
        }
        float* dst = out.chunks.data() + kChunkFloats * chunk;
        for (int c = 0; c < 3; ++c) {
            dst[c] = mn[c];
            dst[3 + c] = mx[c];
        }
        dst[6] = importance_max;
    }
    if (kept != nullptr) *kept = std::move(index);
    return out;
}

}  // namespace splatcore
