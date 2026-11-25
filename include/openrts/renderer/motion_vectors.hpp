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

// The frame as the motion vectors take it.
struct MotionFrame {
    glm::mat4 view_projection{1.0f};        // this frame's, with its jitter (what the scene was drawn with)
    glm::mat4 steady_view_projection{1.0f}; // the same without the jitter
    glm::vec3 eye{0.0f};                     // the camera's place (route space)
    // The player's train's place (route space; none: no train): the trains'
    // pixels move by its move.
    std::optional<glm::vec3> train;
    std::uint32_t width = 0, height = 0;
};

// The screen's motion of every pixel since the last frame (openRTS; for the
// temporal AA and the upscalers): from the camera's depth pre-pass through
// the last frame's view without the jitter, the trains' pixels
// (SceneEffects::moving_depth) moved by the player's train's move. One
// rgba16f texel a pixel: xy the motion to where it was, in texture
// coordinates (0..1 across); z 1 a train's; w 1 where there is a last frame
// (0 after a cut or a jump of the camera).
//
// It also keeps the frame's jitter: Halton 2 / 3 over 8 frames, in pixels.
class MotionVectors {
public:
    explicit MotionVectors(WGPUDevice device);
    ~MotionVectors();
    MotionVectors(const MotionVectors&) = delete;
    MotionVectors& operator=(const MotionVectors&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // The next frame has no last frame (a cut).
    void reset() noexcept;
    // This frame's jitter in pixels (-0.5..0.5).
    [[nodiscard]] glm::vec2 jitter() const noexcept;
    // Adds the jitter to a projection for a frame `width` x `height`.
    [[nodiscard]] glm::mat4 jittered(const glm::mat4& projection, std::uint32_t width, std::uint32_t height) const noexcept;
    // Draws this frame's motion and moves to the next jitter; null without
    // the depth.
    [[nodiscard]] WGPUTextureView compute(WGPUCommandEncoder encoder, WGPUTextureView depth, WGPUTextureView moving_depth,
                                          const MotionFrame& frame);
    // Before compute(): the last frame's view-projection without the jitter,
    // and whether there is a last frame (none after reset() or a jump).
    [[nodiscard]] glm::mat4 last_view_projection() const noexcept;
    [[nodiscard]] bool has_last() const noexcept;
    // The last compute()'s texture (null before one).
    [[nodiscard]] WGPUTextureView view() const noexcept;
    [[nodiscard]] WGPUTexture texture() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
