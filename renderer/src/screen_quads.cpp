#include <openrts/renderer/screen_quads.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>

#include <array>
#include <cstddef>

namespace openrts::renderer {
namespace {

constexpr const char* screen_shader = R"(
struct Frame {
    view_projection: mat4x4<f32>,
    eye: vec4<f32>,
};
@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var screen_texture: texture_2d<f32>;
@group(0) @binding(2) var screen_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
    @location(1) uv: vec2<f32>,
};

@vertex
fn vs_main(@location(0) position: vec3<f32>, @location(1) uv: vec2<f32>, @location(2) colour: vec4<f32>) -> VertexOut {
    let to_eye = frame.eye.xyz - position;
    let length_to_eye = max(length(to_eye), 0.001);
    let lifted = position + to_eye / length_to_eye * 0.001;
    var out: VertexOut;
    out.position = frame.view_projection * vec4<f32>(lifted, 1.0);
    out.colour = colour;
    out.uv = uv;
    return out;
}

// Half a mip level sharper than the plain choice: a screen's small print
// is read from the seat (testers, 2026-11-12: the ЭП20's БЛОК was blurry).
@fragment
fn fs_blend(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSampleBias(screen_texture, screen_sampler, input.uv, -0.5) * input.colour;
    return texel;
}

@fragment
fn fs_opaque(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSampleBias(screen_texture, screen_sampler, input.uv, -0.5) * input.colour;
    return vec4<f32>(texel.rgb, 1.0);
}
)";

struct FrameUniform {
    glm::mat4 view_projection;
    glm::vec4 eye;
};

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

} // namespace

struct ScreenQuadRenderer::Impl {
    struct Texture {
        WgpuTexture texture;
        WGPUTextureView view = nullptr;
        WGPUBindGroup group = nullptr;
    };
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline blend = nullptr;
    WGPURenderPipeline opaque = nullptr;
    WGPUBuffer uniform = nullptr;
    WGPUSampler sampler = nullptr;
    std::vector<Texture> textures; // [0] unused: ids start at 1
    WGPUBuffer vertices = nullptr;
    std::uint64_t capacity = 0;
    std::vector<ScreenQuadDraw> draws;

    ~Impl() {
        for (auto& texture : textures) {
            if (texture.group) wgpuBindGroupRelease(texture.group);
            if (texture.view) wgpuTextureViewRelease(texture.view);
        }
        if (vertices) wgpuBufferRelease(vertices);
        if (sampler) wgpuSamplerRelease(sampler);
        if (uniform) wgpuBufferRelease(uniform);
        if (blend) wgpuRenderPipelineRelease(blend);
        if (opaque) wgpuRenderPipelineRelease(opaque);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }

    WGPURenderPipeline make_pipeline(const WGPUShaderModule shader, const WGPUPipelineLayout pipeline_layout,
                                     const WGPUTextureFormat format, const bool blended) const {
        const std::array<WGPUVertexAttribute, 3> attributes{{
            {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(ScreenQuadVertex, position), .shaderLocation = 0},
            {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(ScreenQuadVertex, uv), .shaderLocation = 1},
            {.format = WGPUVertexFormat_Unorm8x4, .offset = offsetof(ScreenQuadVertex, colour), .shaderLocation = 2},
        }};
        const WGPUVertexBufferLayout vertex_layout{
            .stepMode = WGPUVertexStepMode_Vertex,
            .arrayStride = sizeof(ScreenQuadVertex),
            .attributeCount = attributes.size(),
            .attributes = attributes.data(),
        };
        constexpr WGPUStencilFaceState keep{
            .compare = WGPUCompareFunction_Always,
            .failOp = WGPUStencilOperation_Keep,
            .depthFailOp = WGPUStencilOperation_Keep,
            .passOp = WGPUStencilOperation_Keep,
        };
        const WGPUDepthStencilState depth{
            .format = depth_format,
            .depthWriteEnabled = WGPUOptionalBool_False,
            .depthCompare = depth_nearer_or_equal,
            .stencilFront = keep,
            .stencilBack = keep,
            .stencilReadMask = ~0u,
            .stencilWriteMask = ~0u,
        };
        const WGPUBlendState alpha{
            .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_SrcAlpha,
                      .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
            .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One,
                      .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        };
        const WGPUColorTargetState target{.format = format, .blend = blended ? &alpha : nullptr, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader,
                                         .entryPoint = wgsl_name(blended ? "fs_blend" : "fs_opaque"),
                                         .targetCount = 1,
                                         .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = pipeline_layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main"), .bufferCount = 1, .buffers = &vertex_layout},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .depthStencil = &depth,
            .multisample = scene_multisample(),
            .fragment = &fragment,
        };
        return wgpuDeviceCreateRenderPipeline(device, &descriptor);
    }
};

ScreenQuadRenderer::ScreenQuadRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    impl.textures.emplace_back();
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(FrameUniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor uniform_descriptor{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                                                  .size = sizeof(FrameUniform)};
    impl.uniform = wgpuDeviceCreateBuffer(device, &uniform_descriptor);
    WGPUSamplerDescriptor sampler{};
    sampler.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler.addressModeW = WGPUAddressMode_ClampToEdge;
    sampler.magFilter = WGPUFilterMode_Linear;
    sampler.minFilter = WGPUFilterMode_Linear;
    // Mipmapped as every RTS texture (0x44150C: GL_LINEAR_MIPMAP_LINEAR; the
    // ACE's levels come with the image, Rgba8ImageView::mips).
    sampler.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampler.lodMaxClamp = 32.0f;
    // openRTS: anisotropic, as the screens are seen at a slant from the seat
    // (RTS samples them as any texture, without it).
    sampler.maxAnisotropy = 16;
    impl.sampler = wgpuDeviceCreateSampler(device, &sampler);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(screen_shader)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    impl.blend = impl.make_pipeline(shader, pipeline_layout, target_format, true);
    impl.opaque = impl.make_pipeline(shader, pipeline_layout, target_format, false);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
}

ScreenQuadRenderer::~ScreenQuadRenderer() = default;

bool ScreenQuadRenderer::ready() const noexcept {
    return impl_->blend && impl_->opaque && impl_->layout && impl_->uniform && impl_->sampler;
}

std::uint32_t ScreenQuadRenderer::add_texture(const Rgba8ImageView image) {
    auto& impl = *impl_;
    auto upload = upload_rgba8_texture(impl.device, image, "cab screen");
    if (!upload.ok()) return 0;
    Impl::Texture texture{.texture = std::move(upload.texture)};
    texture.view = wgpuTextureCreateView(texture.texture.get(), nullptr);
    std::array<WGPUBindGroupEntry, 3> entries{};
    entries[0].binding = 0;
    entries[0].buffer = impl.uniform;
    entries[0].size = sizeof(FrameUniform);
    entries[1].binding = 1;
    entries[1].textureView = texture.view;
    entries[2].binding = 2;
    entries[2].sampler = impl.sampler;
    const WGPUBindGroupDescriptor descriptor{.layout = impl.layout, .entryCount = entries.size(), .entries = entries.data()};
    texture.group = wgpuDeviceCreateBindGroup(impl.device, &descriptor);
    impl.textures.push_back(std::move(texture));
    return static_cast<std::uint32_t>(impl.textures.size() - 1);
}

void ScreenQuadRenderer::set(std::vector<ScreenQuadVertex> vertices, std::vector<ScreenQuadDraw> draws) {
    auto& impl = *impl_;
    std::erase_if(draws, [&](const ScreenQuadDraw& d) {
        return d.texture == 0 || d.texture >= impl.textures.size() || d.count == 0 ||
               static_cast<std::size_t>(d.first) + d.count > vertices.size();
    });
    impl.draws = std::move(draws);
    if (impl.draws.empty()) return;
    const auto bytes = static_cast<std::uint64_t>(vertices.size()) * sizeof(ScreenQuadVertex);
    if (bytes > impl.capacity) {
        if (impl.vertices) wgpuBufferRelease(impl.vertices);
        impl.capacity = bytes * 2;
        const WGPUBufferDescriptor descriptor{.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, .size = impl.capacity};
        impl.vertices = wgpuDeviceCreateBuffer(impl.device, &descriptor);
    }
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.vertices, 0, vertices.data(), bytes);
        wgpuQueueRelease(queue);
    }
}

void ScreenQuadRenderer::render(const WGPURenderPassEncoder pass, const glm::mat4& view_projection, const glm::vec3& eye) {
    auto& impl = *impl_;
    if (!ready() || impl.draws.empty() || !impl.vertices) return;
    const FrameUniform frame{.view_projection = view_projection, .eye = glm::vec4{eye, 0.0f}};
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.uniform, 0, &frame, sizeof(frame));
        wgpuQueueRelease(queue);
    }
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, impl.vertices, 0, WGPU_WHOLE_SIZE);
    WGPURenderPipeline current = nullptr;
    for (const auto& draw : impl.draws) {
        const auto pipeline = draw.blend ? impl.blend : impl.opaque;
        if (pipeline != current) {
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            current = pipeline;
        }
        wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.textures[draw.texture].group, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, draw.count, 1, draw.first, 0);
    }
}

} // namespace openrts::renderer
#endif
