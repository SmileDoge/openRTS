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
// pipeline is created (the viewer's --msaa): 1 or 4, the counts WebGPU
// guarantees. RTS's own setting is "Antialiasing = 4x MSAA +Alpha"
// (alpha-to-coverage on alpha-tested surfaces).
inline std::uint32_t& render_sample_count() {
    static std::uint32_t count = 1;
    return count;
}

// Anisotropic filtering for samplers with linear min, mag and mip filters
// (WebGPU allows it only then). RTS config: Anisotropy = 16.
inline constexpr std::uint16_t max_anisotropy = 16;

// Multisample state for a scene pipeline; alpha-to-coverage for
// alpha-tested surfaces when multisampled.
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
