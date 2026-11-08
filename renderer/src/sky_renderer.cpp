#include <openrts/renderer/sky_renderer.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include <glm/mat3x3.hpp>
#include <glm/trigonometric.hpp>

#include <openrts/formats/ace_file.hpp>
#include <openrts/renderer/rts_material.hpp>
#include <openrts/renderer/webgpu_texture.hpp>

namespace openrts::renderer {
namespace {

struct SkyVertex {
    float position[3];
    float uv[2];
};

struct DrawUniform {
    glm::mat4 mvp;
    float shift[4];     // sky: uv offset xy, alpha z
    float colour[4];    // satellite colour
    float azimuth[4];   // satellite azimuth and elevation, radians
    float half_size;    // satellite quad half size
    std::int32_t named_shader;
    std::int32_t draw_flags;
    float padding;
};
constexpr std::uint64_t draw_uniform_stride = 256;
static_assert(sizeof(DrawUniform) <= draw_uniform_stride);

struct FrameUniform {
    float light_diffuse[4];
    float light_ambient[4];
    float fog_colour[4];
    float fog_distance[4];
};

// Ports of RTS sky.vert/frag and satellite.vert/frag.
constexpr char sky_shader[] = R"(
struct Draw {
    mvp: mat4x4<f32>,
    shift: vec4<f32>,
    colour: vec4<f32>,
    azimuth: vec4<f32>,
    half_size: f32,
    named_shader: i32,
    draw_flags: i32,
    padding: f32,
};

struct Frame {
    light_diffuse: vec4<f32>,
    light_ambient: vec4<f32>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
};

@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
@group(1) @binding(0) var<uniform> draw: Draw;
@group(1) @binding(1) var<uniform> frame: Frame;

struct VertexIn {
    @location(0) position: vec3<f32>,
    @location(1) uv: vec2<f32>,
};

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) colour: vec4<f32>,
    @location(2) fog: vec4<f32>,
};

fn fog_for(distance: f32) -> vec4<f32> {
    let fog_start = frame.fog_distance.x;
    let fog_end = frame.fog_distance.y;
    if (fog_end <= 0.0) {
        return vec4<f32>(0.0);
    }
    if (fog_end > fog_start) {
        let amount = clamp(frame.fog_colour.w * ((distance - fog_start) / (fog_end - fog_start)), 0.0, 1.0);
        return vec4<f32>(frame.fog_colour.xyz, amount);
    }
    return frame.fog_colour;
}

fn unlit(shader: i32) -> bool {
    return shader == 1 || shader == 3 || shader == 6;
}

@vertex
fn vs_sky(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    out.uv = input.uv + draw.shift.xy;
    if (!unlit(draw.named_shader)) {
        let diffuse = max(frame.light_diffuse, vec4<f32>(0.0));
        let ambient = max(frame.light_ambient, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        out.colour = max(diffuse, ambient);
    } else {
        out.colour = vec4<f32>(1.0);
    }
    out.colour.w = draw.shift.z;
    out.fog = fog_for(length(input.position));
    return out;
}

@vertex
fn vs_satellite(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    let vertex = vec3<f32>(input.position.xy * draw.half_size, input.position.z);
    let angles = vec4<f32>(cos(-draw.azimuth.x), sin(-draw.azimuth.x), cos(draw.azimuth.y), sin(draw.azimuth.y));
    var scale = 1.0;
    out.fog = vec4<f32>(0.0);

    // The sun grows towards the screen edges and is fogged.
    if ((draw.draw_flags & 1) != 0) {
        let center = vec3<f32>(vertex.z * angles.y * angles.z, vertex.z * angles.w, vertex.z * angles.x * angles.z);
        let length_on_screen = length((draw.mvp * vec4<f32>(center, 1.0)).xy);
        if (length_on_screen > 400.0) {
            scale = min(scale + (length_on_screen - 400.0) * 0.0008, 2.7);
        }
        out.fog = fog_for(length(vertex));
    }

    let position = vec3<f32>(
        (vertex.x * scale * angles.x + vertex.z * angles.y) * angles.z,
        vertex.y * scale * angles.z + vertex.z * angles.w,
        (-vertex.x * scale * angles.y + vertex.z * angles.x) * angles.z);
    out.clip = draw.mvp * vec4<f32>(position, 1.0);
    // openRTS's own sky drawn: the satellite on the far plane (reversed Z),
    // where only the sky is (its pipeline tests the depth for equality).
    if ((draw.draw_flags & 2) != 0) {
        out.clip.z = 0.0;
    }
    out.uv = input.uv;
    if (!unlit(draw.named_shader)) {
        out.colour = draw.colour;
    } else {
        out.colour = vec4<f32>(1.0);
    }
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    var color = textureSample(source_texture, source_sampler, input.uv) * input.colour;
    if (input.fog.w > 0.0) {
        color = vec4<f32>(((1.0 - input.fog.w) * color + input.fog.w * input.fog).xyz, color.w);
    }
    return color;
}
)";

// Satellite quad: RTS draws it at 2000 m with half size 100 * scale.
constexpr float satellite_distance = 2000.0f;
constexpr std::array<SkyVertex, 4> satellite_quad{{
    {{1.0f, 1.0f, satellite_distance}, {0.0f, 1.0f}},
    {{1.0f, -1.0f, satellite_distance}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f, satellite_distance}, {1.0f, 0.0f}},
    {{-1.0f, 1.0f, satellite_distance}, {1.0f, 1.0f}},
}};
constexpr std::array<std::uint32_t, 6> satellite_indices{0, 1, 2, 0, 2, 3};

struct LayerMesh {
    std::vector<SkyVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// Builds a layer exactly as RTS's environment loader (0x55859C) and vertex
// packer (0x565CCC) do, then turns its triangle fan (cap) and triangle strips
// (one per edge step) into a triangle list.
[[nodiscard]] LayerMesh build_layer_mesh(const formats::SkyLayer& layer) {
    LayerMesh mesh;
    const auto faces = static_cast<std::size_t>(layer.top_faces);
    if (faces < 3 || layer.top_radius == 0 || layer.top_height == 0 || layer.edge_steps.empty()) return mesh;

    const auto tiles = layer.shader.frames.empty() ? formats::EnvAnimFrame{} : layer.shader.frames.front();
    const float uv_center[2]{tiles.uv_tiles[0] * 0.5f, tiles.uv_tiles[1] * 0.5f};

    // Raw vertex table: cap centre, cap ring, then inner/outer pairs per step.
    std::vector<SkyVertex> raw;
    raw.push_back({{0.0f, static_cast<float>(layer.top_height), 0.0f}, {uv_center[0], uv_center[1]}});
    constexpr double two_pi = 6.283185307179586;
    for (std::size_t k = 0; k < faces; ++k) {
        const auto angle = -static_cast<double>(k) * two_pi / static_cast<double>(faces);
        const auto s = static_cast<float>(std::sin(angle));
        const auto c = static_cast<float>(std::cos(angle));
        raw.push_back({
            {-s * static_cast<float>(layer.top_radius), static_cast<float>(layer.top_height),
             -c * static_cast<float>(layer.top_radius)},
            {s * uv_center[0] + uv_center[0], c * uv_center[1] + uv_center[1]},
        });
    }

    // Step m reads (height, radius) of step m-1 (the cap for m = 1) and m.
    std::vector<formats::SkyEdgeStep> levels{{.height = layer.top_height, .radius = layer.top_radius}};
    levels.insert(levels.end(), layer.edge_steps.begin(), layer.edge_steps.end());
    std::size_t previous_block = 0;
    for (std::size_t m = 1; m < levels.size(); ++m) {
        const auto radius_ratio = static_cast<float>(levels[m].radius) / static_cast<float>(levels[m - 1].radius);
        const auto uv_scale = static_cast<float>(std::abs(levels[m - 1].height - levels[m].height))
                / static_cast<float>(layer.top_height)
            + radius_ratio;
        for (std::size_t n = 1; n <= faces; ++n) {
            const auto source = raw[m == 1 ? previous_block + n : previous_block + 2 * n - 1];
            raw.push_back(source);
            raw.push_back({
                {radius_ratio * source.position[0], static_cast<float>(levels[m].height),
                 radius_ratio * source.position[2]},
                {(source.uv[0] - uv_center[0]) * uv_scale + uv_center[0],
                 (source.uv[1] - uv_center[1]) * uv_scale + uv_center[1]},
            });
        }
        previous_block += m == 1 ? faces + 1 : 2 * faces;
    }

    // Cap fan: centre, ring, first ring vertex again.
    for (std::size_t i = 0; i <= faces; ++i) mesh.vertices.push_back(raw[i]);
    mesh.vertices.push_back(raw[1]);
    for (std::uint32_t i = 1; i <= faces; ++i) mesh.indices.insert(mesh.indices.end(), {0u, i, i + 1});

    // One strip per step: its pairs plus the first pair again.
    std::size_t next = faces + 1;
    for (std::size_t step = 0; step < layer.edge_steps.size(); ++step) {
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        const auto block = next;
        for (std::size_t k = 0; k < 2 * faces; ++k) mesh.vertices.push_back(raw[next++]);
        mesh.vertices.push_back(raw[block]);
        mesh.vertices.push_back(raw[block + 1]);
        const auto strip_length = static_cast<std::uint32_t>(2 * faces + 2);
        for (std::uint32_t i = 0; i + 2 < strip_length; ++i) {
            mesh.indices.insert(mesh.indices.end(), {first + i, first + i + 1, first + i + 2});
        }
    }
    return mesh;
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

void copy_vec4(float (&target)[4], const glm::vec4& value) {
    target[0] = value.x;
    target[1] = value.y;
    target[2] = value.z;
    target[3] = value.w;
}

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

enum class SkyProgram { layer, satellite };

} // namespace

struct SkyRenderer::Impl {
    struct GpuMesh {
        WGPUBuffer vertices = nullptr;
        WGPUBuffer indices = nullptr;
        std::uint32_t index_count = 0;
    };

    // A textured sky element: one per layer and satellite.
    struct Element {
        GpuMesh mesh; // empty for satellites, which share the quad
        WgpuTextureUploadResult texture;
        WGPUTextureView view = nullptr;
        WGPUSampler sampler = nullptr;
        WGPUBindGroup binding = nullptr;
        rts::NamedShader shader = rts::NamedShader::unknown;
    };

    WGPUDevice device = nullptr;
    WGPUBindGroupLayout material_layout = nullptr;
    WGPUBindGroupLayout frame_layout = nullptr;
    // [program][blend]
    std::array<std::array<WGPURenderPipeline, 2>, 2> pipelines{};
    // Satellites behind the scene, over openRTS's own sky ([blend]).
    std::array<WGPURenderPipeline, 2> behind_pipelines{};
    GpuMesh quad;
    WGPUBuffer frame_buffer = nullptr;
    WGPUBuffer draw_buffer = nullptr;
    std::uint64_t draw_capacity = 0;
    WGPUBindGroup frame_binding = nullptr;

    const formats::EnvironmentDefinition* environment = nullptr;
    std::vector<Element> layers;
    std::vector<Element> satellites;

    // The draw uniforms in slots of draw_capacity each: the frame's sky (0)
    // and the reflection probe's faces (1..6), as every slot's writes reach
    // the queue before the frame's commands run.
    static constexpr std::uint64_t draw_slots = 7;

    // The reflection probe (openRTS): a cube of the target format, drawn
    // multisampled as the sky's pipelines are, resolved into each face.
    static constexpr std::uint32_t probe_size = 128;
    WGPUTexture probe_texture = nullptr;
    WGPUTextureView probe_view = nullptr;
    std::array<WGPUTextureView, 6> probe_faces{};
    WGPUTexture probe_msaa = nullptr;
    WGPUTextureView probe_msaa_view = nullptr;
    WGPUTexture probe_depth = nullptr;
    WGPUTextureView probe_depth_view = nullptr;
    bool probe_drawn = false; // recorded once: it is the sky from the next frame on

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        for (const auto face : probe_faces) {
            if (face) wgpuTextureViewRelease(face);
        }
        if (probe_view) wgpuTextureViewRelease(probe_view);
        if (probe_texture) wgpuTextureRelease(probe_texture);
        if (probe_msaa_view) wgpuTextureViewRelease(probe_msaa_view);
        if (probe_msaa) wgpuTextureRelease(probe_msaa);
        if (probe_depth_view) wgpuTextureViewRelease(probe_depth_view);
        if (probe_depth) wgpuTextureRelease(probe_depth);
        release_elements(layers);
        release_elements(satellites);
        release_mesh(quad);
        if (frame_binding) wgpuBindGroupRelease(frame_binding);
        if (draw_buffer) wgpuBufferRelease(draw_buffer);
        if (frame_buffer) wgpuBufferRelease(frame_buffer);
        for (const auto& program : pipelines) {
            for (const auto pipeline : program) {
                if (pipeline) wgpuRenderPipelineRelease(pipeline);
            }
        }
        for (const auto pipeline : behind_pipelines) {
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
        }
        if (frame_layout) wgpuBindGroupLayoutRelease(frame_layout);
        if (material_layout) wgpuBindGroupLayoutRelease(material_layout);
    }

    static void release_mesh(GpuMesh& mesh) {
        if (mesh.indices) wgpuBufferRelease(mesh.indices);
        if (mesh.vertices) wgpuBufferRelease(mesh.vertices);
        mesh = {};
    }

    static void release_elements(std::vector<Element>& elements) {
        for (auto& element : elements) {
            if (element.binding) wgpuBindGroupRelease(element.binding);
            if (element.sampler) wgpuSamplerRelease(element.sampler);
            if (element.view) wgpuTextureViewRelease(element.view);
            release_mesh(element.mesh);
        }
        elements.clear();
    }

    [[nodiscard]] GpuMesh upload_mesh(const std::vector<SkyVertex>& vertices, const std::vector<std::uint32_t>& indices) const {
        GpuMesh mesh;
        if (vertices.empty() || indices.empty()) return mesh;
        mesh.vertices = create_buffer(device, WGPUBufferUsage_Vertex, vertices.size() * sizeof(SkyVertex), vertices.data());
        mesh.indices = create_buffer(device, WGPUBufferUsage_Index, indices.size() * sizeof(std::uint32_t), indices.data());
        mesh.index_count = static_cast<std::uint32_t>(indices.size());
        return mesh;
    }

    // Loads the element's texture; returns false when it cannot be drawn.
    [[nodiscard]] bool prepare_element(Element& element,
                                       const formats::EnvShader& shader,
                                       const core::Vfs& vfs,
                                       const std::vector<core::VirtualPath>& directories) const {
        element.shader = rts::named_shader_from_environment_name(shader.shader_name);

        std::optional<core::VirtualPath> path;
        for (const auto& directory : directories) {
            const auto candidate = directory.resolve(std::string_view(shader.texture));
            if (candidate && vfs.exists(*candidate)) {
                path = candidate;
                break;
            }
        }
        if (!path) {
            std::cout << "sky texture '" << shader.texture << "': not found\n";
            return false;
        }
        auto decoded = formats::decode_ace_rgba(vfs, *path);
        if (!decoded.ok()) {
            std::cout << "sky texture '" << shader.texture << "': " << decoded.error << '\n';
            return false;
        }
        element.texture = upload_rgba8_texture(
            device,
            {.width = decoded.image->width, .height = decoded.image->height, .pixels = &decoded.image->pixels,
             .mips = &decoded.image->mips},
            shader.texture);
        if (!element.texture.ok()) return false;
        element.view = wgpuTextureCreateView(element.texture.texture.get(), nullptr);

        // terrain_texslot addressing 2 clamps; RTS sets only the linear MAG filter.
        const auto address = shader.texture_addressing == 2 ? WGPUAddressMode_ClampToEdge : WGPUAddressMode_Repeat;
        const WGPUSamplerDescriptor sampler{
            .addressModeU = address,
            .addressModeV = address,
            .addressModeW = address,
            .magFilter = WGPUFilterMode_Linear,
            .minFilter = WGPUFilterMode_Linear,
            .mipmapFilter = WGPUMipmapFilterMode_Linear,
            .lodMaxClamp = 32.0f,
            .maxAnisotropy = 1,
        };
        element.sampler = wgpuDeviceCreateSampler(device, &sampler);

        std::array<WGPUBindGroupEntry, 2> entries{};
        entries[0].binding = 0;
        entries[0].textureView = element.view;
        entries[1].binding = 1;
        entries[1].sampler = element.sampler;
        const WGPUBindGroupDescriptor descriptor{
            .layout = material_layout,
            .entryCount = entries.size(),
            .entries = entries.data(),
        };
        element.binding = wgpuDeviceCreateBindGroup(device, &descriptor);
        return element.binding != nullptr;
    }

    void ensure_draw_capacity(const std::uint64_t count) {
        if (count <= draw_capacity && draw_buffer) return;
        auto capacity = (std::max)(draw_capacity, std::uint64_t{16});
        while (capacity < count) capacity *= 2;

        if (frame_binding) wgpuBindGroupRelease(frame_binding);
        if (draw_buffer) wgpuBufferRelease(draw_buffer);
        draw_buffer = create_buffer(device, WGPUBufferUsage_Uniform, capacity * draw_slots * draw_uniform_stride);
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

    void create_probe(const WGPUTextureFormat format) {
        const auto samples = render_sample_count();
        const WGPUTextureDescriptor cube{
            .label = {.data = "sky-reflection-probe", .length = WGPU_STRLEN},
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = probe_size, .height = probe_size, .depthOrArrayLayers = 6},
            .format = format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        probe_texture = wgpuDeviceCreateTexture(device, &cube);
        if (!probe_texture) return;
        const WGPUTextureViewDescriptor cube_view{
            .format = format,
            .dimension = WGPUTextureViewDimension_Cube,
            .baseMipLevel = 0,
            .mipLevelCount = 1,
            .baseArrayLayer = 0,
            .arrayLayerCount = 6,
            .aspect = WGPUTextureAspect_All,
        };
        probe_view = wgpuTextureCreateView(probe_texture, &cube_view);
        for (std::uint32_t face = 0; face < 6; ++face) {
            const WGPUTextureViewDescriptor face_view{
                .format = format,
                .dimension = WGPUTextureViewDimension_2D,
                .baseMipLevel = 0,
                .mipLevelCount = 1,
                .baseArrayLayer = face,
                .arrayLayerCount = 1,
                .aspect = WGPUTextureAspect_All,
            };
            probe_faces[face] = wgpuTextureCreateView(probe_texture, &face_view);
        }
        if (samples > 1) {
            const WGPUTextureDescriptor msaa{
                .usage = WGPUTextureUsage_RenderAttachment,
                .dimension = WGPUTextureDimension_2D,
                .size = {.width = probe_size, .height = probe_size, .depthOrArrayLayers = 1},
                .format = format,
                .mipLevelCount = 1,
                .sampleCount = samples,
            };
            probe_msaa = wgpuDeviceCreateTexture(device, &msaa);
            if (probe_msaa) probe_msaa_view = wgpuTextureCreateView(probe_msaa, nullptr);
        }
        const WGPUTextureDescriptor depth{
            .usage = WGPUTextureUsage_RenderAttachment,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = probe_size, .height = probe_size, .depthOrArrayLayers = 1},
            .format = depth_format,
            .mipLevelCount = 1,
            .sampleCount = samples,
        };
        probe_depth = wgpuDeviceCreateTexture(device, &depth);
        if (probe_depth) probe_depth_view = wgpuTextureCreateView(probe_depth, nullptr);
    }

    // The sky through `mvp` (no translation) into a pass, its uniforms in `slot`.
    void draw(WGPURenderPassEncoder pass, const glm::mat4& mvp, const rts::SkyState& state, std::uint64_t slot, bool moons_only = false);

    [[nodiscard]] bool probe_ready() const noexcept {
        return probe_view && probe_depth_view && (render_sample_count() == 1 || probe_msaa_view)
               && std::all_of(probe_faces.begin(), probe_faces.end(), [](const auto face) { return face != nullptr; });
    }
};

namespace {

[[nodiscard]] WGPURenderPipeline create_pipeline(const WGPUDevice device,
                                                 const WGPUPipelineLayout layout,
                                                 const WGPUShaderModule shader,
                                                 const WGPUTextureFormat target_format,
                                                 const SkyProgram program,
                                                 const bool blend,
                                                 const WGPUCompareFunction compare = WGPUCompareFunction_Always) {
    const std::array<WGPUVertexAttribute, 2> attributes{{
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(SkyVertex, position), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(SkyVertex, uv), .shaderLocation = 1},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(SkyVertex),
        .attributeCount = attributes.size(),
        .attributes = attributes.data(),
    };

    constexpr WGPUStencilFaceState keep_stencil{
        .compare = WGPUCompareFunction_Always,
        .failOp = WGPUStencilOperation_Keep,
        .depthFailOp = WGPUStencilOperation_Keep,
        .passOp = WGPUStencilOperation_Keep,
    };
    // RTS draws the sky with GL_DEPTH_TEST disabled, which also disables writes.
    const WGPUDepthStencilState depth{
        .format = depth_format,
        .depthWriteEnabled = WGPUOptionalBool_False,
        .depthCompare = compare,
        .stencilFront = keep_stencil,
        .stencilBack = keep_stencil,
        .stencilReadMask = ~0u,
        .stencilWriteMask = ~0u,
    };

    constexpr WGPUBlendComponent alpha_blend_component{
        .operation = WGPUBlendOperation_Add,
        .srcFactor = WGPUBlendFactor_SrcAlpha,
        .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
    };
    constexpr WGPUBlendState alpha_blend{.color = alpha_blend_component, .alpha = alpha_blend_component};
    const WGPUColorTargetState target{
        .format = target_format,
        .blend = blend ? &alpha_blend : nullptr,
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
            .entryPoint = wgsl_name(program == SkyProgram::layer ? "vs_sky" : "vs_satellite"),
            .bufferCount = 1,
            .buffers = &vertex_layout,
        },
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = scene_multisample(),
        .fragment = &fragment,
    };
    return wgpuDeviceCreateRenderPipeline(device, &descriptor);
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

} // namespace

SkyRenderer::SkyRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
    impl_->material_layout = create_material_layout(device);
    impl_->frame_layout = create_frame_layout(device);
    impl_->frame_buffer = create_buffer(device, WGPUBufferUsage_Uniform, sizeof(FrameUniform));
    impl_->quad = impl_->upload_mesh({satellite_quad.begin(), satellite_quad.end()},
                                     {satellite_indices.begin(), satellite_indices.end()});

    const WGPUShaderSourceWGSL source{
        .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code = wgsl_name(sky_shader),
    };
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);

    const std::array<WGPUBindGroupLayout, 2> layouts{impl_->material_layout, impl_->frame_layout};
    const WGPUPipelineLayoutDescriptor layout_descriptor{
        .bindGroupLayoutCount = layouts.size(),
        .bindGroupLayouts = layouts.data(),
    };
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
    for (const auto program : {SkyProgram::layer, SkyProgram::satellite}) {
        for (const bool blend : {false, true}) {
            impl_->pipelines[static_cast<std::size_t>(program)][blend ? 1 : 0] =
                create_pipeline(device, pipeline_layout, shader, target_format, program, blend);
        }
    }
    for (const bool blend : {false, true}) {
        impl_->behind_pipelines[blend ? 1 : 0] =
            create_pipeline(device, pipeline_layout, shader, target_format, SkyProgram::satellite, blend, WGPUCompareFunction_Equal);
    }
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
    impl_->create_probe(target_format);
}

SkyRenderer::~SkyRenderer() = default;
SkyRenderer::SkyRenderer(SkyRenderer&&) noexcept = default;
SkyRenderer& SkyRenderer::operator=(SkyRenderer&&) noexcept = default;

bool SkyRenderer::ready() const noexcept {
    if (!impl_ || !impl_->material_layout || !impl_->frame_layout || !impl_->frame_buffer || !impl_->quad.vertices) {
        return false;
    }
    return std::all_of(impl_->pipelines.begin(), impl_->pipelines.end(), [](const auto& program) {
        return program[0] && program[1];
    });
}

void SkyRenderer::load(const formats::EnvironmentDefinition& environment,
                       const core::Vfs& vfs,
                       const std::vector<core::VirtualPath>& texture_directories) {
    Impl::release_elements(impl_->layers);
    Impl::release_elements(impl_->satellites);
    impl_->environment = &environment;

    for (const auto& layer : environment.sky_layers) {
        Impl::Element element;
        const auto mesh = build_layer_mesh(layer);
        if (impl_->prepare_element(element, layer.shader, vfs, texture_directories)) {
            element.mesh = impl_->upload_mesh(mesh.vertices, mesh.indices);
        }
        impl_->layers.push_back(std::move(element));
    }
    for (const auto& satellite : environment.satellites) {
        Impl::Element element;
        static_cast<void>(impl_->prepare_element(element, satellite.shader, vfs, texture_directories));
        impl_->satellites.push_back(std::move(element));
    }
}

void SkyRenderer::Impl::draw(const WGPURenderPassEncoder pass, const glm::mat4& mvp, const rts::SkyState& state,
                             const std::uint64_t slot, const bool moons_only) {
    auto* const self = this;

    struct EmittedDraw {
        const Impl::GpuMesh* mesh;
        WGPUBindGroup binding;
        SkyProgram program;
        bool blend;
        DrawUniform uniform;
    };
    std::vector<EmittedDraw> emitted;

    const auto emit_satellites = [&] {
        for (std::size_t i = 0; i < self->satellites.size() && i < state.satellites.size(); ++i) {
            const auto& element = self->satellites[i];
            const auto& satellite = state.satellites[i];
            if (!element.binding || !satellite.visible) continue;
            // Over openRTS's sky the lighting satellite (the sun) is its own.
            if (moons_only && i == 0) continue;

            DrawUniform uniform{.mvp = mvp};
            copy_vec4(uniform.colour, rts::colour_from_argb(satellite.direct_colour));
            uniform.azimuth[0] = glm::radians(satellite.azimuth_degrees);
            uniform.azimuth[1] = glm::radians(satellite.elevation_degrees);
            uniform.half_size = 100.0f * satellite.scale * (i == 0 ? 1.5f : 1.0f);
            uniform.named_shader = static_cast<std::int32_t>(element.shader);
            uniform.draw_flags = moons_only ? 2 : (i == 0 ? 1 : 0);
            emitted.push_back({&self->quad, element.binding, SkyProgram::satellite,
                               rts::is_alpha_shader(element.shader), uniform});
        }
    };

    const auto layer_count = self->layers.size();
    const std::size_t satellites_after = self->environment->layers_behind_satellites + 1u;
    if (moons_only) emit_satellites();
    for (std::size_t j = 0; j <= layer_count && !moons_only; ++j) {
        if (j == satellites_after) emit_satellites();
        if (j == layer_count) break;

        const auto& element = self->layers[j];
        if (!element.binding || !element.mesh.vertices || j >= state.layers.size()) continue;
        const auto& layer = state.layers[j];
        if (!layer.visible) continue;

        DrawUniform uniform{.mvp = mvp};
        uniform.shift[0] = layer.uv_offset[0];
        uniform.shift[1] = layer.uv_offset[1];
        uniform.shift[2] = layer.fading ? layer.alpha : 1.0f;
        uniform.shift[3] = 1.0f;
        uniform.named_shader = static_cast<std::int32_t>(element.shader);
        // Opaque layers blend only while fading.
        const bool blend = rts::is_alpha_shader(element.shader) || layer.fading;
        emitted.push_back({&element.mesh, element.binding, SkyProgram::layer, blend, uniform});
    }
    if (emitted.empty()) return;

    self->ensure_draw_capacity(emitted.size());
    if (!self->frame_binding) return;

    const auto base = slot * self->draw_capacity * draw_uniform_stride;
    std::vector<std::byte> uniforms(emitted.size() * draw_uniform_stride);
    for (std::size_t i = 0; i < emitted.size(); ++i) {
        std::memcpy(uniforms.data() + i * draw_uniform_stride, &emitted[i].uniform, sizeof(DrawUniform));
    }
    FrameUniform frame{};
    copy_vec4(frame.light_diffuse, state.lighting.sun_diffuse);
    copy_vec4(frame.light_ambient, state.lighting.sun_ambient);
    copy_vec4(frame.fog_colour, state.fog_colour);
    frame.fog_distance[0] = state.fog_start;
    frame.fog_distance[1] = state.fog_end;

    const auto queue = wgpuDeviceGetQueue(self->device);
    wgpuQueueWriteBuffer(queue, self->draw_buffer, base, uniforms.data(), uniforms.size());
    wgpuQueueWriteBuffer(queue, self->frame_buffer, 0, &frame, sizeof(frame));
    if (queue) wgpuQueueRelease(queue);

    for (std::size_t i = 0; i < emitted.size(); ++i) {
        const auto& item = emitted[i];
        const auto offset = static_cast<std::uint32_t>(base + i * draw_uniform_stride);
        wgpuRenderPassEncoderSetPipeline(pass, moons_only ? self->behind_pipelines[item.blend ? 1 : 0]
                                                          : self->pipelines[static_cast<std::size_t>(item.program)][item.blend ? 1 : 0]);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, item.mesh->vertices, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(pass, item.mesh->indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, item.binding, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(pass, 1, self->frame_binding, 1, &offset);
        wgpuRenderPassEncoderDrawIndexed(pass, item.mesh->index_count, 1, 0, 0, 0);
    }
}

void SkyRenderer::render(const WGPURenderPassEncoder pass,
                         const glm::mat4& projection,
                         const glm::mat4& view,
                         const rts::SkyState& state) {
    if (!ready() || !impl_->environment) return;
    // The sky is centred on the camera: drop the view translation.
    impl_->draw(pass, projection * glm::mat4{glm::mat3{view}}, state, 0);
}

void SkyRenderer::render_satellites(const WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view,
                                    const rts::SkyState& state, const std::uint32_t slot) {
    if (!ready() || !impl_->environment || slot >= Impl::draw_slots || !impl_->behind_pipelines[0] || !impl_->behind_pipelines[1]) return;
    impl_->draw(pass, projection * glm::mat4{glm::mat3{view}}, state, slot, true);
}

void SkyRenderer::render_reflection_probe(const WGPUCommandEncoder encoder, const rts::SkyState& state, const ProbeDraw& draw) {
    if (!ready() || !impl_->environment || !impl_->probe_ready()) return;
    auto& impl = *impl_;
    // Each face as a cube map lays it out (WebGPU, as Vulkan and D3D): the
    // direction it looks along, and those its image's right and top show.
    // A cube is left-handed, so the view is a mirror (rows: right, up, back).
    struct Face {
        glm::vec3 forward, right, up;
    };
    static constexpr std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    }};
    const auto projection = reversed_z_perspective(glm::radians(90.0f), 1.0f, 1.0f, 100000.0f);
    const bool msaa = impl.probe_msaa_view != nullptr;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const auto& face = faces[f];
        glm::mat4 view{1.0f};
        for (int column = 0; column < 3; ++column) {
            view[column][0] = face.right[column];
            view[column][1] = face.up[column];
            view[column][2] = -face.forward[column];
        }
        const WGPURenderPassColorAttachment colour{
            .view = msaa ? impl.probe_msaa_view : impl.probe_faces[f],
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .resolveTarget = msaa ? impl.probe_faces[f] : nullptr,
            .loadOp = WGPULoadOp_Clear,
            .storeOp = msaa ? WGPUStoreOp_Discard : WGPUStoreOp_Store,
            .clearValue = {.r = state.clear_colour.r, .g = state.clear_colour.g, .b = state.clear_colour.b, .a = 1.0},
        };
        const WGPURenderPassDepthStencilAttachment depth{
            .view = impl.probe_depth_view,
            .depthLoadOp = WGPULoadOp_Clear,
            .depthStoreOp = WGPUStoreOp_Discard,
            .depthClearValue = depth_clear_value,
            .depthReadOnly = false,
            .stencilLoadOp = WGPULoadOp_Undefined,
            .stencilStoreOp = WGPUStoreOp_Undefined,
            .stencilReadOnly = true,
        };
        const WGPURenderPassDescriptor descriptor{
            .colorAttachmentCount = 1,
            .colorAttachments = &colour,
            .depthStencilAttachment = &depth,
        };
        const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        if (draw) {
            draw(pass, projection, view, static_cast<std::uint32_t>(1 + f));
        } else {
            impl.draw(pass, projection * view, state, 1 + f);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }
    impl.probe_drawn = true;
}

WGPUTextureView SkyRenderer::reflection_probe() const noexcept {
    return impl_ && impl_->probe_drawn ? impl_->probe_view : nullptr;
}

} // namespace openrts::renderer
#endif
