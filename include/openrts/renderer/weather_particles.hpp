#pragma once

// openRTS's own rain and snow (beyond RTS's sprites, the "Rain and snow"
// setting's openRTS look): tens of thousands of particles made in the
// vertex shader from their index and the time, in a box around the eye that
// wraps as it moves, so the field never ends and the CPU keeps none.
//
// - Rain: thin streaks along each drop's speed against the camera's (a
//   1/30 s exposure): they slant and lengthen as the train runs into them.
//   Far, a streak thinner than a pixel keeps one pixel and its light shrinks
//   with it (no shimmer).
// - Snow: soft round flakes, tumbling as they fall slowly, carried by the
//   wind; drawn out a little at speed.
//
// Both lit by the scene's light, depth-tested without writing depth (a cab's
// walls, a tunnel's roof hide them), blended over the frame.

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/type_precision.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <webgpu/webgpu.h>

namespace openrts::renderer {

// The rain's and snow's look: openRTS's particles (true, the default) or
// RTS's sprites. Read each frame (viewer::PrecipitationView).
inline bool& own_precipitation() {
    static bool on = true;
    return on;
}

struct WeatherParticles {
    float amount{};          // 0..2: RTS's density 1, a storm more
    bool snow = false;
    glm::vec3 wind{};        // m/s, world
    glm::vec3 eye{};         // scene
    glm::vec3 eye_velocity{}; // m/s, the camera's (a train's cab moving with it)
    glm::vec4 light{1.0f};   // the scene's light on a surface facing up (display, 0..1)
    float time{};            // s
    float viewport_height{}; // pixels
    // The cab or salon the eye is in (world -> its shape, its box): none
    // fall inside it.
    // How far the scene's origin has moved (ViewerScene::rebase_near): the
    // drops stay where they were in the world.
    glm::dvec3 origin_shift{0.0};
    bool has_room = false;
    glm::mat4 to_room{1.0f};
    glm::vec3 room_lower{}, room_upper{};
};

class WeatherParticleRenderer {
public:
    WeatherParticleRenderer(WGPUDevice device, WGPUTextureFormat target_format);
    ~WeatherParticleRenderer();
    WeatherParticleRenderer(const WeatherParticleRenderer&) = delete;
    WeatherParticleRenderer& operator=(const WeatherParticleRenderer&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    void render(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, const WeatherParticles& weather);
    // How many particles a frame draws at `amount` (tests, the report).
    [[nodiscard]] static std::uint32_t count(float amount, bool snow) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
