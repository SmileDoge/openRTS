#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

// The frame as the temporal AA takes it.
struct TemporalAAFrame {
    std::uint32_t width = 0, height = 0;
    // What to show (the panel's preview): 0 the result, 1 the motion (and
    // the trains), 2 the depth it reads, 3 the history alone, 4 how much of
    // the history the clip let go.
    int view = 0;
};

// Temporal anti-aliasing over the resolved HDR frame (openRTS's own; over
// MSAA, which stays for alpha to coverage): each frame drawn a sub-pixel
// apart (MotionVectors::jitter), blended with the frames before it, found
// where they were by the motion vectors (MotionVectors), the history
// clipped to the pixel's neighbours' colours.
class TemporalAA {
public:
    explicit TemporalAA(WGPUDevice device);
    ~TemporalAA();
    TemporalAA(const TemporalAA&) = delete;
    TemporalAA& operator=(const TemporalAA&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // The next frame takes no history (a cut).
    void reset() noexcept;
    // Blends `scene` (the frame's HDR, resolved) with the history into a
    // texture of its own and returns its view (the frame to tone map);
    // `scene` itself when it cannot (no depth or motion).
    [[nodiscard]] WGPUTextureView resolve(WGPUCommandEncoder encoder, WGPUTextureView scene, WGPUTextureView depth,
                                          WGPUTextureView motion, const TemporalAAFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
