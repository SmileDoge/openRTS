#ifdef OPENRTS_HAS_WGPU
#include "gpu_timer.hpp"

#include <cstring>

#include <webgpu/wgpu.h>

namespace openrts::viewer {

GpuFrameTimer::GpuFrameTimer(const WGPUDevice device, const WGPUQueue queue) : device_(device) {
    if (!wgpuDeviceHasFeature(device, WGPUFeatureName_TimestampQuery)) return;
    const WGPUQuerySetDescriptor query{.type = WGPUQueryType_Timestamp, .count = 2};
    query_set_ = wgpuDeviceCreateQuerySet(device, &query);
    const WGPUBufferDescriptor resolve{
        .usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc,
        .size = 2 * sizeof(std::uint64_t),
    };
    resolve_ = wgpuDeviceCreateBuffer(device, &resolve);
    for (auto& slot : slots_) {
        const WGPUBufferDescriptor readback{
            .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
            .size = 2 * sizeof(std::uint64_t),
        };
        slot = {.owner = this, .buffer = wgpuDeviceCreateBuffer(device, &readback)};
    }
    // Timestamps are in ticks of this many nanoseconds.
    period_ns_ = wgpuQueueGetTimestampPeriod(queue);
    writes_ = {.querySet = query_set_, .beginningOfPassWriteIndex = 0, .endOfPassWriteIndex = 1};
}

GpuFrameTimer::~GpuFrameTimer() {
    // Pending map callbacks point at the slots: let them finish first.
    bool pending = false;
    for (const auto& slot : slots_) pending = pending || slot.busy;
    if (pending) wgpuDevicePoll(device_, true, nullptr);
    for (auto& slot : slots_) {
        if (slot.buffer) wgpuBufferRelease(slot.buffer);
    }
    if (resolve_) wgpuBufferRelease(resolve_);
    if (query_set_) wgpuQuerySetRelease(query_set_);
}

const WGPURenderPassTimestampWrites* GpuFrameTimer::begin_frame() {
    current_ = nullptr;
    if (!available()) return nullptr;
    for (auto& slot : slots_) {
        if (!slot.busy) {
            current_ = &slot;
            return &writes_;
        }
    }
    return nullptr;
}

void GpuFrameTimer::resolve(const WGPUCommandEncoder encoder) {
    if (!current_) return;
    wgpuCommandEncoderResolveQuerySet(encoder, query_set_, 0, 2, resolve_, 0);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, resolve_, 0, current_->buffer, 0, 2 * sizeof(std::uint64_t));
}

void GpuFrameTimer::submitted() {
    if (!current_) return;
    current_->busy = true;
    const WGPUBufferMapCallbackInfo callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = on_mapped,
        .userdata1 = current_,
    };
    wgpuBufferMapAsync(current_->buffer, WGPUMapMode_Read, 0, 2 * sizeof(std::uint64_t), callback);
    current_ = nullptr;
}

void GpuFrameTimer::on_mapped(const WGPUMapAsyncStatus status, WGPUStringView, void* const slot_pointer, void*) {
    auto& slot = *static_cast<Slot*>(slot_pointer);
    slot.busy = false;
    if (status != WGPUMapAsyncStatus_Success) return;
    std::uint64_t ticks[2]{};
    if (const auto* data = wgpuBufferGetConstMappedRange(slot.buffer, 0, sizeof(ticks))) std::memcpy(ticks, data, sizeof(ticks));
    wgpuBufferUnmap(slot.buffer);
    if (ticks[1] <= ticks[0]) return;
    slot.owner->sum_ms_ += static_cast<double>(ticks[1] - ticks[0]) * slot.owner->period_ns_ / 1.0e6;
    ++slot.owner->count_;
}

double GpuFrameTimer::take_sum_ms(int& count) {
    const double sum = sum_ms_;
    count = count_;
    sum_ms_ = 0.0;
    count_ = 0;
    return sum;
}

} // namespace openrts::viewer
#endif
