#pragma once

// The sky's light on the scene, image based (PBR stage 2,
// docs/experimental-effects.md, "The sky's light"). From the sky's reflection
// probe (SkyRenderer::render_reflection_probe: openRTS's sky or RTS's dome, a
// cube of the HDR frame's format, gamma 2.2 encoded), on the GPU each frame:
//  - the light of the sky and of the ground under it as spherical harmonics
//    of the 3rd order (SH9, Ramamoorthi and Hanrahan), for the Lambert part;
//  - the probe in linear light, prefiltered by GGX into five mips (roughness
//    0, 1/4 .. 1, 128 .. 8 texels a face), for the reflections (the split
//    sum, Karis);
// and once, the split sum's BRDF table (64 x 64, GGX with the
// height-correlated Smith visibility, as the direct light's).
// Under the horizon the probe holds the horizon's haze (openRTS's sky) or
// RTS's clear colour, not the land: there the ground is a Lambert plane of
// the ground's reflectance (SceneEffectSettings::ground_bounce) lit by the
// sun and the sky, the ground's bounce as the lower hemisphere.
// The scene's units throughout: what a white Lambert surface facing a light
// shows (irradiance / pi), the probe's colours as radiance.

#include <array>
#include <cstdint>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <memory>

#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

inline constexpr std::uint32_t sky_light_face = 128;  // the source's and the prefiltered cube's face (the probe's)
inline constexpr std::uint32_t sky_light_mips = 5;    // prefiltered: roughness 0, 1/4, 1/2, 3/4, 1
inline constexpr std::uint32_t sky_light_brdf_size = 64;
// The most light a probe texel gives (radiance, the scene's units; by
// luminance, the colour kept): the sun's disc, thousands of times the sky
// round it, is the sun's own light, already the direct light.
inline constexpr float sky_light_most = 16.0f;

using Sh9 = std::array<glm::vec3, 9>;

// The real spherical harmonics up to l = 2 at a unit direction, in the order
// Y00, Y1-1 (y), Y10 (z), Y11 (x), Y2-2 (xy), Y2-1 (yz), Y20 (3z^2 - 1),
// Y21 (xz), Y22 (x^2 - y^2).
[[nodiscard]] std::array<float, 9> sh9_basis(const glm::vec3& direction) noexcept;
// What a white Lambert surface facing `normal` shows lit by radiance of
// these coefficients (irradiance / pi; not below 0).
[[nodiscard]] glm::vec3 sh9_light(const Sh9& radiance, const glm::vec3& normal) noexcept;
// The coefficients of `radiance` from every way under the horizon (y < 0)
// and none above it.
[[nodiscard]] Sh9 sh9_lower_hemisphere(const glm::vec3& radiance) noexcept;
// The split sum's BRDF (Karis) for n.v and the perceptual roughness: the
// scale and the bias of F0 (F90 1), GGX with the height-correlated Smith
// visibility, `samples` importance-sampled ways.
[[nodiscard]] glm::vec2 split_sum_brdf(float n_dot_v, float roughness, std::uint32_t samples = 256) noexcept;

#ifdef OPENRTS_HAS_WGPU
struct SkyLightInput {
    glm::vec3 to_sun{0.0f, 1.0f, 0.0f}; // unit
    glm::vec3 sun{0.0f};                // the direct sun's light, linear (a white surface facing it shows this)
    float ground_albedo = 0.2f;         // the ground's reflectance (SceneEffectSettings::ground_bounce)
};

// What the scene's shaders read (SceneEffects, group 2).
struct SkyLightView {
    WGPUTextureView prefiltered = nullptr; // a cube, sky_light_mips mips
    WGPUTextureView brdf = nullptr;        // the split sum's table: (A, B) at (n.v, roughness)
    // vec4s: the 9 SH coefficients (rgb) of the sky and the ground; the
    // ground's radiance; the sky's light on a white surface facing up.
    WGPUBuffer irradiance = nullptr;
    // How open the sky is round the eye (1 .. 0 deep in a tunnel), from the
    // scene: the sky's light near the eye is taken by it.
    float openness = 1.0f;
    [[nodiscard]] bool on() const noexcept { return prefiltered && brdf && irradiance; }
};
inline constexpr std::uint64_t sky_light_buffer_size = 12 * 16;

class SkyLight {
public:
    explicit SkyLight(WGPUDevice device);
    ~SkyLight();
    SkyLight(const SkyLight&) = delete;
    SkyLight& operator=(const SkyLight&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // Outside any pass: the SH9 and the prefiltered cube from `probe` (a cube
    // view; its contents as drawn, the next frame's light).
    void update(WGPUCommandEncoder encoder, WGPUTextureView probe, const SkyLightInput& input);
    // Valid after the first update(); empty before.
    [[nodiscard]] SkyLightView view() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
