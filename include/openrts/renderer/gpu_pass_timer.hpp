#pragma once

// The GPU time of each of the frame's passes, from timestamp queries: what a
// frame costs the video card, pass by pass (the panel's Performance, the
// log's "perf:" line).
//
// A pass asks for its timestamp writes as it begins (any thread: the shadow
// cascades are recorded on workers). When every pass of the frame is
// recorded, finish() makes a command buffer of its own that resolves them,
// submitted after the frame's others; the results come a few frames later
// through mapped readback buffers (a frame is not measured while all of
// them are still in flight). Nothing when the device has no TimestampQuery.

#ifdef OPENRTS_HAS_WGPU
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

enum class GpuPass : std::uint8_t {
    sun_shadows,     // the cascades
    cab_shadow,      // the focus map around the cab
    lamp_shadows,
    depth_prepass,   // the camera's depth, for the ambient occlusion
    occlusion,       // GTAO
    occlusion_blur,
    light_clusters,
    clouds,          // openRTS's sky: the clouds marched
    cloud_shadows,
    sky_table,       // its atmosphere's table
    main,            // the scene
    depth_copy,      // for ReShade
    tone,            // HDR to the screen
    overlay,         // the panels over HDR
    reflection_probe, // the sky the water reflects, six faces
    count
};
inline constexpr std::size_t gpu_pass_count = static_cast<std::size_t>(GpuPass::count);
// English, for tr().
[[nodiscard]] const char* gpu_pass_name(GpuPass pass) noexcept;

struct GpuPassTimes {
    std::array<double, gpu_pass_count> ms{}; // summed over `frames`
    std::array<bool, gpu_pass_count> seen{};
    double frame_ms = 0.0; // the first pass's start to the last one's end, summed
    int frames = 0;
};

class GpuPassTimer {
public:
    GpuPassTimer(WGPUDevice device, WGPUQueue queue);
    ~GpuPassTimer();
    GpuPassTimer(const GpuPassTimer&) = delete;
    GpuPassTimer& operator=(const GpuPassTimer&) = delete;

    [[nodiscard]] bool available() const noexcept { return query_set_ != nullptr; }

    // A new frame: measured when a readback buffer is free.
    void begin_frame();
    // The writes for a pass beginning now; none when the frame is not
    // measured or has no queries left.
    [[nodiscard]] std::optional<WGPURenderPassTimestampWrites> render_pass(GpuPass pass);
    [[nodiscard]] std::optional<WGPUComputePassTimestampWrites> compute_pass(GpuPass pass);
    // After every pass of the frame is recorded (the workers' too): the
    // resolve, to be submitted after the frame's other buffers; null when
    // nothing was measured.
    [[nodiscard]] WGPUCommandBuffer finish();
    // After the submit.
    void submitted();
    // What was read since the last call.
    [[nodiscard]] GpuPassTimes take();

private:
    static constexpr std::uint32_t max_pairs = 64;
    struct Slot {
        GpuPassTimer* owner = nullptr;
        WGPUBuffer buffer = nullptr;
        bool busy = false;
        std::uint32_t pairs = 0;
        std::array<GpuPass, max_pairs> passes{};
    };
    [[nodiscard]] std::optional<std::uint32_t> next_pair(GpuPass pass);
    static void on_mapped(WGPUMapAsyncStatus status, WGPUStringView message, void* slot, void* unused);

    static constexpr std::size_t slot_count = 3;
    WGPUDevice device_ = nullptr;
    WGPUQuerySet query_set_ = nullptr;
    WGPUBuffer resolve_ = nullptr;
    std::array<Slot, slot_count> slots_{};
    Slot* current_ = nullptr;   // this frame's readback, null: not measured
    std::atomic<std::uint32_t> used_{0}; // pairs asked for this frame
    std::array<GpuPass, max_pairs> passes_{};
    bool resolved_ = false;
    double period_ns_ = 1.0;
    GpuPassTimes read_{};
};

// The timer the frame's passes write into (the window's; null: none).
[[nodiscard]] GpuPassTimer*& gpu_pass_timer() noexcept;
// A pass's writes from that timer, if any (for the descriptors' timestampWrites).
[[nodiscard]] std::optional<WGPURenderPassTimestampWrites> render_pass_timestamps(GpuPass pass);
[[nodiscard]] std::optional<WGPUComputePassTimestampWrites> compute_pass_timestamps(GpuPass pass);

} // namespace openrts::renderer
#endif
