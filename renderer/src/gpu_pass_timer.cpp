#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/gpu_pass_timer.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include <webgpu/wgpu.h>

namespace openrts::renderer {

const char* gpu_pass_name(const GpuPass pass) noexcept {
    switch (pass) {
    case GpuPass::sun_shadows: return "sun shadows";
    case GpuPass::cab_shadow: return "cab shadow map";
    case GpuPass::lamp_shadows: return "lamp shadows";
    case GpuPass::depth_prepass: return "depth pre-pass";
    case GpuPass::occlusion: return "GTAO";
    case GpuPass::occlusion_blur: return "GTAO blur";
    case GpuPass::light_clusters: return "light clusters";
    case GpuPass::clouds: return "clouds";
    case GpuPass::cloud_shadows: return "cloud shadows";
    case GpuPass::sky_table: return "sky table";
    case GpuPass::main: return "main pass";
    case GpuPass::depth_copy: return "depth copy";
    case GpuPass::tone: return "tone curve";
    case GpuPass::overlay: return "interface";
    case GpuPass::reflection_probe: return "water's sky";
    case GpuPass::count: break;
    }
    return "";
}

GpuPassTimer::GpuPassTimer(const WGPUDevice device, const WGPUQueue queue) : device_(device) {
    if (!wgpuDeviceHasFeature(device, WGPUFeatureName_TimestampQuery)) return;
    const WGPUQuerySetDescriptor query{.type = WGPUQueryType_Timestamp, .count = max_pairs * 2};
    query_set_ = wgpuDeviceCreateQuerySet(device, &query);
    if (!query_set_) return;
    constexpr std::uint64_t bytes = max_pairs * 2 * sizeof(std::uint64_t);
    const WGPUBufferDescriptor resolve{.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc, .size = bytes};
    resolve_ = wgpuDeviceCreateBuffer(device, &resolve);
    for (auto& slot : slots_) {
        const WGPUBufferDescriptor readback{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, .size = bytes};
        slot.owner = this;
        slot.buffer = wgpuDeviceCreateBuffer(device, &readback);
    }
    // Timestamps are in ticks of this many nanoseconds.
    period_ns_ = wgpuQueueGetTimestampPeriod(queue);
}

GpuPassTimer::~GpuPassTimer() {
    if (gpu_pass_timer() == this) gpu_pass_timer() = nullptr;
    // Pending map callbacks point at the slots: let them finish first.
    bool pending = false;
    for (const auto& slot : slots_) pending = pending || slot.busy;
    if (pending) wgpuDevicePoll(device_, true, nullptr);
    for (auto& slot : slots_)
        if (slot.buffer) wgpuBufferRelease(slot.buffer);
    if (resolve_) wgpuBufferRelease(resolve_);
    if (query_set_) wgpuQuerySetRelease(query_set_);
}

void GpuPassTimer::begin_frame() {
    current_ = nullptr;
    used_ = 0;
    resolved_ = false;
    if (!available()) return;
    for (auto& slot : slots_) {
        if (!slot.busy) {
            current_ = &slot;
            return;
        }
    }
}

std::optional<std::uint32_t> GpuPassTimer::next_pair(const GpuPass pass) {
    if (!current_ || resolved_) return std::nullopt;
    const auto pair = used_.fetch_add(1);
    if (pair >= max_pairs) return std::nullopt;
    // Each pair its own element: the workers write different ones.
    passes_[pair] = pass;
    return pair;
}

std::optional<WGPURenderPassTimestampWrites> GpuPassTimer::render_pass(const GpuPass pass) {
    const auto pair = next_pair(pass);
    if (!pair) return std::nullopt;
    return WGPURenderPassTimestampWrites{.querySet = query_set_, .beginningOfPassWriteIndex = *pair * 2, .endOfPassWriteIndex = *pair * 2 + 1};
}

std::optional<WGPUComputePassTimestampWrites> GpuPassTimer::compute_pass(const GpuPass pass) {
    const auto pair = next_pair(pass);
    if (!pair) return std::nullopt;
    return WGPUComputePassTimestampWrites{.querySet = query_set_, .beginningOfPassWriteIndex = *pair * 2, .endOfPassWriteIndex = *pair * 2 + 1};
}

WGPUCommandBuffer GpuPassTimer::finish() {
    resolved_ = true;
    if (!current_) return nullptr;
    const auto pairs = (std::min)(used_.load(), max_pairs);
    if (pairs == 0) {
        current_ = nullptr;
        return nullptr;
    }
    current_->pairs = pairs;
    std::copy_n(passes_.begin(), pairs, current_->passes.begin());
    const auto encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
    wgpuCommandEncoderResolveQuerySet(encoder, query_set_, 0, pairs * 2, resolve_, 0);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, resolve_, 0, current_->buffer, 0, pairs * 2 * sizeof(std::uint64_t));
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    return commands;
}

void GpuPassTimer::submitted() {
    if (!current_ || current_->pairs == 0) {
        current_ = nullptr;
        return;
    }
    current_->busy = true;
    const WGPUBufferMapCallbackInfo callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = on_mapped,
        .userdata1 = current_,
    };
    wgpuBufferMapAsync(current_->buffer, WGPUMapMode_Read, 0, current_->pairs * 2 * sizeof(std::uint64_t), callback);
    current_ = nullptr;
}

void GpuPassTimer::on_mapped(const WGPUMapAsyncStatus status, WGPUStringView, void* const slot_pointer, void*) {
    auto& slot = *static_cast<Slot*>(slot_pointer);
    slot.busy = false;
    if (status != WGPUMapAsyncStatus_Success) return;
    std::vector<std::uint64_t> ticks(slot.pairs * 2);
    const auto bytes = ticks.size() * sizeof(std::uint64_t);
    if (const auto* data = wgpuBufferGetConstMappedRange(slot.buffer, 0, bytes)) std::memcpy(ticks.data(), data, bytes);
    wgpuBufferUnmap(slot.buffer);
    auto& read = slot.owner->read_;
    const double period = slot.owner->period_ns_;
    std::uint64_t first = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t last = 0;
    bool any = false;
    for (std::uint32_t i = 0; i < slot.pairs; ++i) {
        const auto begin = ticks[i * 2];
        const auto end = ticks[i * 2 + 1];
        if (begin == 0 || end <= begin) continue;
        const auto pass = static_cast<std::size_t>(slot.passes[i]);
        read.ms[pass] += static_cast<double>(end - begin) * period / 1.0e6;
        read.seen[pass] = true;
        first = (std::min)(first, begin);
        last = (std::max)(last, end);
        any = true;
    }
    if (!any) return;
    read.frame_ms += static_cast<double>(last - first) * period / 1.0e6;
    ++read.frames;
}

GpuPassTimes GpuPassTimer::take() {
    auto taken = read_;
    read_ = {};
    return taken;
}

GpuPassTimer*& gpu_pass_timer() noexcept {
    static GpuPassTimer* timer = nullptr;
    return timer;
}

std::optional<WGPURenderPassTimestampWrites> render_pass_timestamps(const GpuPass pass) {
    auto* timer = gpu_pass_timer();
    return timer ? timer->render_pass(pass) : std::nullopt;
}

std::optional<WGPUComputePassTimestampWrites> compute_pass_timestamps(const GpuPass pass) {
    auto* timer = gpu_pass_timer();
    return timer ? timer->compute_pass(pass) : std::nullopt;
}

} // namespace openrts::renderer
#endif
