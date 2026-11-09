#pragma once

// WGSL of the fog's colour by direction (FogRing, fog_ring.hpp) for shaders
// whose frame uniform `frame` ends with
//   fog_sun: vec4<f32>,               // xy: the sun's way along the ground (0: off), zw: the eye's x, z
//   fog_ring: array<vec4<f32>, 12>,   // the horizon's colour at each fog_ring_angle
// Internal to openrts_renderer.

namespace openrts::renderer::detail {

inline constexpr char fog_ring_wgsl[] = R"(
// The fog's colour towards `world` (openRTS's sky: the horizon behind it,
// by its angle from the sun's way); `fallback`, RTS's one colour, while the
// ring is off.
fn fog_ring_colour(world: vec3<f32>, fallback: vec3<f32>) -> vec3<f32> {
    let way = frame.fog_sun.xy;
    let flat = world.xz - frame.fog_sun.zw;
    let size = length(flat);
    if (dot(way, way) < 0.5 || size < 1e-3) {
        return fallback;
    }
    let s = sqrt(acos(clamp(dot(flat / size, way), -1.0, 1.0)) / 3.14159265) * 11.0;
    let i = min(u32(s), 10u);
    return mix(frame.fog_ring[i].xyz, frame.fog_ring[i + 1u].xyz, clamp(s - f32(i), 0.0, 1.0));
}
)";

} // namespace openrts::renderer::detail
