#include <openrts/renderer/bloom.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include <openrts/renderer/depth.hpp>
#include <openrts/renderer/gpu_pass_timer.hpp>

namespace openrts::renderer {
namespace {

constexpr const char* bloom_shader = R"(
struct Params {
    texel: vec2<f32>,  // the input's texel
    threshold: f32,    // what is over it blooms (scene-linear)
    knee: f32,         // the threshold's soft start
    night: f32,        // 0 day .. 1 night: the brightest texels bloom (see light_at)
    linear: f32,       // 1: the scene and the lights' source hold linear light, 0: gamma 2.2
    pad1: vec2<f32>,
};
@group(0) @binding(0) var input_a: texture_2d<f32>; // the scene (the first pass), else the larger image
@group(0) @binding(1) var input_b: texture_2d<f32>; // the lights' source (the first pass)
@group(0) @binding(2) var linear_clamp: sampler;
@group(0) @binding(3) var<uniform> params: Params;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOut {
    let p = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    var out: VertexOut;
    out.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = vec2<f32>(p.x, 1.0 - p.y);
    return out;
}

fn luma(c: vec3<f32>) -> f32 {
    return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}

// What is over the threshold, its brightest channel's share, softly.
fn over(c: vec3<f32>) -> vec3<f32> {
    let bright = max(c.r, max(c.g, c.b));
    var soft = clamp(bright - params.threshold + params.knee, 0.0, 2.0 * params.knee);
    soft = soft * soft / (4.0 * params.knee + 1e-5);
    let w = max(soft, bright - params.threshold) / max(bright, 1e-5);
    return c * max(w, 0.0);
}

// The light that blooms at uv: the scene over the threshold; at night also
// the brightest of what the textures paint (a lamp's bulb, a lit window, a
// sign: drawn near white, 0.8 to 1 as displayed; a dark wall, a lamp's pool
// painted on a platform stay below), as the night's eye sees them; and the
// lights' glows.
fn light_at(uv: vec2<f32>) -> vec3<f32> {
    let stored = max(textureSampleLevel(input_a, linear_clamp, uv, 0.0).rgb, vec3<f32>(0.0));
    let linear_frame = params.linear > 0.5;
    let scene = select(pow(stored, vec3<f32>(2.2)), stored, linear_frame);
    // How bright it is as displayed (the texture's own brightness).
    let encoded = select(stored, pow(stored, vec3<f32>(1.0 / 2.2)), linear_frame);
    var light = over(scene);
    if (params.night > 0.0) {
        let top = max(encoded.r, max(encoded.g, encoded.b));
        light = max(light, scene * smoothstep(0.8, 1.0, top) * params.night);
    }
    let glow_stored = max(textureSampleLevel(input_b, linear_clamp, uv, 0.0).rgb, vec3<f32>(0.0));
    let glow = select(pow(glow_stored, vec3<f32>(2.2)), glow_stored, linear_frame);
    return min(light + glow, vec3<f32>(1000.0));
}

fn take(uv: vec2<f32>) -> vec3<f32> {
    return textureSampleLevel(input_a, linear_clamp, uv, 0.0).rgb;
}

// The first step down (13 taps, Jimenez's), each of its five boxes weighed
// by 1 / (1 + luma) (Karis): a lone bright texel does not flicker.
@fragment
fn fs_prefilter(input: VertexOut) -> @location(0) vec4<f32> {
    let t = params.texel;
    let uv = input.uv;
    let a = light_at(uv + t * vec2<f32>(-2.0, -2.0));
    let b = light_at(uv + t * vec2<f32>(0.0, -2.0));
    let c = light_at(uv + t * vec2<f32>(2.0, -2.0));
    let d = light_at(uv + t * vec2<f32>(-2.0, 0.0));
    let e = light_at(uv);
    let f = light_at(uv + t * vec2<f32>(2.0, 0.0));
    let g = light_at(uv + t * vec2<f32>(-2.0, 2.0));
    let h = light_at(uv + t * vec2<f32>(0.0, 2.0));
    let i = light_at(uv + t * vec2<f32>(2.0, 2.0));
    let j = light_at(uv + t * vec2<f32>(-1.0, -1.0));
    let k = light_at(uv + t * vec2<f32>(1.0, -1.0));
    let l = light_at(uv + t * vec2<f32>(-1.0, 1.0));
    let m = light_at(uv + t * vec2<f32>(1.0, 1.0));
    let g0 = (j + k + l + m) * 0.25;
    let g1 = (a + b + d + e) * 0.25;
    let g2 = (b + c + e + f) * 0.25;
    let g3 = (d + e + g + h) * 0.25;
    let g4 = (e + f + h + i) * 0.25;
    let w0 = 0.5 / (1.0 + luma(g0));
    let w1 = 0.125 / (1.0 + luma(g1));
    let w2 = 0.125 / (1.0 + luma(g2));
    let w3 = 0.125 / (1.0 + luma(g3));
    let w4 = 0.125 / (1.0 + luma(g4));
    let sum = g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4;
    return vec4<f32>(sum / (w0 + w1 + w2 + w3 + w4), 1.0);
}

// A step down: the 13 taps, plainly weighed.
@fragment
fn fs_down(input: VertexOut) -> @location(0) vec4<f32> {
    let t = params.texel;
    let uv = input.uv;
    let a = take(uv + t * vec2<f32>(-2.0, -2.0));
    let b = take(uv + t * vec2<f32>(0.0, -2.0));
    let c = take(uv + t * vec2<f32>(2.0, -2.0));
    let d = take(uv + t * vec2<f32>(-2.0, 0.0));
    let e = take(uv);
    let f = take(uv + t * vec2<f32>(2.0, 0.0));
    let g = take(uv + t * vec2<f32>(-2.0, 2.0));
    let h = take(uv + t * vec2<f32>(0.0, 2.0));
    let i = take(uv + t * vec2<f32>(2.0, 2.0));
    let j = take(uv + t * vec2<f32>(-1.0, -1.0));
    let k = take(uv + t * vec2<f32>(1.0, -1.0));
    let l = take(uv + t * vec2<f32>(-1.0, 1.0));
    let m = take(uv + t * vec2<f32>(1.0, 1.0));
    let sum = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    return vec4<f32>(sum, 1.0);
}

// A step up: the smaller image through a 3 x 3 tent, added to the larger.
@fragment
fn fs_up(input: VertexOut) -> @location(0) vec4<f32> {
    let t = params.texel;
    let uv = input.uv;
    var sum = take(uv) * 4.0;
    sum = sum + (take(uv + vec2<f32>(0.0, -t.y)) + take(uv + vec2<f32>(-t.x, 0.0)) + take(uv + vec2<f32>(t.x, 0.0))
                 + take(uv + vec2<f32>(0.0, t.y))) * 2.0;
    sum = sum + take(uv + vec2<f32>(-t.x, -t.y)) + take(uv + vec2<f32>(t.x, -t.y)) + take(uv + vec2<f32>(-t.x, t.y))
              + take(uv + vec2<f32>(t.x, t.y));
    return vec4<f32>(sum / 16.0, 1.0);
}
)";

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

struct Params {
    float texel[2]{};
    float threshold = 1.0f;
    float knee = 0.5f;
    float night = 0.0f;
    float linear = 0.0f;
    float pad1[2]{};
};
static_assert(sizeof(Params) == 32);

constexpr std::uint32_t slot_size = 256; // the uniform offset alignment
constexpr std::uint32_t max_levels = 6;
constexpr std::uint32_t max_slots = 2 * max_levels + 2;

struct Image {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    std::uint32_t width = 0, height = 0;

    void release() {
        if (view) wgpuTextureViewRelease(view);
        if (texture) wgpuTextureRelease(texture);
        view = nullptr;
        texture = nullptr;
        width = height = 0;
    }
};

Image make_image(const WGPUDevice device, const std::uint32_t width, const std::uint32_t height) {
    Image image;
    const WGPUTextureDescriptor descriptor{
        .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = (std::max)(width, 1u), .height = (std::max)(height, 1u), .depthOrArrayLayers = 1},
        .format = hdr_format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    image.texture = wgpuDeviceCreateTexture(device, &descriptor);
    if (image.texture) image.view = wgpuTextureCreateView(image.texture, nullptr);
    image.width = descriptor.size.width;
    image.height = descriptor.size.height;
    return image;
}

} // namespace

struct BloomRenderer::Impl {
    WGPUDevice device = nullptr;
    WGPUSampler sampler = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline prefilter = nullptr;
    WGPURenderPipeline down = nullptr;
    WGPURenderPipeline up = nullptr;
    WGPUBuffer params = nullptr;
    Image source;
    Image black; // the source when no lights were drawn
    std::vector<Image> levels;
    std::uint32_t width = 0, height = 0;
    bool sources_drawn = false;

    ~Impl() {
        source.release();
        black.release();
        for (auto& level : levels) level.release();
        if (params) wgpuBufferRelease(params);
        for (const auto pipeline : {prefilter, down, up})
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (sampler) wgpuSamplerRelease(sampler);
    }

    void ensure(const std::uint32_t w, const std::uint32_t h) {
        if (w == width && h == height && source.texture) return;
        source.release();
        for (auto& level : levels) level.release();
        levels.clear();
        width = w;
        height = h;
        source = make_image(device, w, h);
        // Halves down to some 16 pixels on the shorter side, at most max_levels.
        std::uint32_t lw = (w + 1) / 2, lh = (h + 1) / 2;
        while (levels.size() < max_levels && (std::min)(lw, lh) >= 16) {
            levels.push_back(make_image(device, lw, lh));
            lw = (lw + 1) / 2;
            lh = (lh + 1) / 2;
        }
        if (levels.empty()) levels.push_back(make_image(device, (w + 1) / 2, (h + 1) / 2));
    }

    [[nodiscard]] WGPUBindGroup group(const WGPUTextureView a, const WGPUTextureView b, const std::uint32_t slot) const {
        std::array<WGPUBindGroupEntry, 4> entries{};
        entries[0].binding = 0;
        entries[0].textureView = a;
        entries[1].binding = 1;
        entries[1].textureView = b;
        entries[2].binding = 2;
        entries[2].sampler = sampler;
        entries[3].binding = 3;
        entries[3].buffer = params;
        entries[3].offset = static_cast<std::uint64_t>(slot) * slot_size;
        entries[3].size = sizeof(Params);
        const WGPUBindGroupDescriptor descriptor{.layout = layout, .entryCount = entries.size(), .entries = entries.data()};
        return wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    void pass(const WGPUCommandEncoder encoder, const WGPURenderPipeline pipeline, const Image& target, const bool keep,
              const WGPUBindGroup bind, const bool timed = false) const {
        const WGPURenderPassColorAttachment colour{
            .view = target.view,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = keep ? WGPULoadOp_Load : WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = {0.0, 0.0, 0.0, 1.0},
        };
        const auto timestamps = timed ? render_pass_timestamps(GpuPass::bloom) : std::nullopt;
        const WGPURenderPassDescriptor descriptor{
            .colorAttachmentCount = 1, .colorAttachments = &colour, .timestampWrites = timestamps ? &*timestamps : nullptr};
        const auto encoder_pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        wgpuRenderPassEncoderSetPipeline(encoder_pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(encoder_pass, 0, bind, 0, nullptr);
        wgpuRenderPassEncoderDraw(encoder_pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(encoder_pass);
        wgpuRenderPassEncoderRelease(encoder_pass);
    }
};

BloomRenderer::BloomRenderer(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    WGPUSamplerDescriptor sampler{};
    sampler.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler.addressModeW = WGPUAddressMode_ClampToEdge;
    sampler.magFilter = WGPUFilterMode_Linear;
    sampler.minFilter = WGPUFilterMode_Linear;
    sampler.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampler.lodMaxClamp = 1.0f;
    sampler.maxAnisotropy = 1;
    impl.sampler = wgpuDeviceCreateSampler(device, &sampler);

    std::array<WGPUBindGroupLayoutEntry, 4> entries{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Fragment;
        entries[i].texture.sampleType = WGPUTextureSampleType_Float;
        entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
    }
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Fragment;
    entries[3].buffer.type = WGPUBufferBindingType_Uniform;
    entries[3].buffer.minBindingSize = sizeof(Params);
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                                      .size = static_cast<std::uint64_t>(slot_size) * max_slots};
    impl.params = wgpuDeviceCreateBuffer(device, &buffer);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(bloom_shader)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const WGPUBlendState added{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_One},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_Zero, .dstFactor = WGPUBlendFactor_One},
    };
    const auto make = [&](const char* entry, const WGPUBlendState* blend) {
        const WGPUColorTargetState target{.format = hdr_format, .blend = blend, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name(entry), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = pipeline_layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main")},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u},
            .fragment = &fragment,
        };
        return wgpuDeviceCreateRenderPipeline(device, &descriptor);
    };
    impl.prefilter = make("fs_prefilter", nullptr);
    impl.down = make("fs_down", nullptr);
    impl.up = make("fs_up", &added);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);

    // The black stand-in source: one texel, cleared once in build().
    impl.black = make_image(device, 1, 1);
}

BloomRenderer::~BloomRenderer() = default;

bool BloomRenderer::ready() const noexcept {
    const auto& impl = *impl_;
    return impl.sampler && impl.layout && impl.prefilter && impl.down && impl.up && impl.params && impl.black.view;
}

WGPURenderPassEncoder BloomRenderer::begin_sources(const WGPUCommandEncoder encoder, const std::uint32_t width, const std::uint32_t height,
                                                   const WGPUTextureView depth) {
    auto& impl = *impl_;
    if (!ready() || !depth || width == 0 || height == 0) return nullptr;
    impl.ensure(width, height);
    if (!impl.source.view) return nullptr;
    const WGPURenderPassColorAttachment colour{
        .view = impl.source.view,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const WGPURenderPassDepthStencilAttachment depth_attachment{
        .view = depth,
        .depthLoadOp = WGPULoadOp_Undefined,
        .depthStoreOp = WGPUStoreOp_Undefined,
        .depthReadOnly = true,
        .stencilLoadOp = WGPULoadOp_Undefined,
        .stencilStoreOp = WGPUStoreOp_Undefined,
        .stencilReadOnly = true,
    };
    const auto timestamps = render_pass_timestamps(GpuPass::bloom_sources);
    const WGPURenderPassDescriptor descriptor{.colorAttachmentCount = 1,
                                              .colorAttachments = &colour,
                                              .depthStencilAttachment = &depth_attachment,
                                              .timestampWrites = timestamps ? &*timestamps : nullptr};
    impl.sources_drawn = true;
    return wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
}

WGPUTextureView BloomRenderer::build(const WGPUCommandEncoder encoder, const WGPUTextureView scene, const std::uint32_t width,
                                     const std::uint32_t height, const BloomSettings& settings) {
    auto& impl = *impl_;
    if (!ready() || !scene || width == 0 || height == 0) return nullptr;
    impl.ensure(width, height);
    if (impl.levels.empty() || !impl.levels[0].view) return nullptr;
    const bool sources = impl.sources_drawn;
    impl.sources_drawn = false;

    // The parameters of every pass, one slot each, written before any runs.
    std::array<Params, max_slots> params{};
    std::uint32_t slots = 0;
    const auto add = [&](const std::uint32_t input_width, const std::uint32_t input_height, const float night) {
        auto& p = params[slots];
        p.texel[0] = 1.0f / static_cast<float>((std::max)(input_width, 1u));
        p.texel[1] = 1.0f / static_cast<float>((std::max)(input_height, 1u));
        p.night = night;
        p.linear = settings.linear ? 1.0f : 0.0f;
        return slots++;
    };
    const auto first = add(width, height, settings.textures ? std::clamp(settings.night, 0.0f, 1.0f) : 0.0f);
    std::vector<std::uint32_t> down_slots, up_slots;
    for (std::size_t i = 0; i + 1 < impl.levels.size(); ++i) down_slots.push_back(add(impl.levels[i].width, impl.levels[i].height, 0.0f));
    for (std::size_t i = impl.levels.size() - 1; i >= 1; --i) up_slots.push_back(add(impl.levels[i].width, impl.levels[i].height, 0.0f));
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        for (std::uint32_t s = 0; s < slots; ++s)
            wgpuQueueWriteBuffer(queue, impl.params, static_cast<std::uint64_t>(s) * slot_size, &params[s], sizeof(Params));
        wgpuQueueRelease(queue);
    }

    std::vector<WGPUBindGroup> groups;
    // The black stand-in, cleared (no lights drawn this frame).
    if (!sources) {
        const WGPURenderPassColorAttachment colour{
            .view = impl.black.view,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = {0.0, 0.0, 0.0, 0.0},
        };
        const WGPURenderPassDescriptor descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
        const auto clear = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        wgpuRenderPassEncoderEnd(clear);
        wgpuRenderPassEncoderRelease(clear);
    }
    groups.push_back(impl.group(scene, sources ? impl.source.view : impl.black.view, first));
    impl.pass(encoder, impl.prefilter, impl.levels[0], false, groups.back(), true);
    for (std::size_t i = 0; i + 1 < impl.levels.size(); ++i) {
        groups.push_back(impl.group(impl.levels[i].view, impl.black.view, down_slots[i]));
        impl.pass(encoder, impl.down, impl.levels[i + 1], false, groups.back());
    }
    for (std::size_t n = 0; n < up_slots.size(); ++n) {
        const auto i = impl.levels.size() - 1 - n;
        groups.push_back(impl.group(impl.levels[i].view, impl.black.view, up_slots[n]));
        impl.pass(encoder, impl.up, impl.levels[i - 1], true, groups.back());
    }
    for (const auto bind : groups) wgpuBindGroupRelease(bind);
    return impl.levels[0].view;
}

} // namespace openrts::renderer
#endif
