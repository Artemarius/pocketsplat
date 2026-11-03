// sortbench — standalone correctness test + benchmark for the radix sort (ADR-0004).
// Verifies key order AND stability against a CPU std::stable_sort reference on random keys
// (defaults cover the 1e5..4e6 range required before any sort change merges), then reports
// GPU timings for two input orders that bracket the renderer's case:
//   presorted — re-sorting already sorted data: every downsweep scatter is coherent (best case,
//               the only number older versions of this tool reported);
//   unsorted  — the original random order restored before every run: scattered writes (worst
//               case; the renderer's depth keys in Morton order fall in between).
// --stamp-probe additionally checks that the driver honors intermediate timestamps between
// dependent compute work (the renderer's per-pass split relies on it). --profile splits the
// unsorted time into the upsweep / spine / downsweep stages. Unsorted and profile runs use the
// renderer's mode (the last pass writes only the values); --keep-keys times them with the sorted
// keys written back too. --count-rows N overrides the downsweep's shared count rows (occupancy
// knob); --ranking mask selects the shared-bitmask downsweep, --scatter staged the block-staged
// one (whole block ranked, staged in shared memory, written in runs). --subgroup-probe reports the
// device's subgroup layout and checks the mapping the ballot ranking's stability relies on.
// --stamp-stage all|compute|top (default bottom-of-pipe) and --stamp-barrier change how --profile
// stamps.
// Headless, so it also runs as a plain arm64 executable on Android
// (`adb shell /data/local/tmp/sortbench`).
// Exit code: 0 = all sizes pass, 1 = any mismatch or setup failure.

#include <volk.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "splatrender/radix_sort.h"
#include "splatrender/vk_buffer.h"
#include "splatrender/vk_context.h"
#include "splatrender/vk_descriptors.h"
#include "splatrender/vk_pipeline.h"
#include "subgroup_probe_comp.spv.h"

namespace {

using splatrender::Buffer;
using splatrender::Context;

constexpr std::uint32_t kMaxStamps = 16;  // --profile: 3 stages x 4 digit passes + 2

// One-shot command submission (load/measure tool; blocking by design).
class OneShot {
public:
    explicit OneShot(Context& context) : m_context(context) {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = context.queue_family();
        vkCreateCommandPool(context.device(), &pool_info, nullptr, &m_pool);
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = m_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        vkAllocateCommandBuffers(context.device(), &alloc_info, &m_cmd);

        VkQueryPoolCreateInfo query_info{};
        query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = kMaxStamps;
        vkCreateQueryPool(context.device(), &query_info, nullptr, &m_queries);
    }
    ~OneShot() {
        vkDestroyQueryPool(m_context.device(), m_queries, nullptr);
        vkDestroyCommandPool(m_context.device(), m_pool, nullptr);
    }

    VkCommandBuffer begin() {
        vkResetCommandBuffer(m_cmd, 0);
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &begin_info);
        vkCmdResetQueryPool(m_cmd, m_queries, 0, kMaxStamps);
        vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_queries, 0);
        m_stamps = 1;
        return m_cmd;
    }

    // Intermediate boundary (segments are split here when finish() reads them back).
    void stamp(VkPipelineStageFlags2 stage) {
        if (m_stamps < kMaxStamps - 1) vkCmdWriteTimestamp2(m_cmd, stage, m_queries, m_stamps++);
    }

    // Ends, submits, waits; returns GPU milliseconds from the first to the last timestamp.
    double finish() {
        vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_queries, m_stamps++);
        vkEndCommandBuffer(m_cmd);
        VkCommandBufferSubmitInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_info.commandBuffer = m_cmd;
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_info;
        vkQueueSubmit2(m_context.queue(), 1, &submit_info, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_context.queue());
        std::uint64_t stamps[kMaxStamps] = {};
        vkGetQueryPoolResults(m_context.device(), m_queries, 0, m_stamps, sizeof(stamps), stamps,
                              sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
        const double period_ms = static_cast<double>(m_context.timestamp_period_ns()) / 1e6;
        m_segments.clear();
        for (std::uint32_t i = 1; i < m_stamps; ++i) {
            m_segments.push_back(static_cast<double>(stamps[i] - stamps[i - 1]) * period_ms);
        }
        return static_cast<double>(stamps[m_stamps - 1] - stamps[0]) * period_ms;
    }

    const std::vector<double>& segments() const { return m_segments; }

private:
    Context& m_context;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkQueryPool m_queries = VK_NULL_HANDLE;
    std::uint32_t m_stamps = 0;
    std::vector<double> m_segments;
};

bool download(Context& context, OneShot& shot, Buffer& src, void* dst, VkDeviceSize bytes) {
    Buffer staging;
    if (!staging
             .init(context.allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
             .ok()) {
        return false;
    }
    VkCommandBuffer cmd = shot.begin();
    VkBufferCopy region{0, 0, bytes};
    vkCmdCopyBuffer(cmd, src.handle(), staging.handle(), 1, &region);
    shot.finish();
    staging.invalidate();
    std::memcpy(dst, staging.mapped(), static_cast<std::size_t>(bytes));
    return true;
}

double median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples.empty() ? 0.0 : samples[samples.size() / 2];
}

// Device copies of one random input: pristine originals plus the working pair the sort permutes.
struct SortInput {
    std::uint32_t n = 0;
    VkDeviceSize bytes = 0;
    std::vector<std::uint32_t> keys;
    Buffer pristine_keys, pristine_values, keys_buffer, values_buffer;

    // Untimed submission that puts the original (unsorted) order back into the working pair.
    void restore(OneShot& shot) const {
        VkCommandBuffer cmd = shot.begin();
        const VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(cmd, pristine_keys.handle(), keys_buffer.handle(), 1, &region);
        vkCmdCopyBuffer(cmd, pristine_values.handle(), values_buffer.handle(), 1, &region);
        shot.finish();
    }
};

bool make_input(Context& context, std::uint32_t n, int key_bits, SortInput& input) {
    std::mt19937 rng(12345u + n);
    const std::uint32_t key_mask = key_bits == 16 ? 0xFFFFu : 0xFFFFFFFFu;
    input.n = n;
    input.bytes = VkDeviceSize{n} * sizeof(std::uint32_t);
    input.keys.resize(n);
    // Half full-range, half low-range to force duplicate keys (stability coverage).
    for (std::uint32_t i = 0; i < n; ++i) {
        input.keys[i] = (rng() & key_mask) & (i % 2 == 0 ? key_mask : 0x3FFu);
    }
    std::vector<std::uint32_t> values(n);
    std::iota(values.begin(), values.end(), 0u);

    const VkBufferUsageFlags work_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return splatrender::create_device_buffer(context, input.keys.data(), input.bytes,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, input.pristine_keys)
               .ok() &&
           splatrender::create_device_buffer(context, values.data(), input.bytes,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, input.pristine_values)
               .ok() &&
           input.keys_buffer.init(context.allocator(), input.bytes, work_usage).ok() &&
           input.values_buffer.init(context.allocator(), input.bytes, work_usage).ok();
}

bool run_size(Context& context, splatrender::RadixSort& sort, OneShot& shot, std::uint32_t n, int key_bits,
              int bench_iters, bool keep_keys) {
    SortInput input;
    if (!make_input(context, n, key_bits, input)) {
        std::fprintf(stderr, "buffer setup failed at n=%u\n", n);
        return false;
    }

    // Correctness + stability reference: stable sort of (key, original index). Checked in the
    // renderer's values-only mode first, then with the sorted keys kept (the state the presorted
    // runs below start from).
    std::vector<std::uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
                     [&input](std::uint32_t a, std::uint32_t b) { return input.keys[a] < input.keys[b]; });
    std::vector<std::uint32_t> gpu_keys(n), gpu_values(n);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    for (const bool keep : {false, true}) {
        input.restore(shot);
        cmd = shot.begin();
        sort.record(cmd, input.keys_buffer, input.values_buffer, n, key_bits, keep);
        shot.finish();
        if (!download(context, shot, input.values_buffer, gpu_values.data(), input.bytes) ||
            (keep && !download(context, shot, input.keys_buffer, gpu_keys.data(), input.bytes))) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            if (gpu_values[i] != order[i] || (keep && gpu_keys[i] != input.keys[order[i]])) {
                std::fprintf(
                    stderr,
                    "MISMATCH n=%u bits=%d keep_keys=%d at %u: gpu value %u (key %u), expected value %u "
                    "(key %u)\n",
                    n, key_bits, keep ? 1 : 0, i, gpu_values[i], keep ? gpu_keys[i] : 0u, order[i],
                    input.keys[order[i]]);
                // Shape of the failure: how much is wrong, and whether the values are still a
                // permutation (misplaced) or not (lost / overwritten).
                std::uint32_t wrong = 0, zeros = 0, missing = 0;
                std::vector<std::uint8_t> seen(n, 0);
                for (std::uint32_t j = 0; j < n; ++j) {
                    wrong += gpu_values[j] != order[j] ? 1u : 0u;
                    zeros += gpu_values[j] == 0u ? 1u : 0u;
                    if (gpu_values[j] < n) seen[gpu_values[j]] = 1;
                }
                for (std::uint32_t j = 0; j < n; ++j) missing += seen[j] == 0 ? 1u : 0u;
                std::fprintf(stderr, "  %u of %u values wrong, %u zero, %u indices missing\n", wrong, n,
                             zeros, missing);
                return false;
            }
        }
    }

    // Presorted: the working pair holds sorted data now; every run re-sorts it.
    std::vector<double> presorted;
    for (int iteration = 0; iteration < bench_iters; ++iteration) {
        cmd = shot.begin();
        sort.record(cmd, input.keys_buffer, input.values_buffer, n, key_bits);
        presorted.push_back(shot.finish());
    }
    // Unsorted: restore the random order before each timed run.
    std::vector<double> unsorted;
    for (int iteration = 0; iteration < bench_iters; ++iteration) {
        input.restore(shot);
        cmd = shot.begin();
        sort.record(cmd, input.keys_buffer, input.values_buffer, n, key_bits, keep_keys);
        unsorted.push_back(shot.finish());
    }
    const double unsorted_ms = median(unsorted);
    std::printf(
        "n=%8u bits=%d  PASS  presorted best %.3f ms median %.3f ms | unsorted best %.3f ms median %.3f ms"
        "  (%.2f ns/key unsorted)\n",
        n, key_bits, *std::min_element(presorted.begin(), presorted.end()), median(presorted),
        *std::min_element(unsorted.begin(), unsorted.end()), unsorted_ms, unsorted_ms * 1e6 / n);
    return true;
}

// Two dependent sorts with one intermediate timestamp: sort A on the unsorted input, then sort B
// on A's (now sorted) output. A driver that honors the stamp reports A ≈ the unsorted median and
// B ≈ the presorted median; one that collapses boundaries reports A ≈ 0 and B ≈ the sum.
bool stamp_probe(Context& context, splatrender::RadixSort& sort, OneShot& shot, std::uint32_t n, int iters) {
    SortInput input;
    if (!make_input(context, n, 32, input)) return false;
    struct Variant {
        const char* name;
        VkPipelineStageFlags2 stage;
    };
    const Variant variants[] = {{"ALL_COMMANDS", VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                {"COMPUTE_SHADER", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
                                {"BOTTOM_OF_PIPE", VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT},
                                {"TOP_OF_PIPE", VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT}};
    for (const Variant& variant : variants) {
        std::vector<double> first, second, total;
        for (int iteration = 0; iteration < iters; ++iteration) {
            input.restore(shot);
            VkCommandBuffer cmd = shot.begin();
            sort.record(cmd, input.keys_buffer, input.values_buffer, n, 32);  // ends with a barrier
            shot.stamp(variant.stage);
            sort.record(cmd, input.keys_buffer, input.values_buffer, n, 32);
            total.push_back(shot.finish());
            first.push_back(shot.segments()[0]);
            second.push_back(shot.segments()[1]);
        }
        std::printf("stamp-probe n=%u mid-stamp %-14s  A %.3f ms  B %.3f ms  total %.3f ms (medians)\n", n,
                    variant.name, median(first), median(second), median(total));
    }
    return true;
}

// What the driver reports about the subgroups of a 256-wide workgroup, and whether the builtins
// agree with what ballots see. The ballot-ranked downsweeps are stable only if local invocation
// index == SubgroupID * SubgroupSize + SubgroupInvocationID (nothing in Vulkan guarantees it), so
// this is the check to run first on a new GPU (docs/sorting.md section 8).
bool subgroup_probe(Context& context, OneShot& shot) {
    constexpr std::uint32_t kGroups = 64, kLocal = 256, kFields = 8;
    const VkDeviceSize bytes = VkDeviceSize{kGroups} * kLocal * kFields * sizeof(std::uint32_t);
    Buffer rows;
    splatrender::DescriptorSetLayout layout;
    splatrender::DescriptorPool pool;
    splatrender::Pipeline pipeline;
    const VkDescriptorSetLayoutBinding bindings[] = {
        splatrender::storage_binding(0, VK_SHADER_STAGE_COMPUTE_BIT)};
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    if (!rows.init(context.allocator(), bytes,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
             .ok() ||
        !layout.init(context.device(), bindings).ok() || !pool.init(context.device(), sizes, 1).ok()) {
        return false;
    }
    splatcore::Result<VkDescriptorSet> set = pool.allocate(layout.handle());
    if (!set) return false;
    splatrender::write_buffer_descriptor(context.device(), set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         rows.handle());
    splatrender::ComputePipelineCreateInfo info;
    info.spirv = std::span(k_subgroup_probe_comp_spv);
    const VkDescriptorSetLayout layouts[] = {layout.handle()};
    info.set_layouts = layouts;
    info.workgroup_size = kLocal;
    if (!pipeline.init_compute(context.device(), info).ok()) return false;

    VkCommandBuffer cmd = shot.begin();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
    VkDescriptorSet handle = set.value();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0, 1, &handle, 0,
                            nullptr);
    vkCmdDispatch(cmd, kGroups, 1, 1);
    shot.finish();
    std::vector<std::uint32_t> data(static_cast<std::size_t>(bytes / sizeof(std::uint32_t)));
    if (!download(context, shot, rows, data.data(), bytes)) return false;

    std::uint32_t linear_ok = 0, rank_ok = 0, full_ok = 0;
    std::vector<std::string> shapes;
    for (std::uint32_t i = 0; i < kGroups * kLocal; ++i) {
        const std::uint32_t* r = &data[static_cast<std::size_t>(i) * kFields];
        linear_ok += r[0] == r[1] * r[3] + r[2] && r[7] == r[1] * r[3] ? 1u : 0u;
        rank_ok += r[2] == r[6] ? 1u : 0u;
        full_ok += r[5] == r[3] ? 1u : 0u;
        const std::string shape = "size " + std::to_string(r[3]) + " x " + std::to_string(r[4]) +
                                  " subgroups, " + std::to_string(r[5]) + " active";
        if (std::find(shapes.begin(), shapes.end(), shape) == shapes.end()) shapes.push_back(shape);
    }
    const std::uint32_t total = kGroups * kLocal;
    std::printf("subgroup-probe: ");
    for (const std::string& shape : shapes) std::printf("[%s] ", shape.c_str());
    std::printf("\n  linear mapping %u/%u, invocation id == ballot rank %u/%u, full subgroups %u/%u\n",
                linear_ok, total, rank_ok, total, full_ok, total);
    if (linear_ok == total && rank_ok == total) return true;
    std::printf("  rows (lid id inv size num active rank minlid):");
    for (const std::uint32_t i : {0u, 1u, 31u, 32u, 63u, 64u, 65u, 127u, 128u, 129u, 255u}) {
        const std::uint32_t* r = &data[static_cast<std::size_t>(i) * kFields];
        std::printf(" [%u %u %u %u %u %u %u %u]", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
    std::printf("\n");
    return false;
}

// How --profile writes its intermediate timestamps. BOTTOM_OF_PIPE by default, like the renderer's
// GpuTimer: the Adreno 830 writes ALL_COMMANDS and TOP_OF_PIPE stamps before the preceding
// dispatch has finished (docs/sorting.md section 8); --stamp-stage reproduces that.
struct ProfileStamp {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    const char* stage_name = "BOTTOM_OF_PIPE";
    bool full_barrier = false;  // an ALL_COMMANDS -> ALL_COMMANDS barrier right before each stamp
};

void full_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// Per-stage split of the unsorted case: one timestamp after every dispatch's trailing barrier
// (RadixSort's stage hook), summed per stage over the digit passes. Medians of the per-run sums;
// the first segment also holds the args upload (negligible).
bool profile_size(Context& context, splatrender::RadixSort& sort, OneShot& shot, std::uint32_t n,
                  int key_bits, int iters, bool keep_keys, const ProfileStamp& stamp) {
    SortInput input;
    if (!make_input(context, n, key_bits, input)) return false;
    struct Hook {
        OneShot* shot;
        ProfileStamp stamp;
        std::vector<std::string> stages;
    };
    Hook hook{&shot, stamp, {}};
    sort.set_stage_hook(
        [](void* user, VkCommandBuffer cmd, const char* stage) {
            Hook* h = static_cast<Hook*>(user);
            if (h->stamp.full_barrier) full_barrier(cmd);
            h->shot->stamp(h->stamp.stage);
            h->stages.emplace_back(stage);
        },
        &hook);
    std::vector<double> upsweep, spine, downsweep, total;
    for (int iteration = 0; iteration < iters; ++iteration) {
        input.restore(shot);
        hook.stages.clear();
        VkCommandBuffer cmd = shot.begin();
        sort.record(cmd, input.keys_buffer, input.values_buffer, n, key_bits, keep_keys);
        total.push_back(shot.finish());
        double sums[3] = {};
        const std::vector<double>& segments = shot.segments();
        for (std::size_t i = 0; i < hook.stages.size() && i < segments.size(); ++i) {
            const std::string& stage = hook.stages[i];
            sums[stage == "upsweep" ? 0 : stage == "spine" ? 1 : 2] += segments[i];
        }
        upsweep.push_back(sums[0]);
        spine.push_back(sums[1]);
        downsweep.push_back(sums[2]);
    }
    sort.set_stage_hook(nullptr, nullptr);
    const double total_ms = median(total);
    std::printf(
        "profile n=%8u bits=%d  upsweep %.3f  spine %.3f  downsweep %.3f  total %.3f ms  (%.2f ns/key)  "
        "[stamp %s%s]\n",
        n, key_bits, median(upsweep), median(spine), median(downsweep), total_ms, total_ms * 1e6 / n,
        stamp.stage_name, stamp.full_barrier ? " + full barrier" : "");
    return true;
}

std::vector<std::uint32_t> parse_list(const char* text) {
    std::vector<std::uint32_t> out;
    for (const char* p = text; *p != '\0';) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(p, &end, 10);
        if (end == p) break;
        out.push_back(static_cast<std::uint32_t>(value));
        p = *end == ',' ? end + 1 : end;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    bool validation = false;
    bool probe = false;
    bool profile = false;
    bool keep_keys = false;
    bool sg_probe = false;
    std::string gpu;  // --gpu: substring of the GPU name (default: discrete first)
    ProfileStamp profile_stamp;
    std::uint32_t count_rows = 0;
    splatrender::RadixRanking ranking = splatrender::RadixRanking::ballot;
    splatrender::RadixScatter scatter = splatrender::RadixScatter::direct;
    int iters = 20;
    std::vector<std::uint32_t> sizes32 = {100'000u, 262'144u, 1'000'000u, 2'000'000u, 4'000'000u};
    std::vector<std::uint32_t> sizes16 = {100'000u, 1'000'000u};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--validation") validation = true;
        if (arg == "--gpu" && i + 1 < argc) gpu = argv[++i];
        if (arg == "--stamp-probe") probe = true;
        if (arg == "--profile") profile = true;
        if (arg == "--keep-keys") keep_keys = true;
        if (arg == "--subgroup-probe") sg_probe = true;
        if (arg == "--stamp-barrier") profile_stamp.full_barrier = true;
        if (arg == "--stamp-stage" && i + 1 < argc) {
            const std::string name = argv[++i];
            if (name == "all") {
                profile_stamp.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                profile_stamp.stage_name = "ALL_COMMANDS";
            } else if (name == "compute") {
                profile_stamp.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                profile_stamp.stage_name = "COMPUTE_SHADER";
            } else if (name == "top") {
                profile_stamp.stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                profile_stamp.stage_name = "TOP_OF_PIPE";
            }
        }
        if (arg == "--count-rows" && i + 1 < argc)
            count_rows = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        if (arg == "--ranking" && i + 1 < argc && std::string(argv[++i]) == "mask")
            ranking = splatrender::RadixRanking::mask;
        if (arg == "--scatter" && i + 1 < argc) {
            const std::string name = argv[++i];
            if (name == "staged") scatter = splatrender::RadixScatter::staged;
        }
        if (arg == "--iters" && i + 1 < argc) iters = std::atoi(argv[++i]);
        if (arg == "--sizes" && i + 1 < argc) sizes32 = sizes16 = parse_list(argv[++i]);
    }

    Context context;
    splatrender::ContextCreateInfo info;
    info.app_name = "sortbench";
    info.enable_validation = validation;
    info.preferred_device = gpu.c_str();
    if (splatcore::Status s = context.init(info); !s.ok()) {
        std::fprintf(stderr, "sortbench: %s\n", s.error().c_str());
        return 1;
    }
    std::printf("device: %s\n", context.device_name());

    std::uint32_t max_n = 1'000'000u;
    for (std::uint32_t n : sizes32) max_n = std::max(max_n, n);
    splatrender::RadixSort sort;
    if (splatcore::Status s = sort.init(context, max_n, count_rows, ranking, scatter); !s.ok()) {
        std::fprintf(stderr, "sortbench: %s\n", s.error().c_str());
        return 1;
    }

    OneShot shot(context);
    bool ok = true;
    if (sg_probe) ok = subgroup_probe(context, shot) && ok;
    for (std::uint32_t n : sizes32) ok = run_size(context, sort, shot, n, 32, iters, keep_keys) && ok;
    for (std::uint32_t n : sizes16) ok = run_size(context, sort, shot, n, 16, iters, keep_keys) && ok;
    if (profile) {
        for (std::uint32_t n : sizes32) {
            ok = profile_size(context, sort, shot, n, 32, iters, keep_keys, profile_stamp) && ok;
        }
        for (std::uint32_t n : sizes16) {
            ok = profile_size(context, sort, shot, n, 16, iters, keep_keys, profile_stamp) && ok;
        }
    }
    if (probe) ok = stamp_probe(context, sort, shot, 1'000'000u, iters) && ok;
    const std::uint64_t validation_messages = context.validation_message_count();
    if (validation_messages > 0) {
        std::fprintf(stderr, "validation messages: %llu\n",
                     static_cast<unsigned long long>(validation_messages));
    }
    std::printf("%s\n", ok && validation_messages == 0 ? "ALL PASS" : "FAILURES");
    return ok && validation_messages == 0 ? 0 : 1;
}
