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
};
#endif

} // namespace openrts::renderer
