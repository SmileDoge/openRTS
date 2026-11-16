#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/mat4x4.hpp>
#include <webgpu/webgpu.h>

namespace openrts::renderer {

// Depth convention shared by every scene pipeline: reversed Z in a 32-bit
// float buffer. Near maps to 1 and far to 0, so the float's precision goes to
// the distance where perspective depth needs it: coplanar layers a millimetre
// apart (signal head housings) stay apart hundreds of metres away while the
// near plane can still be a centimetre for close inspection. RTS uses a
// 24-bit buffer with a 0.2 m near plane; this resolves at least as finely.
inline constexpr WGPUTextureFormat depth_format = WGPUTextureFormat_Depth32Float;
inline constexpr float depth_clear_value = 0.0f;
// "Nearer or equal" (GL_LEQUAL in RTS) under reversed Z.
inline constexpr WGPUCompareFunction depth_nearer_or_equal = WGPUCompareFunction_GreaterEqual;

// Multisampling of the scene targets, chosen once before any renderer or
// pipeline is created (the viewer's --msaa): 1, 2, 4 or 8, as the device
// takes it (supported_sample_count; WebGPU guarantees 1 and 4). RTS's own
// setting is "Antialiasing = 4x MSAA +Alpha" (alpha-to-coverage on every
// BlendATex / BlendATexDiff surface).
inline std::uint32_t& render_sample_count() {
    static std::uint32_t count = 1;
    return count;
}

// Streamed models made on loading threads (their buffers and textures
// created and written there; ModelLibrary), not one a frame on the render
// thread. Off on OpenGL, whose wgpu backend runs one context under a lock:
// the window sets it when the device is made.
inline bool& models_made_off_thread() {
    static bool on = true;
    return on;
}

// Anisotropic filtering for samplers with linear min, mag and mip filters
// (WebGPU allows it only then). RTS config: Anisotropy = 16.
inline constexpr std::uint16_t max_anisotropy = 16;

// HDR frame (--hdr): the scene is drawn into hdr_format, still gamma 2.2
// encoded but with values above 1 kept, and a ToneMapper makes the screen's
// picture. Set before the scene's pipelines are made.
inline bool& scene_hdr() {
    static bool on = false;
    return on;
}
inline constexpr WGPUTextureFormat hdr_format = WGPUTextureFormat_RGBA16Float;

// The scene frame's encoding this frame (docs/experimental-effects.md, "The
// frame's encoding"): the experimental path's HDR frame holds linear light,
// the RTS path's (and any frame without --hdr) RTS's display-encoded colours
// (gamma 2.2), blended as RTS blends them. Set once a frame by the window,
// before any pass writes its uniforms; every pass that writes the frame or
// reads it takes it from here, so a frame has one encoding throughout.
enum class FrameEncoding : std::uint8_t { gamma, linear };
inline FrameEncoding& frame_encoding() {
    static FrameEncoding encoding = FrameEncoding::gamma;
    return encoding;
}
[[nodiscard]] inline bool frame_linear() {
    return frame_encoding() == FrameEncoding::linear;
}
// The experimental HDR frame in linear light (on by default); off, as until
// 2026-11-16, gamma 2.2: the switch to compare the two (--hdr-encoding).
inline bool& linear_hdr() {
    static bool on = true;
    return on;
}
// The experimental path's physically based light (PBR stage 1: GGX, Smith,
// Schlick, Lambert; renderer/pbr.hpp) instead of its Blinn-Phong: chosen
// here (--pbr), in effect only in a linear frame, which its light needs.
inline bool& pbr_shading() {
    static bool on = true;
    return on;
}
[[nodiscard]] inline bool frame_pbr() {
    return pbr_shading() && frame_linear();
}
// The physically based light's sky light from the sky's probe (PBR stage 2,
// SkyLight: SH9 and the prefiltered cube; on by default); off, the old sky
// light with the BRDF (stage 1): the switch to compare (--sky-light).
inline bool& sky_light_from_probe() {
    static bool on = true;
    return on;
}
[[nodiscard]] inline bool frame_sky_light() {
    return frame_pbr() && sky_light_from_probe();
}

// The experimental path's physical falloff of the further lights (headlights,
// scenery lamps): 1 / distance squared, a soft cone edge, Lambert; off: the
// RTS light shapes. Read each frame (the frame uniforms' fog_distance.w).
inline bool& physical_light_falloff() {
    static bool on = false;
    return on;
}

// The experimental path's sky light at night, a factor on the environment's
// ambient taken as the sun is weaker (full at night, none by full sun): the
// unlit surroundings brighter without the lamps. Read each frame.
inline float& night_ambient_scale() {
    static float scale = 1.0f;
    return scale;
}

// The experimental path's light range (m): scenery lamps light the scene
// while their object is this near (each lamp's own Distance stretched alike);
// 0 keeps RTS's MaxLightDistance.
inline float& light_range() {
    static float range = 0.0f;
    return range;
}

// The experimental path's reach of the trains' headlights and searchlights,
// as a factor of the .eng's (1: as RTS).
inline float& headlight_reach() {
    static float factor = 1.0f;
    return factor;
}

// The experimental path lights FullBright surfaces (MSTS light material -8)
// as ordinary ones: sun, sky and lamps (their night textures are then left
// out, TextureVariant::fullbright_lit). Read each frame.
inline bool& light_fullbright() {
    static bool on = false;
    return on;
}

// The models' night textures (TEXTURES\NIGHT) at night, as RTS; off: the
// day's at night too, lit as the rest (the NIGHT ones have the lamps'
// light painted in, which the lamps then light again). Read each frame.
inline bool& night_textures() {
    static bool on = true;
    return on;
}

// The experimental path's lamp shadows: a scenery lamp (ESD_Light_Sources)
// throws no shadow of the model it belongs to (its own pole, head and
// glass; it still lights them). Some shapes have the light's place inside
// the lamp's body, which then threw its own shadow over everything round.
// Read each frame.
inline bool& lamp_skips_own_model() {
    static bool on = true;
    return on;
}

// Multisample state for a scene pipeline; alpha-to-coverage (for the blend
// shaders' surfaces, alpha-tested or blended) when multisampled.
[[nodiscard]] inline WGPUMultisampleState scene_multisample(bool alpha_to_coverage = false) {
    const auto count = render_sample_count();
    return {.count = count, .mask = ~0u, .alphaToCoverageEnabled = alpha_to_coverage && count > 1};
}

// A right-handed perspective projection into the reversed-Z depth range.
[[nodiscard]] inline glm::mat4 reversed_z_perspective(float vertical_fov, float aspect, float near_plane, float far_plane) {
    return glm::perspectiveRH_ZO(vertical_fov, aspect, far_plane, near_plane);
}

} // namespace openrts::renderer
#endif
