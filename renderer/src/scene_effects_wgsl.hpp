#pragma once

// WGSL of the experimental path's frame effects (scene_effects.hpp): the
// uniform, and for the main pass the group-2 bindings and their helpers.
// Internal to openrts_renderer. The main-pass part needs `Light`
// (rts_light_shapes.hpp) declared before it.

namespace openrts::renderer::detail {

// Laid out as EffectsUniform in scene_effects.cpp.
inline constexpr char effects_struct_wgsl[] = R"(
struct Effects {
    cascades: array<mat4x4<f32>, 4>, // world -> shadow clip, per cascade
    splits: vec4<f32>,         // each cascade's far view depth
    shadow: vec4<f32>,         // x: cascades (0: no shadows), y: strength, z: softness (texels), w: map size
    eye: vec4<f32>,            // xyz: the camera, w: near plane
    forward: vec4<f32>,        // xyz: the camera's forward, w: far plane
    clusters: vec4<u32>,       // grid x, y, z; w: lights a cluster holds at most
    cluster_params: vec4<f32>, // x, y: tile size in pixels; z, w: slice = log(depth) * z + w
    screen: vec4<f32>,         // width, height, 1 / width, 1 / height
    flags: vec4<u32>,          // x: clustered lights, y: ambient occlusion, z: aerial perspective, w: lights
    sun: vec4<f32>,            // xyz: towards the sun
    aerial: vec4<f32>,         // x: extinction per metre at the eye, y: its fall-off with height (1/m), z: Mie g, w: sun glow
    haze: vec4<f32>,           // linear in-scattered colour (the horizon's)
    sun_colour: vec4<f32>,     // linear
    texel: vec4<f32>,          // a shadow texel's size in metres, per cascade
    view: mat4x4<f32>,         // world -> view
    inv_projection: mat4x4<f32>,
    ao: vec4<f32>,             // x: radius (m), y: strength (power), z: tan(vertical fov / 2), w: aspect
    horizon: vec4<f32>,        // xyz: RTS's fog colour (linear), w: where its fog ends (m)
    focus: mat4x4<f32>,        // world -> the focus map's clip (the cab)
    focus_params: vec4<f32>,   // x: its layer + 1 (0: none), y: its texel (m), z: margin (clip units), w: least filter radius (m)
    lamps: vec4<f32>,          // x: how much daylight dims the lamps (0: not at all)
    lamp_shadows: array<mat4x4<f32>, 4>, // world -> each shadowed lamp's clip
    lamp_shadow_ids: vec4<u32>,          // each one's index in scene_lights + 1
    lamp_shadow_texel: vec4<f32>,        // a texel a metre from the lamp (parallel: in metres)
    lamp_shadow_params: vec4<f32>,       // x: lamps, y: map size, z: filter radius (texels)
};
)";

inline constexpr char effects_main_wgsl[] = R"(
@group(2) @binding(0) var<uniform> effects: Effects;
@group(2) @binding(1) var shadow_map: texture_depth_2d_array;
@group(2) @binding(2) var shadow_sampler: sampler_comparison;
@group(2) @binding(3) var ao_texture: texture_2d<f32>;
@group(2) @binding(4) var<storage, read> scene_lights: array<Light>;
@group(2) @binding(5) var<storage, read> cluster_counts: array<u32>;
@group(2) @binding(6) var<storage, read> cluster_lights: array<u32>;
@group(2) @binding(7) var lamp_shadow_map: texture_depth_2d_array;

// A lamp's light reaching a point (`index`: the lamp in scene_lights): 1
// unless it is one of the lamps throwing shadows and something stands
// between (8 filtered compares on a disc, turned per pixel).
fn effects_lamp_shadow(index: u32, world: vec3<f32>, normal: vec3<f32>, frag: vec4<f32>) -> f32 {
    let count = u32(effects.lamp_shadow_params.x);
    for (var s = 0u; s < count; s = s + 1u) {
        if (effects.lamp_shadow_ids[s] != index + 1u) { continue; }
        let m = effects.lamp_shadows[s];
        let rough = m * vec4<f32>(world, 1.0);
        if (rough.w <= 0.0) { return 1.0; }
        let texel = effects.lamp_shadow_texel[s] * rough.w;
        let clip = m * vec4<f32>(world + normalize(normal) * texel * 2.0, 1.0);
        let ndc = clip.xyz / clip.w;
        let uv = ndc.xy * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5);
        if (any(uv < vec2<f32>(0.0)) || any(uv > vec2<f32>(1.0)) || ndc.z < 0.0 || ndc.z > 1.0) { return 1.0; }
        let radius = effects.lamp_shadow_params.z / effects.lamp_shadow_params.y;
        let turn = 6.2831853 * fract(52.9829189 * fract(dot(floor(frag.xy), vec2<f32>(0.06711056, 0.00583715))));
        var lit = 0.0;
        for (var i = 0; i < 8; i = i + 1) {
            let r = sqrt((f32(i) + 0.5) / 8.0) * radius;
            let angle = f32(i) * 2.3999632 + turn;
            lit = lit + textureSampleCompareLevel(lamp_shadow_map, shadow_sampler, uv + vec2<f32>(cos(angle), sin(angle)) * r, s, ndc.z);
        }
        return lit / 8.0;
    }
    return 1.0;
}

// The sun's light reaching a point: 1 lit, 0 in shadow (cascaded shadow
// maps; 16 filtered compares on a disc of the softness's radius, turned per
// pixel; the point pushed off its surface by a texel or more on slopes and
// with the softness; fades out at the last cascade's end).
fn effects_shadow(world: vec3<f32>, normal: vec3<f32>, frag: vec4<f32>) -> f32 {
    let count = u32(effects.shadow.x);
    if (count == 0u) { return 1.0; }
    let depth = dot(world - effects.eye.xyz, effects.forward.xyz);
    let n = normalize(normal);
    let slope = 1.0 - clamp(abs(dot(n, effects.sun.xyz)), 0.0, 1.0);
    let softness = max(effects.shadow.z, 0.5);
    let push = 1.0 + 2.0 * slope + 0.5 * softness;
    var layer = count;
    var texel = 0.0;
    var clip = vec4<f32>(0.0);
    var fade = 1.0;
    // The focus map (the cab) first, where it covers the point.
    if (effects.focus_params.x > 0.0) {
        let focus_texel = effects.focus_params.y;
        let c = effects.focus * vec4<f32>(world + n * focus_texel * push, 1.0);
        if (all(abs(c.xy) < vec2<f32>(1.0 - effects.focus_params.z)) && c.z >= 0.0 && c.z <= 1.0) {
            layer = u32(effects.focus_params.x) - 1u;
            texel = focus_texel;
            clip = c;
        }
    }
    if (texel == 0.0) {
        var cascade = count;
        for (var i = 0u; i < count; i = i + 1u) {
            if (depth < effects.splits[i]) {
                cascade = i;
                break;
            }
        }
        if (cascade >= count) { return 1.0; }
        layer = cascade;
        texel = effects.texel[cascade];
        clip = effects.cascades[cascade] * vec4<f32>(world + n * texel * push, 1.0);
        let last = effects.splits[count - 1u];
        fade = clamp((last - depth) / (last * 0.1), 0.0, 1.0);
    }
    let uv = clip.xy * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5);
    if (any(uv < vec2<f32>(0.0)) || any(uv > vec2<f32>(1.0)) || clip.z > 1.0 || clip.z < 0.0) { return 1.0; }
    // A Vogel disc, turned by interleaved gradient noise; at least the
    // least penumbra wide.
    let radius = max(softness, effects.focus_params.w / texel) / effects.shadow.w;
    let turn = 6.2831853 * fract(52.9829189 * fract(dot(floor(frag.xy), vec2<f32>(0.06711056, 0.00583715))));
    var lit = 0.0;
    for (var i = 0; i < 16; i = i + 1) {
        let r = sqrt((f32(i) + 0.5) / 16.0) * radius;
        let angle = f32(i) * 2.3999632 + turn;
        let tap = uv + vec2<f32>(cos(angle), sin(angle)) * r;
        lit = lit + textureSampleCompareLevel(shadow_map, shadow_sampler, tap, layer, clip.z);
    }
    lit = lit / 16.0;
    return mix(1.0, lit, fade * effects.shadow.y);
}

// Ambient occlusion at a fragment (frag: its @builtin(position)).
fn effects_ao(frag: vec4<f32>) -> f32 {
    if (effects.flags.y == 0u) { return 1.0; }
    let size = vec2<i32>(textureDimensions(ao_texture));
    let p = clamp(vec2<i32>(frag.xy), vec2<i32>(0), size - vec2<i32>(1));
    return textureLoad(ao_texture, p, 0).r;
}

// The light cluster of a fragment.
fn effects_cluster(frag: vec4<f32>, world: vec3<f32>) -> u32 {
    let depth = max(dot(world - effects.eye.xyz, effects.forward.xyz), effects.eye.w);
    let slice = u32(clamp(log(depth) * effects.cluster_params.z + effects.cluster_params.w, 0.0, f32(effects.clusters.z - 1u)));
    let x = min(u32(max(frag.x, 0.0) / effects.cluster_params.x), effects.clusters.x - 1u);
    let y = min(u32(max(frag.y, 0.0) / effects.cluster_params.y), effects.clusters.y - 1u);
    return (slice * effects.clusters.y + y) * effects.clusters.x + x;
}

// Aerial perspective on a linear colour: extinction by distance through a
// haze thinning with height, the sky's colour scattered in, brighter
// towards the sun (Henyey-Greenstein); over the last quarter of RTS's fog
// distance it turns into RTS's fog colour, so the land's end stays hidden.
fn effects_aerial(colour: vec3<f32>, world: vec3<f32>) -> vec3<f32> {
    let ray = world - effects.eye.xyz;
    let distance = length(ray);
    if (distance <= 0.0) { return colour; }
    let k = effects.aerial.y;
    var optical = effects.aerial.x * distance;
    let rise = ray.y * k;
    if (abs(rise) > 1e-4) {
        optical = optical * (1.0 - exp(-rise)) / rise;
    }
    let transmittance = exp(-optical);
    let cos_theta = dot(ray / distance, effects.sun.xyz);
    let g = effects.aerial.z;
    let phase = (1.0 - g * g) / (12.566371 * pow(max(1.0 + g * g - 2.0 * g * cos_theta, 1e-4), 1.5));
    let inscatter = effects.haze.xyz + effects.sun_colour.xyz * phase * effects.aerial.w;
    let hazy = colour * transmittance + inscatter * (1.0 - transmittance);
    if (effects.horizon.w <= 0.0) { return hazy; }
    let edge = smoothstep(0.75 * effects.horizon.w, effects.horizon.w, distance);
    return mix(hazy, effects.horizon.xyz, edge);
}
)";

} // namespace openrts::renderer::detail
