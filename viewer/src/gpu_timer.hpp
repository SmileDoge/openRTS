#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <array>
#include <cstdint>

#include <webgpu/webgpu.h>

namespace openrts::viewer {

// GPU time of the main render pass from timestamp queries. Results arrive a
// few frames late through mapped readback buffers; a frame is skipped when all
// of them are still in flight. Unavailable when the device lacks the
// TimestampQuery feature.
class GpuFrameTimer {
public:
    GpuFrameTimer(WGPUDevice device, WGPUQueue queue);
    ~GpuFrameTimer();

    GpuFrameTimer(const GpuFrameTimer&) = delete;
    GpuFrameTimer& operator=(const GpuFrameTimer&) = delete;

    [[nodiscard]] bool available() const noexcept { return query_set_ != nullptr; }

    // Timestamp writes for this frame's main pass, or nullptr (not measured).
    [[nodiscard]] const WGPURenderPassTimestampWrites* begin_frame();
    // After the pass ended, before the encoder is finished.
    void resolve(WGPUCommandEncoder encoder);
    // After the queue submit; the result is read when the device is polled.
    void submitted();

    // Sum of the milliseconds measured since the last call, and their count.
    [[nodiscard]] double take_sum_ms(int& count);

private:
    struct Slot {
        GpuFrameTimer* owner = nullptr;
        WGPUBuffer buffer = nullptr;
        bool busy = false;
    };
    static void on_mapped(WGPUMapAsyncStatus status, WGPUStringView message, void* slot, void* unused);

    static constexpr std::size_t slot_count = 3;
    WGPUDevice device_ = nullptr;
    WGPUQuerySet query_set_ = nullptr;
    WGPUBuffer resolve_ = nullptr;
    std::array<Slot, slot_count> slots_{};
    WGPURenderPassTimestampWrites writes_{};
    Slot* current_ = nullptr;
    double period_ns_ = 1.0;
    double sum_ms_ = 0.0;
    int count_ = 0;
};

} // namespace openrts::viewer
#endif
