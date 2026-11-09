#include <openrts/renderer/glow_sprites.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <glm/matrix.hpp>

namespace openrts::renderer {
namespace {

constexpr const char* glow_shader = R"(
struct Frame {
    view_projection: mat4x4<f32>,
    right: vec4<f32>,  // xyz camera right, w: a pixel's size a metre away (m)
    up: vec4<f32>,     // xyz camera up
    eye: vec4<f32>,    // xyz eye, w: field of view / 45 degrees
};
@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var light_texture: texture_2d<f32>;
@group(0) @binding(2) var light_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
    @location(1) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32, @location(0) centre: vec3<f32>, @location(1) radius: f32,
           @location(2) uv_rect: vec4<f32>, @location(3) colour: vec4<f32>, @location(4) far_scale: f32,
           @location(5) axis_x: vec3<f32>, @location(6) axis_y: vec3<f32>, @location(7) min_pixels: f32,
           @location(8) spread_light: f32) -> VertexOut {
    var corners = array<vec2<f32>, 6>(vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                      vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    let corner = corners[index];
    let to_eye = frame.eye.xyz - centre;
    let distance = length(to_eye);
    let own = max(radius, distance * far_scale) * frame.eye.w;
    let size = max(own, distance * min_pixels * frame.right.w);
    var x = axis_x;
    var y = axis_y;
    if (dot(x, x) < 0.5) {
        x = frame.right.xyz;
        y = frame.up.xyz;
    }
    let world = centre + (x * corner.x + y * corner.y) * size;
    var out: VertexOut;
    out.position = frame.view_projection * vec4<f32>(world, 1.0);
    out.colour = colour;
    // Made bigger than its own size: its light spread over the bigger quad.
    let kept = own / max(size, 1e-6);
    out.colour.a = colour.a * mix(1.0, kept * kept, clamp(spread_light, 0.0, 1.0));
    let t = corner * 0.5 + vec2(0.5, 0.5);
    out.uv = vec2(mix(uv_rect.x, uv_rect.z, t.x), mix(uv_rect.w, uv_rect.y, t.y));
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(light_texture, light_sampler, input.uv);
    let intensity = texel.a * input.colour.a;
    // Premultiplied: added (rgb x alpha) or laid over by its alpha.
    return vec4<f32>(texel.rgb * input.colour.rgb * intensity, intensity);
}
)";

struct FrameUniform {
    glm::mat4 view_projection;
    glm::vec4 right;
    glm::vec4 up;
    glm::vec4 eye;
};

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

// The built-in sprite: a white disc with a soft edge.
[[nodiscard]] std::vector<std::uint8_t> round_glow(std::uint32_t size) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            const float dy = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float core = std::clamp((0.55f - r) / 0.2f, 0.0f, 1.0f);
            const float halo = std::clamp(1.0f - r, 0.0f, 1.0f);
            const float alpha = std::clamp(core + halo * halo * 0.35f, 0.0f, 1.0f);
            auto* p = &pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<std::uint8_t>(alpha * 255.0f);
        }
    }
    return pixels;
}

} // namespace

struct GlowSpriteRenderer::Impl {
    struct Texture {
        WgpuTexture texture;
        WGPUTextureView view = nullptr;
        WGPUBindGroup group = nullptr;
    };
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;      // GlowBlend::added
    WGPURenderPipeline over_pipeline = nullptr; // GlowBlend::over
    WGPUBuffer uniform = nullptr;
    WGPUSampler sampler = nullptr;
    std::vector<Texture> textures;
    WGPUBuffer instances = nullptr;
    std::uint64_t capacity = 0;
    float size_factor = 65.0f / 45.0f;
    std::vector<GlowSprite> sprites;
    // Per texture: first sprite and count, after sorting.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;

    ~Impl() {
        for (auto& texture : textures) {
            if (texture.group) wgpuBindGroupRelease(texture.group);
            if (texture.view) wgpuTextureViewRelease(texture.view);
        }
        if (instances) wgpuBufferRelease(instances);
        if (sampler) wgpuSamplerRelease(sampler);
        if (uniform) wgpuBufferRelease(uniform);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (over_pipeline) wgpuRenderPipelineRelease(over_pipeline);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }

    std::uint32_t add(const Rgba8ImageView image) {
        auto upload = upload_rgba8_texture(device, image, "light sprite");
        if (!upload.ok()) return 0;
        Texture texture{.texture = std::move(upload.texture)};
        texture.view = wgpuTextureCreateView(texture.texture.get(), nullptr);
        std::array<WGPUBindGroupEntry, 3> entries{};
        entries[0].binding = 0;
        entries[0].buffer = uniform;
        entries[0].size = sizeof(FrameUniform);
        entries[1].binding = 1;
        entries[1].textureView = texture.view;
        entries[2].binding = 2;
        entries[2].sampler = sampler;
        const WGPUBindGroupDescriptor descriptor{.layout = layout, .entryCount = entries.size(), .entries = entries.data()};
        texture.group = wgpuDeviceCreateBindGroup(device, &descriptor);
        textures.push_back(std::move(texture));
        return static_cast<std::uint32_t>(textures.size() - 1);
    }
};

GlowSpriteRenderer::GlowSpriteRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
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
    sampler.maxAnisotropy = 1;
    impl.sampler = wgpuDeviceCreateSampler(device, &sampler);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(glow_shader)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const std::array<WGPUVertexAttribute, 9> attributes{{
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(GlowSprite, position), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Float32, .offset = offsetof(GlowSprite, radius), .shaderLocation = 1},
        {.format = WGPUVertexFormat_Float32x4, .offset = offsetof(GlowSprite, uv), .shaderLocation = 2},
        {.format = WGPUVertexFormat_Unorm8x4, .offset = offsetof(GlowSprite, colour), .shaderLocation = 3},
        {.format = WGPUVertexFormat_Float32, .offset = offsetof(GlowSprite, far_scale), .shaderLocation = 4},
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(GlowSprite, axis_x), .shaderLocation = 5},
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(GlowSprite, axis_y), .shaderLocation = 6},
        {.format = WGPUVertexFormat_Float32, .offset = offsetof(GlowSprite, min_pixels), .shaderLocation = 7},
        {.format = WGPUVertexFormat_Float32, .offset = offsetof(GlowSprite, spread_light), .shaderLocation = 8},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Instance,
        .arrayStride = sizeof(GlowSprite),
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
    const WGPUBlendState additive{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_One},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_Zero, .dstFactor = WGPUBlendFactor_One},
    };
    // RTS's light loop (0x449818) blends with the frame's standing
    // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA: premultiplied, One / OneMinusSrcAlpha.
    const WGPUBlendState over{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_Zero, .dstFactor = WGPUBlendFactor_One},
    };
    for (const auto* blend : {&additive, &over}) {
        const WGPUColorTargetState target{.format = target_format, .blend = blend, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_main"), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = pipeline_layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main"), .bufferCount = 1, .buffers = &vertex_layout},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .depthStencil = &depth,
            .multisample = scene_multisample(),
            .fragment = &fragment,
        };
        (blend == &additive ? impl.pipeline : impl.over_pipeline) = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    }
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);

    // Texture 0: the built-in round glow.
    const auto pixels = round_glow(64);
    impl.add({.width = 64, .height = 64, .pixels = &pixels});
}

GlowSpriteRenderer::~GlowSpriteRenderer() = default;

bool GlowSpriteRenderer::ready() const noexcept {
    return impl_->pipeline && impl_->over_pipeline && !impl_->textures.empty();
}

std::uint32_t GlowSpriteRenderer::add_texture(const Rgba8ImageView image) {
    return impl_->add(image);
}

void GlowSpriteRenderer::set_sprites(std::vector<GlowSprite> sprites) {
    auto& impl = *impl_;
    for (auto& sprite : sprites) {
        if (sprite.texture >= impl.textures.size()) sprite.texture = 0;
    }
    std::stable_sort(sprites.begin(), sprites.end(), [](const GlowSprite& a, const GlowSprite& b) { return a.texture < b.texture; });
    impl.ranges.clear();
    for (std::uint32_t i = 0; i < sprites.size(); ++i) {
        if (impl.ranges.empty() || sprites[impl.ranges.back().first].texture != sprites[i].texture) impl.ranges.emplace_back(i, 0);
        ++impl.ranges.back().second;
    }
    impl.sprites = std::move(sprites);
    if (impl.sprites.empty()) return;
    const auto bytes = static_cast<std::uint64_t>(impl.sprites.size()) * sizeof(GlowSprite);
    if (bytes > impl.capacity) {
        if (impl.instances) wgpuBufferRelease(impl.instances);
        impl.capacity = bytes * 2;
        const WGPUBufferDescriptor descriptor{.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, .size = impl.capacity};
        impl.instances = wgpuDeviceCreateBuffer(impl.device, &descriptor);
    }
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.instances, 0, impl.sprites.data(), bytes);
        wgpuQueueRelease(queue);
    }
}

void GlowSpriteRenderer::render(const WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view,
                                const float viewport_height, const GlowBlend blend) {
    auto& impl = *impl_;
    if (!ready() || impl.sprites.empty()) return;
    const glm::mat4 inverse = glm::inverse(view);
    FrameUniform frame{
        .view_projection = projection * view,
        .right = glm::vec4{view[0][0], view[1][0], view[2][0], 0.0f},
        .up = glm::vec4{view[0][1], view[1][1], view[2][1], 0.0f},
        .eye = glm::vec4{glm::vec3{inverse[3]}, 0.0f},
    };
    // A pixel's size a metre away: the view's height over the pixels (the
    // zoom included).
    frame.right.w = 2.0f / ((std::max)(projection[1][1], 1e-4f) * (std::max)(viewport_height, 1.0f));
    // RTS's fov / 45 at its installed FOV of 65 degrees, fixed (see set_sprites).
    frame.eye.w = impl.size_factor;
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.uniform, 0, &frame, sizeof(frame));
        wgpuQueueRelease(queue);
    }
    wgpuRenderPassEncoderSetPipeline(pass, blend == GlowBlend::over ? impl.over_pipeline : impl.pipeline);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, impl.instances, 0, WGPU_WHOLE_SIZE);
    for (const auto& [first, count] : impl.ranges) {
        wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.textures[impl.sprites[first].texture].group, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 6, count, 0, first);
    }
}

void GlowSpriteRenderer::set_size_factor(const float factor) noexcept {
    if (impl_) impl_->size_factor = factor;
}

} // namespace openrts::renderer
#endif
