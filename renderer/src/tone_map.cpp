#include <openrts/renderer/tone_map.hpp>
#include <openrts/renderer/gpu_pass_timer.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <array>
#include "shader_code.hpp"

namespace openrts::renderer {
namespace {

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

struct SettingsUniform {
    std::uint32_t curve{};
    float exposure = 1.0f;
    float night = 0.0f;
    float night_exposure = 1.5f;
    float saturation = 1.0f;
    float bloom = 0.0f;
    float shafts = 0.0f;
    float linear_scene = 0.0f;
    float auto_exposure = 0.0f;
    float hdr_scale = 0.0f; // paper white / 80 nits: scRGB out (0: SDR)
    float hdr_peak = 0.0f;  // the peak over the paper white
    float pad = 0.0f;
};
static_assert(sizeof(SettingsUniform) == 48);

} // namespace

struct ToneMapper::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer settings = nullptr;
    WGPUSampler sampler = nullptr;
    WGPUTexture black = nullptr; // the bloom when there is none
    WGPUTextureView black_view = nullptr;
    WGPUBuffer no_exposure = nullptr; // the auto exposure's state when there is none (not valid)

    ~Impl() {
        if (no_exposure) wgpuBufferRelease(no_exposure);
        if (black_view) wgpuTextureViewRelease(black_view);
        if (black) wgpuTextureRelease(black);
        if (sampler) wgpuSamplerRelease(sampler);
        if (settings) wgpuBufferRelease(settings);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }
};

ToneMapper::ToneMapper(const WGPUDevice device, const WGPUTextureFormat output_format) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    std::array<WGPUBindGroupLayoutEntry, 6> entries{};
    entries[5].binding = 5;
    entries[5].visibility = WGPUShaderStage_Fragment;
    entries[5].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    entries[5].buffer.minBindingSize = 16;
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = sizeof(SettingsUniform);
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].texture.sampleType = WGPUTextureSampleType_Float;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Fragment;
    entries[3].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[4].binding = 4;
    entries[4].visibility = WGPUShaderStage_Fragment;
    entries[4].texture.sampleType = WGPUTextureSampleType_Float;
    entries[4].texture.viewDimension = WGPUTextureViewDimension_2D;
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
    {
        const WGPUTextureDescriptor black{
            .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = 1, .height = 1, .depthOrArrayLayers = 1},
            .format = WGPUTextureFormat_RGBA8Unorm,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        impl.black = wgpuDeviceCreateTexture(device, &black);
        if (impl.black) {
            impl.black_view = wgpuTextureCreateView(impl.black, nullptr);
            const std::uint8_t zero[4] = {0, 0, 0, 0};
            const auto queue = wgpuDeviceGetQueue(device);
            const WGPUTexelCopyTextureInfo target{.texture = impl.black};
            const WGPUTexelCopyBufferLayout layout{.bytesPerRow = 4, .rowsPerImage = 1};
            const WGPUExtent3D extent{.width = 1, .height = 1, .depthOrArrayLayers = 1};
            wgpuQueueWriteTexture(queue, &target, zero, sizeof(zero), &layout, &extent);
            wgpuQueueRelease(queue);
        }
    }
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(SettingsUniform)};
    impl.settings = wgpuDeviceCreateBuffer(device, &buffer);
    const WGPUBufferDescriptor no_exposure{.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, .size = 16};
    impl.no_exposure = wgpuDeviceCreateBuffer(device, &no_exposure);
    if (impl.no_exposure) {
        const float state[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        const auto queue = wgpuDeviceGetQueue(device);
        wgpuQueueWriteBuffer(queue, impl.no_exposure, 0, state, sizeof(state));
        wgpuQueueRelease(queue);
    }

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(detail::shader_code("post/tone_map"))};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const WGPUColorTargetState target{.format = output_format, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_main"), .targetCount = 1, .targets = &target};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = pipeline_layout,
        .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main")},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .multisample = {.count = 1, .mask = ~0u},
        .fragment = &fragment,
    };
    impl.pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
}

ToneMapper::~ToneMapper() = default;

bool ToneMapper::ready() const noexcept {
    return impl_->pipeline && impl_->layout && impl_->settings && impl_->sampler && impl_->black_view && impl_->no_exposure;
}

void ToneMapper::render(const WGPUCommandEncoder encoder, const WGPUTextureView scene, const WGPUTextureView output,
                        const ToneSettings& tone, const WGPUTextureView bloom, const WGPUTextureView shafts, const WGPUBuffer exposure) {
    auto& impl = *impl_;
    if (!ready() || !scene || !output) return;
    const SettingsUniform settings{.curve = static_cast<std::uint32_t>(tone.curve),
                                   .exposure = tone.exposure,
                                   .night = tone.night,
                                   .night_exposure = tone.night_exposure,
                                   .saturation = tone.saturation,
                                   .bloom = bloom ? (std::max)(tone.bloom, 0.0f) : 0.0f,
                                   .shafts = shafts && tone.shafts ? 1.0f : 0.0f,
                                   .linear_scene = tone.linear_scene ? 1.0f : 0.0f,
                                   .auto_exposure = tone.auto_exposure && exposure ? 1.0f : 0.0f,
                                   .hdr_scale = tone.paper_white > 0.0f ? tone.paper_white / 80.0f : 0.0f,
                                   .hdr_peak = tone.paper_white > 0.0f ? (std::max)(tone.peak / tone.paper_white, 1.0f) : 1.0f};
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.settings, 0, &settings, sizeof(settings));
        wgpuQueueRelease(queue);
    }
    std::array<WGPUBindGroupEntry, 6> entries{};
    entries[5].binding = 5;
    entries[5].buffer = exposure ? exposure : impl.no_exposure;
    entries[5].size = 16;
    entries[0].binding = 0;
    entries[0].textureView = scene;
    entries[1].binding = 1;
    entries[1].buffer = impl.settings;
    entries[1].size = sizeof(SettingsUniform);
    entries[2].binding = 2;
    entries[2].textureView = bloom ? bloom : impl.black_view;
    entries[3].binding = 3;
    entries[3].sampler = impl.sampler;
    entries[4].binding = 4;
    entries[4].textureView = shafts && tone.shafts ? shafts : impl.black_view;
    const WGPUBindGroupDescriptor group_descriptor{.layout = impl.layout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = wgpuDeviceCreateBindGroup(impl.device, &group_descriptor);
    const WGPURenderPassColorAttachment colour{
        .view = output,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {0.0, 0.0, 0.0, 1.0},
    };
    const auto timestamps = render_pass_timestamps(GpuPass::tone);
    const WGPURenderPassDescriptor pass_descriptor{
        .colorAttachmentCount = 1, .colorAttachments = &colour, .timestampWrites = timestamps ? &*timestamps : nullptr};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

} // namespace openrts::renderer
#endif
