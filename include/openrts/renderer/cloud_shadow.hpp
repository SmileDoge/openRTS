#pragma once

// openRTS's clouds (native_sky.hpp) on the ground: a map of the sunlight
// their layer lets through, over a square around the eye's own shadow point
// at the layer's middle height. SceneEffects reads it with the sun's shadow
// (effects_shadow), so the land, the objects and the water darken where a
// cloud stands between them and the sun.

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

#ifdef OPENRTS_HAS_WGPU
struct CloudShadow {
    WGPUTextureView map = nullptr; // R8Unorm: the light let through (1 none taken)
    // xy: the map's middle from the eye (m, x and z), z: its half width (m),
    // w: the layer's middle height above the eye (m).
    glm::vec4 area{0.0f};
    // xyz: towards the sun (as the clouds are marched), w: strength (0: off).
    glm::vec4 sun{0.0f};
    // The edge: the light read from the map taken through smoothstep(x, y),
    // so a shadow is dark through and its edge a narrow band, as a cumulus's
    // on the fields seen from a plane (narrower with the sharpness).
    glm::vec2 edge{0.0f, 1.0f};
    // The sunlight let through where the map's shadow is not (1 none taken):
    // the map fades out as the sun goes down to the horizon (its rays then
    // cross the layer kilometres away, off the map), and a low sun is taken
    // by the clouds' cover there instead (2026-11-21: until then a rising
    // sun under a thunderstorm's 97 % lit the cab and the land as on a clear
    // morning, testers). The shaders: low_sun - sun.w x (1 - the map's).
    float low_sun = 1.0f;
};
#endif

} // namespace openrts::renderer
