#include <openrts/renderer/screen_quads.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace openrts::renderer {
namespace {

// Straight onto the panel, as RTS draws them (and the supersampled path off).
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

// A screen's objects into its own image (RGBA8UnormSrgb: the blending and
// the filtering in linear light), colour premultiplied by alpha.
constexpr const char* content_shader = R"(
struct Surface {
    area: vec4<f32>, // the part of the screen the image covers: x0 y0 x1 y1 in its 0..1
};
@group(0) @binding(0) var<uniform> surface: Surface;
@group(1) @binding(0) var source: texture_2d<f32>;
@group(1) @binding(1) var source_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
    @location(1) uv: vec2<f32>,
};

@vertex
fn vs_content(@location(1) uv: vec2<f32>, @location(2) colour: vec4<f32>, @location(3) local: vec2<f32>) -> VertexOut {
    let t = (local - surface.area.xy) / (surface.area.zw - surface.area.xy);
    var out: VertexOut;
    out.position = vec4<f32>(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    out.colour = colour;
    out.uv = uv;
    return out;
}

// The display's encoding to linear light (sRGB), as the image stores it.
fn to_linear(c: vec3<f32>) -> vec3<f32> {
    return select(pow((c + 0.055) / 1.055, vec3<f32>(2.4)), c / 12.92, c <= vec3<f32>(0.04045));
}

// The texture premultiplied in linear light (add_texture's premultiplied);
// the tint is display-encoded, as RTS's vertex colour.
@fragment
fn fs_content_blend(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(source, source_sampler, input.uv);
    let tint = to_linear(input.colour.rgb);
    return vec4<f32>(texel.rgb * tint * input.colour.a, texel.a * input.colour.a);
}

// Opaque shaders (Tex): the plain texture's colour, alpha left out.
@fragment
fn fs_content_opaque(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(source, source_sampler, input.uv);
    return vec4<f32>(to_linear(texel.rgb * input.colour.rgb), 1.0);
}
)";

// A mip level from the one above: the bilinear tap in the middle of each
// 2 x 2 block is their average, in linear light (an sRGB image).
constexpr const char* down_shader = R"(
@group(0) @binding(0) var above: texture_2d<f32>;
@group(0) @binding(1) var above_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_full(@builtin(vertex_index) index: u32) -> VertexOut {
    let p = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u)) * 2.0 - 1.0;
    var out: VertexOut;
    out.position = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

@fragment
fn fs_down(input: VertexOut) -> @location(0) vec4<f32> {
    return textureSampleLevel(above, above_sampler, input.uv, 0.0);
}
)";

// A screen's image on the panel: linear premultiplied back to the display's
// encoding, blended as RTS blends (straight alpha, display values).
constexpr const char* composite_shader = R"(
struct Frame {
    view_projection: mat4x4<f32>,
    eye: vec4<f32>,
};
@group(0) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(0) var image: texture_2d<f32>;
@group(1) @binding(1) var image_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_composite(@location(0) position: vec3<f32>, @location(1) uv: vec2<f32>) -> VertexOut {
    let to_eye = frame.eye.xyz - position;
    let length_to_eye = max(length(to_eye), 0.001);
    let lifted = position + to_eye / length_to_eye * 0.001;
    var out: VertexOut;
    out.position = frame.view_projection * vec4<f32>(lifted, 1.0);
    out.uv = uv;
    return out;
}

fn to_display(c: vec3<f32>) -> vec3<f32> {
    return select(1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - 0.055, c * 12.92, c <= vec3<f32>(0.0031308));
}

// Half a mip level sharper, as the straight path.
@fragment
fn fs_composite(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSampleBias(image, image_sampler, input.uv, -0.5);
    let straight = min(texel.rgb / max(texel.a, 0.00001), vec3<f32>(1.0));
    return vec4<f32>(to_display(straight), texel.a);
}
)";

constexpr WGPUTextureFormat image_format = WGPUTextureFormat_RGBA8UnormSrgb;
// A screen's image: about this many pixels a metre of the screen, within the
// sides below (the БЛОК, 0.23 m: 1135 x 865; on a Full HD monitor it covers
// some 185 pixels across).
constexpr float pixels_per_metre = 5000.0f;
constexpr std::uint32_t min_side = 64;
constexpr std::uint32_t max_side = 2048;

struct FrameUniform {
    glm::mat4 view_projection;
    glm::vec4 eye;
};

struct CompositeVertex {
    glm::vec3 position{};
    glm::vec2 uv{};
};

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

[[nodiscard]] WGPUShaderModule shader_module(const WGPUDevice device, const char* code) {
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(code)};
    const WGPUShaderModuleDescriptor descriptor{.nextInChain = &source.chain};
    return wgpuDeviceCreateShaderModule(device, &descriptor);
}

[[nodiscard]] float srgb_to_linear(const float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Linear light to an sRGB byte, through a table fine enough at the dark end.
class SrgbEncoder {
public:
    SrgbEncoder() {
        for (std::size_t i = 0; i < table_.size(); ++i) {
            const float l = static_cast<float>(i) / static_cast<float>(table_.size() - 1);
            const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            table_[i] = static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
        }
    }
    [[nodiscard]] std::uint8_t operator()(const float linear) const {
        const float clamped = std::clamp(linear, 0.0f, 1.0f);
        return table_[static_cast<std::size_t>(clamped * static_cast<float>(table_.size() - 1) + 0.5f)];
    }

private:
    std::array<std::uint8_t, 16384> table_{};
};

} // namespace

std::vector<std::vector<std::uint8_t>> premultiplied_linear_levels(const std::uint32_t width, const std::uint32_t height,
                                                                   const std::vector<std::uint8_t>& pixels) {
    std::vector<std::vector<std::uint8_t>> levels;
    if (width == 0 || height == 0 || pixels.size() != static_cast<std::size_t>(width) * height * 4) return levels;
    std::array<float, 256> decode{};
    for (std::size_t i = 0; i < decode.size(); ++i) decode[i] = srgb_to_linear(static_cast<float>(i) / 255.0f);
    static const SrgbEncoder encode;
    // Level 0 in floats (linear, premultiplied), kept for the mipmaps.
    std::vector<float> level(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const float a = static_cast<float>(pixels[i + 3]) / 255.0f;
        for (std::size_t c = 0; c < 3; ++c) level[i + c] = decode[pixels[i + c]] * a;
        level[i + 3] = a;
    }
    auto bytes_of = [&](const std::vector<float>& f) {
        std::vector<std::uint8_t> out(f.size());
        for (std::size_t i = 0; i < f.size(); i += 4) {
            for (std::size_t c = 0; c < 3; ++c) out[i + c] = encode(f[i + c]);
            out[i + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(f[i + 3], 0.0f, 1.0f) * 255.0f));
        }
        return out;
    };
    levels.push_back(bytes_of(level));
    std::uint32_t w = width, h = height;
    while (w > 1 || h > 1) {
        const std::uint32_t nw = (std::max)(w / 2, 1u), nh = (std::max)(h / 2, 1u);
        std::vector<float> next(static_cast<std::size_t>(nw) * nh * 4);
        for (std::uint32_t y = 0; y < nh; ++y) {
            const std::uint32_t y0 = (std::min)(y * 2, h - 1), y1 = (std::min)(y * 2 + 1, h - 1);
            for (std::uint32_t x = 0; x < nw; ++x) {
                const std::uint32_t x0 = (std::min)(x * 2, w - 1), x1 = (std::min)(x * 2 + 1, w - 1);
                for (std::size_t c = 0; c < 4; ++c) {
                    const auto at = [&](const std::uint32_t px, const std::uint32_t py) {
                        return level[(static_cast<std::size_t>(py) * w + px) * 4 + c];
                    };
                    next[(static_cast<std::size_t>(y) * nw + x) * 4 + c] = (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1)) * 0.25f;
                }
            }
        }
        level = std::move(next);
        w = nw;
        h = nh;
        levels.push_back(bytes_of(level));
    }
    return levels;
}

struct ScreenQuadRenderer::Impl {
    struct Texture {
        WgpuTexture texture;
        WGPUTextureView view = nullptr;
        WGPUBindGroup direct = nullptr;  // the straight path: frame, texture, sampler
        WGPUBindGroup content = nullptr; // into a screen's image: texture, sampler
    };
    // A screen's image and what draws into it and from it.
    struct Image {
        WGPUTexture texture = nullptr;
        std::uint32_t width = 0, height = 0, levels = 0;
        std::vector<WGPUTextureView> level_views;
        std::vector<WGPUBindGroup> down_groups; // [l]: level l - 1 to sample (none at 0)
        WGPUTextureView view = nullptr;         // every level, for the panel
        WGPUBindGroup composite = nullptr;
        WGPUBuffer area = nullptr;
        WGPUBindGroup area_group = nullptr;
        glm::vec4 covered{0.0f, 0.0f, 1.0f, 1.0f};
        bool drawn = false;

        void release() {
            for (auto group : down_groups)
                if (group) wgpuBindGroupRelease(group);
            for (auto view_of_level : level_views)
                if (view_of_level) wgpuTextureViewRelease(view_of_level);
            down_groups.clear();
            level_views.clear();
            if (composite) wgpuBindGroupRelease(composite);
            if (view) wgpuTextureViewRelease(view);
            if (texture) wgpuTextureRelease(texture);
            if (area_group) wgpuBindGroupRelease(area_group);
            if (area) wgpuBufferRelease(area);
            composite = nullptr;
            view = nullptr;
            texture = nullptr;
            area_group = nullptr;
            area = nullptr;
            width = height = levels = 0;
        }
    };

    WGPUDevice device = nullptr;
    // The straight path.
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline blend = nullptr;
    WGPURenderPipeline opaque = nullptr;
    WGPUBuffer uniform = nullptr;
    WGPUSampler sampler = nullptr;
    // The supersampled path.
    WGPUBindGroupLayout area_layout = nullptr;    // the image's covered part
    WGPUBindGroupLayout texture_layout = nullptr; // a texture and its sampler (content, mip levels, the panel)
    WGPUBindGroupLayout frame_layout = nullptr;   // the frame uniform alone
    WGPURenderPipeline content_blend = nullptr;
    WGPURenderPipeline content_opaque = nullptr;
    WGPURenderPipeline down = nullptr;
    WGPURenderPipeline composite = nullptr;
    WGPUSampler source_sampler = nullptr; // the screens' textures into an image
    WGPUSampler down_sampler = nullptr;
    WGPUSampler image_sampler = nullptr;  // the images on the panels
    WGPUBindGroup frame_group = nullptr;
    WGPUBuffer composite_vertices = nullptr;
    std::uint64_t composite_capacity = 0;
    std::vector<Image> images;

    std::vector<Texture> textures; // [0] unused: ids start at 1
    WGPUBuffer vertices = nullptr;
    std::uint64_t capacity = 0;
    std::vector<ScreenQuadVertex> frame_vertices; // for the images' covered parts
    std::vector<ScreenQuadDraw> draws;
    std::vector<ScreenSurface> surfaces;
    bool prepared = false; // prepare() drew this frame's images

    ~Impl() {
        for (auto& image : images) image.release();
        for (auto& texture : textures) {
            if (texture.direct) wgpuBindGroupRelease(texture.direct);
            if (texture.content) wgpuBindGroupRelease(texture.content);
            if (texture.view) wgpuTextureViewRelease(texture.view);
        }
        for (auto* buffer : {vertices, uniform, composite_vertices})
            if (buffer) wgpuBufferRelease(buffer);
        for (auto* s : {sampler, source_sampler, down_sampler, image_sampler})
            if (s) wgpuSamplerRelease(s);
        if (frame_group) wgpuBindGroupRelease(frame_group);
        for (auto* p : {blend, opaque, content_blend, content_opaque, down, composite})
            if (p) wgpuRenderPipelineRelease(p);
        for (auto* l : {layout, area_layout, texture_layout, frame_layout})
            if (l) wgpuBindGroupLayoutRelease(l);
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
        const auto depth = panel_depth();
        const WGPUBlendState alpha = straight_alpha();
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

    // Tested against the scene's depth, not written (RTS 0x46B668).
    [[nodiscard]] static WGPUDepthStencilState panel_depth() {
        constexpr WGPUStencilFaceState keep{
            .compare = WGPUCompareFunction_Always,
            .failOp = WGPUStencilOperation_Keep,
            .depthFailOp = WGPUStencilOperation_Keep,
            .passOp = WGPUStencilOperation_Keep,
        };
        return {
            .format = depth_format,
            .depthWriteEnabled = WGPUOptionalBool_False,
            .depthCompare = depth_nearer_or_equal,
            .stencilFront = keep,
            .stencilBack = keep,
            .stencilReadMask = ~0u,
            .stencilWriteMask = ~0u,
        };
    }
    [[nodiscard]] static WGPUBlendState straight_alpha() {
        return {
            .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_SrcAlpha,
                      .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
            .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One,
                      .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        };
    }

    WGPURenderPipeline make_content_pipeline(const WGPUShaderModule shader, const WGPUPipelineLayout pipeline_layout,
                                             const bool blended) const {
        const std::array<WGPUVertexAttribute, 3> attributes{{
            {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(ScreenQuadVertex, uv), .shaderLocation = 1},
            {.format = WGPUVertexFormat_Unorm8x4, .offset = offsetof(ScreenQuadVertex, colour), .shaderLocation = 2},
            {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(ScreenQuadVertex, local), .shaderLocation = 3},
        }};
        const WGPUVertexBufferLayout vertex_layout{
            .stepMode = WGPUVertexStepMode_Vertex,
            .arrayStride = sizeof(ScreenQuadVertex),
            .attributeCount = attributes.size(),
            .attributes = attributes.data(),
        };
        // Premultiplied: over what is already in the image, in linear light.
        const WGPUBlendState premultiplied{
            .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One,
                      .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
            .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One,
                      .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        };
        const WGPUColorTargetState target{.format = image_format, .blend = &premultiplied, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader,
                                         .entryPoint = wgsl_name(blended ? "fs_content_blend" : "fs_content_opaque"),
                                         .targetCount = 1,
                                         .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = pipeline_layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_content"), .bufferCount = 1, .buffers = &vertex_layout},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u, .alphaToCoverageEnabled = false},
            .fragment = &fragment,
        };
        return wgpuDeviceCreateRenderPipeline(device, &descriptor);
    }

    WGPUBindGroup texture_group(const WGPUTextureView view, const WGPUSampler with) const {
        std::array<WGPUBindGroupEntry, 2> entries{};
        entries[0].binding = 0;
        entries[0].textureView = view;
        entries[1].binding = 1;
        entries[1].sampler = with;
        const WGPUBindGroupDescriptor descriptor{.layout = texture_layout, .entryCount = entries.size(), .entries = entries.data()};
        return wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    // The image for screen `index` at that size (made again when it changes).
    Image& image_for(const std::size_t index, const std::uint32_t width, const std::uint32_t height) {
        if (images.size() <= index) images.resize(index + 1);
        auto& image = images[index];
        if (image.texture && image.width == width && image.height == height) return image;
        image.release();
        std::uint32_t levels = 1;
        for (std::uint32_t side = (std::max)(width, height); side > 1; side >>= 1) ++levels;
        const WGPUTextureDescriptor descriptor{
            .label = {.data = "cab screen image", .length = WGPU_STRLEN},
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = width, .height = height, .depthOrArrayLayers = 1},
            .format = image_format,
            .mipLevelCount = levels,
            .sampleCount = 1,
        };
        image.texture = wgpuDeviceCreateTexture(device, &descriptor);
        if (!image.texture) return image;
        image.width = width;
        image.height = height;
        image.levels = levels;
        for (std::uint32_t level = 0; level < levels; ++level) {
            const WGPUTextureViewDescriptor view{
                .format = image_format,
                .dimension = WGPUTextureViewDimension_2D,
                .baseMipLevel = level,
                .mipLevelCount = 1,
                .baseArrayLayer = 0,
                .arrayLayerCount = 1,
                .aspect = WGPUTextureAspect_All,
            };
            image.level_views.push_back(wgpuTextureCreateView(image.texture, &view));
            image.down_groups.push_back(level == 0 ? nullptr : texture_group(image.level_views[level - 1], down_sampler));
        }
        image.view = wgpuTextureCreateView(image.texture, nullptr);
        image.composite = texture_group(image.view, image_sampler);
        const WGPUBufferDescriptor area{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(glm::vec4)};
        image.area = wgpuDeviceCreateBuffer(device, &area);
        WGPUBindGroupEntry entry{};
        entry.binding = 0;
        entry.buffer = image.area;
        entry.size = sizeof(glm::vec4);
        const WGPUBindGroupDescriptor group{.layout = area_layout, .entryCount = 1, .entries = &entry};
        image.area_group = wgpuDeviceCreateBindGroup(device, &group);
        return image;
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
    // The images on the panels: the same.
    impl.image_sampler = wgpuDeviceCreateSampler(device, &sampler);
    // The screens' textures into an image: face on, no anisotropy needed.
    sampler.maxAnisotropy = 1;
    impl.source_sampler = wgpuDeviceCreateSampler(device, &sampler);
    // A mip level from the one above: bilinear, the level given.
    sampler.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampler.lodMaxClamp = 0.0f;
    impl.down_sampler = wgpuDeviceCreateSampler(device, &sampler);

    const auto shader = shader_module(device, screen_shader);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    impl.blend = impl.make_pipeline(shader, pipeline_layout, target_format, true);
    impl.opaque = impl.make_pipeline(shader, pipeline_layout, target_format, false);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);

    // The supersampled path's layouts.
    {
        WGPUBindGroupLayoutEntry area{};
        area.binding = 0;
        area.visibility = WGPUShaderStage_Vertex;
        area.buffer.type = WGPUBufferBindingType_Uniform;
        area.buffer.minBindingSize = sizeof(glm::vec4);
        const WGPUBindGroupLayoutDescriptor area_descriptor{.entryCount = 1, .entries = &area};
        impl.area_layout = wgpuDeviceCreateBindGroupLayout(device, &area_descriptor);
        std::array<WGPUBindGroupLayoutEntry, 2> texture_entries{};
        texture_entries[0].binding = 0;
        texture_entries[0].visibility = WGPUShaderStage_Fragment;
        texture_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
        texture_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
        texture_entries[1].binding = 1;
        texture_entries[1].visibility = WGPUShaderStage_Fragment;
        texture_entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
        const WGPUBindGroupLayoutDescriptor texture_descriptor{.entryCount = texture_entries.size(), .entries = texture_entries.data()};
        impl.texture_layout = wgpuDeviceCreateBindGroupLayout(device, &texture_descriptor);
        WGPUBindGroupLayoutEntry frame{};
        frame.binding = 0;
        frame.visibility = WGPUShaderStage_Vertex;
        frame.buffer.type = WGPUBufferBindingType_Uniform;
        frame.buffer.minBindingSize = sizeof(FrameUniform);
        const WGPUBindGroupLayoutDescriptor frame_descriptor{.entryCount = 1, .entries = &frame};
        impl.frame_layout = wgpuDeviceCreateBindGroupLayout(device, &frame_descriptor);
        WGPUBindGroupEntry frame_entry{};
        frame_entry.binding = 0;
        frame_entry.buffer = impl.uniform;
        frame_entry.size = sizeof(FrameUniform);
        const WGPUBindGroupDescriptor frame_group{.layout = impl.frame_layout, .entryCount = 1, .entries = &frame_entry};
        impl.frame_group = wgpuDeviceCreateBindGroup(device, &frame_group);
    }
    {
        const auto content = shader_module(device, content_shader);
        const std::array<WGPUBindGroupLayout, 2> layouts{impl.area_layout, impl.texture_layout};
        const WGPUPipelineLayoutDescriptor descriptor{.bindGroupLayoutCount = layouts.size(), .bindGroupLayouts = layouts.data()};
        const auto content_layout = wgpuDeviceCreatePipelineLayout(device, &descriptor);
        impl.content_blend = impl.make_content_pipeline(content, content_layout, true);
        impl.content_opaque = impl.make_content_pipeline(content, content_layout, false);
        wgpuPipelineLayoutRelease(content_layout);
        wgpuShaderModuleRelease(content);
    }
    {
        const auto down = shader_module(device, down_shader);
        const WGPUPipelineLayoutDescriptor descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.texture_layout};
        const auto down_layout = wgpuDeviceCreatePipelineLayout(device, &descriptor);
        const WGPUColorTargetState target{.format = image_format, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = down, .entryPoint = wgsl_name("fs_down"), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor pipeline{
            .layout = down_layout,
            .vertex = {.module = down, .entryPoint = wgsl_name("vs_full")},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u, .alphaToCoverageEnabled = false},
            .fragment = &fragment,
        };
        impl.down = wgpuDeviceCreateRenderPipeline(device, &pipeline);
        wgpuPipelineLayoutRelease(down_layout);
        wgpuShaderModuleRelease(down);
    }
    {
        const auto composite = shader_module(device, composite_shader);
        const std::array<WGPUBindGroupLayout, 2> layouts{impl.frame_layout, impl.texture_layout};
        const WGPUPipelineLayoutDescriptor descriptor{.bindGroupLayoutCount = layouts.size(), .bindGroupLayouts = layouts.data()};
        const auto composite_layout = wgpuDeviceCreatePipelineLayout(device, &descriptor);
        const std::array<WGPUVertexAttribute, 2> attributes{{
            {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(CompositeVertex, position), .shaderLocation = 0},
            {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(CompositeVertex, uv), .shaderLocation = 1},
        }};
        const WGPUVertexBufferLayout vertex_layout{
            .stepMode = WGPUVertexStepMode_Vertex,
            .arrayStride = sizeof(CompositeVertex),
            .attributeCount = attributes.size(),
            .attributes = attributes.data(),
        };
        const auto depth = Impl::panel_depth();
        const auto alpha = Impl::straight_alpha();
        const WGPUColorTargetState target{.format = target_format, .blend = &alpha, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = composite, .entryPoint = wgsl_name("fs_composite"), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor pipeline{
            .layout = composite_layout,
            .vertex = {.module = composite, .entryPoint = wgsl_name("vs_composite"), .bufferCount = 1, .buffers = &vertex_layout},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .depthStencil = &depth,
            .multisample = scene_multisample(),
            .fragment = &fragment,
        };
        impl.composite = wgpuDeviceCreateRenderPipeline(device, &pipeline);
        wgpuPipelineLayoutRelease(composite_layout);
        wgpuShaderModuleRelease(composite);
    }
}

ScreenQuadRenderer::~ScreenQuadRenderer() = default;

bool ScreenQuadRenderer::ready() const noexcept {
    const auto& impl = *impl_;
    return impl.blend && impl.opaque && impl.layout && impl.uniform && impl.sampler && impl.content_blend && impl.content_opaque &&
           impl.down && impl.composite && impl.frame_group && impl.source_sampler && impl.down_sampler && impl.image_sampler;
}

std::uint32_t ScreenQuadRenderer::add_texture(const Rgba8ImageView image, const bool premultiplied) {
    auto& impl = *impl_;
    WgpuTextureUploadResult upload;
    if (premultiplied && image.pixels) {
        auto levels = premultiplied_linear_levels(image.width, image.height, *image.pixels);
        if (levels.empty()) return 0;
        const std::vector<std::vector<std::uint8_t>> mips(std::make_move_iterator(levels.begin() + 1), std::make_move_iterator(levels.end()));
        upload = upload_rgba8_texture(impl.device, {.width = image.width, .height = image.height, .pixels = &levels[0], .mips = &mips},
                                      "cab screen (premultiplied)", WGPUTextureFormat_RGBA8UnormSrgb);
    } else {
        upload = upload_rgba8_texture(impl.device, image, "cab screen");
    }
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
    texture.direct = wgpuDeviceCreateBindGroup(impl.device, &descriptor);
    texture.content = impl.texture_group(texture.view, impl.source_sampler);
    impl.textures.push_back(std::move(texture));
    return static_cast<std::uint32_t>(impl.textures.size() - 1);
}

void ScreenQuadRenderer::set(std::vector<ScreenQuadVertex> vertices, std::vector<ScreenQuadDraw> draws,
                             std::vector<ScreenSurface> surfaces) {
    auto& impl = *impl_;
    impl.prepared = false;
    std::erase_if(draws, [&](const ScreenQuadDraw& d) {
        return d.texture == 0 || d.texture >= impl.textures.size() || d.count == 0 ||
               static_cast<std::size_t>(d.first) + d.count > vertices.size();
    });
    impl.draws = std::move(draws);
    impl.surfaces = std::move(surfaces);
    impl.frame_vertices = std::move(vertices);
    if (impl.draws.empty()) return;
    const auto bytes = static_cast<std::uint64_t>(impl.frame_vertices.size()) * sizeof(ScreenQuadVertex);
    if (bytes > impl.capacity) {
        if (impl.vertices) wgpuBufferRelease(impl.vertices);
        impl.capacity = bytes * 2;
        const WGPUBufferDescriptor descriptor{.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, .size = impl.capacity};
        impl.vertices = wgpuDeviceCreateBuffer(impl.device, &descriptor);
    }
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.vertices, 0, impl.frame_vertices.data(), bytes);
        wgpuQueueRelease(queue);
    }
}

void ScreenQuadRenderer::prepare(const WGPUCommandEncoder encoder) {
    auto& impl = *impl_;
    impl.prepared = false;
    if (!cab_screens_supersampled() || !ready() || impl.draws.empty() || !impl.vertices) return;
    const auto queue = wgpuDeviceGetQueue(impl.device);
    if (!queue) return;
    for (auto& image : impl.images) image.drawn = false;
    for (std::size_t s = 0; s < impl.surfaces.size(); ++s) {
        const auto& surface = impl.surfaces[s];
        // What the screen's objects cover: its own 0..1, more where one
        // reaches past it (drawn on the panel by the straight path too).
        glm::vec2 low{0.0f, 0.0f}, high{1.0f, 1.0f};
        bool any = false;
        for (const auto& draw : impl.draws) {
            if (draw.surface != s) continue;
            any = true;
            for (std::uint32_t v = draw.first; v < draw.first + draw.count; ++v) {
                low = glm::min(low, impl.frame_vertices[v].local);
                high = glm::max(high, impl.frame_vertices[v].local);
            }
        }
        if (!any) continue;
        low = glm::max(low, glm::vec2{-0.5f});
        high = glm::min(high, glm::vec2{1.5f});
        const auto side = [](const float metres, const float share) {
            const auto pixels = static_cast<std::uint32_t>(std::lround(metres * share * pixels_per_metre));
            return std::clamp((pixels + 1u) & ~1u, min_side, max_side);
        };
        auto& image = impl.image_for(s, side(surface.width, high.x - low.x), side(surface.height, high.y - low.y));
        if (!image.texture || !image.area_group) continue;
        image.covered = {low.x, low.y, high.x, high.y};
        wgpuQueueWriteBuffer(queue, image.area, 0, &image.covered, sizeof(glm::vec4));

        WGPURenderPassColorAttachment colour{
            .view = image.level_views[0],
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = {0.0, 0.0, 0.0, 0.0},
        };
        const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
        const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, impl.vertices, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, image.area_group, 0, nullptr);
        WGPURenderPipeline current = nullptr;
        for (const auto& draw : impl.draws) {
            if (draw.surface != s) continue;
            const auto pipeline = draw.blend ? impl.content_blend : impl.content_opaque;
            if (pipeline != current) {
                wgpuRenderPassEncoderSetPipeline(pass, pipeline);
                current = pipeline;
            }
            wgpuRenderPassEncoderSetBindGroup(pass, 1, impl.textures[draw.texture].content, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, draw.count, 1, draw.first, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        // The mipmaps, each from the one above, in linear light.
        for (std::uint32_t level = 1; level < image.levels; ++level) {
            colour.view = image.level_views[level];
            const auto down = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
            wgpuRenderPassEncoderSetPipeline(down, impl.down);
            wgpuRenderPassEncoderSetBindGroup(down, 0, image.down_groups[level], 0, nullptr);
            wgpuRenderPassEncoderDraw(down, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(down);
            wgpuRenderPassEncoderRelease(down);
        }
        image.drawn = true;
    }
    wgpuQueueRelease(queue);
    impl.prepared = true;
}

void ScreenQuadRenderer::render(const WGPURenderPassEncoder pass, const glm::mat4& view_projection, const glm::vec3& eye) {
    auto& impl = *impl_;
    if (!ready() || impl.draws.empty() || !impl.vertices) return;
    const FrameUniform frame{.view_projection = view_projection, .eye = glm::vec4{eye, 0.0f}};
    const auto queue = wgpuDeviceGetQueue(impl.device);
    if (!queue) return;
    wgpuQueueWriteBuffer(queue, impl.uniform, 0, &frame, sizeof(frame));
    if (impl.prepared && cab_screens_supersampled()) {
        // Each screen's image over the part of the panel it covers.
        std::vector<CompositeVertex> quads;
        std::vector<std::size_t> shown;
        for (std::size_t s = 0; s < impl.surfaces.size() && s < impl.images.size(); ++s) {
            const auto& image = impl.images[s];
            if (!image.drawn) continue;
            const auto& surface = impl.surfaces[s];
            const glm::vec3 across = surface.top_right - surface.top_left, down = surface.bottom_right - surface.top_right;
            const auto at = [&](const float x, const float y) { return surface.top_left + across * x + down * y; };
            const auto& c = image.covered;
            const CompositeVertex corners[4] = {{at(c.x, c.y), {0.0f, 0.0f}}, {at(c.x, c.w), {0.0f, 1.0f}},
                                                {at(c.z, c.w), {1.0f, 1.0f}}, {at(c.z, c.y), {1.0f, 0.0f}}};
            for (const int k : {0, 1, 2, 0, 2, 3}) quads.push_back(corners[k]);
            shown.push_back(s);
        }
        if (!quads.empty()) {
            const auto bytes = static_cast<std::uint64_t>(quads.size()) * sizeof(CompositeVertex);
            if (bytes > impl.composite_capacity) {
                if (impl.composite_vertices) wgpuBufferRelease(impl.composite_vertices);
                impl.composite_capacity = bytes * 2;
                const WGPUBufferDescriptor descriptor{.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                                      .size = impl.composite_capacity};
                impl.composite_vertices = wgpuDeviceCreateBuffer(impl.device, &descriptor);
            }
            wgpuQueueWriteBuffer(queue, impl.composite_vertices, 0, quads.data(), bytes);
            wgpuRenderPassEncoderSetPipeline(pass, impl.composite);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, impl.composite_vertices, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.frame_group, 0, nullptr);
            for (std::size_t i = 0; i < shown.size(); ++i) {
                wgpuRenderPassEncoderSetBindGroup(pass, 1, impl.images[shown[i]].composite, 0, nullptr);
                wgpuRenderPassEncoderDraw(pass, 6, 1, static_cast<std::uint32_t>(i * 6), 0);
            }
        }
        wgpuQueueRelease(queue);
        return;
    }
    wgpuQueueRelease(queue);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, impl.vertices, 0, WGPU_WHOLE_SIZE);
    WGPURenderPipeline current = nullptr;
    for (const auto& draw : impl.draws) {
        const auto pipeline = draw.blend ? impl.blend : impl.opaque;
        if (pipeline != current) {
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            current = pipeline;
        }
        wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.textures[draw.texture].direct, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, draw.count, 1, draw.first, 0);
    }
}

} // namespace openrts::renderer
#endif
