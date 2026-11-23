#pragma once

// The additional light sources of the RTS shaders, shared by the model and
// terrain renderers: the std140 light record, its packing, and the WGSL light
// shapes (lights.glsl and the LightsInt loop of shape.frag).

#include <cstring>

#include <openrts/renderer/model_renderer.hpp>

namespace openrts::renderer::detail {

// std140 GLLightObj of the RTS shaders (192 bytes); WGSL lays it out identically.
struct GpuLight {
    float diffuse[4];
    float ambient[4];
    float position[3];
    float padding0;
    float direction[3];
    float padding1;
    float size[4];
    float translation[16];
    float type[4]; // x: light type (Light[0]: number of further lights), y: array count, z: spot exponent
    float attenuation[4];
    float array_step[4];
};
static_assert(sizeof(GpuLight) == 192);

inline void pack_light(GpuLight& to, const ModelExtraLight& from) {
    std::memcpy(to.diffuse, &from.diffuse[0], sizeof(to.diffuse));
    std::memcpy(to.translation, &from.to_light[0][0], sizeof(to.translation));
    to.direction[0] = from.direction.x;
    to.direction[1] = from.direction.y;
    to.direction[2] = from.direction.z;
    std::memcpy(to.size, &from.size[0], sizeof(to.size));
    to.type[0] = from.kind;
    to.type[1] = from.array_count >= 2 ? static_cast<float>(from.array_count) : 0.0f;
    std::memcpy(to.array_step, &from.array_step[0], sizeof(to.array_step));
    to.type[2] = from.spot_exponent;
    std::memcpy(to.attenuation, &from.attenuation[0], sizeof(to.attenuation));
}

} // namespace openrts::renderer::detail
