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

inline constexpr char light_shapes_wgsl[] = R"(
// A further light as a physical source (the experimental path's option,
// physical_light_falloff()): the RTS light's shape gives its cone or volume
// with a soft edge (from SpotExponent); it falls off as 1 / distance squared
// from the lamp, giving its colour at the start of its fade (Size.z *
// Attenuation.y: a scenery lamp's PointsTo plane), at most 8 times that near
// the lamp, smoothly to nothing at its reach; Lambert to the lamp.
fn physical_light(light: Light, world: vec4<f32>, normal: vec3<f32>) -> f32 {
    let p = light.translation * world;
    let flags = i32(light.kind.x);
    let kind = flags & 31;
    let reach = light.size[2] * (light.attenuation[1] + light.attenuation[2]);
    let distance = length(p.xyz);
    if (reach <= 0.0 || distance >= reach || distance <= 0.0) { return 0.0; }
    var d = 0.0;
    if (kind == 1) { // spotlight
        if (p.z <= 0.0) { return 0.0; }
        d = max(sqrt(p.x * p.x + p.y * p.y) - light.size[3], 0.0) / (p.z * light.size[1]);
    } else if (kind == 4) { // cone
        if (p.z <= 0.0) { return 0.0; }
        let x = p.x / light.size[0];
        let y = p.y / light.size[1];
        d = sqrt(x * x + y * y) / p.z;
    } else if (kind == 2) { // sphere: 1 at its ellipsoid
        let x = p.x * light.size[2] / light.size[0];
        let y = p.y * light.size[2] / light.size[1];
        d = sqrt(x * x + y * y + p.z * p.z) / light.size[2];
    } else if (kind == 3) { // orthogonal
        if (p.z <= 0.0) { return 0.0; }
        d = max(abs(p.x) / light.size[0], abs(p.y) / light.size[1]);
    } else {
        return 0.0;
    }
    let inner = clamp(1.0 - clamp(light.kind.z / 128.0, 0.0, 1.0), 0.0, 0.9);
    let edge = 1.0 - smoothstep(inner, 1.0, d);
    if (edge <= 0.0) { return 0.0; }
    var falloff = 1.0;
    var towards = -p.xyz / distance;
    if (kind == 3) {
        towards = vec3<f32>(0.0, 0.0, -1.0); // parallel light: no distance falloff
    } else {
        let reference = max(light.size[2] * light.attenuation[1], 0.01);
        falloff = min(reference * reference / (distance * distance), 8.0);
    }
    let window = clamp(1.0 - pow(distance / reach, 4.0), 0.0, 1.0);
    var lambert = 1.0;
    if ((flags & 32) == 0) {
        let n = normalize((light.translation * vec4<f32>(normal, 0.0)).xyz);
        lambert = max(dot(n, towards), 0.0);
    }
    return edge * falloff * window * window * lambert;
}
struct Light {
    diffuse: vec4<f32>,
    ambient: vec4<f32>,
    position: vec3<f32>,
    direction: vec3<f32>,
    size: vec4<f32>,
    translation: mat4x4<f32>,
    kind: vec4<f32>,
    attenuation: vec4<f32>,
    array_step: vec4<f32>,
};
// Contribution factor of an additional RTS light source (lights.glsl).
// Returns a negative value when the fragment is outside the light.
fn light_factor(light: Light, world: vec4<f32>, normal: vec3<f32>) -> f32 {
    var position = light.translation * world;
    let flags = i32(light.kind.x);
    let kind = flags & 31;

    if (light.kind.y >= 2.0) { // light array: use the closest element
        let count = i32(light.kind.y);
        var projection = position * light.array_step;
        let along = (projection.x + projection.y + projection.z) / light.array_step.w;
        var closest = i32(round(along));
        if (closest <= -2 || closest > count) {
            return -1.0;
        }
        closest = clamp(closest, 0, count - 1);
        position = position - vec4<f32>(light.array_step.xyz, 0.0) * f32(closest);
    }

    let reach = light.size[2] * (light.attenuation[1] + light.attenuation[2]);
    var d = 0.0;
    if (kind == 0) {
        return -1.0;
    } else if (kind == 1) { // spotlight
        if (!(position.z > 0.0 && position.z <= reach)) { return -1.0; }
        d = (sqrt(position.x * position.x + position.y * position.y) - light.size[3]) / (position.z * light.size[1]);
    } else if (kind == 4) { // cone
        if (!(position.z > 0.0 && position.z <= reach)) { return -1.0; }
        let x = position.x / light.size[0];
        let y = position.y / light.size[1];
        d = sqrt(x * x + y * y) / position.z;
    } else if (kind == 2) { // sphere
        if (abs(position.z) > reach) { return -1.0; }
        let x = position.x * light.size[2] / light.size[0];
        let y = position.y * light.size[2] / light.size[1];
        d = sqrt(x * x + y * y + position.z * position.z) / light.size[2];
    } else if (kind == 3) { // orthogonal
        if (!(position.z > 0.0 && position.z <= reach)) { return -1.0; }
        d = max(abs(position.x) / light.size[0], abs(position.y) / light.size[1]);
    }

    let spot_exponent = light.kind.z / 128.0;
    if (spot_exponent < 1.0) {
        if (spot_exponent != 0.0) {
            if (d < 1.0 - spot_exponent) {
                d = 0.0;
            } else {
                d = (d - 1.0 + spot_exponent) / abs(spot_exponent);
            }
        } else if (d < 1.0) {
            d = 0.0;
        }
    }
    if (d > 1.0) { return -1.0; }

    var factor = clamp(1.0 - d, 0.0, 1.0);
    let attenuation = i32(light.attenuation[0]);
    if (position.z > light.size[2] * light.attenuation[1]) {
        let depth = position.z / light.size[2] - light.attenuation[1];
        if (attenuation == 2) { // linear
            factor = factor * (1.0 - depth / light.attenuation[2]);
        } else if (attenuation == 1) { // quadratic
            factor = factor * (1.0 - pow(depth, 2.0) / pow(light.attenuation[2], 2.0));
        }
    }
    if (factor > 0.0 && (flags & 32) == 0) {
        factor = factor * clamp(-dot(normal, light.direction) * 2.0 + 0.3, 0.0, 1.0);
    }
    return factor;
}

// An interior lamp's factor (RTS shape.frag, the LightsInt loop): kind 4 a
// cone, 2 a sphere, 3 a box; negative outside.
fn interior_factor(light: Light, world: vec4<f32>, normal: vec3<f32>) -> f32 {
    var position = light.translation * world;
    let flags = i32(light.kind.x);
    let kind = flags & 31;
    var d = 0.0;
    if (kind == 4) {
        if (!(position.z > 0.0 && position.z <= light.size[2])) { return -1.0; }
        d = sqrt(position.x * position.x / light.size[0] + position.y * position.y / light.size[1]) / position.z;
    } else if (kind == 2) {
        if (abs(position.z) > light.size[2]) { return -1.0; }
        let x = position.x * light.size[2] / light.size[0];
        let y = position.y * light.size[2] / light.size[1];
        d = sqrt(x * x + y * y + position.z * position.z) / light.size[2];
    } else if (kind == 3) {
        if (!(position.z > 0.0 && position.z <= light.size[2])) { return -1.0; }
        d = max(abs(position.x) / light.size[0], max(abs(position.y) / light.size[1], abs(position.z) / light.size[2]));
    } else {
        return -1.0;
    }
    let spot_exponent = light.kind.z / 128.0;
    if (spot_exponent < 1.0) {
        if (spot_exponent != 0.0) {
            if (d < 1.0 - spot_exponent) {
                d = 0.0;
            } else {
                d = (d - 1.0 + spot_exponent) / abs(spot_exponent);
            }
        } else if (d < 1.0) {
            d = 0.0;
        }
    }
    var factor = clamp(1.0 - d, 0.0, 1.0);
    let attenuation = i32(light.attenuation[0]);
    if (position.z > light.size[2] * light.attenuation[1]) {
        let depth = position.z / light.size[2] - light.attenuation[1];
        if (attenuation == 2) {
            factor = factor * (1.0 - depth / light.attenuation[2]);
        } else if (attenuation == 1) {
            factor = factor * (1.0 - pow(depth, 2.0) / pow(light.attenuation[2], 2.0));
        }
    }
    if (factor > 0.0 && (flags & 32) == 0) {
        factor = factor * clamp(-dot(normal, light.direction) * 2.0 + 0.3, 0.0, 1.0);
    }
    return factor;
}

)";

} // namespace openrts::renderer::detail
