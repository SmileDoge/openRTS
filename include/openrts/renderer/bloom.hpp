#pragma once

// Bloom (openRTS's own, the HDR experimental path): the light of what is
// brighter than the eye takes spread round it, as a lens and an eye scatter
// it. From the scene: what is over 1 before the exposure (the sun, the sky's
// brightest, lights laid over lit surfaces), and at night (`textures`) the
// brightest of what the textures paint, near white as displayed: a lamp's
// bulb and a lit window in a night texture, a sign; a dark wall, or a lamp's
// pool painted onto a platform, stay below it. The lights come as a source of
// their own: the signals' and the train's glows drawn once more, tested
// against the camera's depth pre-pass (a lamp behind a house does not bloom;
// the cab's glass, blended, is not in the pre-pass).
//
// The source is taken down a chain of half-size images (a 13-tap filter,
// the first with Karis's average against fireflies) and back up (a 3 x 3
// tent each, added), and the tone mapper adds the result to the scene before
// its exposure.

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

struct BloomSettings {
    bool textures = true; // at night the textures' brightest (bulbs, lit windows)
    float night = 0.0f;   // 0 day .. 1 night (the tone curve's)
    bool linear = false;  // the scene frame and the lights' source hold linear light (frame_encoding())
};

class BloomRenderer {
public:
    explicit BloomRenderer(WGPUDevice device);
    ~BloomRenderer();
    BloomRenderer(const BloomRenderer&) = delete;
    BloomRenderer& operator=(const BloomRenderer&) = delete;

    [[nodiscard]] bool ready() const noexcept;

    // The lights' source for this frame at the frame's size (hdr_format,
    // cleared): a pass depth-tested against `depth` (single-sample, read
    // only; null: no pass, the glows are left out). The caller draws the
    // glows (GlowSpriteRenderer::render_bloom) and ends it.
    [[nodiscard]] WGPURenderPassEncoder begin_sources(WGPUCommandEncoder encoder, std::uint32_t width, std::uint32_t height,
                                                      WGPUTextureView depth);
    // The bloom of `scene` (the HDR frame, display-encoded as drawn) and the
    // sources (black when begin_sources was not called this frame): the view
    // of the result (half the frame's size, scene-linear), for ToneMapper.
    [[nodiscard]] WGPUTextureView build(WGPUCommandEncoder encoder, WGPUTextureView scene, std::uint32_t width, std::uint32_t height,
                                        const BloomSettings& settings);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
