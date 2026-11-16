#include <openrts/renderer/sky_light.hpp>

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace openrts::renderer {

std::array<float, 9> sh9_basis(const glm::vec3& d) noexcept {
    const float x = d.x;
    const float y = d.y;
    const float z = d.z;
    return {0.282095f,         0.488603f * y,     0.488603f * z,
            0.488603f * x,     1.092548f * x * y, 1.092548f * y * z,
            0.315392f * (3.0f * z * z - 1.0f), 1.092548f * x * z, 0.546274f * (x * x - y * y)};
}

glm::vec3 sh9_light(const Sh9& radiance, const glm::vec3& normal) noexcept {
    // The cosine's convolution by band, over pi (Ramamoorthi and Hanrahan:
    // A0 = pi, A1 = 2 pi / 3, A2 = pi / 4).
    constexpr std::array<float, 9> band{1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    const auto basis = sh9_basis(normal);
    glm::vec3 light{0.0f};
    for (std::size_t i = 0; i < 9; ++i) light += radiance[i] * (basis[i] * band[i]);
    return glm::max(light, glm::vec3{0.0f});
}

Sh9 sh9_lower_hemisphere(const glm::vec3& radiance) noexcept {
    // Over y < 0: Y00 * 2 pi; 0.488603 y over it, -0.488603 pi; every other
    // basis function to l = 2 integrates to 0 there.
    Sh9 c{};
    c[0] = radiance * 1.7724539f;
    c[1] = radiance * -1.5349900f;
    return c;
}

namespace {

[[nodiscard]] float radical_inverse(std::uint32_t bits) noexcept {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

} // namespace

glm::vec2 split_sum_brdf(const float n_dot_v_in, const float roughness_in, const std::uint32_t samples) noexcept {
    const float n_dot_v = std::clamp(n_dot_v_in, 1e-4f, 1.0f);
    const float roughness = std::clamp(roughness_in, 0.045f, 1.0f);
    const float alpha = roughness * roughness;
    const float a2 = alpha * alpha;
    const glm::vec3 v{std::sqrt(1.0f - n_dot_v * n_dot_v), 0.0f, n_dot_v};
    double scale = 0.0;
    double bias = 0.0;
    for (std::uint32_t i = 0; i < samples; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(samples);
        const float w = radical_inverse(i);
        const float phi = 2.0f * 3.14159265f * u;
        const float cos_theta = std::sqrt((1.0f - w) / (1.0f + (a2 - 1.0f) * w));
        const float sin_theta = std::sqrt((std::max)(1.0f - cos_theta * cos_theta, 0.0f));
        const glm::vec3 h{sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta};
        const float v_dot_h = glm::dot(v, h);
        const glm::vec3 l = 2.0f * v_dot_h * h - v;
        const float n_dot_l = l.z;
        if (n_dot_l <= 0.0f || v_dot_h <= 0.0f) continue;
        // The height-correlated Smith visibility, as pbr_wgsl.hpp's.
        const float gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0f - a2) + a2);
        const float gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0f - a2) + a2);
        const float visibility = 0.5f / (std::max)(gv + gl, 1e-6f);
        // f cos / pdf with pdf = D n.h / (4 v.h): 4 V n.l v.h / n.h.
        const float weighted = 4.0f * visibility * n_dot_l * v_dot_h / cos_theta;
        const float fresnel = std::pow(1.0f - v_dot_h, 5.0f);
        scale += (1.0f - fresnel) * weighted;
        bias += fresnel * weighted;
    }
    return {static_cast<float>(scale / samples), static_cast<float>(bias / samples)};
}

} // namespace openrts::renderer

#ifdef OPENRTS_HAS_WGPU
#include <cstring>
#include <string>
#include <vector>

#include <glm/gtc/packing.hpp>

#include <openrts/renderer/gpu_pass_timer.hpp>

namespace openrts::renderer {
namespace {

constexpr WGPUTextureFormat light_format = WGPUTextureFormat_RGBA16Float;
constexpr std::uint32_t source_mips = 8; // 128 .. 1
constexpr std::uint64_t slot_stride = 256;

struct Params {
    float to_sun[4];    // xyz: towards the sun; w: the ground's reflectance
    float sun[4];       // rgb: the direct sun's light (linear); w: the most a texel gives
    float pass_info[4]; // x: the target's face size, y: its roughness, z: the source's face size, w: the source's mips
};
static_assert(sizeof(Params) <= slot_stride);

constexpr char sky_light_wgsl[] = R"(
struct Params {
    to_sun: vec4<f32>,
    sun: vec4<f32>,
    pass_info: vec4<f32>,
};

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var probe: texture_cube<f32>;
@group(0) @binding(2) var linear_sampler: sampler;
@group(0) @binding(3) var<storage, read_write> light: array<vec4<f32>, 12>;
@group(0) @binding(5) var output: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(6) var source_mip: texture_2d_array<f32>;
@group(0) @binding(7) var source: texture_cube<f32>;

const pi: f32 = 3.14159265;

// A cube face's texel (uv -1..1: right, down) as a way, as WebGPU lays the
// faces out (SkyRenderer::render_reflection_probe draws them so).
fn cube_direction(face: u32, uv: vec2<f32>) -> vec3<f32> {
    switch face {
        case 0u: { return normalize(vec3<f32>(1.0, -uv.y, -uv.x)); }
        case 1u: { return normalize(vec3<f32>(-1.0, -uv.y, uv.x)); }
        case 2u: { return normalize(vec3<f32>(uv.x, 1.0, uv.y)); }
        case 3u: { return normalize(vec3<f32>(uv.x, -1.0, -uv.y)); }
        case 4u: { return normalize(vec3<f32>(uv.x, -uv.y, 1.0)); }
        default: { return normalize(vec3<f32>(-uv.x, -uv.y, -1.0)); }
    }
}

fn basis(d: vec3<f32>) -> array<f32, 9> {
    return array<f32, 9>(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x, 1.092548 * d.x * d.y,
                         1.092548 * d.y * d.z, 0.315392 * (3.0 * d.z * d.z - 1.0), 1.092548 * d.x * d.z,
                         0.546274 * (d.x * d.x - d.y * d.y));
}

// The probe's light that way, linear: its colours are gamma 2.2 encoded; a
// part not a finite number (tested by its bits) none; at most the clamp, by
// luminance (the sun's disc is the direct light's).
fn probe_light(direction: vec3<f32>) -> vec3<f32> {
    let encoded = textureSampleLevel(probe, linear_sampler, direction, 0.0).rgb;
    let finite = (bitcast<vec3<u32>>(encoded) & vec3<u32>(0x7f800000u)) != vec3<u32>(0x7f800000u);
    let linear = pow(max(select(vec3<f32>(0.0), encoded, finite), vec3<f32>(0.0)), vec3<f32>(2.2));
    let luminance = dot(linear, vec3<f32>(0.2126, 0.7152, 0.0722));
    return select(linear, linear * (params.sun.w / luminance), luminance > params.sun.w);
}

const sh_grid: u32 = 64u;
var<workgroup> partial: array<array<vec4<f32>, 9>, 64>;
var<workgroup> total: array<vec4<f32>, 9>;

// The SH9 of the sky over the horizon (64 x 64 ways a face, each by its
// solid angle), then the ground under it: a Lambert plane of the ground's
// reflectance lit by the sun and by that sky, added as the lower hemisphere.
@compute @workgroup_size(64)
fn sh(@builtin(local_invocation_index) thread: u32) {
    var sum: array<vec3<f32>, 9>;
    for (var k = 0u; k < 9u; k = k + 1u) {
        sum[k] = vec3<f32>(0.0);
    }
    let per_face = sh_grid * sh_grid;
    for (var i = thread; i < 6u * per_face; i = i + 64u) {
        let face = i / per_face;
        let rest = i % per_face;
        let uv = (vec2<f32>(f32(rest % sh_grid), f32(rest / sh_grid)) + 0.5) / f32(sh_grid) * 2.0 - 1.0;
        let direction = cube_direction(face, uv);
        if (direction.y < 0.0) {
            continue;
        }
        let solid = 4.0 / f32(per_face) / pow(1.0 + dot(uv, uv), 1.5);
        let l = probe_light(direction) * solid;
        var b = basis(direction);
        for (var k = 0u; k < 9u; k = k + 1u) {
            sum[k] = sum[k] + l * b[k];
        }
    }
    for (var k = 0u; k < 9u; k = k + 1u) {
        partial[thread][k] = vec4<f32>(sum[k], 0.0);
    }
    workgroupBarrier();
    if (thread < 9u) {
        var t = vec3<f32>(0.0);
        for (var j = 0u; j < 64u; j = j + 1u) {
            t = t + partial[j][thread].xyz;
        }
        total[thread] = vec4<f32>(t, 0.0);
    }
    workgroupBarrier();
    if (thread == 0u) {
        var band = array<f32, 9>(1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25);
        var up = basis(vec3<f32>(0.0, 1.0, 0.0));
        var sky_up = vec3<f32>(0.0);
        for (var k = 0u; k < 9u; k = k + 1u) {
            sky_up = sky_up + total[k].xyz * (up[k] * band[k]);
        }
        sky_up = max(sky_up, vec3<f32>(0.0));
        let ground = params.to_sun.w * (params.sun.xyz * max(params.to_sun.y, 0.0) + sky_up);
        var lower = array<f32, 9>(1.7724539, -1.5349900, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        for (var k = 0u; k < 9u; k = k + 1u) {
            light[k] = vec4<f32>(total[k].xyz + ground * lower[k], 0.0);
        }
        light[9] = vec4<f32>(ground, 0.0);
        light[10] = vec4<f32>(sky_up, 0.0);
        light[11] = vec4<f32>(0.0);
    }
}

// The source: the probe in linear light over the horizon, the ground under it.
@compute @workgroup_size(8, 8, 1)
fn source_light(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = u32(params.pass_info.x);
    if (id.x >= size || id.y >= size) {
        return;
    }
    let uv = (vec2<f32>(id.xy) + 0.5) / f32(size) * 2.0 - 1.0;
    let direction = cube_direction(id.z, uv);
    var l = light[9].xyz;
    if (direction.y >= 0.0) {
        l = probe_light(direction);
    }
    textureStore(output, id.xy, id.z, vec4<f32>(l, 1.0));
}

// The source's next mip: each texel the mean of the four under it.
@compute @workgroup_size(8, 8, 1)
fn downsample(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = textureDimensions(output).x;
    if (id.x >= size || id.y >= size) {
        return;
    }
    let p = vec2<i32>(id.xy) * 2;
    let layer = i32(id.z);
    let sum = textureLoad(source_mip, p, layer, 0) + textureLoad(source_mip, p + vec2<i32>(1, 0), layer, 0)
        + textureLoad(source_mip, p + vec2<i32>(0, 1), layer, 0) + textureLoad(source_mip, p + vec2<i32>(1, 1), layer, 0);
    textureStore(output, id.xy, id.z, sum * 0.25);
}

fn hammersley(i: u32, count: u32) -> vec2<f32> {
    return vec2<f32>((f32(i) + 0.5) / f32(count), f32(reverseBits(i)) * 2.3283064365386963e-10);
}

const prefilter_samples: u32 = 48u;

// A mip of the prefiltered cube: the source as a surface of this roughness
// facing each way reflects it seen along its normal (GGX importance
// sampled, each sample from the source's mip its solid angle fits: Karis,
// Colbert and Krivanek's filtered importance sampling); roughness 0 the
// source itself.
@compute @workgroup_size(8, 8, 1)
fn prefilter(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = u32(params.pass_info.x);
    if (id.x >= size || id.y >= size) {
        return;
    }
    let uv = (vec2<f32>(id.xy) + 0.5) / f32(size) * 2.0 - 1.0;
    let n = cube_direction(id.z, uv);
    let roughness = params.pass_info.y;
    if (roughness <= 0.0) {
        textureStore(output, id.xy, id.z, vec4<f32>(textureSampleLevel(source, linear_sampler, n, 0.0).rgb, 1.0));
        return;
    }
    let alpha = roughness * roughness;
    let a2 = alpha * alpha;
    let helper = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(n.z) < 0.999);
    let tangent = normalize(cross(helper, n));
    let bitangent = cross(n, tangent);
    let texel_solid = 4.0 * pi / (6.0 * params.pass_info.z * params.pass_info.z);
    var sum = vec3<f32>(0.0);
    var weight = 0.0;
    for (var i = 0u; i < prefilter_samples; i = i + 1u) {
        let xi = hammersley(i, prefilter_samples);
        let phi = 2.0 * pi * xi.x;
        let cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        let sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
        let h = normalize(tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + n * cos_theta);
        let l = 2.0 * dot(n, h) * h - n;
        let n_dot_l = dot(n, l);
        if (n_dot_l > 0.0) {
            let d = n_dot_h_ggx(cos_theta, a2);
            let pdf = d * 0.25; // D n.h / (4 v.h), the eye along the normal
            let sample_solid = 1.0 / (f32(prefilter_samples) * pdf + 1e-6);
            let lod = clamp(0.5 * log2(sample_solid / texel_solid) + 1.0, 0.0, params.pass_info.w - 1.0);
            sum = sum + textureSampleLevel(source, linear_sampler, l, lod).rgb * n_dot_l;
            weight = weight + n_dot_l;
        }
    }
    textureStore(output, id.xy, id.z, vec4<f32>(sum / max(weight, 1e-4), 1.0));
}

fn n_dot_h_ggx(n_dot_h: f32, a2: f32) -> f32 {
    let d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (pi * d * d);
}
)";

[[nodiscard]] WGPUStringView text(const char* s) {
    return {.data = s, .length = std::strlen(s)};
}

struct LightTexture {
    WGPUTexture texture = nullptr;
    WGPUTextureView cube = nullptr;
    std::vector<WGPUTextureView> mips; // each a 2D array of the six faces

    void release() {
        for (const auto mip : mips) {
            if (mip) wgpuTextureViewRelease(mip);
        }
        mips.clear();
        if (cube) wgpuTextureViewRelease(cube);
        if (texture) {
            wgpuTextureDestroy(texture);
            wgpuTextureRelease(texture);
        }
        texture = nullptr;
        cube = nullptr;
    }
};

[[nodiscard]] LightTexture make_cube(const WGPUDevice device, const std::uint32_t size, const std::uint32_t mips) {
    LightTexture result;
    const WGPUTextureDescriptor descriptor{
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = size, .height = size, .depthOrArrayLayers = 6},
        .format = light_format,
        .mipLevelCount = mips,
        .sampleCount = 1,
    };
    result.texture = wgpuDeviceCreateTexture(device, &descriptor);
    if (!result.texture) return result;
    const WGPUTextureViewDescriptor cube{
        .format = light_format,
        .dimension = WGPUTextureViewDimension_Cube,
        .baseMipLevel = 0,
        .mipLevelCount = mips,
        .baseArrayLayer = 0,
        .arrayLayerCount = 6,
        .aspect = WGPUTextureAspect_All,
    };
    result.cube = wgpuTextureCreateView(result.texture, &cube);
    for (std::uint32_t m = 0; m < mips; ++m) {
        const WGPUTextureViewDescriptor mip{
            .format = light_format,
            .dimension = WGPUTextureViewDimension_2DArray,
            .baseMipLevel = m,
            .mipLevelCount = 1,
            .baseArrayLayer = 0,
            .arrayLayerCount = 6,
            .aspect = WGPUTextureAspect_All,
        };
        result.mips.push_back(wgpuTextureCreateView(result.texture, &mip));
    }
    return result;
}

[[nodiscard]] WGPUBindGroupLayoutEntry uniform_entry() {
    WGPUBindGroupLayoutEntry e{};
    e.binding = 0;
    e.visibility = WGPUShaderStage_Compute;
    e.buffer.type = WGPUBufferBindingType_Uniform;
    e.buffer.hasDynamicOffset = true;
    e.buffer.minBindingSize = sizeof(Params);
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry cube_entry(const std::uint32_t binding) {
    WGPUBindGroupLayoutEntry e{};
    e.binding = binding;
    e.visibility = WGPUShaderStage_Compute;
    e.texture.sampleType = WGPUTextureSampleType_Float;
    e.texture.viewDimension = WGPUTextureViewDimension_Cube;
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry sampler_entry() {
    WGPUBindGroupLayoutEntry e{};
    e.binding = 2;
    e.visibility = WGPUShaderStage_Compute;
    e.sampler.type = WGPUSamplerBindingType_Filtering;
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry light_entry() {
    WGPUBindGroupLayoutEntry e{};
    e.binding = 3;
    e.visibility = WGPUShaderStage_Compute;
    e.buffer.type = WGPUBufferBindingType_Storage;
    e.buffer.minBindingSize = sky_light_buffer_size;
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry target_entry() {
    WGPUBindGroupLayoutEntry e{};
    e.binding = 5;
    e.visibility = WGPUShaderStage_Compute;
    e.storageTexture = {.access = WGPUStorageTextureAccess_WriteOnly, .format = light_format, .viewDimension = WGPUTextureViewDimension_2DArray};
    return e;
}

[[nodiscard]] WGPUBindGroupLayoutEntry source_mip_entry() {
    WGPUBindGroupLayoutEntry e{};
    e.binding = 6;
    e.visibility = WGPUShaderStage_Compute;
    e.texture.sampleType = WGPUTextureSampleType_Float;
    e.texture.viewDimension = WGPUTextureViewDimension_2DArray;
    return e;
}

[[nodiscard]] WGPUBindGroupLayout make_layout(const WGPUDevice device, const std::vector<WGPUBindGroupLayoutEntry>& entries) {
    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

} // namespace

struct SkyLight::Impl {
    WGPUDevice device = nullptr;
    WGPUShaderModule shader = nullptr;
    WGPUBindGroupLayout sh_layout = nullptr;
    WGPUBindGroupLayout source_layout = nullptr;
    WGPUBindGroupLayout down_layout = nullptr;
    WGPUBindGroupLayout prefilter_layout = nullptr;
    WGPUComputePipeline sh = nullptr;
    WGPUComputePipeline source_light = nullptr;
    WGPUComputePipeline downsample = nullptr;
    WGPUComputePipeline prefilter = nullptr;
    WGPUSampler sampler = nullptr;
    WGPUBuffer params = nullptr;
    WGPUBuffer light = nullptr;
    LightTexture source;
    LightTexture prefiltered;
    WGPUTexture brdf = nullptr;
    WGPUTextureView brdf_view = nullptr;
    std::vector<WGPUBindGroup> down_groups;      // [k - 1]: mip k from k - 1
    std::vector<WGPUBindGroup> prefilter_groups; // [m]
    WGPUTextureView probe = nullptr;             // the probe the next two are for
    WGPUBindGroup sh_group = nullptr;
    WGPUBindGroup source_group = nullptr;
    bool updated = false;

    ~Impl() {
        release_probe_groups();
        for (const auto group : down_groups) {
            if (group) wgpuBindGroupRelease(group);
        }
        for (const auto group : prefilter_groups) {
            if (group) wgpuBindGroupRelease(group);
        }
        source.release();
        prefiltered.release();
        if (brdf_view) wgpuTextureViewRelease(brdf_view);
        if (brdf) {
            wgpuTextureDestroy(brdf);
            wgpuTextureRelease(brdf);
        }
        if (light) wgpuBufferRelease(light);
        if (params) wgpuBufferRelease(params);
        if (sampler) wgpuSamplerRelease(sampler);
        for (const auto pipeline : {sh, source_light, downsample, prefilter}) {
            if (pipeline) wgpuComputePipelineRelease(pipeline);
        }
        for (const auto layout : {sh_layout, source_layout, down_layout, prefilter_layout}) {
            if (layout) wgpuBindGroupLayoutRelease(layout);
        }
        if (shader) wgpuShaderModuleRelease(shader);
    }

    void release_probe_groups() {
        if (sh_group) wgpuBindGroupRelease(sh_group);
        if (source_group) wgpuBindGroupRelease(source_group);
        sh_group = nullptr;
        source_group = nullptr;
        probe = nullptr;
    }

    [[nodiscard]] WGPUComputePipeline make_pipeline(const WGPUBindGroupLayout layout, const char* entry) const {
        const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout};
        const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
        const WGPUComputePipelineDescriptor descriptor{.layout = pipeline_layout, .compute = {.module = shader, .entryPoint = text(entry)}};
        const auto pipeline = wgpuDeviceCreateComputePipeline(device, &descriptor);
        wgpuPipelineLayoutRelease(pipeline_layout);
        return pipeline;
    }

    [[nodiscard]] WGPUBindGroup make_group(const WGPUBindGroupLayout layout, const std::vector<WGPUBindGroupEntry>& entries) const {
        const WGPUBindGroupDescriptor descriptor{.layout = layout, .entryCount = entries.size(), .entries = entries.data()};
        return wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    // The split sum's table, made on the CPU once (a few milliseconds).
    void make_brdf() {
        const WGPUTextureDescriptor descriptor{
            .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = sky_light_brdf_size, .height = sky_light_brdf_size, .depthOrArrayLayers = 1},
            .format = light_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        brdf = wgpuDeviceCreateTexture(device, &descriptor);
        if (!brdf) return;
        brdf_view = wgpuTextureCreateView(brdf, nullptr);
        std::vector<std::uint16_t> texels(std::size_t{sky_light_brdf_size} * sky_light_brdf_size * 4);
        for (std::uint32_t row = 0; row < sky_light_brdf_size; ++row) {
            const float roughness = (static_cast<float>(row) + 0.5f) / static_cast<float>(sky_light_brdf_size);
            for (std::uint32_t column = 0; column < sky_light_brdf_size; ++column) {
                const float n_dot_v = (static_cast<float>(column) + 0.5f) / static_cast<float>(sky_light_brdf_size);
                const auto ab = split_sum_brdf(n_dot_v, roughness);
                const auto at = (std::size_t{row} * sky_light_brdf_size + column) * 4;
                texels[at] = glm::packHalf1x16(ab.x);
                texels[at + 1] = glm::packHalf1x16(ab.y);
                texels[at + 2] = 0;
                texels[at + 3] = glm::packHalf1x16(1.0f);
            }
        }
        const auto queue = wgpuDeviceGetQueue(device);
        const WGPUTexelCopyTextureInfo target{.texture = brdf};
        const WGPUTexelCopyBufferLayout layout{.bytesPerRow = sky_light_brdf_size * 8, .rowsPerImage = sky_light_brdf_size};
        const WGPUExtent3D extent{.width = sky_light_brdf_size, .height = sky_light_brdf_size, .depthOrArrayLayers = 1};
        wgpuQueueWriteTexture(queue, &target, texels.data(), texels.size() * sizeof(std::uint16_t), &layout, &extent);
        wgpuQueueRelease(queue);
    }
};

SkyLight::SkyLight(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = text(sky_light_wgsl)};
    const WGPUShaderModuleDescriptor shader{.nextInChain = &source.chain};
    impl.shader = wgpuDeviceCreateShaderModule(device, &shader);
    impl.sh_layout = make_layout(device, {uniform_entry(), cube_entry(1), sampler_entry(), light_entry()});
    impl.source_layout = make_layout(device, {uniform_entry(), cube_entry(1), sampler_entry(), light_entry(), target_entry()});
    impl.down_layout = make_layout(device, {target_entry(), source_mip_entry()});
    impl.prefilter_layout = make_layout(device, {uniform_entry(), sampler_entry(), target_entry(), cube_entry(7)});
    impl.sh = impl.make_pipeline(impl.sh_layout, "sh");
    impl.source_light = impl.make_pipeline(impl.source_layout, "source_light");
    impl.downsample = impl.make_pipeline(impl.down_layout, "downsample");
    impl.prefilter = impl.make_pipeline(impl.prefilter_layout, "prefilter");
    const WGPUSamplerDescriptor sampler{
        .addressModeU = WGPUAddressMode_ClampToEdge,
        .addressModeV = WGPUAddressMode_ClampToEdge,
        .addressModeW = WGPUAddressMode_ClampToEdge,
        .magFilter = WGPUFilterMode_Linear,
        .minFilter = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Linear,
        .lodMaxClamp = 32.0f,
        .maxAnisotropy = 1,
    };
    impl.sampler = wgpuDeviceCreateSampler(device, &sampler);
    const WGPUBufferDescriptor params{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = slot_stride * sky_light_mips};
    impl.params = wgpuDeviceCreateBuffer(device, &params);
    const WGPUBufferDescriptor light{.usage = WGPUBufferUsage_Storage, .size = sky_light_buffer_size};
    impl.light = wgpuDeviceCreateBuffer(device, &light);
    impl.source = make_cube(device, sky_light_face, source_mips);
    impl.prefiltered = make_cube(device, sky_light_face, sky_light_mips);
    impl.make_brdf();
    if (!impl.source.texture || !impl.prefiltered.texture || !impl.params || !impl.light) return;
    for (std::uint32_t k = 1; k < source_mips; ++k) {
        impl.down_groups.push_back(impl.make_group(impl.down_layout, {{.binding = 5, .textureView = impl.source.mips[k]},
                                                                      {.binding = 6, .textureView = impl.source.mips[k - 1]}}));
    }
    for (std::uint32_t m = 0; m < sky_light_mips; ++m) {
        impl.prefilter_groups.push_back(impl.make_group(impl.prefilter_layout, {{.binding = 0, .buffer = impl.params, .size = sizeof(Params)},
                                                                                {.binding = 2, .sampler = impl.sampler},
                                                                                {.binding = 5, .textureView = impl.prefiltered.mips[m]},
                                                                                {.binding = 7, .textureView = impl.source.cube}}));
    }
}

SkyLight::~SkyLight() = default;

bool SkyLight::ready() const noexcept {
    const auto& impl = *impl_;
    return impl.shader && impl.sh && impl.source_light && impl.downsample && impl.prefilter && impl.sampler && impl.brdf_view
           && impl.source.cube && impl.prefiltered.cube && impl.down_groups.size() == source_mips - 1
           && impl.prefilter_groups.size() == sky_light_mips;
}

void SkyLight::update(const WGPUCommandEncoder encoder, const WGPUTextureView probe, const SkyLightInput& input) {
    if (!ready() || !probe) return;
    auto& impl = *impl_;
    if (probe != impl.probe) {
        impl.release_probe_groups();
        impl.sh_group = impl.make_group(impl.sh_layout, {{.binding = 0, .buffer = impl.params, .size = sizeof(Params)},
                                                         {.binding = 1, .textureView = probe},
                                                         {.binding = 2, .sampler = impl.sampler},
                                                         {.binding = 3, .buffer = impl.light, .size = sky_light_buffer_size}});
        impl.source_group = impl.make_group(impl.source_layout, {{.binding = 0, .buffer = impl.params, .size = sizeof(Params)},
                                                                 {.binding = 1, .textureView = probe},
                                                                 {.binding = 2, .sampler = impl.sampler},
                                                                 {.binding = 3, .buffer = impl.light, .size = sky_light_buffer_size},
                                                                 {.binding = 5, .textureView = impl.source.mips[0]}});
        impl.probe = probe;
    }
    if (!impl.sh_group || !impl.source_group) return;

    // A slot for each prefiltered mip; the first is the SH's and the source's too.
    std::vector<std::byte> slots(slot_stride * sky_light_mips);
    for (std::uint32_t m = 0; m < sky_light_mips; ++m) {
        Params p{};
        p.to_sun[0] = input.to_sun.x;
        p.to_sun[1] = input.to_sun.y;
        p.to_sun[2] = input.to_sun.z;
        p.to_sun[3] = std::clamp(input.ground_albedo, 0.0f, 1.0f);
        p.sun[0] = (std::max)(input.sun.x, 0.0f);
        p.sun[1] = (std::max)(input.sun.y, 0.0f);
        p.sun[2] = (std::max)(input.sun.z, 0.0f);
        p.sun[3] = sky_light_most;
        p.pass_info[0] = static_cast<float>(sky_light_face >> m);
        p.pass_info[1] = static_cast<float>(m) / static_cast<float>(sky_light_mips - 1);
        p.pass_info[2] = static_cast<float>(sky_light_face);
        p.pass_info[3] = static_cast<float>(source_mips);
        std::memcpy(slots.data() + slot_stride * m, &p, sizeof(p));
    }
    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.params, 0, slots.data(), slots.size());
    wgpuQueueRelease(queue);

    const auto timestamps = compute_pass_timestamps(GpuPass::sky_light);
    const WGPUComputePassDescriptor descriptor{.timestampWrites = timestamps ? &*timestamps : nullptr};
    const auto pass = wgpuCommandEncoderBeginComputePass(encoder, &descriptor);
    const auto groups = [](const std::uint32_t size) { return (size + 7) / 8; };
    std::uint32_t offset = 0;
    wgpuComputePassEncoderSetPipeline(pass, impl.sh);
    wgpuComputePassEncoderSetBindGroup(pass, 0, impl.sh_group, 1, &offset);
    wgpuComputePassEncoderDispatchWorkgroups(pass, 1, 1, 1);
    wgpuComputePassEncoderSetPipeline(pass, impl.source_light);
    wgpuComputePassEncoderSetBindGroup(pass, 0, impl.source_group, 1, &offset);
    wgpuComputePassEncoderDispatchWorkgroups(pass, groups(sky_light_face), groups(sky_light_face), 6);
    wgpuComputePassEncoderSetPipeline(pass, impl.downsample);
    for (std::uint32_t k = 1; k < source_mips; ++k) {
        const auto size = (std::max)(sky_light_face >> k, 1u);
        wgpuComputePassEncoderSetBindGroup(pass, 0, impl.down_groups[k - 1], 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups(size), groups(size), 6);
    }
    wgpuComputePassEncoderSetPipeline(pass, impl.prefilter);
    for (std::uint32_t m = 0; m < sky_light_mips; ++m) {
        const auto size = sky_light_face >> m;
        const auto slot = static_cast<std::uint32_t>(slot_stride * m);
        wgpuComputePassEncoderSetBindGroup(pass, 0, impl.prefilter_groups[m], 1, &slot);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups(size), groups(size), 6);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    impl.updated = true;
}

SkyLightView SkyLight::view() const noexcept {
    if (!impl_ || !impl_->updated) return {};
    return {.prefiltered = impl_->prefiltered.cube, .brdf = impl_->brdf_view, .irradiance = impl_->light};
}

} // namespace openrts::renderer
#endif
