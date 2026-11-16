#pragma once

// Light shafts (openRTS's own, the HDR experimental path; RTS never had
// them): the sun's light the air scatters towards the eye where nothing
// shades it, so the shadows of trees, masts, bridges, the cab's frame and
// the clouds stand in the air as beams.
//
// At half the frame's size each pixel's view ray is marched from the eye to
// what the camera's depth pre-pass shows there (SceneEffects::camera_depth),
// 32 steps closer together near the eye, each asking the effects' shadows
// (the cab's map, the cascades; group 2 of the experimental pipelines) and
// the clouds' shadow whether the sun reaches it; past the last cascade on to
// 1.5 km with the clouds' shadow only. The air: a thin dust outdoors (more
// in haze and fog: RTS's fog distance), a dustier one inside the room the
// engineer is in (the cab's or salon's box), where a beam through a window
// shows over a metre. A Henyey-Greenstein phase (g 0.7) brightens them
// towards the sun. The result is blurred along the frame's depth and taken
// up to the frame's size by depth, and the tone mapper adds it to the scene
// before its exposure (scene-linear).

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>
#include <optional>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

namespace openrts::renderer {

// The room the eye is in: world -> its space, and its box there.
struct LightShaftRoom {
    glm::mat4 to_room{1.0f};
    glm::vec3 lower{}, upper{};
};

struct LightShaftFrame {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f}; // reversed Z (depth.hpp)
    glm::vec3 eye{};
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    float strength = 1.0f;       // 0..4, 1 as made
    float shadow_distance = 250.0f; // m: the last cascade's end (SceneEffectSettings)
    float fog_end = 3000.0f;     // m: RTS's fog distance (the haze thickens the dust)
    std::optional<LightShaftRoom> room;
};

class LightShaftRenderer {
public:
    explicit LightShaftRenderer(WGPUDevice device);
    ~LightShaftRenderer();
    LightShaftRenderer(const LightShaftRenderer&) = delete;
    LightShaftRenderer& operator=(const LightShaftRenderer&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // This frame's shafts at the frame's size (scene-linear, for
    // ToneMapper::render), from `depth` (the camera's depth pre-pass,
    // single-sample) and `effects` (SceneEffects::bind_group()). Null when
    // it cannot (no depth, not ready).
    [[nodiscard]] WGPUTextureView render(WGPUCommandEncoder encoder, WGPUBindGroup effects, WGPUTextureView depth,
                                         const LightShaftFrame& frame);
    // The texture render() last gave the view of (RGBA16Float), for a
    // capture's report; null before.
    [[nodiscard]] WGPUTexture texture() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
