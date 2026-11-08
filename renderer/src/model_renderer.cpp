#include <openrts/renderer/model_renderer.hpp>

#include <openrts/core/profile.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <set>
#include <span>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_animation.hpp>
#include <openrts/renderer/gpu_retire_queue.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/rts_material.hpp>
#include <openrts/renderer/webgpu_texture.hpp>

#include <openrts/renderer/frustum.hpp>
#include <openrts/renderer/scene_effects.hpp>
#include <openrts/renderer/task_pool.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include "rts_light_shapes.hpp"
#include "scene_effects_wgsl.hpp"
#include <webgpu/wgpu.h>

namespace openrts::renderer {
namespace {

// Matches the RTS vertex attributes position, normal, uv, colour1, colour2.
// Colours keep the raw 0xAARRGGBB value; RTS feeds those four little-endian
// bytes as an unnormalised ubyte4 and divides by 255 in the shader.
struct GpuVertex {
    float position[3];
    float normal[3];
    float uv[2];
    std::uint32_t colour1;
    std::uint32_t colour2;
};

// Constants of one shape primitive, fixed for the model generation: its
// shape matrix (rest pose) and material. One 256-byte slot per primitive in
// the model's material buffer.
// The light material of a draw RTS draws unlit at night (its shape has the
// night bit, rts1.exe 0x4B042C): unlit in both paths, whatever
// light_fullbright() says (it is not an MSTS index).
constexpr std::int32_t night_texture_material = -16;

struct MaterialUniform {
    glm::mat4 local;
    glm::mat4 local_normal; // transpose(inverse(local))
    std::int32_t light_material;
    std::int32_t named_shader;
    std::int32_t matrix_index; // into an animated instance's locals, -1: none
    float lod_bias;
};
// The model's draws one after another in a storage buffer (WGSL array<Draw>,
// stride 144): a binding serves every draw of the model with its texture.
constexpr std::uint64_t material_stride = sizeof(MaterialUniform);
static_assert(sizeof(MaterialUniform) == 144);

// One instance of one draw call: the instance and the primitive it draws (its
// slot in the model's draws). A multi-draw call covers several primitives of
// a model, so each id names its own.
struct IdEntry {
    std::uint32_t instance;
    std::uint32_t draw;
};

// DrawIndexedIndirect arguments (the indirect buffer, one per primitive drawn).
struct IndirectDraw {
    std::uint32_t index_count;
    std::uint32_t instance_count;
    std::uint32_t first_index;
    std::int32_t base_vertex;
    std::uint32_t first_instance;
};
static_assert(sizeof(IndirectDraw) == 20);

// One drawn model instance per frame. The normal matrix is
// transpose(inverse(transform)), as the RTS shader computes it; the shader
// combines both with the primitive's local matrices.
struct GpuInstance {
    glm::mat4 transform;
    glm::mat4 normal;
    std::int32_t locals; // first animated local of this instance, -1: rest pose
    std::int32_t ring;   // its ring of further lights (LightRings::ring_for)
    // 1: a decal on the ground (a transfer), drawn a hair nearer (vs_main).
    std::int32_t decal;
    std::int32_t padding;
};
static_assert(sizeof(GpuInstance) == 144);

// Animated shape matrix of an instance (node world matrix within the shape).
struct GpuLocal {
    glm::mat4 matrix;
    glm::mat4 normal;
};

using detail::GpuLight;

// The further lights: LightRings, nine a ring (RTS Shader.NumLights 10, the sun first).
constexpr std::size_t ring_lights = LightRings::count * LightRings::capacity;
constexpr std::size_t max_interior_lights = 8;

struct FrameUniform {
    GpuLight lights[1]; // the sun
    GpuLight rings[ring_lights];
    std::int32_t ring_counts[12]; // lights in each ring
    GpuLight interior[max_interior_lights];
    float fog_colour[4];
    float fog_distance[4]; // x: start, y: end (<= 0 disables), z: interior lamps
    glm::mat4 view_projection;
    float eye[4]; // camera position (experimental path: view vector)
    float shading[4]; // x: night_ambient_scale()
};

// Bindings and types shared by both shading paths.
constexpr char shader_interface[] = R"(

struct Frame {
    lights: array<Light, 1>, // the sun
    rings: array<Light, 90>, // the further lights, nine a ring
    ring_counts: array<vec4<i32>, 3>,
    interior: array<Light, 8>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
    view_projection: mat4x4<f32>,
    eye: vec4<f32>,
    shading: vec4<f32>, // x: the experimental path's night ambient factor
};

// One primitive of the shape (static per model generation).
struct Draw {
    local: mat4x4<f32>,
    local_normal: mat4x4<f32>,
    light_material: i32,
    named_shader: i32,
    matrix_index: i32,
    lod_bias: f32,
};

struct Instance {
    transform: mat4x4<f32>,
    normal: mat4x4<f32>,
    locals: i32,
    ring: i32,
    decal: i32,
};

struct Local {
    matrix: mat4x4<f32>,
    normal: mat4x4<f32>,
};

// Alpha test is a pipeline variant: RTS toggles it between primitives.
override alpha_test: bool = false;
// MSAA with alpha-to-coverage: the alpha test's edge becomes coverage.
override alpha_to_coverage: bool = false;
// HDR frame: the experimental path keeps its highlights for the tone curve.
override hdr_output: bool = false;

// Alpha-to-coverage for an alpha test at `cutoff` (Golus, "Anti-aliased
// Alpha Test"): the edge spread over one pixel (`width`: fwidth of the
// alpha) and the alpha raised in the far mips (`mip`) so foliage and fences
// do not thin out with distance. Zero coverage drops the fragment.
fn coverage_alpha(alpha: f32, cutoff: f32, width: f32, mip: f32) -> f32 {
    let kept = alpha * (1.0 + max(mip, 0.0) * 0.25);
    return clamp((kept - cutoff) / max(width, 0.0001) + 0.5, 0.0, 1.0);
}

@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
// The model's primitives, by slot; each id says which one it draws.
@group(0) @binding(2) var<storage, read> draws: array<Draw>;
@group(1) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(1) var<storage, read> instances: array<Instance>;
// Per draw call a contiguous run of (instance, primitive) entries, starting at
// the draw's first instance; a multi-draw call has a run per primitive.
@group(1) @binding(2) var<storage, read> instance_ids: array<vec2<u32>>;
@group(1) @binding(3) var<storage, read> locals: array<Local>;
// The primitive being drawn: set first thing in each entry point.
var<private> draw: Draw;

struct Placement {
    model: mat4x4<f32>,
    normal_matrix: mat4x4<f32>,
};

// World and normal matrix of this primitive for one instance:
// instance transform times the primitive's (possibly animated) local matrix.
fn placement(instance_index: u32) -> Placement {
    let instance = instances[instance_ids[instance_index].x];
    var local = draw.local;
    var local_normal = draw.local_normal;
    if (instance.locals >= 0 && draw.matrix_index >= 0) {
        let animated = locals[u32(instance.locals + draw.matrix_index)];
        local = animated.matrix;
        local_normal = animated.normal;
    }
    return Placement(instance.transform * local, instance.normal * local_normal);
}

struct VertexIn {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) colour1: vec4<f32>,
    @location(4) colour2: vec4<f32>,
    @builtin(instance_index) instance: u32,
};

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world: vec4<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) colour: vec4<f32>,
    @location(4) fog: vec4<f32>,
    @location(5) @interpolate(flat) ring: u32, // the instance's ring of further lights
    @location(6) @interpolate(flat) draw_slot: u32, // the primitive (draws[])
};

fn ring_count(ring: u32) -> i32 {
    return min(frame.ring_counts[ring / 4u][ring % 4u], 9);
}

// RTS fog: linear in clip-space depth, scaled by the fog colour alpha.
fn fog_for(clip_z: f32) -> vec4<f32> {
    let fog_start = frame.fog_distance.x;
    let fog_end = frame.fog_distance.y;
    if (fog_end <= 0.0) {
        return vec4<f32>(0.0);
    }
    if (fog_end > fog_start) {
        let amount = clamp(frame.fog_colour.w * ((clip_z - fog_start) / (fog_end - fog_start)), 0.0, 1.0);
        return vec4<f32>(frame.fog_colour.xyz, amount);
    }
    return frame.fog_colour;
}
)";


constexpr char rts_shader_body[] = R"(
fn transformed_normal(normal_matrix: mat4x4<f32>, input_normal: vec3<f32>) -> vec3<f32> {
    if (draw.light_material == -9) { // Circumference
        return vec3<f32>(0.0, 1.0, 0.0);
    }
    return (normal_matrix * vec4<f32>(input_normal, 1.0)).xyz;
}

// Port of RTS shape.vert.
@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.draw_slot = instance_ids[input.instance].y;
    draw = draws[out.draw_slot];
    let place = placement(input.instance);
    out.world = place.model * vec4<f32>(input.position, 1.0);
    out.clip = frame.view_projection * out.world;
    // openRTS: a transfer lies in the terrain's own triangles' planes
    // (transfer_shape.cpp); the two transforms round apart by far less than
    // a ten-thousandth of the distance, so it is drawn that much nearer
    // (reversed Z: a larger depth) and does not flicker against the ground.
    if (instances[instance_ids[input.instance].x].decal != 0) {
        out.clip.z = out.clip.z * 1.0001;
    }
    out.uv = input.uv;
    out.ring = u32(clamp(instances[instance_ids[input.instance].x].ring, 0, 9));
    out.normal = vec3<f32>(0.0);

    let shader = draw.named_shader;
    let material = draw.light_material;
    // RTS draws HalfBright (-11) and a night shape at night (-16) as
    // NamedShader 3 (rts1.exe 0x4B042C): unlit, Ambient 1.
    let forced_unlit = material == -11 || material == -16;
    let unlit_shader = shader == 1 || shader == 3 || shader == 6 || forced_unlit;
    var luminance = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    var ambient = vec4<f32>(0.0);

    if (!unlit_shader && material != -8) {
        var diffuse = vec4<f32>(0.0);
        if (material == -11) { // HalfBright
            ambient = input.colour1;
        } else if (material == -1) { // vertex colour emission
            luminance = input.colour2;
        }
        out.normal = transformed_normal(place.normal_matrix, input.normal);

        let sun = frame.lights[0];
        let sun_factor = clamp(-dot(out.normal, sun.direction) * 2.0, 0.0, 1.0);
        var light_ambient = sun.ambient;
        var light_diffuse = sun.diffuse;
        if (material == -12) { // DarkShade
            light_ambient = vec4<f32>(0.0, 0.0, 0.0, 1.0);
            light_diffuse = sun.ambient;
        }
        diffuse = max(light_diffuse * sun_factor, diffuse);
        ambient = max(light_ambient, ambient);
        ambient = max(diffuse, ambient);
    } else {
        if (ring_count(out.ring) > 0) {
            out.normal = transformed_normal(place.normal_matrix, input.normal);
        }
        if (unlit_shader) {
            ambient = vec4<f32>(1.0);
        } else {
            ambient = input.colour1;
        }
    }
    out.colour = max(ambient, luminance);
    out.fog = fog_for(out.clip.z);
    return out;
}

// Port of RTS shape.frag.
@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    draw = draws[input.draw_slot];
    let texel = textureSampleBias(source_texture, source_sampler, input.uv, draw.lod_bias);
    let alpha_width = fwidth(texel.w);
    let texels = input.uv * vec2<f32>(textureDimensions(source_texture));
    let texels_dx = dpdx(texels);
    let texels_dy = dpdy(texels);
    let alpha_mip = 0.5 * log2(max(max(dot(texels_dx, texels_dx), dot(texels_dy, texels_dy)), 1e-8));
    let shader = draw.named_shader;
    // Forced to NamedShader 3 (HalfBright, a night shape at night): an alpha shader.
    let alpha_shader = shader == 3 || shader == 4 || shader == 6 || shader == 7 || draw.light_material == -11
                       || draw.light_material == -16;

    var color = vec4<f32>(0.0);
    if (texel.w > 0.0 || !alpha_shader) {
        var specular_factor = 0.1;
        if (draw.light_material == -6) { // LoShine
            specular_factor = 0.3;
        } else if (draw.light_material == -7) { // HiShine
            specular_factor = 0.8;
        } else if (draw.light_material == -12) { // DarkShade
            specular_factor = 0.04;
        }

        let normal = normalize(input.normal);
        var light_colour = vec4<f32>(0.0);
        var specular = vec4<f32>(0.0);
        let extra_lights = ring_count(input.ring);
        for (var i = 0; i < extra_lights; i = i + 1) {
            let light = frame.rings[input.ring * 9u + u32(i)];
            let factor = light_factor(light, input.world, normal);
            if (factor < 0.0) { continue; }
            light_colour = max(light_colour, light.diffuse * factor);
            specular = max(specular, light.diffuse * factor * specular_factor);
        }

        // The lamps of the cab or salon (RTS shape.frag, LightsInt).
        let interior_lights = min(i32(frame.fog_distance.z), 8);
        for (var i = 0; i < interior_lights; i = i + 1) {
            let factor = interior_factor(frame.interior[i], input.world, normal);
            if (factor < 0.0) { continue; }
            light_colour = max(light_colour, frame.interior[i].diffuse * factor);
            specular = max(specular, frame.interior[i].diffuse * factor * specular_factor);
        }

        specular.w = 0.0;
        color = texel * max(input.colour, light_colour) + specular;
        if (input.fog.w > 0.0) {
            color = vec4<f32>(((1.0 - input.fog.w) * color + input.fog.w * input.fog).xyz, color.w);
        }
    }

    // Fixed-function alpha test of RTS: GL_GREATER 0.3; with MSAA its edge
    // as coverage.
    if (alpha_test) {
        if (alpha_to_coverage) {
            color.w = coverage_alpha(color.w, 0.3, alpha_width, alpha_mip);
            if (color.w <= 0.0) {
                discard;
            }
        } else if (!(color.w > 0.3)) {
            discard;
        }
    }
    return color;
}
)";

// openRTS's own shading. Lighting is computed in linear light (textures and
// the RTS sun colours are display-encoded, gamma 2.2) and encoded back with a
// soft shoulder instead of a hard clip:
//  - ambient from the sky: full from above, darker towards the ground;
//  - Lambert sun diffuse;
//  - Blinn-Phong sun highlight, strength and sharpness by the MSTS light
//    material (normal faint, LoShine, HiShine);
//  - MSTS light materials: FullBright and the unlit shaders take the texture
//    as is, HalfBright uses the vertex colour as ambient, DarkShade gets only
//    the dimmed ambient as its light, vertex-colour emission adds the vertex
//    colour, Circumference lights as if facing up.
// Fog is applied in display space, as the fog colour is given (RTS).
constexpr char experimental_shader_body[] = R"(
@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.draw_slot = instance_ids[input.instance].y;
    draw = draws[out.draw_slot];
    let place = placement(input.instance);
    out.world = place.model * vec4<f32>(input.position, 1.0);
    out.clip = frame.view_projection * out.world;
    // openRTS: a transfer lies in the terrain's own triangles' planes
    // (transfer_shape.cpp); the two transforms round apart by far less than
    // a ten-thousandth of the distance, so it is drawn that much nearer
    // (reversed Z: a larger depth) and does not flicker against the ground.
    if (instances[instance_ids[input.instance].x].decal != 0) {
        out.clip.z = out.clip.z * 1.0001;
    }
    out.uv = input.uv;
    out.ring = u32(clamp(instances[instance_ids[input.instance].x].ring, 0, 9));
    out.normal = (place.normal_matrix * vec4<f32>(input.normal, 0.0)).xyz;
    if (draw.light_material == -9) { // Circumference
        out.normal = vec3<f32>(0.0, 1.0, 0.0);
    }
    // HalfBright: ambient from the first vertex colour; emission: the second.
    out.colour = vec4<f32>(0.0);
    if (draw.light_material == -11) {
        out.colour = input.colour1;
    } else if (draw.light_material == -1) {
        out.colour = input.colour2;
    }
    out.fog = fog_for(out.clip.z);
    return out;
}

fn to_linear(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(2.2));
}

fn to_display(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.2));
}

// One further light's linear contribution: physical falloff, or RTS's shape
// factor taken in display space.
fn lamp_light(light: Light, world: vec4<f32>, n: vec3<f32>, physical: bool) -> vec3<f32> {
    if (physical) {
        let strength = physical_light(light, world, n);
        if (strength > 0.0) {
            return to_linear(light.diffuse.xyz) * strength;
        }
        return vec3<f32>(0.0);
    }
    let factor = light_factor(light, world, n);
    if (factor > 0.0) {
        return to_linear(light.diffuse.xyz * factor);
    }
    return vec3<f32>(0.0);
}

// Linear up to the knee, then an exponential roll-off towards 1.
fn shoulder(c: vec3<f32>) -> vec3<f32> {
    let knee = 0.6;
    let room = 1.0 - knee;
    let over = max(c - vec3<f32>(knee), vec3<f32>(0.0));
    let rolled = vec3<f32>(knee) + room * (vec3<f32>(1.0) - exp(-over / room));
    return select(c, rolled, c > vec3<f32>(knee));
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    draw = draws[input.draw_slot];
    let texel = textureSampleBias(source_texture, source_sampler, input.uv, draw.lod_bias);
    let alpha_width = fwidth(texel.w);
    let texels = input.uv * vec2<f32>(textureDimensions(source_texture));
    let texels_dx = dpdx(texels);
    let texels_dy = dpdy(texels);
    let alpha_mip = 0.5 * log2(max(max(dot(texels_dx, texels_dx), dot(texels_dy, texels_dy)), 1e-8));
    var coverage = texel.w;
    if (alpha_test) {
        if (alpha_to_coverage) {
            coverage = coverage_alpha(texel.w, 0.5, alpha_width, alpha_mip);
            if (coverage <= 0.0) {
                discard;
            }
        } else if (texel.w < 0.5) {
            discard;
        }
    }

    let shader = draw.named_shader;
    let material = draw.light_material;
    // FullBright is unlit unless the panel lights it (frame.shading.y, light_fullbright()).
    // A night texture (-16) is self-lit whatever the panel says.
    let unlit = shader == 1 || shader == 3 || shader == 6 || (material == -8 && frame.shading.y < 0.5) || material == -16;
    let base = to_linear(texel.xyz);
    var colour = base;
    if (!unlit) {
        let sun = frame.lights[0];
        let sun_colour = to_linear(sun.diffuse.xyz);
        let sky_colour = to_linear(sun.ambient.xyz);
        let view = normalize(frame.eye.xyz - input.world.xyz);
        // The normal as the shape gives it, never turned towards the camera:
        // MSTS trees and many buildings point their normals up on purpose
        // (Circumference), and turning them would darken everything above
        // the camera's height.
        let n = normalize(input.normal);
        let to_sun = -normalize(sun.direction);
        let facing = max(dot(n, to_sun), 0.0);

        // Sky light. Environments give the sun an ambient colour as bright as
        // its direct one at noon (SummerClear: both ffffffff), which RTS
        // shows flat (max of the two). Here the stronger the direct sun, the
        // less of that ambient is taken, down to 35 %, so the sun's
        // direction always shows; at night (no direct sun) it stays as given.
        // Full from above, 55 % from below.
        let sun_strength = clamp(dot(sun_colour, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        var ambient = sky_colour * (1.0 - 0.65 * sun_strength) * mix(0.55, 1.0, n.y * 0.5 + 0.5)
            * mix(1.0, frame.shading.x, 1.0 - sun_strength);
        // The sun's shadow and the ambient occlusion (SceneEffects).
        let shadow = effects_shadow(input.world.xyz, n, input.clip);
        let occlusion = effects_ao(input.clip);
        ambient = ambient * occlusion;
        var diffuse = sun_colour * facing * shadow;
        // Light the ground throws back (SceneEffectSettings::ground_bounce):
        // the ground lit by the sun and the sky, onto surfaces turned down
        // (all of it) or sideways (half); the occlusion keeps gaps dark.
        var bounce = effects.lamps.y * (sun_colour * max(to_sun.y, 0.0) + sky_colour * (1.0 - 0.65 * sun_strength))
            * (0.5 - 0.5 * n.y) * occlusion;
        var strength = 0.04;
        var sharpness = 16.0;
        if (material == -6) { // LoShine
            strength = 0.25;
            sharpness = 32.0;
        } else if (material == -7) { // HiShine
            strength = 0.6;
            sharpness = 64.0;
        } else if (material == -11) { // HalfBright
            ambient = max(ambient, to_linear(input.colour.xyz));
        } else if (material == -12) { // DarkShade
            ambient = vec3<f32>(0.0);
            bounce = vec3<f32>(0.0);
            diffuse = sky_colour * facing;
            strength = 0.0;
        }
        let halfway = normalize(to_sun + view);
        let highlight = sun_colour * strength * pow(max(dot(n, halfway), 0.0), sharpness) * step(0.0, dot(n, to_sun)) * shadow;

        // Headlights, searchlights and the lamps of the cab or salon: the
        // RTS light shapes, added in linear light. A light's falloff is taken
        // in display space, as RTS's (its colour times the factor, then
        // linear): a bright middle fading out, not a flat disc.
        var lamps = vec3<f32>(0.0);
        let physical = frame.fog_distance.w > 0.5; // physical_light_falloff()
        if (effects.flags.x != 0u) {
            // Every lamp of the fragment's cluster (SceneEffects).
            let cluster = effects_cluster(input.clip, input.world.xyz);
            let count = min(cluster_counts[cluster], effects.clusters.w);
            let first = cluster * effects.clusters.w;
            for (var i = 0u; i < count; i = i + 1u) {
                let index = cluster_lights[first + i];
                let light = lamp_light(scene_lights[index], input.world, n, physical);
                if (any(light > vec3<f32>(0.0))) {
                    lamps = lamps + light * effects_lamp_shadow(index, input.world.xyz, n, input.clip);
                }
            }
        } else {
            // The RTS rings; the lamps throwing shadows found by their key.
            let extra_lights = ring_count(input.ring);
            for (var i = 0; i < extra_lights; i = i + 1) {
                let ring_light = frame.rings[input.ring * 9u + u32(i)];
                let light = lamp_light(ring_light, input.world, n, physical);
                if (any(light > vec3<f32>(0.0))) {
                    lamps = lamps + light * effects_lamp_shadow_of(ring_light, input.world.xyz, n, input.clip);
                }
            }
        }
        let interior_lights = min(i32(frame.fog_distance.z), 8);
        for (var i = 0; i < interior_lights; i = i + 1) {
            let factor = interior_factor(frame.interior[i], input.world, n);
            if (factor > 0.0) {
                lamps = lamps + to_linear(frame.interior[i].diffuse.xyz * factor);
            }
        }

        // Weaker by day (SceneEffectSettings::lamps_in_daylight).
        lamps = lamps * (1.0 - effects.lamps.x);
        colour = base * (ambient + diffuse + lamps + bounce) + highlight;
        if (material == -1) { // vertex colour emission
            colour = max(colour, base * to_linear(input.colour.xyz));
        }
    }

    if (effects.flags.z != 0u) {
        // Aerial perspective in linear light instead of RTS's fog.
        colour = effects_aerial(colour, input.world.xyz);
    }
    if (effects.flags.y == 2u) {
        return vec4<f32>(vec3<f32>(effects_ao(input.clip)), coverage); // the occlusion itself (--effects show-ao)
    }
    var display = to_display(select(shoulder(colour), max(colour, vec3<f32>(0.0)), hdr_output));
    if (effects.flags.z == 0u && input.fog.w > 0.0) {
        display = mix(display, input.fog.xyz, input.fog.w);
    }
    return vec4<f32>(display, coverage);
}
)";

[[nodiscard]] glm::vec3 to_vec3(const float (&values)[3]) {
    return {values[0], values[1], values[2]};
}

// MSTS stores a 3x3 rotation followed by a translation for row vectors
// (v' = v * M): each stored row is the image of one basis axis, which is a
// column of the equivalent column-vector matrix.
[[nodiscard]] glm::mat4 shape_matrix(const formats::ShapeMatrix& shape) {
    const auto& v = shape.values;
    glm::mat4 result{1.0f};
    result[0] = glm::vec4{v[0], v[1], v[2], 0.0f};
    result[1] = glm::vec4{v[3], v[4], v[5], 0.0f};
    result[2] = glm::vec4{v[6], v[7], v[8], 0.0f};
    result[3] = glm::vec4{v[9], v[10], v[11], 1.0f};
    return result;
}

// Accumulates the matrix chain from `index` up to the hierarchy root. The visit
// counter guards against cyclic hierarchies in malformed shapes.
[[nodiscard]] glm::mat4 world_matrix(const formats::ShapeModel& model, std::int32_t index) {
    glm::mat4 result{1.0f};
    std::size_t visited = 0;
    while (index >= 0 && static_cast<std::size_t>(index) < model.matrices.size()
           && visited++ <= model.matrices.size()) {
        result = shape_matrix(model.matrices[index]) * result;
        index = model.hierarchy[index];
    }
    return result;
}

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

void copy_vec4(float (&target)[4], const glm::vec4& value) {
    target[0] = value.x;
    target[1] = value.y;
    target[2] = value.z;
    target[3] = value.w;
}

[[nodiscard]] FrameUniform frame_uniform(const ModelLighting& lighting, const glm::mat4& view_projection,
                                         const glm::vec3& eye) {
    FrameUniform result{};
    result.view_projection = view_projection;
    result.eye[0] = eye.x;
    result.eye[1] = eye.y;
    result.eye[2] = eye.z;
    result.eye[3] = 1.0f;
    auto& sun = result.lights[0];
    copy_vec4(sun.diffuse, lighting.sun_diffuse);
    copy_vec4(sun.ambient, lighting.sun_ambient);
    sun.direction[0] = lighting.sun_direction.x;
    sun.direction[1] = lighting.sun_direction.y;
    sun.direction[2] = lighting.sun_direction.z;
    for (std::size_t k = 0; k < LightRings::count; ++k) {
        const auto& ring = lighting.light_rings.lights[k];
        const auto n = (std::min)(ring.size(), LightRings::capacity);
        result.ring_counts[k] = static_cast<std::int32_t>(n);
        for (std::size_t i = 0; i < n; ++i) detail::pack_light(result.rings[k * LightRings::capacity + i], ring[i]);
    }
    sun.type[0] = static_cast<float>(result.ring_counts[0]);
    const auto interior = (std::min)(lighting.interior_lights.size(), max_interior_lights);
    result.fog_distance[2] = static_cast<float>(interior);
    for (std::size_t i = 0; i < interior; ++i) detail::pack_light(result.interior[i], lighting.interior_lights[i]);
    copy_vec4(result.fog_colour, lighting.fog_colour);
    result.fog_distance[0] = lighting.fog_start;
    result.fog_distance[1] = lighting.fog_end;
    result.fog_distance[3] = physical_light_falloff() ? 1.0f : 0.0f;
    result.shading[0] = night_ambient_scale();
    result.shading[1] = light_fullbright() ? 1.0f : 0.0f;
    return result;
}

[[nodiscard]] WGPUBindGroupLayout create_material_layout(const WGPUDevice device) {
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    entries[2].buffer.minBindingSize = sizeof(MaterialUniform);

    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

[[nodiscard]] WGPUBindGroupLayout create_frame_layout(const WGPUDevice device) {
    std::array<WGPUBindGroupLayoutEntry, 4> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(FrameUniform);
    const std::array<std::uint64_t, 3> storage_sizes{sizeof(GpuInstance), sizeof(IdEntry), sizeof(GpuLocal)};
    for (std::uint32_t i = 0; i < storage_sizes.size(); ++i) {
        entries[i + 1].binding = i + 1;
        entries[i + 1].visibility = WGPUShaderStage_Vertex;
        entries[i + 1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        entries[i + 1].buffer.minBindingSize = storage_sizes[i];
    }

    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

[[nodiscard]] WGPUSampler create_sampler(const WGPUDevice device, const rts::TextureFilter filter) {
    using rts::TextureFilter;
    // RTS sets GL_TEXTURE_MIN_FILTER and _MAG_FILTER to the same value; GL rejects
    // mipmapped values for magnification, which then stays linear.
    // GL_LINEAR and GL_NEAREST do not mipmap: only the base level is read.
    auto mag = WGPUFilterMode_Linear;
    auto min = WGPUFilterMode_Linear;
    auto mip = WGPUMipmapFilterMode_Linear;
    float lod_max = 32.0f;
    switch (filter) {
    case TextureFilter::nearest:
        mag = min = WGPUFilterMode_Nearest;
        mip = WGPUMipmapFilterMode_Nearest;
        lod_max = 0.0f;
        break;
    case TextureFilter::linear:
        mip = WGPUMipmapFilterMode_Nearest;
        lod_max = 0.0f;
        break;
    case TextureFilter::nearest_mip_nearest:
        min = WGPUFilterMode_Nearest;
        mip = WGPUMipmapFilterMode_Nearest;
        break;
    case TextureFilter::linear_mip_nearest:
        mip = WGPUMipmapFilterMode_Nearest;
        break;
    case TextureFilter::nearest_mip_linear:
        min = WGPUFilterMode_Nearest;
        break;
    case TextureFilter::linear_mip_linear:
    case TextureFilter::unspecified:
        break;
    }

    const WGPUSamplerDescriptor descriptor{
        .addressModeU = WGPUAddressMode_Repeat,
        .addressModeV = WGPUAddressMode_Repeat,
        .addressModeW = WGPUAddressMode_Repeat,
        .magFilter = mag,
        .minFilter = min,
        .mipmapFilter = mip,
        .lodMaxClamp = lod_max,
        .maxAnisotropy = static_cast<std::uint16_t>(
            mag == WGPUFilterMode_Linear && min == WGPUFilterMode_Linear && mip == WGPUMipmapFilterMode_Linear ? max_anisotropy : 1),
    };
    return wgpuDeviceCreateSampler(device, &descriptor);
}

[[nodiscard]] WGPUShaderModule create_shader_module(const WGPUDevice device, const char* body, const bool effects = false) {
    auto code = std::string(detail::light_shapes_wgsl) + shader_interface;
    if (effects) code += std::string(detail::effects_struct_wgsl) + detail::effects_main_wgsl;
    code += body;
    const WGPUShaderSourceWGSL source{
        .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code = {.data = code.data(), .length = code.size()},
    };
    const WGPUShaderModuleDescriptor descriptor{.nextInChain = &source.chain};
    return wgpuDeviceCreateShaderModule(device, &descriptor);
}

// Depth only (SceneEffects: the shadow cascades and the camera's pre-pass):
// the instance's placement as the shading paths have it, the view-projection
// of the pass by a dynamic offset; alpha-tested primitives cut out at 0.5.
constexpr char depth_shader[] = R"(
struct DepthFrame {
    view_projection: mat4x4<f32>,
};
struct Draw {
    local: mat4x4<f32>,
    local_normal: mat4x4<f32>,
    light_material: i32,
    named_shader: i32,
    matrix_index: i32,
    lod_bias: f32,
};
struct Instance {
    transform: mat4x4<f32>,
    normal: mat4x4<f32>,
    locals: i32,
    ring: i32,
    decal: i32,
};
struct Local {
    matrix: mat4x4<f32>,
    normal: mat4x4<f32>,
};
@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
@group(0) @binding(2) var<storage, read> draws: array<Draw>;
@group(1) @binding(0) var<uniform> depth_frame: DepthFrame;
@group(1) @binding(1) var<storage, read> instances: array<Instance>;
@group(1) @binding(2) var<storage, read> instance_ids: array<vec2<u32>>;
@group(1) @binding(3) var<storage, read> locals: array<Local>;

struct DepthOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_depth(@location(0) position: vec3<f32>, @location(2) uv: vec2<f32>, @builtin(instance_index) index: u32) -> DepthOut {
    let entry = instance_ids[index];
    let instance = instances[entry.x];
    let draw = draws[entry.y];
    var local = draw.local;
    if (instance.locals >= 0 && draw.matrix_index >= 0) {
        local = locals[u32(instance.locals + draw.matrix_index)].matrix;
    }
    var out: DepthOut;
    out.clip = depth_frame.view_projection * (instance.transform * local * vec4<f32>(position, 1.0));
    out.uv = uv;
    return out;
}

@fragment
fn fs_cutout(input: DepthOut) {
    if (textureSample(source_texture, source_sampler, input.uv).a < 0.5) {
        discard;
    }
}
)";

[[nodiscard]] WGPURenderPipeline create_depth_pipeline(const WGPUDevice device, const WGPUPipelineLayout layout, const WGPUShaderModule shader,
                                                       const bool shadow, const bool cutout, const bool cull) {
    const std::array<WGPUVertexAttribute, 2> attributes{{
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(GpuVertex, position), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(GpuVertex, uv), .shaderLocation = 2},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(GpuVertex),
        .attributeCount = attributes.size(),
        .attributes = attributes.data(),
    };
    constexpr WGPUStencilFaceState keep{
        .compare = WGPUCompareFunction_Always,
        .failOp = WGPUStencilOperation_Keep,
        .depthFailOp = WGPUStencilOperation_Keep,
        .passOp = WGPUStencilOperation_Keep,
    };
    // Shadows: ordinary depth (0 near), biased by the slope; both faces cast
    // (MSTS models are often one-sided). The camera's: reversed Z as the main pass.
    const WGPUDepthStencilState depth{
        .format = depth_format,
        .depthWriteEnabled = WGPUOptionalBool_True,
        .depthCompare = shadow ? WGPUCompareFunction_LessEqual : depth_nearer_or_equal,
        .stencilFront = keep,
        .stencilBack = keep,
        .stencilReadMask = ~0u,
        .stencilWriteMask = ~0u,
        .depthBias = shadow ? 2 : 0,
        .depthBiasSlopeScale = shadow ? 2.0f : 0.0f,
        .depthBiasClamp = 0.0f,
    };
    const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_cutout"), .targetCount = 0, .targets = nullptr};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {.module = shader, .entryPoint = wgsl_name("vs_depth"), .bufferCount = 1, .buffers = &vertex_layout},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList,
                      .frontFace = WGPUFrontFace_CW,
                      .cullMode = cull ? WGPUCullMode_Back : WGPUCullMode_None},
        .depthStencil = &depth,
        .multisample = {.count = 1, .mask = ~0u, .alphaToCoverageEnabled = false},
        .fragment = cutout ? &fragment : nullptr,
    };
    return wgpuDeviceCreateRenderPipeline(device, &descriptor);
}

// Pipeline variants correspond to the fixed-function state RTS toggles per
// primitive: GL_BLEND and glDepthMask. Alpha test is a shader uniform.
struct PipelineKey {
    bool blend = false;
    bool depth_write = true;
    bool alpha_test = false;

    [[nodiscard]] std::size_t index() const noexcept {
        return (alpha_test ? 4u : 0u) + (blend ? 2u : 0u) + (depth_write ? 1u : 0u);
    }
};
constexpr std::size_t pipeline_variant_count = 8;
constexpr std::size_t shading_path_count = 2;

[[nodiscard]] std::size_t path_index(const ShadingPath path) noexcept {
    return path == ShadingPath::rts_compatible ? 0 : 1;
}

// Faces: RTS enables GL_CULL_FACE once (create_game_window, 0x40564D) with
// OpenGL's defaults (counter-clockwise front, back faces culled) and turns it
// off only for its shadow map (0x47BEB0) and shader effects (0x4BEDA0,
// 0x4BF3F8, 0x4BF878), never for shapes; glFrontFace is never called.
// In openRTS the faces RTS shows come out clockwise on screen (seen in the
// viewer: with a counter-clockwise front the visible faces were culled), so
// the front is clockwise here. Why the winding differs from RTS's (RTS
// reordering indices on load, or wgpu's convention against OpenGL's) is not
// traced — UNKNOWN. Without culling, the coplanar front and back faces of a
// two-sided part (a signal's back plate) fight in depth.
[[nodiscard]] WGPURenderPipeline create_pipeline(const WGPUDevice device,
                                                 const WGPUPipelineLayout layout,
                                                 const WGPUShaderModule shader,
                                                 const WGPUTextureFormat target_format,
                                                 const PipelineKey key,
                                                 const bool cull_back_faces) {
    const std::array<WGPUVertexAttribute, 5> attributes{{
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(GpuVertex, position), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(GpuVertex, normal), .shaderLocation = 1},
        {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(GpuVertex, uv), .shaderLocation = 2},
        {.format = WGPUVertexFormat_Unorm8x4, .offset = offsetof(GpuVertex, colour1), .shaderLocation = 3},
        {.format = WGPUVertexFormat_Unorm8x4, .offset = offsetof(GpuVertex, colour2), .shaderLocation = 4},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(GpuVertex),
        .attributeCount = attributes.size(),
        .attributes = attributes.data(),
    };

    constexpr WGPUStencilFaceState keep_stencil{
        .compare = WGPUCompareFunction_Always,
        .failOp = WGPUStencilOperation_Keep,
        .depthFailOp = WGPUStencilOperation_Keep,
        .passOp = WGPUStencilOperation_Keep,
    };
    // RTS leaves glDepthFunc at GL_LEQUAL from terrain drawing.
    const WGPUDepthStencilState depth{
        .format = depth_format,
        .depthWriteEnabled = key.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False,
        .depthCompare = depth_nearer_or_equal,
        .stencilFront = keep_stencil,
        .stencilBack = keep_stencil,
        .stencilReadMask = ~0u,
        .stencilWriteMask = ~0u,
    };

    // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) applies to colour and alpha.
    constexpr WGPUBlendComponent alpha_blend_component{
        .operation = WGPUBlendOperation_Add,
        .srcFactor = WGPUBlendFactor_SrcAlpha,
        .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
    };
    constexpr WGPUBlendState alpha_blend{.color = alpha_blend_component, .alpha = alpha_blend_component};
    const WGPUColorTargetState target{
        .format = target_format,
        .blend = key.blend ? &alpha_blend : nullptr,
        .writeMask = WGPUColorWriteMask_All,
    };
    const std::array<WGPUConstantEntry, 3> constants{{
        {.key = wgsl_name("alpha_test"), .value = key.alpha_test ? 1.0 : 0.0},
        {.key = wgsl_name("alpha_to_coverage"), .value = key.alpha_test && render_sample_count() > 1 ? 1.0 : 0.0},
        {.key = wgsl_name("hdr_output"), .value = scene_hdr() ? 1.0 : 0.0},
    }};
    const WGPUFragmentState fragment{
        .module = shader,
        .entryPoint = wgsl_name("fs_main"),
        .constantCount = constants.size(),
        .constants = constants.data(),
        .targetCount = 1,
        .targets = &target,
    };

    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {
            .module = shader,
            .entryPoint = wgsl_name("vs_main"),
            .bufferCount = 1,
            .buffers = &vertex_layout,
        },
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList,
                      .frontFace = WGPUFrontFace_CW,
                      .cullMode = cull_back_faces ? WGPUCullMode_Back : WGPUCullMode_None},
        .depthStencil = &depth,
        // RTS with "+Alpha" enables GL_SAMPLE_ALPHA_TO_COVERAGE for every
        // BlendATex / BlendATexDiff primitive, blended ones too (0x4B0E6A): a
        // texel's alpha is also its coverage, so nearly transparent texels
        // write neither colour nor depth (the shadow quads over the ED9M's
        // gauges do not cut holes in the backlight drawn after them).
        .multisample = scene_multisample(key.alpha_test || key.blend),
        .fragment = &fragment,
    };
    return wgpuDeviceCreateRenderPipeline(device, &descriptor);
}

[[nodiscard]] WGPUBuffer create_buffer(const WGPUDevice device,
                                       const WGPUBufferUsage usage,
                                       const std::uint64_t size,
                                       const void* data = nullptr) {
    const WGPUBufferDescriptor descriptor{.usage = usage | WGPUBufferUsage_CopyDst, .size = size};
    const auto buffer = wgpuDeviceCreateBuffer(device, &descriptor);
    if (!buffer || !data) return buffer;

    if (const auto queue = wgpuDeviceGetQueue(device)) {
        wgpuQueueWriteBuffer(queue, buffer, 0, data, size);
        wgpuQueueRelease(queue);
    }
    return buffer;
}

struct BoundsAccumulator {
    glm::vec3 min{};
    glm::vec3 max{};
    bool empty = true;

    void add(const glm::vec3& point) {
        if (empty) {
            min = max = point;
            empty = false;
            return;
        }
        min = (glm::min)(min, point);
        max = (glm::max)(max, point);
    }

    [[nodiscard]] ModelBounds sphere() const {
        if (empty) return {};
        const auto center = (min + max) * 0.5f;
        return {.center = center, .radius = glm::length(max - center), .min = min, .max = max};
    }
};

// How the texture of an image index is to be drawn.
enum class TextureAvailability {
    uploaded,
    missing,     // file not found: RTS draws with an unbound (black) texture
    unsupported, // openRTS could not decode it: the primitive is not drawn
};

// Group ids by a key of two pointers and a small tag, for batching: open
// addressing over a table kept between frames, emptied by a new generation
// at no cost and grown when half full. A frame that saw no more groups than
// the frames before allocates nothing.
class GroupTable {
public:
    // Empty, ready for about `keys` keys.
    void reset(const std::size_t keys) {
        used_ = 0;
        if (++generation_ == 0 || slots_.size() < 2 * keys) resize(keys);
    }
    // The key's group: the one stored for it, else `fresh`, stored then.
    [[nodiscard]] std::uint32_t find_or_add(const void* a, const void* b, const std::uint32_t tag, const std::uint32_t fresh) {
        if (2 * (used_ + 1) > slots_.size()) resize(2 * (used_ + 1));
        for (auto i = home(a, b, tag);; i = (i + 1) & mask_) {
            auto& slot = slots_[i];
            if (slot.generation != generation_) {
                slot = {a, b, tag, fresh, generation_};
                ++used_;
                return fresh;
            }
            if (slot.a == a && slot.b == b && slot.tag == tag) return slot.value;
        }
    }

private:
    struct Slot {
        const void* a = nullptr;
        const void* b = nullptr;
        std::uint32_t tag = 0;
        std::uint32_t value = 0;
        std::uint32_t generation = 0; // stored in this generation, else empty
    };
    // Fibonacci hashing: the product's high bits.
    [[nodiscard]] std::size_t home(const void* a, const void* b, const std::uint32_t tag) const noexcept {
        auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a));
        key ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(b)) * 0xC2B2AE3D27D4EB4Full;
        key ^= std::uint64_t{tag} * 0x165667B19E3779F9ull;
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }
    // A table for `keys` keys at most half full; this generation's keys move in.
    void resize(const std::size_t keys) {
        std::size_t size = 64;
        unsigned bits = 6;
        while (size < 2 * keys) {
            size *= 2;
            ++bits;
        }
        std::vector<Slot> old(size);
        old.swap(slots_);
        const auto live = generation_;
        mask_ = size - 1;
        shift_ = 64 - bits;
        generation_ = 1;
        used_ = 0;
        if (live == 0) return;
        for (const auto& slot : old) {
            if (slot.generation != live) continue;
            for (auto i = home(slot.a, slot.b, slot.tag);; i = (i + 1) & mask_) {
                if (slots_[i].generation == generation_) continue;
                slots_[i] = {slot.a, slot.b, slot.tag, slot.value, generation_};
                ++used_;
                break;
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t used_ = 0;
    std::uint32_t generation_ = 0;
};

} // namespace

// Vertex and index buffers of one model generation.
struct GpuGeometry {
    WGPUBuffer vertices = nullptr;
    WGPUBuffer indices = nullptr;

    GpuGeometry() = default;
    GpuGeometry(const GpuGeometry&) = delete;
    GpuGeometry& operator=(const GpuGeometry&) = delete;
    ~GpuGeometry() {
        if (indices) wgpuBufferRelease(indices);
        if (vertices) wgpuBufferRelease(vertices);
    }
};

// One uploaded texture generation, shared by every model that uses the file.
struct GpuTexture {
    WgpuTextureUploadResult texture;
    WGPUTextureView view = nullptr;

    GpuTexture() = default;
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;
    ~GpuTexture() {
        if (view) wgpuTextureViewRelease(view);
    }
};

// An immutable model generation: geometry, textures and per-draw bindings.
// Hot reload builds a new generation and swaps it in; the old one is retired
// and released once the GPU no longer uses it.
struct RenderModel::Impl {
    struct Draw {
        formats::ShapeDrawRange range;
        rts::PrimitiveMaterial material;
        WGPUBindGroup binding = nullptr;
        WGPUTextureView view = nullptr; // the texture `binding` samples
        glm::mat4 world{1.0f};
        glm::vec3 center{};
        std::uint32_t image = UINT32_MAX; // the shape image it samples
        std::uint32_t slot{};             // its MaterialUniform slot (draws[] in the shaders)
    };
    // The model's bindings, one per texture, sampler filter and unlit (the
    // draws' own, a TextureOverride's, an unlit node's), made when first
    // asked for; each Draw::binding is one of them. The textures of the
    // overrides stay alive with the generation.
    mutable std::map<std::tuple<WGPUTextureView, int, bool>, WGPUBindGroup> bindings;
    mutable std::vector<std::shared_ptr<const GpuTexture>> variant_textures;
    // The draws' MaterialUniforms with the unlit light material, made for the
    // first unlit node.
    mutable WGPUBuffer unlit_materials = nullptr;

    core::VirtualPath source;
    std::vector<std::string> dependencies;
    std::uint32_t alternative_texture{};
    std::vector<std::string> texture_folders;
    bool fullbright_images = false;
    std::vector<std::string> texture_key;
    // At night a shape with the night bit (ESD_Alternative_Texture 0x100) is
    // drawn unlit, all of it (rts1.exe 0x4B042C); FullBright draws keep their
    // material when they are lit as ordinary ones (fullbright-day).
    bool night_unlit = false;
    bool fullbright_day = false;
    std::vector<formats::ShapeLight> lights;
    std::vector<ModelPrimitive> primitives;
    // Matrices, hierarchy and animations, kept when the shape is animated so
    // instances can be drawn at their own frame.
    std::shared_ptr<const formats::ShapeModel> rig;
    std::shared_ptr<const GpuGeometry> geometry;
    std::vector<std::shared_ptr<const GpuTexture>> textures; // by shape image index
    std::vector<TextureAvailability> availability;
    std::vector<Draw> draws;
    // The draws of each distance level, by index into `draws`: a frame walks
    // an instance's level only (a car has three or four levels; walking all
    // of them for some 1500 instances, three times a frame, was most of
    // prepare()).
    std::vector<std::vector<std::uint32_t>> level_draws;
    [[nodiscard]] std::span<const std::uint32_t> draws_at(const std::size_t level) const noexcept {
        if (level >= level_draws.size()) return {};
        return level_draws[level];
    }
    // The draws' MaterialUniforms by slot (a storage buffer), and how many
    // slots: a draw keeps its slot when one before it is dropped.
    WGPUBuffer materials = nullptr;
    std::uint32_t slot_count = 0;
    std::uint16_t model_flags = 0;
    ModelBounds bounds{};
    // Distance levels (ShapeModel::level_distances); empty: one level.
    std::vector<float> level_distances;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        for (auto& [_, binding] : bindings) {
            if (binding) wgpuBindGroupRelease(binding);
        }
        if (materials) wgpuBufferRelease(materials);
        if (unlit_materials) wgpuBufferRelease(unlit_materials);
    }
};

RenderModel::RenderModel() = default;
RenderModel::~RenderModel() = default;
RenderModel::RenderModel(RenderModel&&) noexcept = default;
RenderModel& RenderModel::operator=(RenderModel&&) noexcept = default;

RenderModel::operator bool() const noexcept {
    return static_cast<bool>(impl_);
}

ModelBounds RenderModel::bounds() const noexcept {
    return impl_ ? impl_->bounds : ModelBounds{};
}

const core::VirtualPath& RenderModel::source() const noexcept {
    static const core::VirtualPath none;
    return impl_ ? impl_->source : none;
}

std::vector<std::string> RenderModel::dependency_keys() const {
    return impl_ ? impl_->dependencies : std::vector<std::string>{};
}

const formats::ShapeModel* RenderModel::rig() const noexcept {
    return impl_ ? impl_->rig.get() : nullptr;
}

std::uint32_t RenderModel::alternative_texture() const noexcept { return impl_ ? impl_->alternative_texture : 0u; }

bool RenderModel::fullbright_images() const noexcept { return impl_ && impl_->fullbright_images; }

const std::vector<ModelPrimitive>& RenderModel::primitives() const noexcept {
    static const std::vector<ModelPrimitive> none;
    return impl_ ? impl_->primitives : none;
}

const std::vector<float>& RenderModel::level_distances() const noexcept {
    static const std::vector<float> none;
    return impl_ ? impl_->level_distances : none;
}

const std::vector<std::string>& RenderModel::texture_key() const noexcept {
    static const std::vector<std::string> none;
    return impl_ ? impl_->texture_key : none;
}

const std::vector<std::string>& RenderModel::texture_folders() const noexcept {
    static const std::vector<std::string> none;
    return impl_ ? impl_->texture_folders : none;
}

const std::vector<formats::ShapeLight>& RenderModel::lights() const noexcept {
    static const std::vector<formats::ShapeLight> none;
    return impl_ ? impl_->lights : none;
}

ModelAnimationInfo RenderModel::animation() const noexcept {
    if (!impl_ || !impl_->rig || impl_->rig->animations.empty()) return {};
    const auto& animation = impl_->rig->animations.front();
    return {.frame_count = animation.frame_count, .frame_rate = animation.frame_rate};
}

struct ModelRenderer::Impl {
    struct QueuedModel {
        RenderModel::Impl* model = nullptr;
        ModelTransform transform{};
        // Distance level drawn for this instance.
        std::uint8_t level = 0;
        // Its texture overrides in `frame_overrides` (first, count).
        std::uint32_t overrides = 0, override_count = 0;
        // Its unlit and hidden nodes in `frame_unlit` / `frame_hidden` (first, count).
        std::uint32_t unlit = 0, unlit_count = 0;
        std::uint32_t hidden = 0, hidden_count = 0;
        // RTS pass 3 (the cab): drawn last, in the shape's order.
        bool last_pass = false;
        // A shadow caster out of view: drawn in the shadow passes only.
        bool shadow_only = false;
        // The world object's StaticFlags and whether it is a transfer: its
        // RTS object pass (rts::object_pass, 0x4E0BFC).
        std::uint32_t static_flags = 0;
        bool transfer = false;
        std::uint32_t owner = 0; // set_owner
        // First of this instance's animated local matrices in
        // `frame_locals`, or -1 for the model's rest pose.
        std::ptrdiff_t matrices = -1;
    };

    WGPUDevice device = nullptr;
    WGPUBindGroupLayout material_layout = nullptr;
    WGPUBindGroupLayout frame_layout = nullptr;
    std::array<std::array<WGPURenderPipeline, pipeline_variant_count>, shading_path_count> pipelines{};
    std::array<std::array<WGPURenderPipeline, pipeline_variant_count>, shading_path_count> pipelines_two_sided{};
    bool face_culling = true;
    ShadingPath shading = ShadingPath::rts_compatible;
    bool batching = true;
    std::map<rts::TextureFilter, WGPUSampler> samplers;
    WgpuTextureUploadResult black_texture;
    WGPUTextureView black_view = nullptr;

    // Per-frame data: lights and camera, the drawn instances, the instance
    // index runs of the draw calls and animated local matrices.
    WGPUBuffer frame_buffer = nullptr;
    WGPUBuffer instance_buffer = nullptr;
    WGPUBuffer id_buffer = nullptr;
    WGPUBuffer local_buffer = nullptr;
    std::uint64_t instance_capacity = 0;
    std::uint64_t id_capacity = 0;
    std::uint64_t local_capacity = 0;
    WGPUBindGroup frame_binding = nullptr;
    // Multi-draw indirect: the arguments of every primitive drawn (the main
    // pass's first, then a region per depth slot), and whether the device
    // takes a first instance in them (IndirectFirstInstance) and it is on.
    WGPUBuffer indirect_buffer = nullptr;
    std::uint64_t indirect_capacity = 0;
    std::size_t depth_indirect_base = 0;
    bool multi_draw_supported = false;
    bool multi_draw = true;
    [[nodiscard]] bool use_multi_draw() const noexcept { return multi_draw && multi_draw_supported && indirect_buffer; }
    std::size_t last_commands = 0;

    // The frame's prepared draws (prepare()).
    struct Batch {
        const RenderModel::Impl::Draw* draw;
        const RenderModel::Impl* model;
        PipelineKey pipeline;
        std::uint32_t first;
        std::uint32_t count;
        bool translucent = false; // the RTS translucent pass / the experimental blended phase, or later
        WGPUBindGroup binding = nullptr;
        bool last = false;        // the cab (RTS pass 3)
    };
    struct DepthBatch {
        const RenderModel::Impl::Draw* draw = nullptr;
        const RenderModel::Impl* model = nullptr;
        WGPUBindGroup binding = nullptr;
        bool alpha_test = false;
        bool shadow_only = false; // a caster out of view: shadow passes only
        // Its instances: depth_instances[first, first + count), culled per
        // depth pass (render_depth).
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };
    // Each queued instance's bounding sphere in world space (centre, radius).
    std::vector<glm::vec4> instance_spheres;
    std::vector<std::uint32_t> instance_owners; // set_owner, by instance
    // Instance ids of the depth passes: after the main draws' ids, a region
    // of `depth_id_total` per slot.
    std::size_t depth_id_base = 0;
    std::size_t depth_id_total = 0;
    std::vector<Batch> batches;
    std::vector<DepthBatch> depth_batches;
    bool prepared = false;
    bool uploaded = false;
    // Whether depth passes are wanted this frame (set_depth_passes), and the
    // workers for the frame's per-instance data (set_task_pool).
    bool depth_passes = true;
    TaskPool* pool = nullptr;

    // Depth-only passes: a view-projection per slot (dynamic offset).
    static constexpr std::uint32_t depth_slots = 14; // cascades, the focus, 8 lamps
    static constexpr std::uint64_t depth_stride = 256;
    WGPUBindGroupLayout depth_layout = nullptr;
    WGPUBuffer depth_buffer = nullptr;
    WGPUBindGroup depth_binding = nullptr;
    // Shadow opaque, shadow cut-out; camera opaque, cut-out (culled), then two-sided.
    std::array<WGPURenderPipeline, 6> depth_pipelines{};
    [[nodiscard]] WGPURenderPipeline depth_pipeline(const bool shadow, const bool cutout) const {
        if (shadow) return depth_pipelines[cutout ? 1 : 0];
        return depth_pipelines[2 + (cutout ? 1 : 0) + (face_culling ? 0 : 2)];
    }

    // Group 2 of the experimental path: the viewer's SceneEffects, or a
    // built-in one with every effect off.
    WGPUBindGroupLayout effects_layout = nullptr;
    WGPUBindGroup effects_binding = nullptr;
    std::unique_ptr<SceneEffects> fallback_effects;
    [[nodiscard]] WGPUBindGroup effects_group() {
        if (effects_binding) return effects_binding;
        if (!fallback_effects) fallback_effects = std::make_unique<SceneEffects>(device);
        return fallback_effects->bind_group();
    }

    ModelFrameContext frame{};
    std::vector<QueuedModel> queue;
    // Generations drawn this frame stay alive until the next begin_frame().
    std::vector<std::shared_ptr<const RenderModel::Impl>> frame_models;
    std::vector<GpuLocal> frame_locals;
    // prepare()'s arrays, kept between frames for their capacity: once a
    // scene has been seen, a frame allocates nothing for them.
    struct Item {
        const RenderModel::Impl::Draw* draw;
        const RenderModel::Impl* model;
        PipelineKey pipeline;
        std::uint32_t instance;
        int phase;
        WGPUBindGroup binding;
    };
    struct Translucent {
        std::size_t instance;
        const RenderModel::Impl::Draw* draw;
        float distance2;
    };
    std::vector<Item> frame_items;
    std::vector<Translucent> frame_translucent;
    std::vector<IdEntry> frame_ids;
    std::vector<IndirectDraw> frame_indirect;
    std::vector<GpuInstance> frame_instances;
    // Batching the main draws (this thread): per item its group (or none);
    // per group its first item, its instance count, the next id it fills;
    // the groups in draw order; the items drawn in their own order.
    struct BatchScratch {
        GroupTable table;
        std::vector<std::uint32_t> item_group;
        std::vector<std::uint32_t> head;
        std::vector<std::uint32_t> count;
        std::vector<std::uint32_t> cursor;
        std::vector<std::uint32_t> order;
        std::vector<std::uint32_t> blended;
    } batch_scratch;
    // Batching the depth draws (on a worker, at the same time): each depth
    // primitive's instance and group, in order; per group the next one.
    struct DepthScratch {
        GroupTable table;
        std::vector<std::uint32_t> instance;
        std::vector<std::uint32_t> group;
        std::vector<std::uint32_t> cursor;
    } depth_scratch;
    // The depth batches' instances, a range per batch.
    std::vector<std::uint32_t> depth_instances;
    std::size_t last_primitives = 0;
    std::size_t last_draw_calls = 0;
    std::size_t last_instances = 0;

    // Uploaded textures by virtual path key; models hold the strong references.
    std::unordered_map<std::string, std::weak_ptr<const GpuTexture>> texture_cache;
    std::vector<TextureOverride> frame_overrides;
    std::vector<std::int32_t> frame_unlit;
    std::vector<std::int32_t> frame_hidden;
    // Whether a queued instance leaves this draw out (a hidden node's own primitive).
    [[nodiscard]] bool hides(const QueuedModel& queued, const RenderModel::Impl::Draw& draw) const {
        for (std::uint32_t k = 0; k < queued.hidden_count; ++k) {
            if (frame_hidden[queued.hidden + k] == draw.range.matrix_index) return true;
        }
        return false;
    }
    // A draw's MaterialUniform with the given light material.
    [[nodiscard]] static MaterialUniform material_uniform(const RenderModel::Impl::Draw& draw, const std::int32_t light_material) {
        return {
            .local = draw.world,
            .local_normal = glm::transpose(glm::inverse(draw.world)),
            .light_material = light_material,
            .named_shader = static_cast<std::int32_t>(draw.material.shader_uniform),
            .matrix_index = draw.range.matrix_index,
            .lod_bias = draw.material.lod_bias,
        };
    }

    // The binding a queued instance's draw is drawn with: its texture
    // override and/or unlit node, else the draw's own.
    [[nodiscard]] WGPUBindGroup binding_for(const QueuedModel& queued, const RenderModel::Impl::Draw& draw) {
        const GpuTexture* texture = nullptr;
        std::shared_ptr<const GpuTexture> held;
        if (draw.image != UINT32_MAX) {
            for (std::uint32_t k = 0; k < queued.override_count; ++k) {
                const auto& o = frame_overrides[queued.overrides + k];
                if (o.image != draw.image || !o.texture || !o.texture->view) continue;
                texture = o.texture.get();
                held = o.texture;
                break;
            }
        }
        bool unlit = false;
        for (std::uint32_t k = 0; k < queued.unlit_count && !unlit; ++k) {
            unlit = frame_unlit[queued.unlit + k] == draw.range.matrix_index;
        }
        if (!texture && !unlit) return draw.binding;
        const auto& model = *queued.model;
        if (unlit && !model.unlit_materials) {
            std::vector<std::byte> materials(std::size_t{model.slot_count} * material_stride);
            for (const auto& each : model.draws) {
                const auto uniform = material_uniform(each, night_texture_material);
                std::memcpy(materials.data() + std::size_t{each.slot} * material_stride, &uniform, sizeof(uniform));
            }
            model.unlit_materials = create_buffer(device, WGPUBufferUsage_Storage, materials.size(), materials.data());
            if (!model.unlit_materials) return draw.binding;
        }
        const auto binding = model_binding(model, texture ? texture->view : draw.view, draw.material.filter, unlit);
        if (held && std::find(model.variant_textures.begin(), model.variant_textures.end(), held) == model.variant_textures.end())
            model.variant_textures.push_back(held);
        return binding ? binding : draw.binding;
    }
    // A model's binding for a texture, filter and unlit: every draw of the
    // model with them shares it (made when first asked for).
    [[nodiscard]] WGPUBindGroup model_binding(const RenderModel::Impl& model, const WGPUTextureView view,
                                              const rts::TextureFilter filter, const bool unlit) {
        const auto key = std::tuple{view, static_cast<int>(filter), unlit};
        if (const auto it = model.bindings.find(key); it != model.bindings.end()) return it->second;
        const auto buffer = unlit ? model.unlit_materials : model.materials;
        if (!buffer || !view) return nullptr;
        std::array<WGPUBindGroupEntry, 3> entries{};
        entries[0].binding = 0;
        entries[0].textureView = view;
        entries[1].binding = 1;
        entries[1].sampler = sampler(filter);
        entries[2].binding = 2;
        entries[2].buffer = buffer;
        entries[2].offset = 0;
        entries[2].size = std::uint64_t{model.slot_count} * material_stride;
        const WGPUBindGroupDescriptor descriptor{.layout = material_layout, .entryCount = entries.size(), .entries = entries.data()};
        const auto binding = wgpuDeviceCreateBindGroup(device, &descriptor);
        model.bindings.emplace(key, binding);
        return binding;
    }
    GpuRetireQueue retired;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        // Retired generations may still be in flight: wait for the GPU.
        if (device) wgpuDevicePoll(device, true, nullptr);
        retired.drain();
        fallback_effects.reset();
        if (frame_binding) wgpuBindGroupRelease(frame_binding);
        if (frame_buffer) wgpuBufferRelease(frame_buffer);
        if (depth_binding) wgpuBindGroupRelease(depth_binding);
        if (depth_buffer) wgpuBufferRelease(depth_buffer);
        for (const auto pipeline : depth_pipelines) {
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
        }
        if (depth_layout) wgpuBindGroupLayoutRelease(depth_layout);
        if (effects_layout) wgpuBindGroupLayoutRelease(effects_layout);
        for (const auto buffer : {instance_buffer, id_buffer, local_buffer, indirect_buffer}) {
            if (buffer) wgpuBufferRelease(buffer);
        }
        if (black_view) wgpuTextureViewRelease(black_view);
        for (const auto& [_, sampler] : samplers) {
            if (sampler) wgpuSamplerRelease(sampler);
        }
        for (const auto* set : {&pipelines, &pipelines_two_sided}) {
            for (const auto& variants : *set) {
                for (const auto pipeline : variants) {
                    if (pipeline) wgpuRenderPipelineRelease(pipeline);
                }
            }
        }
        if (frame_layout) wgpuBindGroupLayoutRelease(frame_layout);
        if (material_layout) wgpuBindGroupLayoutRelease(material_layout);
    }

    [[nodiscard]] bool ready() const noexcept {
        return material_layout && frame_layout && frame_buffer && black_view
            && std::all_of(pipelines.begin(), pipelines.end(), [](const auto& variants) {
                   return std::all_of(variants.begin(), variants.end(), [](auto pipeline) { return pipeline; });
               });
    }

    [[nodiscard]] WGPUSampler sampler(const rts::TextureFilter filter) {
        auto& sampler = samplers[filter];
        if (!sampler) sampler = create_sampler(device, filter);
        return sampler;
    }

    // Grows the per-frame storage buffers (and rebuilds the frame binding).
    void ensure_capacity(std::uint64_t instances, std::uint64_t ids, std::uint64_t locals, std::uint64_t indirect_draws);

    [[nodiscard]] std::shared_ptr<const GpuTexture> upload(const core::VirtualPath& path, const formats::AceRgbaImage& image) const;
    // Texture of one shape image: the cached generation, or a fresh upload.
    [[nodiscard]] TextureAvailability texture_for(const StreamedTextureAsset& asset,
                                                  std::shared_ptr<const GpuTexture>& texture);
    void create_draws(RenderModel::Impl& gpu, const formats::ShapeModel& model);
    // Texture a primitive samples, or nullopt when RTS-equivalent drawing skips it.
    [[nodiscard]] std::optional<WGPUTextureView> primitive_texture(const RenderModel::Impl& gpu,
                                                                   const formats::ShapeModel& model,
                                                                   const formats::ShapePrimitiveState& state) const;
};

void ModelRenderer::Impl::ensure_capacity(const std::uint64_t instances,
                                          const std::uint64_t ids,
                                          const std::uint64_t locals,
                                          const std::uint64_t indirect_draws) {
    if (multi_draw_supported && (!indirect_buffer || indirect_draws > indirect_capacity)) {
        auto size = (std::max)(indirect_capacity, std::uint64_t{1024});
        while (size < indirect_draws) size *= 2;
        if (indirect_buffer) wgpuBufferRelease(indirect_buffer);
        indirect_buffer = create_buffer(device, WGPUBufferUsage_Indirect, size * sizeof(IndirectDraw));
        indirect_capacity = indirect_buffer ? size : 0;
    }
    bool grown = false;
    const auto grow = [&](WGPUBuffer& buffer, std::uint64_t& capacity, const std::uint64_t needed,
                          const std::uint64_t stride) {
        if (buffer && needed <= capacity) return;
        auto size = (std::max)(capacity, std::uint64_t{256});
        while (size < needed) size *= 2;
        if (buffer) wgpuBufferRelease(buffer);
        buffer = create_buffer(device, WGPUBufferUsage_Storage, size * stride);
        capacity = size;
        grown = true;
    };
    grow(instance_buffer, instance_capacity, instances, sizeof(GpuInstance));
    grow(id_buffer, id_capacity, ids, sizeof(IdEntry));
    grow(local_buffer, local_capacity, locals, sizeof(GpuLocal));
    if (!grown && frame_binding) return;

    if (frame_binding) wgpuBindGroupRelease(frame_binding);
    frame_binding = nullptr;
    if (!instance_buffer || !id_buffer || !local_buffer) return;
    std::array<WGPUBindGroupEntry, 4> entries{};
    entries[0].binding = 0;
    entries[0].buffer = frame_buffer;
    entries[0].size = sizeof(FrameUniform);
    entries[1].binding = 1;
    entries[1].buffer = instance_buffer;
    entries[1].size = instance_capacity * sizeof(GpuInstance);
    entries[2].binding = 2;
    entries[2].buffer = id_buffer;
    entries[2].size = id_capacity * sizeof(IdEntry);
    entries[3].binding = 3;
    entries[3].buffer = local_buffer;
    entries[3].size = local_capacity * sizeof(GpuLocal);
    const WGPUBindGroupDescriptor descriptor{
        .layout = frame_layout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    frame_binding = wgpuDeviceCreateBindGroup(device, &descriptor);

    if (depth_binding) wgpuBindGroupRelease(depth_binding);
    depth_binding = nullptr;
    if (!depth_layout || !depth_buffer) return;
    std::array<WGPUBindGroupEntry, 4> depth_entries = entries;
    depth_entries[0].buffer = depth_buffer;
    depth_entries[0].size = sizeof(glm::mat4);
    const WGPUBindGroupDescriptor depth_descriptor{.layout = depth_layout, .entryCount = depth_entries.size(), .entries = depth_entries.data()};
    depth_binding = wgpuDeviceCreateBindGroup(device, &depth_descriptor);
}

std::shared_ptr<const GpuTexture> ModelRenderer::Impl::upload(const core::VirtualPath& path,
                                                              const formats::AceRgbaImage& image) const {
    auto texture = std::make_shared<GpuTexture>();
    texture->texture = upload_rgba8_texture(device,
                                            {.width = image.width, .height = image.height, .pixels = &image.pixels,
                                             .mips = &image.mips},
                                            std::string(path.filename()));
    if (!texture->texture.ok()) {
        std::cout << "texture '" << path.str() << "': upload failed (" << texture->texture.error << ")\n";
        return nullptr;
    }
    texture->view = wgpuTextureCreateView(texture->texture.texture.get(), nullptr);
    return texture;
}

TextureAvailability ModelRenderer::Impl::texture_for(const StreamedTextureAsset& asset,
                                                     std::shared_ptr<const GpuTexture>& texture) {
    if (!asset.source_path) return TextureAvailability::missing;
    if (const auto it = texture_cache.find(asset.source_path->key()); it != texture_cache.end()) {
        if ((texture = it->second.lock())) return TextureAvailability::uploaded;
    }
    if (!asset.image) {
        std::cout << "texture '" << asset.source_path->str() << "': " << asset.error << '\n';
        return TextureAvailability::unsupported;
    }
    texture = upload(*asset.source_path, *asset.image);
    if (!texture) return TextureAvailability::unsupported;
    texture_cache[asset.source_path->key()] = texture;
    return TextureAvailability::uploaded;
}

std::optional<WGPUTextureView> ModelRenderer::Impl::primitive_texture(const RenderModel::Impl& gpu,
                                                                      const formats::ShapeModel& model,
                                                                      const formats::ShapePrimitiveState& state) const {
    // A primitive without textures samples texture name 0, which reads black.
    if (state.texture_count == 0 || state.texture_index >= model.textures.size()) return black_view;

    const auto image = model.textures[state.texture_index].image_index;
    if (image >= gpu.availability.size()) return black_view;
    switch (gpu.availability[image]) {
    case TextureAvailability::uploaded:
        return gpu.textures[image]->view;
    case TextureAvailability::missing: // HideFailedTextures = 0
        return black_view;
    case TextureAvailability::unsupported:
        return std::nullopt;
    }
    return std::nullopt;
}

// Creates one draw per shape draw range and computes the model's bounding sphere
// from the transformed vertices actually referenced by those draws.
void ModelRenderer::Impl::create_draws(RenderModel::Impl& gpu, const formats::ShapeModel& model) {
    gpu.model_flags = rts::model_flags(model);
    gpu.level_distances = model.level_distances;

    BoundsAccumulator bounds;
    std::vector<WGPUTextureView> views; // per kept draw
    for (const auto& range : model.draws) {
        if (range.primitive_state_index >= model.primitive_states.size()) continue;
        const auto& state = model.primitive_states[range.primitive_state_index];

        RenderModel::Impl::Draw draw{
            .range = range,
            .material = rts::classify_primitive(model, state),
            .world = world_matrix(model, range.matrix_index),
        };

        glm::vec3 local_sum{};
        std::uint32_t point_count = 0;
        const auto end = (std::min)(range.first_index + range.index_count, static_cast<std::uint32_t>(model.indices.size()));
        for (auto index = range.first_index; index < end; ++index) {
            const auto vertex = model.indices[index];
            if (vertex >= model.vertices.size()) continue;
            const auto local = to_vec3(model.vertices[vertex].position);
            bounds.add(glm::vec3{draw.world * glm::vec4{local, 1.0f}});
            local_sum += local;
            ++point_count;
        }
        if (point_count > 0) {
            draw.center = glm::vec3{draw.world * glm::vec4{local_sum / static_cast<float>(point_count), 1.0f}};
        }

        const auto view = primitive_texture(gpu, model, state);
        if (!view) continue;
        if (state.texture_count > 0 && state.texture_index < model.textures.size())
            draw.image = model.textures[state.texture_index].image_index;
        gpu.draws.push_back(draw);
        views.push_back(*view);
    }
    gpu.bounds = bounds.sphere();
    if (gpu.draws.empty()) return;

    std::vector<std::byte> materials(gpu.draws.size() * material_stride);
    for (std::size_t i = 0; i < gpu.draws.size(); ++i) {
        const auto& draw = gpu.draws[i];
        // At night a shape with the night bit is drawn unlit (RTS 0x4B042C:
        // NamedShader 3): the Rostov backdrop's lit city, lit windows.
        const auto uniform = material_uniform(draw, gpu.night_unlit && !(gpu.fullbright_day && draw.material.light_material == -8)
                                                        ? night_texture_material
                                                        : draw.material.light_material);
        std::memcpy(materials.data() + i * material_stride, &uniform, sizeof(uniform));
    }
    gpu.materials = create_buffer(device, WGPUBufferUsage_Storage, materials.size(), materials.data());
    gpu.slot_count = static_cast<std::uint32_t>(gpu.draws.size());
    if (!gpu.materials) {
        gpu.draws.clear();
        return;
    }

    // A binding per texture and filter (a draw's slot comes with each id):
    // the draws sharing them are drawn by one multi-draw call. A draw keeps
    // its slot even when one before it is dropped (the buffer is in slots).
    for (std::size_t i = 0; i < gpu.draws.size(); ++i) {
        auto& draw = gpu.draws[i];
        draw.slot = static_cast<std::uint32_t>(i);
        draw.view = views[i];
        draw.binding = model_binding(gpu, views[i], draw.material.filter, false);
    }
    std::erase_if(gpu.draws, [](const auto& draw) { return draw.binding == nullptr; });
    for (std::uint32_t i = 0; i < gpu.draws.size(); ++i) {
        const std::size_t level = gpu.draws[i].range.level;
        if (gpu.level_draws.size() <= level) gpu.level_draws.resize(level + 1);
        gpu.level_draws[level].push_back(i);
    }
}

ModelRenderer::ModelRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
    impl_->multi_draw_supported = wgpuDeviceHasFeature(device, WGPUFeatureName_IndirectFirstInstance);
    impl_->material_layout = create_material_layout(device);
    impl_->frame_layout = create_frame_layout(device);
    impl_->frame_buffer = create_buffer(device, WGPUBufferUsage_Uniform, sizeof(FrameUniform));

    static const std::vector<std::uint8_t> black_pixel{0, 0, 0, 255};
    impl_->black_texture = upload_rgba8_texture(device, {.width = 1, .height = 1, .pixels = &black_pixel}, "rts-unbound");
    if (impl_->black_texture.ok()) impl_->black_view = wgpuTextureCreateView(impl_->black_texture.texture.get(), nullptr);

    impl_->effects_layout = create_scene_effects_layout(device);
    const std::array<WGPUBindGroupLayout, 3> layouts{impl_->material_layout, impl_->frame_layout, impl_->effects_layout};
    const WGPUPipelineLayoutDescriptor rts_layout_descriptor{.bindGroupLayoutCount = 2, .bindGroupLayouts = layouts.data()};
    const WGPUPipelineLayoutDescriptor experimental_layout_descriptor{.bindGroupLayoutCount = 3, .bindGroupLayouts = layouts.data()};
    const auto rts_layout = wgpuDeviceCreatePipelineLayout(device, &rts_layout_descriptor);
    const auto experimental_layout = wgpuDeviceCreatePipelineLayout(device, &experimental_layout_descriptor);

    for (const auto path : {ShadingPath::rts_compatible, ShadingPath::experimental}) {
        const bool experimental = path == ShadingPath::experimental;
        const auto shader = create_shader_module(device, experimental ? experimental_shader_body : rts_shader_body, experimental);
        const auto pipeline_layout = experimental ? experimental_layout : rts_layout;
        for (const bool blend : {false, true}) {
            for (const bool depth_write : {false, true}) {
                for (const bool alpha_test : {false, true}) {
                    const PipelineKey key{.blend = blend, .depth_write = depth_write, .alpha_test = alpha_test};
                    impl_->pipelines[path_index(path)][key.index()] =
                        create_pipeline(device, pipeline_layout, shader, target_format, key, true);
                    impl_->pipelines_two_sided[path_index(path)][key.index()] =
                        create_pipeline(device, pipeline_layout, shader, target_format, key, false);
                }
            }
        }
        wgpuShaderModuleRelease(shader);
    }
    wgpuPipelineLayoutRelease(rts_layout);
    wgpuPipelineLayoutRelease(experimental_layout);

    // Depth only: group 1 is the pass's view-projection and the instances.
    {
        std::array<WGPUBindGroupLayoutEntry, 4> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Vertex;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.hasDynamicOffset = true;
        entries[0].buffer.minBindingSize = sizeof(glm::mat4);
        const std::array<std::uint64_t, 3> storage_sizes{sizeof(GpuInstance), sizeof(IdEntry), sizeof(GpuLocal)};
        for (std::uint32_t i = 0; i < storage_sizes.size(); ++i) {
            entries[i + 1].binding = i + 1;
            entries[i + 1].visibility = WGPUShaderStage_Vertex;
            entries[i + 1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
            entries[i + 1].buffer.minBindingSize = storage_sizes[i];
        }
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl_->depth_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
        impl_->depth_buffer = create_buffer(device, WGPUBufferUsage_Uniform, Impl::depth_slots * Impl::depth_stride);
        const std::array<WGPUBindGroupLayout, 2> depth_layouts{impl_->material_layout, impl_->depth_layout};
        const WGPUPipelineLayoutDescriptor depth_layout_descriptor{.bindGroupLayoutCount = 2, .bindGroupLayouts = depth_layouts.data()};
        const auto depth_pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &depth_layout_descriptor);
        const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(depth_shader)};
        const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
        const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
        auto& p = impl_->depth_pipelines;
        p[0] = create_depth_pipeline(device, depth_pipeline_layout, shader, true, false, false);
        p[1] = create_depth_pipeline(device, depth_pipeline_layout, shader, true, true, false);
        p[2] = create_depth_pipeline(device, depth_pipeline_layout, shader, false, false, true);
        p[3] = create_depth_pipeline(device, depth_pipeline_layout, shader, false, true, true);
        p[4] = create_depth_pipeline(device, depth_pipeline_layout, shader, false, false, false);
        p[5] = create_depth_pipeline(device, depth_pipeline_layout, shader, false, true, false);
        wgpuShaderModuleRelease(shader);
        wgpuPipelineLayoutRelease(depth_pipeline_layout);
    }
}

ModelRenderer::~ModelRenderer() = default;
ModelRenderer::ModelRenderer(ModelRenderer&&) noexcept = default;
ModelRenderer& ModelRenderer::operator=(ModelRenderer&&) noexcept = default;

bool ModelRenderer::ready() const noexcept {
    return impl_ && impl_->ready();
}

void ModelRenderer::set_shading_path(const ShadingPath path) noexcept {
    if (impl_) impl_->shading = path;
}

ShadingPath ModelRenderer::shading_path() const noexcept {
    return impl_ ? impl_->shading : ShadingPath::rts_compatible;
}

void ModelRenderer::set_face_culling(const bool enabled) noexcept {
    if (impl_) impl_->face_culling = enabled;
}

bool ModelRenderer::face_culling() const noexcept {
    return impl_ ? impl_->face_culling : true;
}

void ModelRenderer::set_multi_draw(const bool enabled) noexcept {
    if (impl_) impl_->multi_draw = enabled;
}

bool ModelRenderer::multi_draw() const noexcept {
    return impl_ && impl_->multi_draw && impl_->multi_draw_supported;
}

bool ModelRenderer::multi_draw_supported() const noexcept {
    return impl_ && impl_->multi_draw_supported;
}

void ModelRenderer::set_batching(const bool enabled) noexcept {
    if (impl_) impl_->batching = enabled;
}

bool ModelRenderer::batching() const noexcept {
    return impl_ && impl_->batching;
}

RenderModel ModelRenderer::create_model(StreamedModelAsset&& asset) {
    RenderModel result;
    if (!ready() || !asset.ok()) return result;
    const auto& model = asset.model;
    if (model.vertices.empty() || model.indices.empty()) return result;

    auto gpu = std::make_shared<RenderModel::Impl>();
    gpu->source = asset.source_path;
    gpu->alternative_texture = asset.alternative_texture;
    gpu->texture_folders = asset.texture_folders;
    gpu->fullbright_images = asset.fullbright_images;
    gpu->texture_key = asset.texture_key;
    gpu->lights = asset.lights;
    gpu->dependencies = asset.dependency_keys();
    gpu->primitives.reserve(model.primitive_states.size());
    for (std::size_t i = 0; i < model.primitive_states.size(); ++i) {
        const auto& state = model.primitive_states[i];
        ModelPrimitive p{.index = static_cast<std::uint32_t>(i), .image = formats::primitive_image(model, static_cast<std::uint32_t>(i))};
        if (state.shader_index < model.shader_names.size()) p.shader = model.shader_names[state.shader_index];
        if (state.vertex_state_index >= 0 && static_cast<std::size_t>(state.vertex_state_index) < model.vertex_states.size())
            p.light_material = model.vertex_states[static_cast<std::size_t>(state.vertex_state_index)].light_material_index;
        p.shape_light_material = i < asset.original_light.size() ? asset.original_light[i] : p.light_material;
        for (const auto& draw : model.draws)
            if (draw.primitive_state_index == i) ++p.draws;
        gpu->primitives.push_back(std::move(p));
    }
    {
        // Kept for every model: animations, and posing named nodes (bogies,
        // wheels) of rolling stock.
        auto rig = std::make_shared<formats::ShapeModel>();
        rig->matrices = model.matrices;
        rig->hierarchy = model.hierarchy;
        rig->animations = model.animations;
        gpu->rig = std::move(rig);
    }

    std::vector<GpuVertex> vertices;
    vertices.reserve(model.vertices.size());
    for (const auto& vertex : model.vertices) {
        vertices.push_back({
            .position = {vertex.position[0], vertex.position[1], vertex.position[2]},
            .normal = {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
            .uv = {vertex.uv[0], vertex.uv[1]},
            .colour1 = vertex.colour1,
            .colour2 = vertex.colour2,
        });
    }
    auto geometry = std::make_shared<GpuGeometry>();
    geometry->vertices = create_buffer(
        impl_->device, WGPUBufferUsage_Vertex, vertices.size() * sizeof(GpuVertex), vertices.data());
    geometry->indices = create_buffer(
        impl_->device, WGPUBufferUsage_Index, model.indices.size() * sizeof(std::uint32_t), model.indices.data());
    if (!geometry->vertices || !geometry->indices) return result;
    gpu->geometry = std::move(geometry);

    gpu->textures.resize(model.images.size());
    gpu->availability.resize(model.images.size(), TextureAvailability::missing);
    for (std::size_t i = 0; i < model.images.size() && i < asset.textures.size(); ++i) {
        gpu->availability[i] = impl_->texture_for(asset.textures[i], gpu->textures[i]);
    }
    const auto has = [](const std::vector<std::string>& list, const char* item) {
        return std::find(list.begin(), list.end(), item) != list.end();
    };
    gpu->night_unlit = (asset.alternative_texture & 0x100u) != 0 && has(asset.texture_folders, "NIGHT");
    gpu->fullbright_day = has(asset.texture_key, "fullbright-day");

    impl_->create_draws(*gpu, model);
    if (gpu->draws.empty()) return result;

    result.impl_ = std::move(gpu);
    return result;
}

std::shared_ptr<const void> ModelRenderer::replace_texture(const core::VirtualPath& path, const formats::AceRgbaImage& image) {
    if (!impl_) return nullptr;
    auto texture = impl_->upload(path, image);
    if (texture) impl_->texture_cache[path.key()] = texture;
    return texture;
}

void ModelRenderer::forget_texture(const core::VirtualPath& path) {
    if (impl_) impl_->texture_cache.erase(path.key());
}

void ModelRenderer::replace(RenderModel& slot, RenderModel&& next) {
    retire(std::move(slot));
    slot = std::move(next);
}

void ModelRenderer::retire(RenderModel&& model) {
    if (impl_ && model.impl_) impl_->retired.retire(std::move(model.impl_));
    model.impl_.reset();
}

namespace {

void on_frame_done(const WGPUQueueWorkDoneStatus status, void* queue, void* frame) {
    if (status == WGPUQueueWorkDoneStatus_Success || status == WGPUQueueWorkDoneStatus_InstanceDropped) {
        static_cast<GpuRetireQueue*>(queue)->complete(reinterpret_cast<std::uintptr_t>(frame));
    }
}

} // namespace

void ModelRenderer::frame_submitted(const WGPUQueue queue) {
    if (!impl_ || !queue) return;
    const auto frame = impl_->retired.submit();
    wgpuQueueOnSubmittedWorkDone(queue, {
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = on_frame_done,
        .userdata1 = &impl_->retired,
        .userdata2 = reinterpret_cast<void*>(static_cast<std::uintptr_t>(frame)),
    });
}

std::size_t ModelRenderer::collect_retired() {
    if (!impl_) return 0;
    wgpuDevicePoll(impl_->device, false, nullptr);
    return impl_->retired.collect();
}

std::string ModelRenderer::draw_report(const std::size_t top) const {
    if (!impl_) return {};
    struct Row {
        const RenderModel::Impl* model = nullptr;
        std::size_t calls = 0, instances = 0, singles = 0, translucent = 0;
    };
    std::unordered_map<const RenderModel::Impl*, Row> rows;
    std::size_t singles = 0;
    for (const auto& batch : impl_->batches) {
        auto& row = rows[batch.model];
        row.model = batch.model;
        ++row.calls;
        row.instances += batch.count;
        if (batch.count == 1) {
            ++row.singles;
            ++singles;
        }
        if (batch.translucent) ++row.translucent;
    }
    std::vector<Row> sorted;
    sorted.reserve(rows.size());
    for (const auto& [_, row] : rows) sorted.push_back(row);
    std::sort(sorted.begin(), sorted.end(), [](const Row& a, const Row& b) { return a.calls > b.calls; });
    std::ostringstream out;
    out << "draw calls: " << impl_->batches.size() << " from " << rows.size() << " models, " << singles
        << " of one instance; " << impl_->last_commands << " commands (multi-draw " << (impl_->use_multi_draw() ? "on" : "off")
        << ")\n";
    // What changes between calls as they are recorded (render()): the
    // pipeline, the model (vertex and index buffers), the draw's binding
    // (texture and the draw's uniform); and how often the texture alone
    // would change if a model's draws shared a binding per texture.
    {
        std::size_t pipelines = 0, models = 0, bindings = 0, textures = 0, translucent = 0;
        std::size_t last_pipeline = SIZE_MAX;
        const void* last_model = nullptr;
        const void* last_binding = nullptr;
        std::pair<const void*, const void*> last_texture{nullptr, nullptr};
        for (const auto& batch : impl_->batches) {
            if (batch.pipeline.index() != last_pipeline) ++pipelines;
            if (batch.model != last_model) ++models;
            if (batch.binding != last_binding) ++bindings;
            const std::pair<const void*, const void*> texture{batch.model, batch.draw ? static_cast<const void*>(batch.draw->view) : nullptr};
            if (texture != last_texture || batch.pipeline.index() != last_pipeline) ++textures;
            last_pipeline = batch.pipeline.index();
            last_model = batch.model;
            last_binding = batch.binding;
            last_texture = texture;
            if (batch.translucent) ++translucent;
        }
        out << "  switches: " << pipelines << " pipelines, " << models << " models, " << bindings << " bindings; "
            << textures << " if a binding were per model and texture; " << translucent << " translucent calls; "
            << impl_->frame_locals.size() << " node matrices of animated instances ("
            << impl_->frame_locals.size() * sizeof(impl_->frame_locals[0]) / 1024 << " KB a frame)\n";
    }
    // At the nearest level: the shape's parts, and how few draws one per
    // material (primitive state) would leave, the most merging could do.
    std::size_t all_parts = 0, all_states = 0;
    for (const auto& [model, _] : rows) {
        if (!model) continue;
        std::set<std::uint32_t> states;
        for (const auto& draw : model->draws) {
            if (draw.range.level != 0) continue;
            ++all_parts;
            states.insert(draw.range.primitive_state_index);
        }
        all_states += states.size();
    }
    out << "  nearest level: " << all_parts << " parts in these models, " << all_states << " materials\n";
    for (std::size_t i = 0; i < sorted.size() && i < top; ++i) {
        const auto& row = sorted[i];
        std::size_t parts = 0;
        std::set<std::uint32_t> states;
        if (row.model) {
            for (const auto& draw : row.model->draws) {
                if (draw.range.level != 0) continue;
                ++parts;
                states.insert(draw.range.primitive_state_index);
            }
        }
        out << "  " << row.calls << " calls, " << row.instances << " instances, " << row.singles << " single, " << row.translucent
            << " translucent; nearest level " << parts << " parts, " << states.size()
            << " materials: " << (row.model ? row.model->source.str() : std::string("?")) << '\n';
    }
    return out.str();
}

ModelRendererStats ModelRenderer::stats() const {
    if (!impl_) return {};
    std::size_t live = 0;
    for (const auto& [_, texture] : impl_->texture_cache) live += texture.expired() ? 0 : 1;
    return {
        .cached_textures = live,
        .retired_pending = impl_->retired.pending(),
        .completed_frame = impl_->retired.completed_frame(),
        .instances = impl_->last_instances,
        .primitives = impl_->last_primitives,
        .draw_calls = impl_->last_draw_calls,
        .draw_commands = impl_->last_commands,
    };
}

void ModelRenderer::begin_frame(const ModelFrameContext& frame) {
    if (!impl_) return;
    impl_->frame = frame;
    impl_->prepared = false;
    impl_->uploaded = false;
    impl_->queue.clear();
    impl_->frame_overrides.clear();
    impl_->frame_unlit.clear();
    impl_->frame_hidden.clear();
    impl_->frame_models.clear();
    impl_->frame_locals.clear();
}

void ModelRenderer::submit(RenderModel& model, const ModelTransform& transform) {
    if (!impl_ || !model.impl_) return;
    if (impl_->frame_models.empty() || impl_->frame_models.back() != model.impl_) impl_->frame_models.push_back(model.impl_);
    // Distance level (MSTS rule, PROBABLE for RTS): the first level whose
    // dlevel_selection reaches the camera distance to the model's centre;
    // beyond all of them the last level.
    std::uint8_t level = 0;
    const auto& distances = model.impl_->level_distances;
    if (distances.size() > 1) {
        const auto centre = glm::vec3{transform * glm::vec4{model.impl_->bounds.center, 1.0f}};
        const float distance = glm::distance(centre, impl_->frame.camera_position);
        level = static_cast<std::uint8_t>(distances.size() - 1);
        for (std::size_t i = 0; i < distances.size(); ++i) {
            if (distance <= distances[i]) {
                level = static_cast<std::uint8_t>(i);
                break;
            }
        }
    }
    impl_->queue.push_back({.model = model.impl_.get(), .transform = transform, .level = level});
}

namespace {

// The world matrix of every node (its local times its parents', up to the
// root) and its normal matrix, appended to `out` in node order. Each node
// takes its parent's world once it is known: an animated instance's palette
// (a car: some 20 nodes, 500 cars a frame at a big station) was walked up to
// the root for every node, and its normal matrix was a full 4 x 4 inverse;
// the shaders take only the normal matrix's xyz rows (as the instance's).
void append_locals(std::vector<GpuLocal>& out, const std::vector<glm::mat4>& local, const std::vector<std::int32_t>& hierarchy) {
    const auto count = local.size();
    const auto first = out.size();
    out.resize(first + count);
    std::vector<std::uint8_t> known(count, 0);
    std::vector<std::size_t> chain;
    const auto parent = [&](const std::size_t index) -> std::int64_t {
        return index < hierarchy.size() ? hierarchy[index] : -1;
    };
    for (std::size_t start = 0; start < count; ++start) {
        if (known[start]) continue;
        // Up to a node already known, or the root (a loop stops at count).
        chain.clear();
        std::int64_t current = static_cast<std::int64_t>(start);
        while (current >= 0 && static_cast<std::size_t>(current) < count && !known[static_cast<std::size_t>(current)]
               && chain.size() <= count) {
            chain.push_back(static_cast<std::size_t>(current));
            current = parent(static_cast<std::size_t>(current));
        }
        glm::mat4 world = current >= 0 && static_cast<std::size_t>(current) < count && known[static_cast<std::size_t>(current)]
                              ? out[first + static_cast<std::size_t>(current)].matrix
                              : glm::mat4{1.0f};
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            world = world * local[*it];
            out[first + *it] = {.matrix = world, .normal = glm::mat4{glm::inverseTranspose(glm::mat3{world})}};
            known[*it] = 1;
        }
    }
}

} // namespace

void ModelRenderer::submit(RenderModel& model, const ModelTransform& transform, const float frame) {
    if (!impl_ || !model.impl_) return;
    const auto& rig = model.impl_->rig;
    if (!rig || rig->animations.empty()) {
        submit(model, transform);
        return;
    }
    // World matrices of every node at this frame, through the hierarchy.
    const auto animated = formats::animate_matrices(*rig, 0, frame);
    std::vector<glm::mat4> local(animated.size());
    for (std::size_t index = 0; index < animated.size(); ++index) local[index] = shape_matrix(animated[index]);
    const auto first = static_cast<std::ptrdiff_t>(impl_->frame_locals.size());
    append_locals(impl_->frame_locals, local, rig->hierarchy);
    submit(model, transform);
    impl_->queue.back().matrices = first;
}

void ModelRenderer::submit(RenderModel& model, const ModelTransform& transform, const std::span<const NodeAdjustment> adjustments,
                           const std::span<const TextureOverride> textures, const ObjectPass object_pass) {
    if (!impl_ || !model.impl_) return;
    const auto before = impl_->queue.size();
    submit(model, transform, adjustments);
    if (impl_->queue.size() == before) return;
    auto& queued = impl_->queue.back();
    queued.last_pass = object_pass == ObjectPass::last;
    if (textures.empty()) return;
    queued.overrides = static_cast<std::uint32_t>(impl_->frame_overrides.size());
    queued.override_count = static_cast<std::uint32_t>(textures.size());
    impl_->frame_overrides.insert(impl_->frame_overrides.end(), textures.begin(), textures.end());
}

std::shared_ptr<const GpuTexture> ModelRenderer::texture(const core::Vfs& vfs, const core::VirtualPath& path) {
    if (!impl_) return nullptr;
    if (const auto it = impl_->texture_cache.find(path.key()); it != impl_->texture_cache.end()) {
        if (auto texture = it->second.lock()) return texture;
    }
    const auto decoded = formats::decode_ace_rgba(vfs, path);
    if (!decoded.ok()) return nullptr;
    auto texture = impl_->upload(path, *decoded.image);
    if (texture) impl_->texture_cache[path.key()] = texture;
    return texture;
}

void ModelRenderer::submit(RenderModel& model, const ModelTransform& transform,
                           const std::span<const NodeAdjustment> adjustments) {
    if (!impl_ || !model.impl_) return;
    // Unlit nodes (RTS node flag 0x40) apply whatever the pose.
    const auto unlit_first = impl_->frame_unlit.size();
    const auto hidden_first = impl_->frame_hidden.size();
    for (const auto& adjustment : adjustments) {
        if (adjustment.matrix < 0) continue;
        if (adjustment.unlit) impl_->frame_unlit.push_back(adjustment.matrix);
        if (adjustment.hidden) impl_->frame_hidden.push_back(adjustment.matrix);
    }
    const auto before = impl_->queue.size();
    submit_posed(model, transform, adjustments);
    if (impl_->queue.size() != before) {
        auto& queued = impl_->queue.back();
        queued.unlit = static_cast<std::uint32_t>(unlit_first);
        queued.unlit_count = static_cast<std::uint32_t>(impl_->frame_unlit.size() - unlit_first);
        queued.hidden = static_cast<std::uint32_t>(hidden_first);
        queued.hidden_count = static_cast<std::uint32_t>(impl_->frame_hidden.size() - hidden_first);
    }
}

void ModelRenderer::submit_posed(RenderModel& model, const ModelTransform& transform,
                                 const std::span<const NodeAdjustment> adjustments) {
    const auto& rig = model.impl_->rig;
    if (!rig || adjustments.empty()) {
        submit(model, transform);
        return;
    }
    const auto count = rig->matrices.size();
    std::vector<glm::mat4> local(count);
    for (std::size_t index = 0; index < count; ++index) local[index] = shape_matrix(rig->matrices[index]);
    for (const auto& adjustment : adjustments) {
        if (adjustment.matrix < 0 || static_cast<std::size_t>(adjustment.matrix) >= count) continue;
        const auto m = static_cast<std::size_t>(adjustment.matrix);
        // A node posed at its own animation frame (pantographs, cab switches).
        if (adjustment.frame >= 0.0f && !rig->animations.empty()) {
            local[m] = shape_matrix(formats::animate_matrix(*rig, 0, m, adjustment.frame));
        }
        local[m] *= adjustment.local;
    }
    const auto first = static_cast<std::ptrdiff_t>(impl_->frame_locals.size());
    append_locals(impl_->frame_locals, local, rig->hierarchy);
    submit(model, transform);
    impl_->queue.back().matrices = first;
}

void ModelRenderer::submit(RenderModel& model) {
    submit(model, ModelTransform{1.0f});
}

void ModelRenderer::set_world_object(const std::uint32_t static_flags, const bool transfer) {
    if (!impl_ || impl_->queue.empty()) return;
    impl_->queue.back().static_flags = static_flags;
    impl_->queue.back().transfer = transfer;
}

void ModelRenderer::set_owner(const std::uint32_t owner) {
    if (!impl_ || impl_->queue.empty()) return;
    impl_->queue.back().owner = owner;
}

// Builds the frame's primitive list for the active shading path, merges
// identical primitives of different instances into instanced draw calls,
// uploads the frame data at once and records the draws.
//
// Merging: within a pass (RTS: opaque, translucent; experimental: opaque,
// masked, blended), primitives that write depth without blending are grouped
// by primitive and state and drawn first, sorted by state and model to save
// state changes; the depth test makes their order irrelevant. The others
// (blended, or not writing depth) follow in their original order, and only
// consecutive identical ones are merged, so their result is unchanged.
void ModelRenderer::prepare() {
    if (!impl_ || impl_->prepared) return;
    OPENRTS_ZONE("models: prepare");
    impl_->prepared = true;
    impl_->uploaded = false;
    impl_->batches.clear();
    impl_->depth_batches.clear();
    impl_->depth_id_total = 0;
    if (!ready()) return;

    // Depth-only draws (SceneEffects): every depth-writing primitive that is
    // not blended, the cab's too, instanced by primitive, binding and alpha
    // test; shadow casters out of view apart, for the shadow passes only.
    // Built on a worker while the main draws are batched here: it only reads
    // the queue and takes each draw's own binding (binding_for may make new
    // ones, on this thread only), so a texture override does not reach a
    // cut-out's shadow.
    const bool build_depth_batches = impl_->shading == ShadingPath::experimental && impl_->depth_passes;
    const auto build_depth = [this] {
        OPENRTS_ZONE("models: depth draws");
        auto& scratch = impl_->depth_scratch;
        auto& batches = impl_->depth_batches;
        scratch.instance.clear();
        scratch.group.clear();
        scratch.table.reset(batches.capacity());
        for (std::size_t instance = 0; instance < impl_->queue.size(); ++instance) {
            const auto& queued = impl_->queue[instance];
            for (const auto index : queued.model->draws_at(queued.level)) {
                const auto& draw = queued.model->draws[index];
                if (impl_->hides(queued, draw)) continue;
                const auto& material = draw.material;
                const bool blend_shader = material.shader == rts::NamedShader::blend_a_tex || material.shader == rts::NamedShader::blend_a_tex_diff;
                if ((blend_shader && !material.alpha_test_mode) || !material.depth_write) continue;
                const bool alpha_test = blend_shader && material.alpha_test_mode;
                const auto binding = draw.binding;
                // By primitive, binding, alpha test and whether only the shadows take it.
                const auto fresh = static_cast<std::uint32_t>(batches.size());
                const auto group = scratch.table.find_or_add(&draw, binding, (alpha_test ? 1u : 0u) | (queued.shadow_only ? 2u : 0u), fresh);
                if (group == fresh) {
                    Impl::DepthBatch head;
                    head.draw = &draw;
                    head.model = queued.model;
                    head.binding = binding;
                    head.alpha_test = alpha_test;
                    head.shadow_only = queued.shadow_only;
                    batches.push_back(head);
                }
                ++batches[group].count;
                scratch.instance.push_back(static_cast<std::uint32_t>(instance));
                scratch.group.push_back(group);
            }
        }
        // Each batch's instances together, in the order they came.
        std::uint32_t next = 0;
        scratch.cursor.resize(batches.size());
        for (std::size_t g = 0; g < batches.size(); ++g) {
            batches[g].first = next;
            scratch.cursor[g] = next;
            next += batches[g].count;
        }
        impl_->depth_instances.resize(next);
        for (std::size_t e = 0; e < scratch.instance.size(); ++e) {
            impl_->depth_instances[scratch.cursor[scratch.group[e]]++] = scratch.instance[e];
        }
        impl_->depth_id_total = next;
    };
    bool depth_on_worker = false;
    if (build_depth_batches) {
        if (impl_->pool) {
            impl_->pool->start({build_depth});
            depth_on_worker = true;
        } else {
            build_depth();
        }
    }

    core::SequentialZone part;
    part.open("models: primitives");
    using Item = Impl::Item;
    auto& items = impl_->frame_items;
    items.clear();
    const auto emit = [&](const std::size_t instance, const RenderModel::Impl::Draw& draw, const PipelineKey pipeline,
                          const int phase) {
        const auto& queued = impl_->queue[instance];
        items.push_back({&draw, queued.model, pipeline, static_cast<std::uint32_t>(instance), phase,
                         impl_->binding_for(queued, draw)});
    };

    if (impl_->shading == ShadingPath::rts_compatible) {
        // RTS object passes -2..3 (0x408574): transfers (-2), Terrain-flagged
        // objects (-1), Global ones' depthless primitives (0), every model in
        // pass 1 and, with translucent primitives, again in 2; the cab in pass
        // 3 after them. Each primitive sets its own blend and alpha test
        // (0x4B042C); nothing is sorted.
        for (const int object_pass_number : {rts::transfer_pass, rts::terrain_pass, rts::global_pass, rts::opaque_pass,
                                             rts::translucent_pass, rts::last_pass}) {
            for (std::size_t instance = 0; instance < impl_->queue.size(); ++instance) {
                const auto& queued = impl_->queue[instance];
                if (queued.shadow_only || queued.last_pass != (object_pass_number == rts::last_pass)) continue;
                const auto& model = *queued.model;
                const auto object_pass = queued.transfer ? rts::transfer_pass : rts::object_pass(model.model_flags, queued.static_flags);
                for (const auto index : model.draws_at(queued.level)) {
                    const auto& draw = model.draws[index];
                    if (impl_->hides(queued, draw)) continue;
                    if (object_pass_number != rts::last_pass
                        && !rts::draws_in_pass(object_pass_number, object_pass, model.model_flags, draw.material.flags,
                                               queued.static_flags)) {
                        continue;
                    }
                    const auto state = rts::apply_material(draw.material);
                    emit(instance, draw,
                         {.blend = state.blend, .depth_write = draw.material.depth_write, .alpha_test = state.alpha_test},
                         object_pass_number);
                }
            }
        }
    } else {
        // Experimental: explicit per-material state, every primitive drawn,
        // opaque and alpha-tested first, blended geometry back to front.
        auto& translucent = impl_->frame_translucent;
        translucent.clear();
        for (const bool masked_pass : {false, true}) {
            for (std::size_t instance = 0; instance < impl_->queue.size(); ++instance) {
                const auto& submitted = impl_->queue[instance];
                if (submitted.last_pass || submitted.shadow_only) continue;
                for (const auto index : submitted.model->draws_at(submitted.level)) {
                    const auto& draw = submitted.model->draws[index];
                    if (impl_->hides(submitted, draw)) continue;
                    const auto& material = draw.material;
                    const bool blend_shader = material.shader == rts::NamedShader::blend_a_tex
                        || material.shader == rts::NamedShader::blend_a_tex_diff;
                    const bool blended = blend_shader && !material.alpha_test_mode;
                    if (blended) {
                        if (masked_pass) continue;
                        const auto center = glm::vec3{submitted.transform * glm::vec4{draw.center, 1.0f}};
                        const auto delta = center - impl_->frame.camera_position;
                        translucent.push_back({instance, &draw, glm::dot(delta, delta)});
                        continue;
                    }
                    const bool masked = blend_shader && material.alpha_test_mode;
                    if (masked != masked_pass) continue;
                    emit(instance, draw, {.blend = false, .depth_write = material.depth_write, .alpha_test = masked},
                         masked ? 1 : 0);
                }
            }
        }
        std::stable_sort(translucent.begin(), translucent.end(),
                         [](const auto& a, const auto& b) { return a.distance2 > b.distance2; });
        for (const auto& item : translucent) {
            // Depth is written as the material says, as RTS does in its
            // translucent pass: MSTS content counts on it, and without it
            // anything drawn later (track lines, a misordered pane) shows
            // through the glass.
            emit(item.instance, *item.draw, {.blend = true, .depth_write = item.draw->material.depth_write, .alpha_test = false}, 2);
        }
        // The cab last, in its shape's order (RTS pass 3).
        for (std::size_t instance = 0; instance < impl_->queue.size(); ++instance) {
            const auto& submitted = impl_->queue[instance];
            if (!submitted.last_pass || submitted.shadow_only) continue;
            for (const auto index : submitted.model->draws_at(submitted.level)) {
                const auto& draw = submitted.model->draws[index];
                if (impl_->hides(submitted, draw)) continue;
                const auto& material = draw.material;
                const bool blend_shader = material.shader == rts::NamedShader::blend_a_tex
                    || material.shader == rts::NamedShader::blend_a_tex_diff;
                emit(instance, draw,
                     {.blend = blend_shader && !material.alpha_test_mode, .depth_write = material.depth_write,
                      .alpha_test = blend_shader && material.alpha_test_mode},
                     rts::last_pass);
            }
        }
    }
    impl_->last_instances = impl_->queue.size();
    impl_->last_primitives = items.size();
    impl_->last_draw_calls = 0;

    part.open("models: batches");
    // Merge into batches: runs of instance ids per (primitive, pipeline).
    const int translucent_phase = impl_->shading == ShadingPath::rts_compatible ? rts::translucent_pass : 2;
    auto& batches = impl_->batches;
    auto& ids = impl_->frame_ids;
    ids.clear();
    ids.reserve(items.size());
    // Phase by phase, in flat arrays kept between frames: the groupable items
    // get a group by primitive, binding and pipeline (a draw with an
    // overridden texture is a group of its own); the groups go by pipeline
    // and model, else as they came (a stable sort's order); each group's ids
    // follow the items' order. Then the others, in their own order.
    auto& scratch = impl_->batch_scratch;
    scratch.item_group.resize(items.size());
    constexpr std::uint32_t ungrouped = UINT32_MAX;
    const auto batch_of = [&](const Item& item, const std::uint32_t first, const std::uint32_t count) {
        return Impl::Batch{item.draw, item.model, item.pipeline, first, count, item.phase >= translucent_phase, item.binding,
                           item.phase == rts::last_pass};
    };
    for (std::size_t from = 0; from < items.size();) {
        const int phase = items[from].phase;
        std::size_t to = from + 1;
        while (to < items.size() && items[to].phase == phase) ++to;
        scratch.table.reset(scratch.head.capacity());
        scratch.head.clear();
        scratch.count.clear();
        scratch.blended.clear();
        for (std::size_t k = from; k < to; ++k) {
            const auto& item = items[k];
            // Pass 3 keeps the shape's order: opaque and blended primitives interleave.
            if (item.pipeline.blend || !item.pipeline.depth_write || !impl_->batching || item.phase == rts::last_pass) {
                scratch.item_group[k] = ungrouped;
                scratch.blended.push_back(static_cast<std::uint32_t>(k));
                continue;
            }
            const auto fresh = static_cast<std::uint32_t>(scratch.head.size());
            const auto group = scratch.table.find_or_add(item.draw, item.binding, static_cast<std::uint32_t>(item.pipeline.index()), fresh);
            if (group == fresh) {
                scratch.head.push_back(static_cast<std::uint32_t>(k));
                scratch.count.push_back(0);
            }
            ++scratch.count[group];
            scratch.item_group[k] = group;
        }
        scratch.order.resize(scratch.head.size());
        std::iota(scratch.order.begin(), scratch.order.end(), std::uint32_t{0});
        std::sort(scratch.order.begin(), scratch.order.end(), [&](const std::uint32_t a, const std::uint32_t b) {
            const auto& x = items[scratch.head[a]];
            const auto& y = items[scratch.head[b]];
            const auto px = x.pipeline.index();
            const auto py = y.pipeline.index();
            if (px != py) return px < py;
            if (x.model != y.model) return std::less<const void*>{}(x.model, y.model);
            return a < b;
        });
        scratch.cursor.resize(scratch.head.size());
        auto next = static_cast<std::uint32_t>(ids.size());
        for (const auto group : scratch.order) {
            scratch.cursor[group] = next;
            batches.push_back(batch_of(items[scratch.head[group]], next, scratch.count[group]));
            next += scratch.count[group];
        }
        ids.resize(next);
        for (std::size_t k = from; k < to; ++k) {
            const auto group = scratch.item_group[k];
            if (group != ungrouped) ids[scratch.cursor[group]++] = {items[k].instance, items[k].draw->slot};
        }
        for (const auto k : scratch.blended) {
            const auto& item = items[k];
            auto* last = batches.empty() ? nullptr : &batches.back();
            const bool same = impl_->batching && last && last->draw == item.draw && last->binding == item.binding &&
                last->pipeline.index() == item.pipeline.index()
                && last->first + last->count == ids.size() && last->translucent == (item.phase >= translucent_phase)
                && last->last == (item.phase == rts::last_pass);
            if (same) {
                ++last->count;
            } else {
                batches.push_back(batch_of(item, static_cast<std::uint32_t>(ids.size()), 1));
            }
            ids.push_back({item.instance, item.draw->slot});
        }
        from = to;
    }
    impl_->last_draw_calls = batches.size();
    // The main pass's indirect arguments: one per primitive drawn, in order;
    // the depth slots' regions follow (render_depth).
    auto& indirect = impl_->frame_indirect;
    indirect.clear();
    if (impl_->multi_draw_supported) {
        indirect.reserve(batches.size());
        for (const auto& batch : batches) {
            indirect.push_back({batch.draw->range.index_count, batch.count, batch.draw->range.first_index, 0, batch.first});
        }
    }


    // Depth-only draws for SceneEffects (the experimental path): every
    // depth-writing primitive that is not blended, the cab's too, instanced
    // by primitive, binding and alpha test; shadow casters out of view
    // (submit_shadow_caster) apart, for the shadow passes only.
    part.open("models: waiting for the depth draws");
    impl_->depth_id_base = ids.size();
    if (depth_on_worker) impl_->pool->wait();
    part.open("models: instances");

    const auto count = impl_->queue.size();
    auto& instances = impl_->frame_instances;
    instances.resize(count);
    impl_->instance_spheres.resize(count);
    impl_->instance_owners.resize(count);
    const auto& rings = impl_->frame.lighting.light_rings;
    const auto fill = [&](const std::size_t from, const std::size_t to) {
        for (std::size_t k = from; k < to; ++k) {
            const auto& submitted = impl_->queue[k];
            // Its ring of further lights by its bounding sphere (RTS 0x4B042C).
            const auto& bounds = submitted.model->bounds;
            const auto centre = glm::vec3{submitted.transform * glm::vec4{bounds.center, 1.0f}};
            const float scale = (std::max)({glm::length(glm::vec3{submitted.transform[0]}), glm::length(glm::vec3{submitted.transform[1]}),
                                            glm::length(glm::vec3{submitted.transform[2]})});
            // transpose(inverse()) of the 3 x 3 part: the shaders take only
            // the normal matrix's xyz rows (with w = 1), where it is the same.
            instances[k] = {
                .transform = submitted.transform,
                .normal = glm::mat4{glm::inverseTranspose(glm::mat3{submitted.transform})},
                .locals = static_cast<std::int32_t>(submitted.matrices),
                .ring = static_cast<std::int32_t>(rings.ring_for(centre, bounds.radius * scale)),
                .decal = submitted.transfer ? 1 : 0,
            };
            impl_->instance_spheres[k] = glm::vec4{centre, bounds.radius * scale};
            impl_->instance_owners[k] = submitted.owner;
        }
    };
    constexpr std::size_t chunk = 1024;
    if (impl_->pool && count > chunk) {
        std::vector<std::function<void()>> tasks;
        for (std::size_t from = 0; from < count; from += chunk) tasks.emplace_back([&fill, from, count] { fill(from, (std::min)(from + chunk, count)); });
        impl_->pool->start(std::move(tasks));
        impl_->pool->wait();
    } else {
        fill(0, count);
    }
    part.open("models: uploads");
    impl_->depth_indirect_base = indirect.size();
    impl_->ensure_capacity(instances.size(), (std::max)(ids.size() + Impl::depth_slots * impl_->depth_id_total, std::size_t{1}),
                           (std::max)(impl_->frame_locals.size(), std::size_t{1}),
                           indirect.size() + Impl::depth_slots * impl_->depth_batches.size());
    if (!impl_->frame_binding) return;
    impl_->uploaded = true;

    const auto lighting = frame_uniform(impl_->frame.lighting, impl_->frame.view_projection, impl_->frame.camera_position);
    const auto queue = wgpuDeviceGetQueue(impl_->device);
    wgpuQueueWriteBuffer(queue, impl_->frame_buffer, 0, &lighting, sizeof(lighting));
    if (!instances.empty()) wgpuQueueWriteBuffer(queue, impl_->instance_buffer, 0, instances.data(), instances.size() * sizeof(GpuInstance));
    if (!ids.empty()) wgpuQueueWriteBuffer(queue, impl_->id_buffer, 0, ids.data(), ids.size() * sizeof(IdEntry));
    if (!indirect.empty() && impl_->indirect_buffer)
        wgpuQueueWriteBuffer(queue, impl_->indirect_buffer, 0, indirect.data(), indirect.size() * sizeof(IndirectDraw));
    if (!impl_->frame_locals.empty()) {
        wgpuQueueWriteBuffer(queue, impl_->local_buffer, 0, impl_->frame_locals.data(),
                             impl_->frame_locals.size() * sizeof(GpuLocal));
    }
    if (queue) wgpuQueueRelease(queue);
    part.close(nullptr);
}

void ModelRenderer::render(const WGPURenderPassEncoder pass, const std::function<void()>& before_translucent,
                           const std::function<void()>& before_last) {
    // The helpers' calls happen exactly once each and in order, whatever returns early.
    bool called = false;
    bool called_last = false;
    const auto call = [&] {
        if (!called && before_translucent) before_translucent();
        called = true;
    };
    const auto call_last = [&] {
        call();
        if (!called_last && before_last) before_last();
        called_last = true;
    };
    struct CallAtExit {
        const decltype(call_last)& f;
        ~CallAtExit() { f(); }
    } at_exit{call_last};
    if (!ready()) return;
    prepare();
    OPENRTS_ZONE("models: main pass");
    if (!impl_->uploaded || impl_->batches.empty()) return;

    const bool experimental = impl_->shading == ShadingPath::experimental;
    const auto effects = experimental ? impl_->effects_group() : nullptr;
    if (experimental && !effects) return;
    const auto bind_frame = [&] {
        wgpuRenderPassEncoderSetBindGroup(pass, 1, impl_->frame_binding, 0, nullptr);
        if (experimental) wgpuRenderPassEncoderSetBindGroup(pass, 2, effects, 0, nullptr);
    };
    bind_frame();
    std::optional<std::size_t> bound_pipeline;
    const RenderModel::Impl* bound_model = nullptr;
    WGPUBindGroup bound_draw = nullptr;
    // The primitives waiting to be drawn with the bound state: one multi-draw
    // call for several (the order kept: a decal after its body), a plain call
    // for one.
    const bool multi = impl_->use_multi_draw();
    std::size_t run_first = 0;
    std::size_t run_count = 0;
    std::size_t commands = 0;
    const auto& batches = impl_->batches;
    const auto flush = [&] {
        if (run_count == 0) return;
        if (multi && run_count > 1) {
            wgpuRenderPassEncoderMultiDrawIndexedIndirect(pass, impl_->indirect_buffer, run_first * sizeof(IndirectDraw),
                                                          static_cast<std::uint32_t>(run_count));
            ++commands;
        } else {
            for (std::size_t k = run_first; k < run_first + run_count; ++k) {
                const auto& b = batches[k];
                wgpuRenderPassEncoderDrawIndexed(pass, b.draw->range.index_count, b.count, b.draw->range.first_index, 0, b.first);
                ++commands;
            }
        }
        run_count = 0;
    };
    for (std::size_t i = 0; i < batches.size(); ++i) {
        const auto& batch = batches[i];
        if ((batch.translucent && !called) || (batch.last && !called_last)) {
            flush();
            if (batch.last) {
                call_last();
            } else {
                call();
            }
            // The helpers set their own pipelines and bindings.
            bound_pipeline.reset();
            bound_model = nullptr;
            bound_draw = nullptr;
            bind_frame();
        }
        if (bound_pipeline != batch.pipeline.index()) {
            flush();
            bound_pipeline = batch.pipeline.index();
            const auto& set = impl_->face_culling ? impl_->pipelines : impl_->pipelines_two_sided;
            wgpuRenderPassEncoderSetPipeline(pass, set[path_index(impl_->shading)][*bound_pipeline]);
        }
        if (bound_model != batch.model) {
            flush();
            bound_model = batch.model;
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, bound_model->geometry->vertices, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetIndexBuffer(pass, bound_model->geometry->indices, WGPUIndexFormat_Uint32, 0,
                                                WGPU_WHOLE_SIZE);
        }
        if (bound_draw != batch.binding) {
            flush();
            bound_draw = batch.binding;
            wgpuRenderPassEncoderSetBindGroup(pass, 0, batch.binding, 0, nullptr);
        }
        if (run_count == 0) run_first = i;
        ++run_count;
    }
    flush();
    impl_->last_commands = commands;
}

void ModelRenderer::render_depth(const WGPURenderPassEncoder pass, const std::uint32_t slot, const glm::mat4& view_projection,
                                 const DepthPassKind kind, const std::uint32_t skip_owner) {
    if (!ready() || slot >= Impl::depth_slots) return;
    prepare();
    OPENRTS_ZONE("models: depth pass");
    if (!impl_->uploaded || impl_->depth_batches.empty() || !impl_->depth_binding) return;
    auto& impl = *impl_;
    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.depth_buffer, static_cast<std::uint64_t>(slot) * Impl::depth_stride, &view_projection,
                         sizeof(view_projection));
    if (queue) wgpuQueueRelease(queue);
    const bool shadow = kind == DepthPassKind::shadow;
    // The instances this pass sees, in the slot's own region of the ids.
    const auto frustum = Frustum::from_view_projection(view_projection);
    const auto base = impl.depth_id_base + static_cast<std::size_t>(slot) * impl.depth_id_total;
    struct Run {
        const Impl::DepthBatch* batch;
        std::uint32_t first;
        std::uint32_t count;
    };
    std::vector<Run> runs;
    std::vector<IdEntry> ids;
    ids.reserve(impl.depth_id_total);
    for (const auto& batch : impl.depth_batches) {
        if (batch.shadow_only && !shadow) continue;
        const auto first = ids.size();
        for (std::uint32_t k = batch.first; k < batch.first + batch.count; ++k) {
            const auto instance = impl.depth_instances[k];
            if (skip_owner != 0 && instance < impl.instance_owners.size() && impl.instance_owners[instance] == skip_owner) continue;
            const auto& sphere = impl.instance_spheres[instance];
            if (frustum.intersects_sphere(glm::vec3{sphere}, sphere.w)) ids.push_back({instance, batch.draw->slot});
        }
        if (ids.size() > first) {
            runs.push_back({&batch, static_cast<std::uint32_t>(base + first), static_cast<std::uint32_t>(ids.size() - first)});
        }
    }
    if (runs.empty()) return;
    // The slot's indirect arguments, a region of its own (the passes may be
    // recorded on several threads at once).
    const bool multi = impl.use_multi_draw() && runs.size() <= impl.depth_batches.size();
    const auto indirect_base = impl.depth_indirect_base + static_cast<std::size_t>(slot) * impl.depth_batches.size();
    {
        const auto ids_queue = wgpuDeviceGetQueue(impl.device);
        wgpuQueueWriteBuffer(ids_queue, impl.id_buffer, base * sizeof(IdEntry), ids.data(), ids.size() * sizeof(IdEntry));
        if (multi) {
            std::vector<IndirectDraw> indirect;
            indirect.reserve(runs.size());
            for (const auto& run : runs) indirect.push_back({run.batch->draw->range.index_count, run.count, run.batch->draw->range.first_index, 0, run.first});
            wgpuQueueWriteBuffer(ids_queue, impl.indirect_buffer, indirect_base * sizeof(IndirectDraw), indirect.data(),
                                 indirect.size() * sizeof(IndirectDraw));
        }
        if (ids_queue) wgpuQueueRelease(ids_queue);
    }
    const auto offset = static_cast<std::uint32_t>(slot * Impl::depth_stride);
    wgpuRenderPassEncoderSetBindGroup(pass, 1, impl.depth_binding, 1, &offset);
    WGPURenderPipeline bound = nullptr;
    const RenderModel::Impl* bound_model = nullptr;
    WGPUBindGroup bound_draw = nullptr;
    std::size_t run_first = 0;
    std::size_t run_count = 0;
    const auto flush = [&] {
        if (run_count == 0) return;
        if (multi && run_count > 1) {
            wgpuRenderPassEncoderMultiDrawIndexedIndirect(pass, impl.indirect_buffer, (indirect_base + run_first) * sizeof(IndirectDraw),
                                                          static_cast<std::uint32_t>(run_count));
        } else {
            for (std::size_t k = run_first; k < run_first + run_count; ++k) {
                const auto& run = runs[k];
                wgpuRenderPassEncoderDrawIndexed(pass, run.batch->draw->range.index_count, run.count, run.batch->draw->range.first_index,
                                                 0, run.first);
            }
        }
        run_count = 0;
    };
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const auto& batch = *runs[i].batch;
        const auto pipeline = impl.depth_pipeline(shadow, batch.alpha_test);
        if (!pipeline) {
            flush();
            continue;
        }
        if (pipeline != bound) {
            flush();
            bound = pipeline;
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        }
        if (bound_model != batch.model) {
            flush();
            bound_model = batch.model;
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, bound_model->geometry->vertices, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetIndexBuffer(pass, bound_model->geometry->indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        }
        if (bound_draw != batch.binding) {
            flush();
            bound_draw = batch.binding;
            wgpuRenderPassEncoderSetBindGroup(pass, 0, batch.binding, 0, nullptr);
        }
        if (run_count == 0) run_first = i;
        ++run_count;
    }
    flush();
}

void ModelRenderer::submit_shadow_caster(RenderModel& model, const ModelTransform& transform) {
    if (!impl_ || !model.impl_) return;
    submit(model, transform);
    if (!impl_->queue.empty()) impl_->queue.back().shadow_only = true;
}

void ModelRenderer::set_effects(const WGPUBindGroup effects) {
    if (impl_) impl_->effects_binding = effects;
}

void ModelRenderer::set_depth_passes(const bool wanted) {
    if (impl_) impl_->depth_passes = wanted;
}

void ModelRenderer::set_task_pool(TaskPool* pool) {
    if (impl_) impl_->pool = pool;
}

void ModelRenderer::draw(const WGPURenderPassEncoder pass, RenderModel& model, const ModelFrameContext& frame) {
    begin_frame(frame);
    submit(model);
    render(pass);
}

} // namespace openrts::renderer
#endif
