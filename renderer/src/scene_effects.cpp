#include <openrts/renderer/scene_effects.hpp>

#include <openrts/core/profile.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <openrts/renderer/depth.hpp>

#include "rts_light_shapes.hpp"
#include "scene_effects_wgsl.hpp"

namespace openrts::renderer {
namespace {

constexpr std::uint32_t cluster_x = 16;
constexpr std::uint32_t cluster_y = 9;
constexpr std::uint32_t cluster_z = 24;
constexpr std::uint32_t cluster_capacity = 64; // lights a cluster holds at most
constexpr std::uint32_t cluster_count = cluster_x * cluster_y * cluster_z;
constexpr WGPUTextureFormat shadow_format = WGPUTextureFormat_Depth32Float;
constexpr WGPUTextureFormat ao_format = WGPUTextureFormat_R32Float;

// Laid out as `Effects` in scene_effects_wgsl.hpp.
struct EffectsUniform {
    glm::mat4 cascades[SceneEffects::max_cascades];
    glm::vec4 splits{0.0f};
    glm::vec4 shadow{0.0f};
    glm::vec4 eye{0.0f};
    glm::vec4 forward{0.0f};
    std::uint32_t clusters[4]{};
    glm::vec4 cluster_params{0.0f};
    glm::vec4 screen{0.0f};
    std::uint32_t flags[4]{};
    glm::vec4 sun{0.0f};
    glm::vec4 aerial{0.0f};
    glm::vec4 haze{0.0f};
    glm::vec4 sun_colour{0.0f};
    glm::vec4 texel{0.0f};
    glm::mat4 view{1.0f};
    glm::mat4 inv_projection{1.0f};
    glm::vec4 ao{0.0f};
    glm::vec4 horizon{0.0f};
    glm::mat4 focus{1.0f};
    glm::vec4 focus_params{0.0f};
    glm::vec4 lamps{0.0f};
    glm::mat4 lamp_shadows[SceneEffects::max_lamp_shadows]{};
    std::uint32_t lamp_shadow_ids[SceneEffects::max_lamp_shadows]{}; // the lamp's index in the light list + 1
    float lamp_shadow_texel[SceneEffects::max_lamp_shadows]{};        // a texel's size a metre from the lamp (or in metres, parallel)
    glm::vec4 lamp_shadow_params{0.0f}; // x: lamps, y: map size, z: filter radius (texels)
    // Each one's to_light translation: finds the lamp among the RTS light
    // rings when the clustered list is off.
    glm::vec4 lamp_shadow_keys[SceneEffects::max_lamp_shadows]{};
    // The sphere the lamps' shadows cover in flight (xyz its middle, w its
    // radius; 0: none): they fade out towards its edge.
    glm::vec4 lamp_focus{0.0f};
};
static_assert(sizeof(EffectsUniform) % 16 == 0);

// How strongly a lamp lights `world` (RTS lights.glsl, the shape, cone and
// attenuation; no surface): 0 outside it.
[[nodiscard]] float lamp_strength_at(const ModelExtraLight& light, const glm::vec3& world) {
    const glm::vec4 p = light.to_light * glm::vec4{world, 1.0f};
    const int kind = static_cast<int>(light.kind) & 31;
    const float reach = light.size.z * (light.attenuation.y + light.attenuation.z);
    float d = 0.0f;
    if (kind == 1) {
        if (!(p.z > 0.0f && p.z <= reach)) return 0.0f;
        d = (std::sqrt(p.x * p.x + p.y * p.y) - light.size.w) / (p.z * light.size.y);
    } else if (kind == 4) {
        if (!(p.z > 0.0f && p.z <= reach)) return 0.0f;
        const float x = p.x / light.size.x, y = p.y / light.size.y;
        d = std::sqrt(x * x + y * y) / p.z;
    } else if (kind == 2) {
        if (std::abs(p.z) > reach) return 0.0f;
        const float x = p.x * light.size.z / light.size.x, y = p.y * light.size.z / light.size.y;
        d = std::sqrt(x * x + y * y + p.z * p.z) / light.size.z;
    } else if (kind == 3) {
        if (!(p.z > 0.0f && p.z <= reach)) return 0.0f;
        d = (std::max)(std::abs(p.x) / light.size.x, std::abs(p.y) / light.size.y);
    } else {
        return 0.0f;
    }
    if (!std::isfinite(d)) return 0.0f;
    const float spot = light.spot_exponent / 128.0f;
    if (spot < 1.0f) {
        if (spot != 0.0f) {
            d = d < 1.0f - spot ? 0.0f : (d - 1.0f + spot) / std::abs(spot);
        } else if (d < 1.0f) {
            d = 0.0f;
        }
    }
    if (d > 1.0f) return 0.0f;
    float factor = std::clamp(1.0f - d, 0.0f, 1.0f);
    const int attenuation = static_cast<int>(light.attenuation.x);
    if (p.z > light.size.z * light.attenuation.y && light.size.z > 0.0f) {
        const float depth = p.z / light.size.z - light.attenuation.y;
        if (attenuation == 2 && light.attenuation.z > 0.0f) {
            factor *= 1.0f - depth / light.attenuation.z;
        } else if (attenuation == 1 && light.attenuation.z > 0.0f) {
            factor *= 1.0f - depth * depth / (light.attenuation.z * light.attenuation.z);
        }
    }
    return std::isfinite(factor) ? (std::max)(factor, 0.0f) : 0.0f;
}

// The least radius of the shadow filter, in metres: about the sun's
// penumbra a metre behind a window frame (the sun is half a degree wide),
// so the focus map's fine texels do not draw a razor edge.
constexpr float min_penumbra = 0.004f;

// Ground-truth ambient occlusion (Jimenez et al. 2016, as XeGTAO lays it
// out): two slices a pixel, six steps each way, horizons weighted down
// beyond the radius; normals from the depth. Then a 5 x 5 blur that keeps
// to surfaces of about the same depth.
constexpr char gtao_wgsl[] = R"(
@group(0) @binding(0) var<uniform> effects: Effects;
@group(0) @binding(1) var depth_texture: texture_depth_2d;
@group(0) @binding(2) var ao_out: texture_storage_2d<r32float, write>;

fn view_position(pixel: vec2<i32>) -> vec3<f32> {
    let size = vec2<i32>(textureDimensions(depth_texture));
    let p = clamp(pixel, vec2<i32>(0), size - vec2<i32>(1));
    let depth = textureLoad(depth_texture, p, 0);
    let uv = (vec2<f32>(p) + vec2<f32>(0.5)) / vec2<f32>(size);
    let v = effects.inv_projection * vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return v.xyz / v.w;
}

@compute @workgroup_size(8, 8)
fn gtao(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = vec2<i32>(textureDimensions(depth_texture));
    let pixel = vec2<i32>(id.xy);
    if (pixel.x >= size.x || pixel.y >= size.y) { return; }
    let depth = textureLoad(depth_texture, pixel, 0);
    if (depth <= 0.0) { // the sky (reversed Z cleared to 0)
        textureStore(ao_out, pixel, vec4<f32>(1.0));
        return;
    }
    let p = view_position(pixel);
    let l = view_position(pixel - vec2<i32>(1, 0));
    let r = view_position(pixel + vec2<i32>(1, 0));
    let u = view_position(pixel - vec2<i32>(0, 1));
    let d = view_position(pixel + vec2<i32>(0, 1));
    let dx = select(p - l, r - p, abs(r.z - p.z) < abs(p.z - l.z));
    let dy = select(p - u, d - p, abs(d.z - p.z) < abs(p.z - u.z));
    let v = normalize(-p);
    var n = normalize(cross(dy, dx));
    if (dot(n, v) < 0.0) { n = -n; }

    let radius = effects.ao.x;
    // Pixels a metre covers at this depth.
    let pixels_per_metre = f32(size.y) * 0.5 / (effects.ao.z * max(-p.z, 0.01));
    let radius_px = min(radius * pixels_per_metre, 96.0);
    if (radius_px < 1.0) {
        textureStore(ao_out, pixel, vec4<f32>(1.0));
        return;
    }
    let noise = fract(52.9829189 * fract(0.06711056 * f32(pixel.x) + 0.00583715 * f32(pixel.y)));
    let noise2 = fract(noise * 7.13 + 0.37);
    let falloff_range = 0.615 * radius;
    let falloff_from = radius * (1.0 - 0.615);
    let falloff_mul = -1.0 / falloff_range;
    let falloff_add = falloff_from / falloff_range + 1.0;
    const PI = 3.14159265;
    const SLICES = 2;
    const STEPS = 6;
    var visibility = 0.0;
    for (var slice = 0; slice < SLICES; slice = slice + 1) {
        let phi = (f32(slice) + noise) * PI / f32(SLICES);
        let omega = vec2<f32>(cos(phi), -sin(phi)); // screen, y down
        let direction = vec3<f32>(omega.x, -omega.y, 0.0);
        let ortho = direction - dot(direction, v) * v;
        let axis = normalize(cross(ortho, v));
        let projected = n - axis * dot(n, axis);
        let projected_length = length(projected);
        if (projected_length < 1e-4) { continue; }
        let sign_n = sign(dot(ortho, projected));
        let cos_n = clamp(dot(projected, v) / projected_length, 0.0, 1.0);
        let angle_n = sign_n * acos(cos_n);
        let low0 = cos(angle_n + PI * 0.5);
        let low1 = cos(angle_n - PI * 0.5);
        var horizon0 = low0;
        var horizon1 = low1;
        for (var step = 0; step < STEPS; step = step + 1) {
            var s = (f32(step) + noise2) / f32(STEPS);
            s = s * s;
            let offset = omega * max(s * radius_px, f32(step) + 1.0);
            let q0 = view_position(pixel + vec2<i32>(round(offset)));
            let q1 = view_position(pixel - vec2<i32>(round(offset)));
            let delta0 = q0 - p;
            let delta1 = q1 - p;
            let dist0 = length(delta0);
            let dist1 = length(delta1);
            let weight0 = clamp(dist0 * falloff_mul + falloff_add, 0.0, 1.0);
            let weight1 = clamp(dist1 * falloff_mul + falloff_add, 0.0, 1.0);
            let cos0 = mix(low0, dot(delta0 / max(dist0, 1e-4), v), weight0);
            let cos1 = mix(low1, dot(delta1 / max(dist1, 1e-4), v), weight1);
            horizon0 = max(horizon0, cos0);
            horizon1 = max(horizon1, cos1);
        }
        var h0 = -acos(clamp(horizon1, -1.0, 1.0));
        var h1 = acos(clamp(horizon0, -1.0, 1.0));
        h0 = angle_n + clamp(h0 - angle_n, -PI * 0.5, PI * 0.5);
        h1 = angle_n + clamp(h1 - angle_n, -PI * 0.5, PI * 0.5);
        let arc0 = (cos_n + 2.0 * h0 * sin(angle_n) - cos(2.0 * h0 - angle_n)) * 0.25;
        let arc1 = (cos_n + 2.0 * h1 * sin(angle_n) - cos(2.0 * h1 - angle_n)) * 0.25;
        visibility = visibility + projected_length * (arc0 + arc1);
    }
    // A slice of a tilted surface can see more than 1; the average over
    // slices (the blur) is 1 unoccluded, so it is clamped only after it (XeGTAO).
    visibility = max(visibility / f32(SLICES), 0.0);
    textureStore(ao_out, pixel, vec4<f32>(pow(visibility, effects.ao.y)));
}
)";

constexpr char blur_wgsl[] = R"(
@group(0) @binding(0) var<uniform> effects: Effects;
@group(0) @binding(1) var depth_texture: texture_depth_2d;
@group(0) @binding(2) var ao_in: texture_2d<f32>;
@group(0) @binding(3) var ao_out: texture_storage_2d<r32float, write>;

fn linear_depth(pixel: vec2<i32>) -> f32 {
    let depth = textureLoad(depth_texture, pixel, 0);
    let v = effects.inv_projection * vec4<f32>(0.0, 0.0, depth, 1.0);
    return -v.z / v.w;
}

@compute @workgroup_size(8, 8)
fn blur(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = vec2<i32>(textureDimensions(ao_in));
    let pixel = vec2<i32>(id.xy);
    if (pixel.x >= size.x || pixel.y >= size.y) { return; }
    let centre = linear_depth(pixel);
    var sum = 0.0;
    var weights = 0.0;
    for (var y = -2; y <= 2; y = y + 1) {
        for (var x = -2; x <= 2; x = x + 1) {
            let q = clamp(pixel + vec2<i32>(x, y), vec2<i32>(0), size - vec2<i32>(1));
            let depth = linear_depth(q);
            let w = exp(-abs(depth - centre) / max(centre * 0.03, 0.02));
            sum = sum + textureLoad(ao_in, q, 0).r * w;
            weights = weights + w;
        }
    }
    textureStore(ao_out, pixel, vec4<f32>(clamp(sum / max(weights, 1e-4), 0.03, 1.0)));
}
)";

// Each cluster (a screen tile between two depths, as a view-space box) gets
// the lights whose spheres reach it, nearest first, at most `clusters.w`.
constexpr char cluster_wgsl[] = R"(
@group(0) @binding(0) var<uniform> effects: Effects;
@group(0) @binding(1) var<storage, read> spheres: array<vec4<f32>>; // view-space centre, radius
@group(0) @binding(2) var<storage, read_write> counts: array<u32>;
@group(0) @binding(3) var<storage, read_write> indices: array<u32>;

@compute @workgroup_size(64)
fn assign(@builtin(global_invocation_id) id: vec3<u32>) {
    let grid = effects.clusters;
    let total = grid.x * grid.y * grid.z;
    let cluster = id.x;
    if (cluster >= total) { return; }
    let x = cluster % grid.x;
    let y = (cluster / grid.x) % grid.y;
    let z = cluster / (grid.x * grid.y);
    let near = exp((f32(z) - effects.cluster_params.w) / effects.cluster_params.z);
    let far = exp((f32(z + 1u) - effects.cluster_params.w) / effects.cluster_params.z);
    let x0 = min(f32(x) * effects.cluster_params.x * effects.screen.z, 1.0) * 2.0 - 1.0;
    let x1 = min(f32(x + 1u) * effects.cluster_params.x * effects.screen.z, 1.0) * 2.0 - 1.0;
    let y0 = 1.0 - min(f32(y + 1u) * effects.cluster_params.y * effects.screen.w, 1.0) * 2.0;
    let y1 = 1.0 - min(f32(y) * effects.cluster_params.y * effects.screen.w, 1.0) * 2.0;
    let tx = effects.ao.z * effects.ao.w;
    let ty = effects.ao.z;
    let xs = vec4<f32>(x0 * near, x1 * near, x0 * far, x1 * far) * tx;
    let ys = vec4<f32>(y0 * near, y1 * near, y0 * far, y1 * far) * ty;
    let lo = vec3<f32>(min(min(xs.x, xs.y), min(xs.z, xs.w)), min(min(ys.x, ys.y), min(ys.z, ys.w)), -far);
    let hi = vec3<f32>(max(max(xs.x, xs.y), max(xs.z, xs.w)), max(max(ys.x, ys.y), max(ys.z, ys.w)), -near);
    var count = 0u;
    let base = cluster * grid.w;
    for (var i = 0u; i < effects.flags.w; i = i + 1u) {
        let s = spheres[i];
        let closest = clamp(s.xyz, lo, hi);
        let d = closest - s.xyz;
        if (dot(d, d) <= s.w * s.w && count < grid.w) {
            indices[base + count] = i;
            count = count + 1u;
        }
    }
    counts[cluster] = count;
}
)";

[[nodiscard]] WGPUStringView name(const char* text) {
    return {.data = text, .length = WGPU_STRLEN};
}

[[nodiscard]] WGPUBuffer make_buffer(const WGPUDevice device, const WGPUBufferUsage usage, const std::uint64_t size) {
    const WGPUBufferDescriptor descriptor{.usage = usage | WGPUBufferUsage_CopyDst, .size = size};
    return wgpuDeviceCreateBuffer(device, &descriptor);
}

[[nodiscard]] WGPUShaderModule make_shader(const WGPUDevice device, const std::string& code) {
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = {.data = code.data(), .length = code.size()}};
    const WGPUShaderModuleDescriptor descriptor{.nextInChain = &source.chain};
    return wgpuDeviceCreateShaderModule(device, &descriptor);
}

[[nodiscard]] WGPUComputePipeline make_compute(const WGPUDevice device, const WGPUBindGroupLayout layout, const std::string& code,
                                               const char* entry) {
    const auto shader = make_shader(device, code);
    const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
    const WGPUComputePipelineDescriptor descriptor{.layout = pipeline_layout, .compute = {.module = shader, .entryPoint = name(entry)}};
    const auto pipeline = wgpuDeviceCreateComputePipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
    return pipeline;
}

struct Texture {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    std::uint32_t width = 0, height = 0, layers = 0;

    void release() {
        if (view) wgpuTextureViewRelease(view);
        if (texture) {
            wgpuTextureDestroy(texture);
            wgpuTextureRelease(texture);
        }
        *this = {};
    }
};

[[nodiscard]] Texture make_texture(const WGPUDevice device, const WGPUTextureFormat format, const WGPUTextureUsage usage,
                                   const std::uint32_t width, const std::uint32_t height, const std::uint32_t layers,
                                   const WGPUTextureViewDimension dimension) {
    Texture result{.width = width, .height = height, .layers = layers};
    const WGPUTextureDescriptor descriptor{
        .usage = usage,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = width, .height = height, .depthOrArrayLayers = layers},
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    result.texture = wgpuDeviceCreateTexture(device, &descriptor);
    const WGPUTextureViewDescriptor view{
        .format = format,
        .dimension = dimension,
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = layers,
        .aspect = WGPUTextureAspect_All,
    };
    if (result.texture) result.view = wgpuTextureCreateView(result.texture, &view);
    return result;
}

[[nodiscard]] glm::vec3 linear(const glm::vec4& display) {
    return glm::pow(glm::max(glm::vec3(display), glm::vec3(0.0f)), glm::vec3(2.2f));
}

// A light's own reach as a sphere around the lamp (lights.glsl's shapes:
// reach Size.z x (Attenuation.y + .z) along its axis, spreading by Size.x,
// Size.y), for clustering.
[[nodiscard]] std::pair<glm::vec3, float> light_sphere(const ModelExtraLight& light) {
    const glm::vec3 position{glm::inverse(light.to_light)[3]};
    const float reach = (std::max)(light.size.z * (light.attenuation.y + light.attenuation.z), 0.0f);
    const int kind = static_cast<int>(light.kind) & 31;
    float radius = reach;
    if (kind == 4 || kind == 1) { // cone / spotlight: spread per metre along the axis
        const float spread = (std::max)(std::abs(light.size.x), std::abs(light.size.y));
        radius = reach * std::sqrt(1.0f + spread * spread) + light.size.w;
    } else if (kind == 2) { // sphere: half sizes Size.x, Size.y, reach along z
        radius = (std::max)({reach, std::abs(light.size.x), std::abs(light.size.y)});
    } else if (kind == 3) { // box
        radius = std::sqrt(reach * reach + light.size.x * light.size.x + light.size.y * light.size.y);
    }
    return {position, radius};
}

} // namespace

WGPUBindGroupLayout create_scene_effects_layout(const WGPUDevice device) {
    std::array<WGPUBindGroupLayoutEntry, 8> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Comparison;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Fragment;
    entries[3].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[3].texture.viewDimension = WGPUTextureViewDimension_2D;
    for (std::uint32_t i = 4; i < 7; ++i) {
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Fragment;
        entries[i].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    }
    entries[4].buffer.minBindingSize = sizeof(detail::GpuLight);
    entries[5].buffer.minBindingSize = sizeof(std::uint32_t);
    entries[6].buffer.minBindingSize = sizeof(std::uint32_t);
    entries[7].binding = 7;
    entries[7].visibility = WGPUShaderStage_Fragment;
    entries[7].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[7].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

struct SceneEffects::Impl {
    WGPUDevice device = nullptr;
    SceneEffectSettings settings;

    WGPUBindGroupLayout main_layout = nullptr;
    WGPUBindGroupLayout gtao_layout = nullptr;
    WGPUBindGroupLayout blur_layout = nullptr;
    WGPUBindGroupLayout cluster_layout = nullptr;
    WGPUComputePipeline gtao = nullptr;
    WGPUComputePipeline blur = nullptr;
    WGPUComputePipeline cluster = nullptr;

    WGPUBuffer uniform = nullptr;
    WGPUBuffer lights = nullptr;
    WGPUBuffer spheres = nullptr;
    WGPUBuffer counts = nullptr;
    WGPUBuffer indices = nullptr;
    WGPUSampler compare_sampler = nullptr;

    Texture shadow;          // the cascades, then the focus map (or a 1 x 1 stand-in)
    std::array<WGPUTextureView, max_cascades + 1> shadow_layers{};
    Texture lamp_shadow;     // a layer a lamp throwing shadows (or a 1 x 1 stand-in)
    std::array<WGPUTextureView, max_lamp_shadows> lamp_layers{};
    Texture depth;           // the camera's pre-pass
    Texture ao_raw;
    Texture ao;              // blurred, read by the main pass (or a 1 x 1 stand-in)
    bool ao_is_real = false;

    WGPUBindGroup main_group = nullptr;
    WGPUBindGroup gtao_group = nullptr;
    WGPUBindGroup blur_group = nullptr;
    WGPUBindGroup cluster_group = nullptr;
    bool groups_stale = true;

    EffectsUniform frame{};
    SceneEffectView view{};
    bool active = false;
    std::uint32_t cascade_count = 0;
    std::uint32_t light_count = 0;
    std::array<glm::mat4, max_cascades + 1> cascade_matrices{};
    // The focus map: the layer after the cascades, drawn every frame.
    static constexpr std::uint32_t focus_slot = max_cascades + 1;
    bool focus_active = false;
    std::uint32_t focus_layer = 0;
    float daylight = 0.0f;
    // The lamps throwing shadows: their maps in depth slots lamp_slot on.
    static constexpr std::uint32_t lamp_slot = max_cascades + 2;
    std::vector<ModelExtraLight> packed_lights; // as the light list holds them
    bool lamp_shadows_wanted = false;
    std::uint32_t lamp_shadow_count = 0;
    std::uint32_t lamps_lighting = 0;
    float lamp_radius = 0.0f;
    std::array<glm::mat4, max_lamp_shadows> lamp_matrices{};
    std::vector<glm::vec3> lamp_shadow_kept; // last frame's lamps, by position
    void fit_focus(const std::optional<ShadowFocus>& focus);
    void choose_lamp_shadows(const std::optional<ShadowFocus>& focus, bool fade);
    [[nodiscard]] std::uint32_t lamp_map_size() const noexcept { return (std::max)(1024u, map_size() / 2); }
    void record_depth(WGPUCommandEncoder encoder, WGPUTextureView target, const glm::mat4& matrix, std::uint32_t slot,
                      const DepthDraw& draw) const;
    // What each cascade's map holds: drawn for this sphere and sun, this
    // frame; redraw[] says which are drawn this frame.
    struct CascadeCache {
        glm::vec3 centre{};
        float radius = 0.0f;
        glm::vec3 to_sun{};
        float texel = 0.0f;
        std::uint64_t generation = 0;
        bool valid = false;
    };
    std::array<CascadeCache, max_cascades> cached{};
    std::array<bool, max_cascades> redraw{};
    std::uint64_t frame_index = 0;
    std::uint64_t shadow_generation = 1; // grows when the shadow array is made again
    // record_parallel(): a command buffer a task, in submission order.
    std::vector<WGPUCommandBuffer> parallel_buffers;
    std::vector<float> parallel_ms; // by parallel_buffers' index: the pass's recording time

    void record_shadow(WGPUCommandEncoder encoder, std::uint32_t layer, std::uint32_t slot, const DepthDraw& draw) const;
    void record_camera(WGPUCommandEncoder encoder, const DepthDraw& draw) const;

    ~Impl() {
        release_groups();
        for (auto& layer : shadow_layers) {
            if (layer) wgpuTextureViewRelease(layer);
        }
        shadow.release();
        for (auto& layer : lamp_layers) {
            if (layer) wgpuTextureViewRelease(layer);
        }
        lamp_shadow.release();
        depth.release();
        ao_raw.release();
        ao.release();
        for (const auto buffer : {uniform, lights, spheres, counts, indices}) {
            if (buffer) wgpuBufferRelease(buffer);
        }
        if (compare_sampler) wgpuSamplerRelease(compare_sampler);
        for (const auto pipeline : {gtao, blur, cluster}) {
            if (pipeline) wgpuComputePipelineRelease(pipeline);
        }
        for (const auto layout : {main_layout, gtao_layout, blur_layout, cluster_layout}) {
            if (layout) wgpuBindGroupLayoutRelease(layout);
        }
    }

    void release_groups() {
        for (auto* group : {&main_group, &gtao_group, &blur_group, &cluster_group}) {
            if (*group) wgpuBindGroupRelease(*group);
            *group = nullptr;
        }
        groups_stale = true;
    }

    // The shadow array: a layer a cascade (and one for the focus) at the
    // chosen size while shadows are on, else 1 x 1.
    void ensure_shadow(const bool wanted) {
        const std::uint32_t size = wanted ? map_size() : 1;
        const std::uint32_t layers = wanted ? cascades_wanted() + (settings.cab_shadows ? 1u : 0u) : 1;
        if (shadow.texture && shadow.width == size && shadow.layers == layers) return;
        for (auto& layer : shadow_layers) {
            if (layer) wgpuTextureViewRelease(layer);
            layer = nullptr;
        }
        shadow.release();
        ++shadow_generation;
        shadow = make_texture(device, shadow_format, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, size, size,
                              layers, WGPUTextureViewDimension_2DArray);
        if (wanted && shadow.texture) {
            for (std::uint32_t i = 0; i < layers; ++i) {
                const WGPUTextureViewDescriptor layer{
                    .format = shadow_format,
                    .dimension = WGPUTextureViewDimension_2D,
                    .baseMipLevel = 0,
                    .mipLevelCount = 1,
                    .baseArrayLayer = i,
                    .arrayLayerCount = 1,
                    .aspect = WGPUTextureAspect_DepthOnly,
                };
                shadow_layers[i] = wgpuTextureCreateView(shadow.texture, &layer);
            }
        }
        release_groups();
    }

    // The lamps' shadow array: max_lamp_shadows layers while lamps throw
    // shadows, else 1 x 1.
    void ensure_lamp_shadow(const bool wanted) {
        const std::uint32_t size = wanted ? lamp_map_size() : 1;
        const std::uint32_t layers = wanted ? max_lamp_shadows : 1;
        if (lamp_shadow.texture && lamp_shadow.width == size && lamp_shadow.layers == layers) return;
        for (auto& layer : lamp_layers) {
            if (layer) wgpuTextureViewRelease(layer);
            layer = nullptr;
        }
        lamp_shadow.release();
        lamp_shadow = make_texture(device, shadow_format, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, size, size,
                                   layers, WGPUTextureViewDimension_2DArray);
        if (wanted && lamp_shadow.texture) {
            for (std::uint32_t i = 0; i < layers; ++i) {
                const WGPUTextureViewDescriptor layer{
                    .format = shadow_format,
                    .dimension = WGPUTextureViewDimension_2D,
                    .baseMipLevel = 0,
                    .mipLevelCount = 1,
                    .baseArrayLayer = i,
                    .arrayLayerCount = 1,
                    .aspect = WGPUTextureAspect_DepthOnly,
                };
                lamp_layers[i] = wgpuTextureCreateView(lamp_shadow.texture, &layer);
            }
        }
        release_groups();
    }

    // The pre-pass depth and the occlusion at the frame's size while AO is
    // on; else a 1 x 1 occlusion of 1.
    void ensure_ao(const bool wanted, const std::uint32_t width, const std::uint32_t height) {
        if (!wanted) {
            if (ao.texture && !ao_is_real) return;
            depth.release();
            ao_raw.release();
            ao.release();
            ao = make_texture(device, ao_format, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, 1, 1, 1,
                              WGPUTextureViewDimension_2D);
            const float one = 1.0f;
            if (ao.texture) {
                const auto queue = wgpuDeviceGetQueue(device);
                const WGPUTexelCopyTextureInfo target{.texture = ao.texture};
                const WGPUTexelCopyBufferLayout layout{.bytesPerRow = 4, .rowsPerImage = 1};
                const WGPUExtent3D extent{.width = 1, .height = 1, .depthOrArrayLayers = 1};
                wgpuQueueWriteTexture(queue, &target, &one, sizeof(one), &layout, &extent);
                wgpuQueueRelease(queue);
            }
            ao_is_real = false;
            release_groups();
            return;
        }
        if (ao_is_real && ao.width == width && ao.height == height) return;
        depth.release();
        ao_raw.release();
        ao.release();
        depth = make_texture(device, depth_format, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, width, height, 1,
                             WGPUTextureViewDimension_2D);
        const auto usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
        ao_raw = make_texture(device, ao_format, usage, width, height, 1, WGPUTextureViewDimension_2D);
        ao = make_texture(device, ao_format, usage, width, height, 1, WGPUTextureViewDimension_2D);
        ao_is_real = true;
        release_groups();
    }

    void ensure_groups() {
        if (!groups_stale) return;
        release_groups();
        {
            std::array<WGPUBindGroupEntry, 8> entries{};
            entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
            entries[1] = {.binding = 1, .textureView = shadow.view};
            entries[7] = {.binding = 7, .textureView = lamp_shadow.view};
            entries[2] = {.binding = 2, .sampler = compare_sampler};
            entries[3] = {.binding = 3, .textureView = ao.view};
            entries[4] = {.binding = 4, .buffer = lights, .size = sizeof(detail::GpuLight) * max_lights};
            entries[5] = {.binding = 5, .buffer = counts, .size = sizeof(std::uint32_t) * cluster_count};
            entries[6] = {.binding = 6, .buffer = indices, .size = sizeof(std::uint32_t) * cluster_count * cluster_capacity};
            const WGPUBindGroupDescriptor descriptor{.layout = main_layout, .entryCount = entries.size(), .entries = entries.data()};
            main_group = wgpuDeviceCreateBindGroup(device, &descriptor);
        }
        if (ao_is_real && depth.view && ao_raw.view && ao.view) {
            std::array<WGPUBindGroupEntry, 3> entries{};
            entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
            entries[1] = {.binding = 1, .textureView = depth.view};
            entries[2] = {.binding = 2, .textureView = ao_raw.view};
            const WGPUBindGroupDescriptor descriptor{.layout = gtao_layout, .entryCount = entries.size(), .entries = entries.data()};
            gtao_group = wgpuDeviceCreateBindGroup(device, &descriptor);
            std::array<WGPUBindGroupEntry, 4> blur_entries{};
            blur_entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
            blur_entries[1] = {.binding = 1, .textureView = depth.view};
            blur_entries[2] = {.binding = 2, .textureView = ao_raw.view};
            blur_entries[3] = {.binding = 3, .textureView = ao.view};
            const WGPUBindGroupDescriptor blur_descriptor{.layout = blur_layout, .entryCount = blur_entries.size(), .entries = blur_entries.data()};
            blur_group = wgpuDeviceCreateBindGroup(device, &blur_descriptor);
        }
        {
            std::array<WGPUBindGroupEntry, 4> entries{};
            entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
            entries[1] = {.binding = 1, .buffer = spheres, .size = sizeof(glm::vec4) * max_lights};
            entries[2] = {.binding = 2, .buffer = counts, .size = sizeof(std::uint32_t) * cluster_count};
            entries[3] = {.binding = 3, .buffer = indices, .size = sizeof(std::uint32_t) * cluster_count * cluster_capacity};
            const WGPUBindGroupDescriptor descriptor{.layout = cluster_layout, .entryCount = entries.size(), .entries = entries.data()};
            cluster_group = wgpuDeviceCreateBindGroup(device, &descriptor);
        }
        groups_stale = false;
    }

    [[nodiscard]] std::uint32_t map_size() const noexcept {
        const auto size = settings.shadow_map_size;
        return size <= 1024 ? 1024u : size >= 4096 ? 4096u : 2048u;
    }
    [[nodiscard]] std::uint32_t cascades_wanted() const noexcept {
        return std::clamp(settings.shadow_cascades, 2u, max_cascades);
    }

    // Stable cascades: each slice of the view (practical split, lambda 0.8)
    // in a sphere, seen orthographically from the sun, its origin snapped to
    // whole texels; 300 m more towards the sun for casters out of view.
    void compute_cascades(const glm::vec3& to_sun) {
        const float near = (std::max)(view.near_plane, 0.05f);
        const float far = (std::min)(settings.shadow_distance, view.far_plane);
        const auto forward = glm::normalize(glm::vec3(glm::inverse(view.view) * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}));
        const auto right = glm::normalize(glm::vec3(glm::inverse(view.view) * glm::vec4{1.0f, 0.0f, 0.0f, 0.0f}));
        const auto up = glm::normalize(glm::vec3(glm::inverse(view.view) * glm::vec4{0.0f, 1.0f, 0.0f, 0.0f}));
        const float tan_y = std::tan(view.vertical_fov * 0.5f);
        const float tan_x = tan_y * view.aspect;
        cascade_count = cascades_wanted();
        const auto size = map_size();
        // The far cascades kept between frames: the last two of 3 or 4, the
        // last of 2; drawn with a tenth more room so a moving camera stays
        // inside them a while.
        const std::uint32_t first_cached = settings.cache_far_cascades ? (cascade_count >= 3 ? cascade_count - 2 : cascade_count - 1) : cascade_count;
        ++frame_index;
        float previous = near;
        for (std::uint32_t i = 0; i < cascade_count; ++i) {
            const float t = static_cast<float>(i + 1) / static_cast<float>(cascade_count);
            const float split = 0.8f * near * std::pow(far / near, t) + 0.2f * (near + (far - near) * t);
            // The slice's bounding sphere: its corners around the centre on the axis.
            const float mid = (previous + split) * 0.5f;
            const glm::vec3 centre = view.eye + forward * mid;
            float radius = 0.0f;
            for (const float d : {previous, split}) {
                for (const float sx : {-1.0f, 1.0f}) {
                    for (const float sy : {-1.0f, 1.0f}) {
                        const glm::vec3 corner = view.eye + forward * d + right * (sx * d * tan_x) + up * (sy * d * tan_y);
                        radius = (std::max)(radius, glm::distance(corner, centre));
                    }
                }
            }
            const bool cacheable = i >= first_cached;
            if (cacheable) radius *= 1.1f;
            radius = std::ceil(radius * 4.0f) / 4.0f;
            // A kept cascade is drawn again on its turn (every other frame,
            // the two far ones alternating), or at once when its map no
            // longer covers this frame's slice, the sun moved or the map is new.
            auto& kept = cached[i];
            if (cacheable && kept.valid && kept.generation == shadow_generation) {
                const bool covered = glm::distance(kept.centre, centre) + radius / 1.1f <= kept.radius;
                const bool same_sun = glm::dot(kept.to_sun, to_sun) > 0.99995f;
                const bool turn = (frame_index + (i - first_cached)) % 2 == 0;
                if (covered && same_sun && !turn) {
                    redraw[i] = false;
                    frame.cascades[i] = cascade_matrices[i];
                    frame.splits[static_cast<int>(i)] = split;
                    frame.texel[static_cast<int>(i)] = kept.texel;
                    previous = split;
                    continue;
                }
            }
            redraw[i] = true;
            constexpr float behind = 300.0f;
            const glm::vec3 light_up = std::abs(to_sun.y) > 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
            const glm::mat4 light_view = glm::lookAtRH(centre + to_sun * (radius + behind), centre, light_up);
            glm::mat4 projection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + behind);
            glm::mat4 matrix = projection * light_view;
            // Snap the world's origin to a whole texel.
            const float half_size = static_cast<float>(size) * 0.5f;
            const glm::vec4 origin = matrix * glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
            const glm::vec2 texels{origin.x * half_size, origin.y * half_size};
            const glm::vec2 offset = (glm::round(texels) - texels) / half_size;
            projection[3][0] += offset.x;
            projection[3][1] += offset.y;
            matrix = projection * light_view;
            cascade_matrices[i] = matrix;
            frame.cascades[i] = matrix;
            frame.splits[static_cast<int>(i)] = split;
            frame.texel[static_cast<int>(i)] = 2.0f * radius / static_cast<float>(size);
            kept = {.centre = centre, .radius = radius, .to_sun = to_sun, .texel = frame.texel[static_cast<int>(i)],
                    .generation = shadow_generation, .valid = true};
            previous = split;
        }
    }
};

SceneEffects::SceneEffects(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    impl.main_layout = create_scene_effects_layout(device);
    {
        std::array<WGPUBindGroupLayoutEntry, 3> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Compute;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Compute;
        entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Compute;
        entries[2].storageTexture = {.access = WGPUStorageTextureAccess_WriteOnly, .format = ao_format, .viewDimension = WGPUTextureViewDimension_2D};
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.gtao_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    {
        std::array<WGPUBindGroupLayoutEntry, 4> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Compute;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Compute;
        entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Compute;
        entries[2].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[3].binding = 3;
        entries[3].visibility = WGPUShaderStage_Compute;
        entries[3].storageTexture = {.access = WGPUStorageTextureAccess_WriteOnly, .format = ao_format, .viewDimension = WGPUTextureViewDimension_2D};
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.blur_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    {
        std::array<WGPUBindGroupLayoutEntry, 4> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Compute;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Compute;
        entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Compute;
        entries[2].buffer.type = WGPUBufferBindingType_Storage;
        entries[3].binding = 3;
        entries[3].visibility = WGPUShaderStage_Compute;
        entries[3].buffer.type = WGPUBufferBindingType_Storage;
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.cluster_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    const std::string header = detail::effects_struct_wgsl;
    impl.gtao = make_compute(device, impl.gtao_layout, header + gtao_wgsl, "gtao");
    impl.blur = make_compute(device, impl.blur_layout, header + blur_wgsl, "blur");
    impl.cluster = make_compute(device, impl.cluster_layout, header + cluster_wgsl, "assign");

    impl.uniform = make_buffer(device, WGPUBufferUsage_Uniform, sizeof(EffectsUniform));
    impl.lights = make_buffer(device, WGPUBufferUsage_Storage, sizeof(detail::GpuLight) * max_lights);
    impl.spheres = make_buffer(device, WGPUBufferUsage_Storage, sizeof(glm::vec4) * max_lights);
    impl.counts = make_buffer(device, WGPUBufferUsage_Storage, sizeof(std::uint32_t) * cluster_count);
    impl.indices = make_buffer(device, WGPUBufferUsage_Storage, sizeof(std::uint32_t) * cluster_count * cluster_capacity);
    const WGPUSamplerDescriptor sampler{
        .addressModeU = WGPUAddressMode_ClampToEdge,
        .addressModeV = WGPUAddressMode_ClampToEdge,
        .addressModeW = WGPUAddressMode_ClampToEdge,
        .magFilter = WGPUFilterMode_Linear,
        .minFilter = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Nearest,
        .lodMaxClamp = 32.0f,
        .compare = WGPUCompareFunction_LessEqual,
        .maxAnisotropy = 1,
    };
    impl.compare_sampler = wgpuDeviceCreateSampler(device, &sampler);
    impl.ensure_shadow(false);
    impl.ensure_lamp_shadow(false);
    impl.ensure_ao(false, 1, 1);
    impl.ensure_groups();
}

SceneEffects::~SceneEffects() = default;

bool SceneEffects::ready() const noexcept {
    const auto& impl = *impl_;
    return impl.main_layout && impl.gtao && impl.blur && impl.cluster && impl.uniform && impl.lights && impl.compare_sampler
        && impl.main_group;
}

void SceneEffects::set_settings(const SceneEffectSettings& settings) {
    impl_->settings = settings;
}

const SceneEffectSettings& SceneEffects::settings() const noexcept {
    return impl_->settings;
}

void SceneEffects::begin_frame(const SceneEffectView& view, const ModelLighting& lighting, const bool active) {
    OPENRTS_ZONE("effects: begin frame");
    auto& impl = *impl_;
    impl.view = view;
    impl.active = active;
    impl.frame = {};
    auto& frame = impl.frame;
    const auto& s = impl.settings;

    const glm::vec3 to_sun = -glm::normalize(lighting.sun_direction);
    const float sun_luma = glm::dot(glm::vec3(lighting.sun_diffuse), glm::vec3{0.2126f, 0.7152f, 0.0722f});
    // Shadows while the sun is up and shines (faded in at low sun).
    const float sun_up = glm::clamp((to_sun.y - 0.02f) / 0.12f, 0.0f, 1.0f);
    const float shadow_strength = sun_up * glm::clamp(sun_luma * 1.6f, 0.0f, 1.0f);
    const bool shadows = active && s.shadows && shadow_strength > 0.01f;
    impl.ensure_shadow(active && s.shadows);
    impl.ensure_lamp_shadow(active && s.lamp_shadows > 0);
    impl.ensure_ao(active && s.ambient_occlusion, (std::max)(view.width, 1u), (std::max)(view.height, 1u));
    impl.ensure_groups();

    const auto inverse_view = glm::inverse(view.view);
    const glm::vec3 forward = glm::normalize(glm::vec3(inverse_view * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}));
    frame.eye = glm::vec4{view.eye, view.near_plane};
    frame.forward = glm::vec4{forward, view.far_plane};
    frame.screen = glm::vec4{static_cast<float>(view.width), static_cast<float>(view.height), 1.0f / static_cast<float>((std::max)(view.width, 1u)),
                             1.0f / static_cast<float>((std::max)(view.height, 1u))};
    frame.sun = glm::vec4{to_sun, 0.0f};
    frame.sun_colour = glm::vec4{linear(lighting.sun_diffuse), 1.0f};
    frame.view = view.view;
    frame.inv_projection = glm::inverse(view.projection);
    frame.ao = glm::vec4{1.2f, 1.6f, std::tan(view.vertical_fov * 0.5f), view.aspect};

    impl.cascade_count = 0;
    impl.focus_active = false;
    frame.focus_params.w = min_penumbra;
    if (shadows && impl.shadow_layers[0]) {
        impl.compute_cascades(to_sun);
        frame.shadow = glm::vec4{static_cast<float>(impl.cascade_count), shadow_strength, std::clamp(s.shadow_softness, 0.5f, 4.0f),
                                 static_cast<float>(impl.map_size())};
    }

    // Clusters: 16 x 9 tiles, 24 slices of log depth from 0.1 m to the far plane.
    const float cluster_near = 0.1f;
    const float cluster_far = (std::max)(view.far_plane, 1.0f);
    const float scale = static_cast<float>(cluster_z) / std::log(cluster_far / cluster_near);
    frame.clusters[0] = cluster_x;
    frame.clusters[1] = cluster_y;
    frame.clusters[2] = cluster_z;
    frame.clusters[3] = cluster_capacity;
    frame.cluster_params = glm::vec4{std::ceil(static_cast<float>(view.width) / cluster_x), std::ceil(static_cast<float>(view.height) / cluster_y),
                                     scale, -scale * std::log(cluster_near)};
    impl.light_count = 0;
    impl.packed_lights.clear();
    impl.lamp_shadow_count = 0;
    if (active && (s.clustered_lights || s.lamp_shadows > 0)) {
        // The scene's lights, nearest first: the clustered list, and the
        // lamps the shadows are chosen from (also without clusters).
        struct Candidate {
            const ModelExtraLight* light;
            glm::vec3 centre;
            float radius;
            float distance;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(lighting.light_rings.all.size());
        for (const auto& entry : lighting.light_rings.all) {
            const auto [centre, radius] = light_sphere(entry.light);
            if (radius <= 0.0f) continue;
            candidates.push_back({&entry.light, centre, radius, glm::distance(centre, view.eye) - radius});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        const auto count = (std::min)(candidates.size(), static_cast<std::size_t>(max_lights));
        std::vector<detail::GpuLight> packed(count);
        std::vector<glm::vec4> spheres(count);
        impl.packed_lights.clear();
        for (std::size_t i = 0; i < count; ++i) {
            detail::pack_light(packed[i], *candidates[i].light);
            impl.packed_lights.push_back(*candidates[i].light);
            spheres[i] = glm::vec4{glm::vec3(view.view * glm::vec4{candidates[i].centre, 1.0f}), candidates[i].radius};
        }
        impl.light_count = s.clustered_lights ? static_cast<std::uint32_t>(count) : 0u;
        if (impl.light_count > 0) {
            const auto queue = wgpuDeviceGetQueue(impl.device);
            wgpuQueueWriteBuffer(queue, impl.lights, 0, packed.data(), packed.size() * sizeof(detail::GpuLight));
            wgpuQueueWriteBuffer(queue, impl.spheres, 0, spheres.data(), spheres.size() * sizeof(glm::vec4));
            wgpuQueueRelease(queue);
        }
    }
    frame.flags[0] = active && s.clustered_lights ? 1u : 0u;
    frame.flags[1] = active && s.ambient_occlusion && impl.ao_is_real ? (s.show_occlusion ? 2u : 1u) : 0u;
    frame.flags[2] = active && s.aerial_perspective ? 1u : 0u;
    frame.flags[3] = impl.light_count;
    impl.lamp_shadows_wanted = active && s.lamp_shadows > 0 && !impl.packed_lights.empty() && impl.lamp_layers[0];

    // Haze from the environment: a third of the light left at RTS's fog end
    // (the sky dome's edge), thinning with height over about 1.5 km; the
    // sky's light scattered in, a sun glow towards the sun; RTS's fog colour
    // over the last quarter of that distance.
    const float fog_end = lighting.fog_end > 0.0f ? lighting.fog_end : 3000.0f;
    frame.aerial = glm::vec4{1.0f / fog_end, 1.0f / 1500.0f, 0.76f, 0.25f * glm::clamp(sun_luma, 0.0f, 1.0f) * sun_up};
    frame.haze = glm::vec4{linear(lighting.sun_ambient), 1.0f};
    frame.horizon = glm::vec4{linear(lighting.fog_colour), lighting.fog_end};

    // Lamps by day: the brighter the sun or the sky (linear), the weaker,
    // down to lamps_in_daylight; as given at night.
    {
        constexpr glm::vec3 luma{0.2126f, 0.7152f, 0.0722f};
        const float light = (std::max)(glm::dot(linear(lighting.sun_diffuse), luma), glm::dot(linear(lighting.sun_ambient), luma));
        const float t = glm::clamp((light - 0.1f) / 0.5f, 0.0f, 1.0f);
        impl.daylight = active ? t * t * (3.0f - 2.0f * t) : 0.0f;
        const float strength = 1.0f - impl.daylight * (1.0f - std::clamp(s.lamps_in_daylight, 0.0f, 1.0f));
        frame.lamps = glm::vec4{1.0f - strength, active ? std::clamp(s.ground_bounce, 0.0f, 0.5f) : 0.0f, 0.0f, 0.0f};
    }

    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.uniform, 0, &frame, sizeof(frame));
    wgpuQueueRelease(queue);
}

void SceneEffects::Impl::fit_focus(const std::optional<ShadowFocus>& focus) {
    auto& impl = *this;
    impl.focus_active = false;
    frame.focus_params.x = 0.0f;
    const auto layer = impl.cascade_count;
    if (!impl.active || layer == 0 || !impl.settings.cab_shadows || !focus || focus->radius <= 0.0f || layer >= impl.shadow.layers
        || !impl.shadow_layers[layer]) {
        return;
    }
    // Seen from the sun, its square centred on the sphere and turned with
    // the axis (no snapping: the texels move with the cab); 300 m more
    // towards the sun for casters out of it, as the cascades.
    const glm::vec3 to_sun{frame.sun};
    const float radius = focus->radius;
    glm::vec3 up = focus->axis;
    if (glm::length(up) < 1e-3f || std::abs(glm::dot(glm::normalize(up), to_sun)) > 0.95f) {
        up = std::abs(to_sun.y) > 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
    }
    constexpr float behind = 300.0f;
    const glm::mat4 light_view = glm::lookAtRH(focus->centre + to_sun * (radius + behind), focus->centre, glm::normalize(up));
    const glm::mat4 projection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + behind);
    const auto matrix = projection * light_view;
    impl.cascade_matrices[layer] = matrix;
    impl.focus_layer = layer;
    impl.focus_active = true;
    const auto size = static_cast<float>(impl.map_size());
    const float texel = 2.0f * radius / size;
    // A point is looked up in it while the filter and the push off its
    // surface stay inside: this many clip units from its edge.
    const float softness = std::clamp(impl.settings.shadow_softness, 0.5f, 4.0f);
    const float reach = (std::max)(softness, min_penumbra / texel) + 4.0f + 0.5f * softness;
    frame.focus = matrix;
    frame.focus_params = glm::vec4{static_cast<float>(layer + 1), texel, reach * 2.0f / size, min_penumbra};
}

// The lamps lighting the focus most throw shadows: each lamp's strength at
// seven points of the sphere (its middle and 0.4 of the radius each way)
// times its brightness, a lamp kept from the last frame counted half as
// much again (no flicker between two alike). Seen from the lamp at the
// sphere when the lamp is outside it; from inside, along its own cone (a
// sphere lamp there is left out: it would need six maps); a parallel
// (box) lamp orthographically along its axis.
void SceneEffects::Impl::choose_lamp_shadows(const std::optional<ShadowFocus>& focus, const bool fade) {
    lamp_shadow_count = 0;
    lamps_lighting = 0;
    lamp_radius = 0.0f;
    std::fill(std::begin(frame.lamp_shadow_ids), std::end(frame.lamp_shadow_ids), 0u);
    std::fill(std::begin(frame.lamp_shadow_keys), std::end(frame.lamp_shadow_keys), glm::vec4{0.0f});
    frame.lamp_shadow_params = glm::vec4{0.0f};
    frame.lamp_focus = glm::vec4{0.0f};
    if (!lamp_shadows_wanted) {
        lamp_shadow_kept.clear();
        return;
    }
    glm::vec3 centre;
    float radius;
    if (focus && focus->radius > 0.0f) {
        centre = focus->centre;
        radius = focus->radius;
    } else {
        // Without a cab: 20 m around a point 12 m ahead of the camera.
        const auto forward = glm::normalize(glm::vec3(glm::inverse(view.view) * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}));
        centre = view.eye + forward * 12.0f;
        radius = 20.0f;
    }
    std::array<glm::vec3, 7> samples{centre, centre, centre, centre, centre, centre, centre};
    const float step = radius * 0.4f;
    samples[1].x += step;
    samples[2].x -= step;
    samples[3].y += step;
    samples[4].y -= step;
    samples[5].z += step;
    samples[6].z -= step;
    struct Pick {
        std::uint32_t index;
        float score;
    };
    std::vector<Pick> picks;
    constexpr glm::vec3 luma{0.2126f, 0.7152f, 0.0722f};
    for (std::uint32_t i = 0; i < packed_lights.size(); ++i) {
        const auto& light = packed_lights[i];
        const glm::vec3 position{glm::inverse(light.to_light)[3]};
        const int kind = static_cast<int>(light.kind) & 31;
        const bool inside = glm::distance(position, centre) < radius * 1.3f;
        if (kind == 2 && inside) continue;
        float strength = 0.0f;
        for (const auto& sample : samples) strength = (std::max)(strength, lamp_strength_at(light, sample));
        // Where its beam falls: a big sphere's seven points may all miss a
        // lamp lighting the ground between them. Nearer the middle counts more.
        {
            const glm::vec3 along = glm::length(light.direction) > 1e-4f ? glm::normalize(light.direction) : glm::vec3{0.0f, -1.0f, 0.0f};
            const float reach = (std::max)(light.size.z * (light.attenuation.y + light.attenuation.z), 1.0f);
            const glm::vec3 beam = position + along * (std::min)(reach * 0.5f, 40.0f);
            const float off = glm::distance(beam, centre);
            if (off < radius) strength = (std::max)(strength, lamp_strength_at(light, beam) * (1.0f - 0.5f * off / radius));
        }
        float score = strength * glm::dot(glm::vec3(light.diffuse), luma);
        if (score < 0.01f) continue;
        for (const auto& kept : lamp_shadow_kept) {
            if (glm::distance(kept, position) < 0.1f) {
                score *= 1.5f;
                break;
            }
        }
        picks.push_back({i, score});
    }
    const auto wanted = (std::min)({static_cast<std::uint32_t>(picks.size()), settings.lamp_shadows, max_lamp_shadows});
    std::partial_sort(picks.begin(), picks.begin() + wanted, picks.end(), [](const Pick& a, const Pick& b) { return a.score > b.score; });
    lamp_shadow_kept.clear();
    const auto size = static_cast<float>(lamp_map_size());
    for (std::uint32_t k = 0; k < wanted; ++k) {
        const auto& light = packed_lights[picks[k].index];
        const glm::mat4 to_world = glm::inverse(light.to_light);
        const glm::vec3 position{to_world[3]};
        glm::vec3 axis = glm::length(light.direction) > 1e-4f ? glm::normalize(light.direction) : glm::vec3{0.0f, -1.0f, 0.0f};
        const int kind = static_cast<int>(light.kind) & 31;
        const float distance = glm::distance(position, centre);
        const auto up_for = [](const glm::vec3& along) {
            return std::abs(along.y) > 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
        };
        glm::mat4 matrix{1.0f};
        float texel = 0.0f;
        if (kind == 3) {
            // Parallel: its plane at the sphere, orthographic along the axis.
            const float along = (std::max)(glm::dot(centre - position, axis), 0.0f);
            const glm::vec3 eye = centre - axis * along;
            const glm::mat4 light_view = glm::lookAtRH(eye, centre, up_for(axis));
            matrix = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, along + radius) * light_view;
            texel = 2.0f * radius / size;
        } else if (distance > radius * 1.3f) {
            // From the lamp, just around the sphere.
            const glm::vec3 to = (centre - position) / distance;
            const float half = std::asin((std::min)(radius * 1.05f / distance, 0.98f));
            const float near_plane = std::clamp((distance - radius) * 0.5f, 0.1f, 1.0f);
            const glm::mat4 light_view = glm::lookAtRH(position, centre, up_for(to));
            matrix = glm::perspectiveRH_ZO(2.0f * half, 1.0f, near_plane, distance + radius * 1.2f) * light_view;
            texel = 2.0f * std::tan(half) / size;
        } else {
            // From inside: along its own cone, at most 120 degrees wide.
            const float spread = kind == 1 ? std::abs(light.size.y) : (std::max)(std::abs(light.size.x), std::abs(light.size.y));
            const float half = (std::min)(std::atan(spread), glm::radians(60.0f));
            const float reach = (std::max)(light.size.z * (light.attenuation.y + light.attenuation.z), 1.0f);
            const glm::mat4 light_view = glm::lookAtRH(position, position + axis, up_for(axis));
            matrix = glm::perspectiveRH_ZO(2.0f * (std::max)(half, glm::radians(5.0f)), 1.0f, 0.1f, reach) * light_view;
            texel = 2.0f * std::tan(half) / size;
        }
        lamp_matrices[k] = matrix;
        frame.lamp_shadows[k] = matrix;
        frame.lamp_shadow_ids[k] = picks[k].index + 1;
        frame.lamp_shadow_keys[k] = glm::vec4{glm::vec3(light.to_light[3]), 1.0f};
        frame.lamp_shadow_texel[static_cast<int>(k)] = texel;
        lamp_shadow_kept.push_back(position);
    }
    lamp_shadow_count = wanted;
    lamps_lighting = static_cast<std::uint32_t>(picks.size());
    lamp_radius = radius;
    frame.lamp_shadow_params = glm::vec4{static_cast<float>(wanted), size, std::clamp(settings.shadow_softness, 0.5f, 4.0f) * 1.5f, 0.0f};
    if (fade && wanted > 0) frame.lamp_focus = glm::vec4{centre, radius};
}

void SceneEffects::set_focus(const std::optional<ShadowFocus>& focus, const std::optional<ShadowFocus>& lamps) {
    OPENRTS_ZONE("effects: focus and lamps");
    auto& impl = *impl_;
    impl.fit_focus(focus);
    const bool flight = lamps && lamps->radius > 0.0f;
    impl.choose_lamp_shadows(flight ? lamps : focus, flight);
    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.uniform, 0, &impl.frame, sizeof(impl.frame));
    wgpuQueueRelease(queue);
}

// One shadow map: a cascade or the focus.
void SceneEffects::Impl::record_shadow(const WGPUCommandEncoder encoder, const std::uint32_t layer, const std::uint32_t slot,
                                       const DepthDraw& draw) const {
    record_depth(encoder, shadow_layers[layer], cascade_matrices[layer], slot, draw);
}

// A depth-only shadow pass into `target`.
void SceneEffects::Impl::record_depth(const WGPUCommandEncoder encoder, const WGPUTextureView target, const glm::mat4& matrix,
                                      const std::uint32_t slot, const DepthDraw& draw) const {
    const WGPURenderPassDepthStencilAttachment attachment{
        .view = target,
        .depthLoadOp = WGPULoadOp_Clear,
        .depthStoreOp = WGPUStoreOp_Store,
        .depthClearValue = 1.0f,
        .depthReadOnly = false,
        .stencilLoadOp = WGPULoadOp_Undefined,
        .stencilStoreOp = WGPUStoreOp_Undefined,
        .stencilReadOnly = true,
    };
    const WGPURenderPassDescriptor descriptor{.depthStencilAttachment = &attachment};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    if (draw) draw(pass, slot, matrix, DepthPassKind::shadow);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

// The camera's depth with the occlusion from it, then the light clusters.
void SceneEffects::Impl::record_camera(const WGPUCommandEncoder encoder, const DepthDraw& draw) const {
    if (frame.flags[1] != 0u && gtao_group && blur_group) {
        const WGPURenderPassDepthStencilAttachment depth_attachment{
            .view = depth.view,
            .depthLoadOp = WGPULoadOp_Clear,
            .depthStoreOp = WGPUStoreOp_Store,
            .depthClearValue = depth_clear_value,
            .depthReadOnly = false,
            .stencilLoadOp = WGPULoadOp_Undefined,
            .stencilStoreOp = WGPUStoreOp_Undefined,
            .stencilReadOnly = true,
        };
        const WGPURenderPassDescriptor descriptor{.depthStencilAttachment = &depth_attachment};
        const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        if (draw) draw(pass, max_cascades, view.projection * view.view, DepthPassKind::camera);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);

        const auto compute = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
        const auto groups_x = (ao.width + 7) / 8;
        const auto groups_y = (ao.height + 7) / 8;
        wgpuComputePassEncoderSetPipeline(compute, gtao);
        wgpuComputePassEncoderSetBindGroup(compute, 0, gtao_group, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(compute, groups_x, groups_y, 1);
        wgpuComputePassEncoderSetPipeline(compute, blur);
        wgpuComputePassEncoderSetBindGroup(compute, 0, blur_group, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(compute, groups_x, groups_y, 1);
        wgpuComputePassEncoderEnd(compute);
        wgpuComputePassEncoderRelease(compute);
    }
    if (frame.flags[0] != 0 && cluster_group) {
        const auto compute = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
        wgpuComputePassEncoderSetPipeline(compute, cluster);
        wgpuComputePassEncoderSetBindGroup(compute, 0, cluster_group, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(compute, (cluster_count + 63) / 64, 1, 1);
        wgpuComputePassEncoderEnd(compute);
        wgpuComputePassEncoderRelease(compute);
    }
}

void SceneEffects::record(const WGPUCommandEncoder encoder, const DepthDraw& draw) {
    auto& impl = *impl_;
    if (!impl.active) return;
    for (std::uint32_t i = 0; i < impl.cascade_count; ++i) {
        if (impl.redraw[i]) impl.record_shadow(encoder, i, i, draw);
    }
    if (impl.focus_active) impl.record_shadow(encoder, impl.focus_layer, Impl::focus_slot, draw);
    for (std::uint32_t k = 0; k < impl.lamp_shadow_count; ++k) {
        impl.record_depth(encoder, impl.lamp_layers[k], impl.lamp_matrices[k], Impl::lamp_slot + k, draw);
    }
    impl.record_camera(encoder, draw);
}

void SceneEffects::record_parallel(const WGPUDevice device, TaskPool& pool, const DepthDraw& draw) {
    auto& impl = *impl_;
    impl.parallel_buffers.assign(impl.active ? impl.cascade_count + 2 + max_lamp_shadows : 0, nullptr);
    impl.parallel_ms.assign(impl.parallel_buffers.size(), 0.0f);
    if (!impl.active) return;
    std::vector<std::function<void()>> tasks;
    const auto encode = [device, &impl](const std::size_t index, const char* name, auto&& record) {
        OPENRTS_ZONE("effects: pass");
        OPENRTS_ZONE_TEXT(name, std::strlen(name));
        const auto start = std::chrono::steady_clock::now();
        const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        {
            OPENRTS_ZONE("effects: record");
            record(encoder);
        }
        {
            // wgpu checks and encodes the pass's commands here.
            OPENRTS_ZONE("effects: finish");
            impl.parallel_buffers[index] = wgpuCommandEncoderFinish(encoder, nullptr);
        }
        wgpuCommandEncoderRelease(encoder);
        impl.parallel_ms[index] = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    // The longest first: the camera's passes, then the shadows (the pool
    // takes them in order; their buffers are submitted by index).
    tasks.emplace_back([encode, &impl, draw] {
        encode(impl.cascade_count + 1 + max_lamp_shadows, "camera depth, GTAO, clusters",
               [&](const WGPUCommandEncoder encoder) { impl.record_camera(encoder, draw); });
    });
    for (std::uint32_t i = 0; i < impl.cascade_count; ++i) {
        if (!impl.redraw[i]) continue;
        tasks.emplace_back([encode, &impl, i, draw] {
            static constexpr const char* names[] = {"sun cascade 1", "sun cascade 2", "sun cascade 3", "sun cascade 4"};
            encode(i, names[i % 4], [&](const WGPUCommandEncoder encoder) { impl.record_shadow(encoder, i, i, draw); });
        });
    }
    if (impl.focus_active) {
        tasks.emplace_back([encode, &impl, draw] {
            encode(impl.cascade_count, "cab focus", [&](const WGPUCommandEncoder encoder) {
                impl.record_shadow(encoder, impl.focus_layer, Impl::focus_slot, draw);
            });
        });
    }
    for (std::uint32_t k = 0; k < impl.lamp_shadow_count; ++k) {
        tasks.emplace_back([encode, &impl, k, draw] {
            encode(impl.cascade_count + 1 + k, "lamp shadow", [&](const WGPUCommandEncoder encoder) {
                impl.record_depth(encoder, impl.lamp_layers[k], impl.lamp_matrices[k], Impl::lamp_slot + k, draw);
            });
        });
    }
    pool.start(std::move(tasks));
}

std::vector<WGPUCommandBuffer> SceneEffects::finish_parallel(TaskPool& pool) {
    pool.wait();
    std::vector<WGPUCommandBuffer> buffers;
    for (const auto buffer : impl_->parallel_buffers) {
        if (buffer) buffers.push_back(buffer);
    }
    impl_->parallel_buffers.clear();
    return buffers;
}

WGPUBindGroup SceneEffects::bind_group() const noexcept {
    return impl_->main_group;
}

float SceneEffects::shadow_range() const noexcept {
    if (!impl_->active) return 0.0f;
    const float sun = impl_->cascade_count > 0 ? impl_->settings.shadow_distance : 0.0f;
    const float lamps = impl_->lamp_shadows_wanted ? 80.0f : 0.0f; // casters near the focus
    return (std::max)(sun, lamps);
}

bool SceneEffects::wants_depth() const noexcept {
    return impl_->active && (impl_->cascade_count > 0 || impl_->frame.flags[1] != 0u || impl_->lamp_shadows_wanted);
}

SceneEffectStats SceneEffects::stats() const noexcept {
    std::uint32_t drawn = 0;
    for (std::uint32_t i = 0; i < impl_->cascade_count; ++i) drawn += impl_->redraw[i] ? 1u : 0u;
    // The workers' passes: cascades first, the focus, the lamps, the camera last.
    std::array<float, 4> cascade_ms{};
    float lamps_ms = 0.0f;
    const auto& ms = impl_->parallel_ms;
    for (std::size_t i = 0; i < impl_->cascade_count && i < cascade_ms.size() && i < ms.size(); ++i) cascade_ms[i] = ms[i];
    for (std::size_t k = 0; k < impl_->lamp_shadow_count; ++k) {
        const auto index = impl_->cascade_count + 1 + k;
        if (index < ms.size()) lamps_ms += ms[index];
    }
    return {.cascades = impl_->cascade_count,
            .cascades_drawn = drawn,
            .lights = impl_->light_count,
            .focus = impl_->focus_active,
            .focus_texel = impl_->focus_active ? impl_->frame.focus_params.y : 0.0f,
            .near_texel = impl_->cascade_count > 0 ? impl_->frame.texel.x : 0.0f,
            .daylight = impl_->daylight,
            .lamp_strength = 1.0f - impl_->frame.lamps.x,
            .lamp_shadows = impl_->lamp_shadow_count,
            .lamps_lighting = impl_->lamps_lighting,
            .lamp_radius = impl_->lamp_radius,
            .record_cascade_ms = cascade_ms,
            .record_focus_ms = impl_->parallel_ms.size() > impl_->cascade_count ? impl_->parallel_ms[impl_->cascade_count] : 0.0f,
            .record_lamps_ms = lamps_ms,
            .record_camera_ms = impl_->parallel_ms.empty() ? 0.0f : impl_->parallel_ms.back()};
}

} // namespace openrts::renderer
#endif
