#pragma once

// openRTS's own sky (RTS has a dome of textured layers): a physical
// atmosphere (single scattering, Rayleigh and Mie with ozone, the planet
// round), the sun's disc, stars at night, and volumetric clouds ray marched
// through a layer of 3D noise, their cover from the weather and drifting with
// the environment's wind. The RTS moon and other satellites are still drawn
// over it (SkyRenderer::render_satellites). A Graphics setting.

#include <cstdint>
#include <memory>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <openrts/renderer/cloud_shadow.hpp>
#include <openrts/renderer/fog_ring.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

// On: the native sky replaces RTS's dome (sky layers and the sun's sprite).
// Read each frame.
inline bool& native_sky() {
    static bool on = true;
    return on;
}

// The clouds' cover the player chose (0..1); below 0: the weather's (clear
// 0.42, snow 0.85, rain 0.92). Read each frame.
inline float& cloud_cover() {
    static float cover = -1.0f;
    return cover;
}

// The clouds' shadows on the ground (NativeSky::cloud_shadow, read by the
// experimental path). Read each frame.
inline bool& cloud_shadows() {
    static bool on = true;
    return on;
}

// How much of the sun a thick cloud's shadow takes (0..1; the rest is what
// it scatters forwards), and how sharp the shadows' edges are: the clouds'
// density in them times this, and their edge band narrowed by its square
// root (1..10; 1 as the sky shows them, their edges as soft as theirs).
// Read each frame.
inline float& cloud_shadow_darkness() {
    static float darkness = 0.85f;
    return darkness;
}
inline float& cloud_shadow_sharpness() {
    static float sharpness = 3.0f;
    return sharpness;
}

// The scene's light scale for the sky: the sun's light at the top of the
// atmosphere, so that a sunlit white surface is about 1 (linear).
inline constexpr float native_sky_sun = 20.0f;

// The atmosphere's light (linear, scaled by native_sky_sun) reaching a point
// `altitude` m above sea level from `view` (unit, y up), the sun towards
// `to_sun`. The CPU twin of the shader's: for the clouds' ambient light, the
// fog colour and tests.
[[nodiscard]] glm::vec3 atmosphere_radiance(float altitude, const glm::vec3& view, const glm::vec3& to_sun);
// The share of sunlight left after the atmosphere at `altitude` (per channel).
[[nodiscard]] glm::vec3 atmosphere_transmittance(float altitude, const glm::vec3& to_sun);

struct NativeSkyFrame {
    glm::vec3 to_sun{0.0f, 1.0f, 0.0f}; // unit, below the horizon at night
    float altitude = 0.0f;              // the eye above sea level (m)
    glm::dvec2 eye_route{0.0};          // the eye in route metres (x east, z north): anchors the clouds
    float coverage = 0.42f;             // the clouds' cover, 0..1 (the weather)
    glm::vec2 wind{6.0f, 0.0f};         // their drift (m/s, x east, z north)
    float elapsed = 0.0f;               // s since the last frame
};

#ifdef OPENRTS_HAS_WGPU
class NativeSky {
public:
    // Uniform slots: the frame (0), the reflection probe's faces (1..6).
    static constexpr std::uint32_t frame_slot = 0;
    static constexpr std::uint32_t face_slot = 1;

    NativeSky(WGPUDevice device, WGPUTextureFormat target_format);
    ~NativeSky();
    NativeSky(const NativeSky&) = delete;
    NativeSky& operator=(const NativeSky&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // The frame's sun, clouds and wind (CPU: the clouds' light, the fog colour).
    void update(const NativeSkyFrame& frame);
    // The atmosphere into its table (256 x 128 around the sky), outside any
    // pass; the sky drawn from the next frame on reads it.
    void render_table(WGPUCommandEncoder encoder);
    // The clouds of the frame's view (`width` x `height` pixels) at half its
    // size each way, before the main pass: each frame marched with its own
    // jitter and blended into the last frames' (found again by direction),
    // so the march's steps leave no bands. render(frame_slot) reads them.
    void render_clouds(WGPUCommandEncoder encoder, const glm::mat4& projection, const glm::mat4& view, std::uint32_t width,
                       std::uint32_t height);
    // The sky where nothing is drawn yet: depth still at the far plane (a
    // pass with the scene's depth, after the opaque objects), or a probe face
    // (its clouds marched there). Blended over what is drawn before it on
    // that far plane (the moon, SkyRenderer::render_satellites, over black):
    // it keeps what the clouds let through.
    void render(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, std::uint32_t slot);
    // The average horizon's colour (display encoded), for RTS's fog colour.
    [[nodiscard]] glm::vec3 horizon_colour() const noexcept;
    // The horizon by direction, for the fog (FogRing).
    [[nodiscard]] FogRing fog_ring() const noexcept;
    // The clouds' shadow on the ground: the map render_clouds() draws (1024 x
    // 1024 texels over 16 km, every fourth frame), where it lies for this
    // frame's eye and the sun; strength 0 before it is first drawn, with
    // cloud_shadows() off or the sun down.
    [[nodiscard]] CloudShadow cloud_shadow() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
