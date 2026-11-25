#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>
#include <optional>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

namespace openrts::renderer {

// The frame as the temporal AA takes it.
struct TemporalAAFrame {
    glm::mat4 view_projection{1.0f};          // this frame's, with its jitter (what the scene was drawn with)
    glm::mat4 steady_view_projection{1.0f};   // the same without the jitter (kept for the next frame)
    glm::vec3 eye{0.0f};                       // the camera's place (route space)
    // The player's train's place (route space; none: no train): the trains'
    // pixels are taken as moving by its move.
    std::optional<glm::vec3> train;
    std::uint32_t width = 0, height = 0;
    // What to show (the panel's preview): 0 the result, 1 the motion (and
    // the moving models), 2 the depth it reads, 3 the history alone, 4 how
    // much of the history the clip let go.
    int view = 0;
};

// Temporal anti-aliasing over the resolved HDR frame (openRTS's own; under
// MSAA, which stays for alpha to coverage): each frame drawn a sub-pixel
// apart (jitter()), blended with the frames before it found where they
// were. Motion: from the camera's depth pre-pass, the camera's last view
// and place; the moving models' pixels (SceneEffects::moving_depth: the
// trains) as moving with the camera's place (the cab the eye rides in),
// the history clipped to the pixel's neighbours' colours either way.
class TemporalAA {
public:
    explicit TemporalAA(WGPUDevice device);
    ~TemporalAA();
    TemporalAA(const TemporalAA&) = delete;
    TemporalAA& operator=(const TemporalAA&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // The next frame takes no history (a cut, a jump of the camera).
    void reset() noexcept;
    // This frame's jitter in pixels (-0.5..0.5, Halton 2 / 3 over 8 frames).
    [[nodiscard]] glm::vec2 jitter() const noexcept;
    // Adds the jitter to a projection for a frame `width` x `height`.
    [[nodiscard]] glm::mat4 jittered(const glm::mat4& projection, std::uint32_t width, std::uint32_t height) const noexcept;
    // Blends `scene` (the frame's HDR, resolved) with the history into a
    // texture of its own and returns its view (the frame to tone map);
    // `scene` itself when it cannot (no depth). Moves to the next jitter.
    [[nodiscard]] WGPUTextureView resolve(WGPUCommandEncoder encoder, WGPUTextureView scene, WGPUTextureView depth,
                                          WGPUTextureView moving_depth, const TemporalAAFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
