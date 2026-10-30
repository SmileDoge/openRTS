#include <openrts/renderer/model_renderer.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <openrts/formats/ace_file.hpp>
#include <openrts/renderer/gpu_retire_queue.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/rts_material.hpp>
#include <openrts/renderer/webgpu_texture.hpp>
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

// Per-draw block, addressed with a dynamic offset into the frame buffer.
struct DrawUniform {
    glm::mat4 mvp;
    glm::mat4 model;
    glm::mat4 normal_matrix; // transpose(inverse(model)), as the RTS shader computes it
    std::int32_t light_material;
    std::int32_t named_shader;
    std::uint32_t alpha_test;
    float lod_bias;
};
constexpr std::uint64_t draw_uniform_stride = 256; // minUniformBufferOffsetAlignment
static_assert(sizeof(DrawUniform) <= draw_uniform_stride);

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

constexpr std::size_t max_lights = 10; // RTS config Shader.NumLights

struct FrameUniform {
    GpuLight lights[max_lights];
    float fog_colour[4];
    float fog_distance[4]; // x: start, y: end (<= 0 disables)
};

// Bindings and types shared by both shading paths.
constexpr char shader_interface[] = R"(
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

struct Frame {
    lights: array<Light, 10>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
};

struct Draw {
    mvp: mat4x4<f32>,
    model: mat4x4<f32>,
    normal_matrix: mat4x4<f32>,
    light_material: i32,
    named_shader: i32,
    alpha_test: u32,
    lod_bias: f32,
};

@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
@group(1) @binding(0) var<uniform> draw: Draw;
@group(1) @binding(1) var<uniform> frame: Frame;

struct VertexIn {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) colour1: vec4<f32>,
    @location(4) colour2: vec4<f32>,
};

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world: vec4<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) colour: vec4<f32>,
    @location(4) fog: vec4<f32>,
};

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
fn transformed_normal(input_normal: vec3<f32>) -> vec3<f32> {
    if (draw.light_material == -9) { // Circumference
        return vec3<f32>(0.0, 1.0, 0.0);
    }
    return (draw.normal_matrix * vec4<f32>(input_normal, 1.0)).xyz;
}

// Port of RTS shape.vert.
@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.world = draw.model * vec4<f32>(input.position, 1.0);
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    out.uv = input.uv;
    out.normal = vec3<f32>(0.0);

    let shader = draw.named_shader;
    let material = draw.light_material;
    let unlit_shader = shader == 1 || shader == 3 || shader == 6;
    var luminance = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    var ambient = vec4<f32>(0.0);

    if (!unlit_shader && material != -8) {
        var diffuse = vec4<f32>(0.0);
        if (material == -11) { // HalfBright
            ambient = input.colour1;
        } else if (material == -1) { // vertex colour emission
            luminance = input.colour2;
        }
        out.normal = transformed_normal(input.normal);

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
        if (frame.lights[0].kind.x > 0.0) {
            out.normal = transformed_normal(input.normal);
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

// Port of RTS shape.frag (without the interior light block).
@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSampleBias(source_texture, source_sampler, input.uv, draw.lod_bias);
    let shader = draw.named_shader;
    let alpha_shader = shader == 3 || shader == 4 || shader == 6 || shader == 7;

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
        let extra_lights = min(i32(frame.lights[0].kind.x), 9);
        for (var i = 1; i <= extra_lights; i = i + 1) {
            let factor = light_factor(frame.lights[i], input.world, normal);
            if (factor < 0.0) { continue; }
            light_colour = max(light_colour, frame.lights[i].diffuse * factor);
            specular = max(specular, frame.lights[i].diffuse * factor * specular_factor);
        }

        specular.w = 0.0;
        color = texel * max(input.colour, light_colour) + specular;
        if (input.fog.w > 0.0) {
            color = vec4<f32>(((1.0 - input.fog.w) * color + input.fog.w * input.fog).xyz, color.w);
        }
    }

    // Fixed-function alpha test of RTS: GL_GREATER 0.3.
    if (draw.alpha_test != 0u && !(color.w > 0.3)) {
        discard;
    }
    return color;
}
)";

// openRTS's own shading: normalised Lambert sun lighting with ambient term.
// Extension point for PBR materials and shadow mapping.
constexpr char experimental_shader_body[] = R"(
@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.world = draw.model * vec4<f32>(input.position, 1.0);
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    out.uv = input.uv;
    out.normal = (draw.normal_matrix * vec4<f32>(input.normal, 0.0)).xyz;
    out.colour = vec4<f32>(1.0);
    out.fog = fog_for(out.clip.z);
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSampleBias(source_texture, source_sampler, input.uv, draw.lod_bias);
    if (draw.alpha_test != 0u && texel.w < 0.5) {
        discard;
    }

    let shader = draw.named_shader;
    let unlit = shader == 1 || shader == 3 || shader == 6 || draw.light_material == -8;
    var lighting = vec3<f32>(1.0);
    if (!unlit) {
        let sun = frame.lights[0];
        let n = normalize(input.normal);
        let diffuse = max(dot(n, -normalize(sun.direction)), 0.0);
        lighting = min(sun.ambient.xyz + sun.diffuse.xyz * diffuse, vec3<f32>(1.0));
    }

    var color = vec4<f32>(texel.xyz * lighting, texel.w);
    if (input.fog.w > 0.0) {
        color = vec4<f32>(mix(color.xyz, input.fog.xyz, input.fog.w), color.w);
    }
    return color;
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

[[nodiscard]] FrameUniform frame_uniform(const ModelLighting& lighting) {
    FrameUniform result{};
    auto& sun = result.lights[0];
    copy_vec4(sun.diffuse, lighting.sun_diffuse);
    copy_vec4(sun.ambient, lighting.sun_ambient);
    sun.direction[0] = lighting.sun_direction.x;
    sun.direction[1] = lighting.sun_direction.y;
    sun.direction[2] = lighting.sun_direction.z;
    sun.type[0] = 0.0f; // no additional light sources yet
    copy_vec4(result.fog_colour, lighting.fog_colour);
    result.fog_distance[0] = lighting.fog_start;
    result.fog_distance[1] = lighting.fog_end;
    return result;
}

[[nodiscard]] WGPUBindGroupLayout create_material_layout(const WGPUDevice device) {
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

[[nodiscard]] WGPUBindGroupLayout create_frame_layout(const WGPUDevice device) {
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.hasDynamicOffset = true;
    entries[0].buffer.minBindingSize = sizeof(DrawUniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = sizeof(FrameUniform);

    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

[[nodiscard]] WGPUSampler create_sampler(const WGPUDevice device, const rts::TextureFilter filter) {
    using rts::TextureFilter;
    // RTS sets GL_TEXTURE_MIN_FILTER and _MAG_FILTER to the same value; GL rejects
    // mipmapped values for magnification, which then stays linear.
    auto mag = WGPUFilterMode_Linear;
    auto min = WGPUFilterMode_Linear;
    auto mip = WGPUMipmapFilterMode_Linear;
    switch (filter) {
    case TextureFilter::nearest:
        mag = min = WGPUFilterMode_Nearest;
        mip = WGPUMipmapFilterMode_Nearest;
        break;
    case TextureFilter::linear:
        mip = WGPUMipmapFilterMode_Nearest;
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
        .lodMaxClamp = 32.0f,
        .maxAnisotropy = 1,
    };
    return wgpuDeviceCreateSampler(device, &descriptor);
}

[[nodiscard]] WGPUShaderModule create_shader_module(const WGPUDevice device, const char* body) {
    const auto code = std::string(shader_interface) + body;
    const WGPUShaderSourceWGSL source{
        .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code = {.data = code.data(), .length = code.size()},
    };
    const WGPUShaderModuleDescriptor descriptor{.nextInChain = &source.chain};
    return wgpuDeviceCreateShaderModule(device, &descriptor);
}

// Pipeline variants correspond to the fixed-function state RTS toggles per
// primitive: GL_BLEND and glDepthMask. Alpha test is a shader uniform.
struct PipelineKey {
    bool blend = false;
    bool depth_write = true;

    [[nodiscard]] std::size_t index() const noexcept { return (blend ? 2u : 0u) + (depth_write ? 1u : 0u); }
};
constexpr std::size_t pipeline_variant_count = 4;
constexpr std::size_t shading_path_count = 2;

[[nodiscard]] std::size_t path_index(const ShadingPath path) noexcept {
    return path == ShadingPath::rts_compatible ? 0 : 1;
}

[[nodiscard]] WGPURenderPipeline create_pipeline(const WGPUDevice device,
                                                 const WGPUPipelineLayout layout,
                                                 const WGPUShaderModule shader,
                                                 const WGPUTextureFormat target_format,
                                                 const PipelineKey key) {
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
        .format = WGPUTextureFormat_Depth24Plus,
        .depthWriteEnabled = key.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False,
        .depthCompare = WGPUCompareFunction_LessEqual,
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
    const WGPUFragmentState fragment{
        .module = shader,
        .entryPoint = wgsl_name("fs_main"),
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
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = {.count = 1, .mask = ~0u},
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
        return {.center = center, .radius = glm::length(max - center)};
    }
};

// How the texture of an image index is to be drawn.
enum class TextureAvailability {
    uploaded,
    missing,     // file not found: RTS draws with an unbound (black) texture
    unsupported, // openRTS could not decode it: the primitive is not drawn
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
        glm::mat4 world{1.0f};
        glm::vec3 center{};
    };

    core::VirtualPath source;
    std::vector<std::string> dependencies;
    std::shared_ptr<const GpuGeometry> geometry;
    std::vector<std::shared_ptr<const GpuTexture>> textures; // by shape image index
    std::vector<TextureAvailability> availability;
    std::vector<Draw> draws;
    std::uint16_t model_flags = 0;
    ModelBounds bounds{};

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        for (auto& draw : draws) {
            if (draw.binding) wgpuBindGroupRelease(draw.binding);
        }
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

struct ModelRenderer::Impl {
    struct QueuedModel {
        RenderModel::Impl* model = nullptr;
        ModelTransform transform{};
    };

    WGPUDevice device = nullptr;
    WGPUBindGroupLayout material_layout = nullptr;
    WGPUBindGroupLayout frame_layout = nullptr;
    std::array<std::array<WGPURenderPipeline, pipeline_variant_count>, shading_path_count> pipelines{};
    ShadingPath shading = ShadingPath::rts_compatible;
    std::map<rts::TextureFilter, WGPUSampler> samplers;
    WgpuTextureUploadResult black_texture;
    WGPUTextureView black_view = nullptr;

    // Per-frame uniforms: one DrawUniform slot per emitted draw plus the lights.
    WGPUBuffer draw_buffer = nullptr;
    std::uint64_t draw_capacity = 0;
    WGPUBuffer frame_buffer = nullptr;
    WGPUBindGroup frame_binding = nullptr;

    ModelFrameContext frame{};
    std::vector<QueuedModel> queue;
    // Generations drawn this frame stay alive until the next begin_frame().
    std::vector<std::shared_ptr<const RenderModel::Impl>> frame_models;

    // Uploaded textures by virtual path key; models hold the strong references.
    std::unordered_map<std::string, std::weak_ptr<const GpuTexture>> texture_cache;
    GpuRetireQueue retired;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        // Retired generations may still be in flight: wait for the GPU.
        if (device) wgpuDevicePoll(device, true, nullptr);
        retired.drain();
        if (frame_binding) wgpuBindGroupRelease(frame_binding);
        if (frame_buffer) wgpuBufferRelease(frame_buffer);
        if (draw_buffer) wgpuBufferRelease(draw_buffer);
        if (black_view) wgpuTextureViewRelease(black_view);
        for (const auto& [_, sampler] : samplers) {
            if (sampler) wgpuSamplerRelease(sampler);
        }
        for (const auto& variants : pipelines) {
            for (const auto pipeline : variants) {
                if (pipeline) wgpuRenderPipelineRelease(pipeline);
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

    void ensure_draw_capacity(std::uint64_t draw_count);

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

void ModelRenderer::Impl::ensure_draw_capacity(const std::uint64_t draw_count) {
    if (draw_count <= draw_capacity && draw_buffer) return;

    auto capacity = (std::max)(draw_capacity, std::uint64_t{256});
    while (capacity < draw_count) capacity *= 2;

    if (frame_binding) wgpuBindGroupRelease(frame_binding);
    if (draw_buffer) wgpuBufferRelease(draw_buffer);
    draw_buffer = create_buffer(device, WGPUBufferUsage_Uniform, capacity * draw_uniform_stride);
    draw_capacity = capacity;

    std::array<WGPUBindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].buffer = draw_buffer;
    entries[0].size = sizeof(DrawUniform);
    entries[1].binding = 1;
    entries[1].buffer = frame_buffer;
    entries[1].size = sizeof(FrameUniform);
    const WGPUBindGroupDescriptor descriptor{
        .layout = frame_layout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    frame_binding = wgpuDeviceCreateBindGroup(device, &descriptor);
}

std::shared_ptr<const GpuTexture> ModelRenderer::Impl::upload(const core::VirtualPath& path,
                                                              const formats::AceRgbaImage& image) const {
    auto texture = std::make_shared<GpuTexture>();
    texture->texture = upload_rgba8_texture(device, {.width = image.width, .height = image.height, .pixels = &image.pixels},
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

    BoundsAccumulator bounds;
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

        std::array<WGPUBindGroupEntry, 2> entries{};
        entries[0].binding = 0;
        entries[0].textureView = *view;
        entries[1].binding = 1;
        entries[1].sampler = sampler(draw.material.filter);
        const WGPUBindGroupDescriptor descriptor{
            .layout = material_layout,
            .entryCount = entries.size(),
            .entries = entries.data(),
        };
        draw.binding = wgpuDeviceCreateBindGroup(device, &descriptor);
        if (draw.binding) gpu.draws.push_back(draw);
    }
    gpu.bounds = bounds.sphere();
}

ModelRenderer::ModelRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
    impl_->material_layout = create_material_layout(device);
    impl_->frame_layout = create_frame_layout(device);
    impl_->frame_buffer = create_buffer(device, WGPUBufferUsage_Uniform, sizeof(FrameUniform));

    static const std::vector<std::uint8_t> black_pixel{0, 0, 0, 255};
    impl_->black_texture = upload_rgba8_texture(device, {.width = 1, .height = 1, .pixels = &black_pixel}, "rts-unbound");
    if (impl_->black_texture.ok()) impl_->black_view = wgpuTextureCreateView(impl_->black_texture.texture.get(), nullptr);

    const std::array<WGPUBindGroupLayout, 2> layouts{impl_->material_layout, impl_->frame_layout};
    const WGPUPipelineLayoutDescriptor layout_descriptor{
        .bindGroupLayoutCount = layouts.size(),
        .bindGroupLayouts = layouts.data(),
    };
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);

    for (const auto path : {ShadingPath::rts_compatible, ShadingPath::experimental}) {
        const auto shader = create_shader_module(
            device, path == ShadingPath::rts_compatible ? rts_shader_body : experimental_shader_body);
        for (const bool blend : {false, true}) {
            for (const bool depth_write : {false, true}) {
                const PipelineKey key{.blend = blend, .depth_write = depth_write};
                impl_->pipelines[path_index(path)][key.index()] =
                    create_pipeline(device, pipeline_layout, shader, target_format, key);
            }
        }
        wgpuShaderModuleRelease(shader);
    }
    wgpuPipelineLayoutRelease(pipeline_layout);
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

RenderModel ModelRenderer::create_model(StreamedModelAsset&& asset) {
    RenderModel result;
    if (!ready() || !asset.ok()) return result;
    const auto& model = asset.model;
    if (model.vertices.empty() || model.indices.empty()) return result;

    auto gpu = std::make_shared<RenderModel::Impl>();
    gpu->source = asset.source_path;
    gpu->dependencies = asset.dependency_keys();

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

ModelRendererStats ModelRenderer::stats() const {
    if (!impl_) return {};
    std::size_t live = 0;
    for (const auto& [_, texture] : impl_->texture_cache) live += texture.expired() ? 0 : 1;
    return {
        .cached_textures = live,
        .retired_pending = impl_->retired.pending(),
        .completed_frame = impl_->retired.completed_frame(),
    };
}

void ModelRenderer::begin_frame(const ModelFrameContext& frame) {
    if (!impl_) return;
    impl_->frame = frame;
    impl_->queue.clear();
    impl_->frame_models.clear();
}

void ModelRenderer::submit(RenderModel& model, const ModelTransform& transform) {
    if (!impl_ || !model.impl_) return;
    if (impl_->frame_models.empty() || impl_->frame_models.back() != model.impl_) impl_->frame_models.push_back(model.impl_);
    impl_->queue.push_back({.model = model.impl_.get(), .transform = transform});
}

void ModelRenderer::submit(RenderModel& model) {
    submit(model, ModelTransform{1.0f});
}

// Builds the frame's draw list for the active shading path, uploads all
// per-draw uniforms at once and records the draws.
void ModelRenderer::render(const WGPURenderPassEncoder pass) {
    if (!ready()) return;

    struct EmittedDraw {
        const RenderModel::Impl* model;
        const RenderModel::Impl::Draw* draw;
        PipelineKey pipeline;
        DrawUniform uniform;
    };
    std::vector<EmittedDraw> emitted;

    const auto emit = [&](const Impl::QueuedModel& submitted, const RenderModel::Impl::Draw& draw,
                          const PipelineKey pipeline, const bool alpha_test) {
        const auto model_matrix = submitted.transform * draw.world;
        emitted.push_back({
            .model = submitted.model,
            .draw = &draw,
            .pipeline = pipeline,
            .uniform = {
                .mvp = impl_->frame.view_projection * model_matrix,
                .model = model_matrix,
                .normal_matrix = glm::transpose(glm::inverse(model_matrix)),
                .light_material = draw.material.light_material,
                .named_shader = static_cast<std::int32_t>(draw.material.shader_uniform),
                .alpha_test = alpha_test ? 1u : 0u,
                .lod_bias = draw.material.lod_bias,
            },
        });
    };

    if (impl_->shading == ShadingPath::rts_compatible) {
        // RTS static object passes: every model is visited in pass 1 and, if it
        // has translucent primitives, again in pass 2. Blend and alpha test carry
        // over between primitives exactly as the GL state did; nothing is sorted.
        auto state = rts::object_pass_entry_state;
        for (const int object_pass_number : {rts::opaque_pass, rts::translucent_pass}) {
            for (const auto& submitted : impl_->queue) {
                const auto& model = *submitted.model;
                const auto object_pass = rts::object_pass(model.model_flags);
                for (const auto& draw : model.draws) {
                    if (!rts::draws_in_pass(object_pass_number, object_pass, model.model_flags, draw.material.flags)) {
                        continue;
                    }
                    state = rts::apply_material(draw.material, state);
                    emit(submitted, draw,
                         {.blend = state.blend, .depth_write = draw.material.depth_write}, state.alpha_test);
                }
            }
        }
    } else {
        // Experimental: explicit per-material state, every primitive drawn,
        // opaque and alpha-tested first, blended geometry back to front.
        struct Translucent {
            Impl::QueuedModel submitted;
            const RenderModel::Impl::Draw* draw;
            float distance2;
        };
        std::vector<Translucent> translucent;
        for (const bool masked_pass : {false, true}) {
            for (const auto& submitted : impl_->queue) {
                for (const auto& draw : submitted.model->draws) {
                    const auto& material = draw.material;
                    const bool blend_shader = material.shader == rts::NamedShader::blend_a_tex
                        || material.shader == rts::NamedShader::blend_a_tex_diff;
                    const bool blended = blend_shader && !material.alpha_test_mode;
                    if (blended) {
                        if (masked_pass) continue;
                        const auto center = glm::vec3{submitted.transform * glm::vec4{draw.center, 1.0f}};
                        const auto delta = center - impl_->frame.camera_position;
                        translucent.push_back({submitted, &draw, glm::dot(delta, delta)});
                        continue;
                    }
                    const bool masked = blend_shader && material.alpha_test_mode;
                    if (masked != masked_pass) continue;
                    emit(submitted, draw,
                         {.blend = false, .depth_write = material.depth_write}, masked);
                }
            }
        }
        std::stable_sort(translucent.begin(), translucent.end(),
                         [](const auto& a, const auto& b) { return a.distance2 > b.distance2; });
        for (const auto& item : translucent) {
            emit(item.submitted, *item.draw, {.blend = true, .depth_write = false}, false);
        }
    }
    if (emitted.empty()) return;

    impl_->ensure_draw_capacity(emitted.size());
    if (!impl_->frame_binding) return;

    std::vector<std::byte> uniforms(emitted.size() * draw_uniform_stride);
    for (std::size_t i = 0; i < emitted.size(); ++i) {
        std::memcpy(uniforms.data() + i * draw_uniform_stride, &emitted[i].uniform, sizeof(DrawUniform));
    }
    const auto lighting = frame_uniform(impl_->frame.lighting);
    const auto queue = wgpuDeviceGetQueue(impl_->device);
    wgpuQueueWriteBuffer(queue, impl_->draw_buffer, 0, uniforms.data(), uniforms.size());
    wgpuQueueWriteBuffer(queue, impl_->frame_buffer, 0, &lighting, sizeof(lighting));
    if (queue) wgpuQueueRelease(queue);

    std::optional<std::size_t> bound_pipeline;
    const RenderModel::Impl* bound_model = nullptr;
    for (std::size_t i = 0; i < emitted.size(); ++i) {
        const auto& item = emitted[i];
        if (bound_pipeline != item.pipeline.index()) {
            bound_pipeline = item.pipeline.index();
            wgpuRenderPassEncoderSetPipeline(pass, impl_->pipelines[path_index(impl_->shading)][*bound_pipeline]);
        }
        if (bound_model != item.model) {
            bound_model = item.model;
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, bound_model->geometry->vertices, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetIndexBuffer(pass, bound_model->geometry->indices, WGPUIndexFormat_Uint32, 0,
                                                WGPU_WHOLE_SIZE);
        }

        const auto offset = static_cast<std::uint32_t>(i * draw_uniform_stride);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, item.draw->binding, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(pass, 1, impl_->frame_binding, 1, &offset);
        wgpuRenderPassEncoderDrawIndexed(pass, item.draw->range.index_count, 1, item.draw->range.first_index, 0, 0);
    }
}

void ModelRenderer::draw(const WGPURenderPassEncoder pass, RenderModel& model, const ModelFrameContext& frame) {
    begin_frame(frame);
    submit(model);
    render(pass);
}

} // namespace openrts::renderer
#endif
