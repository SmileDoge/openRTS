#include <openrts/renderer/temporal_aa.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <array>
#include <cmath>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <openrts/renderer/depth.hpp>
#include "shader_code.hpp"

namespace openrts::renderer {
namespace {

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

struct ParamsUniform {
    float size[4];
    float view[4];
    float input[4];
    float jitter[4];
};

} // namespace

struct TemporalAA::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer params = nullptr;
    WGPUSampler sampler = nullptr;
    // The history: the frame before (read) and this one (written), turn about.
    std::array<WGPUTexture, 2> textures{};
    std::array<WGPUTextureView, 2> views{};
    WGPUTexture shown = nullptr; // the panel's view into it (TemporalAAFrame::view)
    WGPUTextureView shown_view = nullptr;
    std::uint32_t width = 0, height = 0;
    std::uint32_t written = 0; // the one written last
    bool valid = false;        // the last written holds a frame to take

    void release_history() {
        for (auto& view : views) {
            if (view) wgpuTextureViewRelease(view);
            view = nullptr;
        }
        for (auto& texture : textures) {
            if (texture) wgpuTextureRelease(texture);
            texture = nullptr;
        }
        if (shown_view) wgpuTextureViewRelease(shown_view);
        if (shown) wgpuTextureRelease(shown);
        shown_view = nullptr;
        shown = nullptr;
        width = height = 0;
        valid = false;
    }

    void ensure(const std::uint32_t w, const std::uint32_t h) {
        if (w == width && h == height && views[0] && views[1]) return;
        release_history();
        for (std::size_t i = 0; i < 3; ++i) {
            const WGPUTextureDescriptor descriptor{
                .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
                .dimension = WGPUTextureDimension_2D,
                .size = {.width = w, .height = h, .depthOrArrayLayers = 1},
                .format = hdr_format,
                .mipLevelCount = 1,
                .sampleCount = 1,
            };
            auto& texture = i < 2 ? textures[i] : shown;
            auto& view = i < 2 ? views[i] : shown_view;
            texture = wgpuDeviceCreateTexture(device, &descriptor);
            if (texture) view = wgpuTextureCreateView(texture, nullptr);
        }
        width = w;
        height = h;
    }

    ~Impl() {
        release_history();
        if (sampler) wgpuSamplerRelease(sampler);
        if (params) wgpuBufferRelease(params);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }
};

TemporalAA::TemporalAA(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    std::array<WGPUBindGroupLayoutEntry, 6> entries{};
    entries[0] = {.binding = 0, .visibility = WGPUShaderStage_Fragment};
    entries[0].texture.sampleType = WGPUTextureSampleType_Float; // filtered when upscaling
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1] = {.binding = 1, .visibility = WGPUShaderStage_Fragment};
    entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2] = {.binding = 2, .visibility = WGPUShaderStage_Fragment};
    entries[2].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[3] = {.binding = 3, .visibility = WGPUShaderStage_Fragment};
    entries[3].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[3].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[4] = {.binding = 4, .visibility = WGPUShaderStage_Fragment};
    entries[4].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[5] = {.binding = 5, .visibility = WGPUShaderStage_Fragment};
    entries[5].buffer.type = WGPUBufferBindingType_Uniform;
    entries[5].buffer.minBindingSize = sizeof(ParamsUniform);
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);

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
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(ParamsUniform)};
    impl.params = wgpuDeviceCreateBuffer(device, &buffer);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL},
                                      .code = wgsl_name(detail::shader_code("post/temporal_aa"))};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const std::array<WGPUColorTargetState, 2> targets{WGPUColorTargetState{.format = hdr_format, .writeMask = WGPUColorWriteMask_All},
                                                      WGPUColorTargetState{.format = hdr_format, .writeMask = WGPUColorWriteMask_All}};
    const WGPUFragmentState fragment{
        .module = shader, .entryPoint = wgsl_name("fs_main"), .targetCount = targets.size(), .targets = targets.data()};
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

TemporalAA::~TemporalAA() = default;

bool TemporalAA::ready() const noexcept {
    return impl_->pipeline && impl_->layout && impl_->params && impl_->sampler;
}

void TemporalAA::reset() noexcept {
    impl_->valid = false;
}

WGPUTextureView TemporalAA::resolve(const WGPUCommandEncoder encoder, const WGPUTextureView scene, const WGPUTextureView depth,
                                    const WGPUTextureView motion, const TemporalAAFrame& frame) {
    auto& impl = *impl_;
    if (!ready() || !scene || !depth || !motion || frame.width == 0 || frame.height == 0) {
        impl.valid = false;
        return scene;
    }
    impl.ensure(frame.width, frame.height);
    const auto input_width = frame.input_width ? frame.input_width : frame.width;
    const auto input_height = frame.input_height ? frame.input_height : frame.height;
    if (!impl.views[0] || !impl.views[1] || !impl.shown_view) return scene;
    const auto read = impl.written;
    const auto write = 1u - impl.written;
    ParamsUniform params{
        .size = {static_cast<float>(frame.width), static_cast<float>(frame.height), 1.0f / static_cast<float>(frame.width),
                 1.0f / static_cast<float>(frame.height)},
        .view = {static_cast<float>(frame.view), impl.valid ? 1.0f : 0.0f, 0.0f, 0.0f},
        .input = {static_cast<float>(input_width), static_cast<float>(input_height), 1.0f / static_cast<float>(input_width),
                  1.0f / static_cast<float>(input_height)},
        .jitter = {frame.jitter.x, frame.jitter.y, 0.0f, 0.0f},
    };
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.params, 0, &params, sizeof(params));
        wgpuQueueRelease(queue);
    }
    std::array<WGPUBindGroupEntry, 6> entries{};
    entries[0] = {.binding = 0, .textureView = scene};
    entries[1] = {.binding = 1, .textureView = impl.views[read]};
    entries[2] = {.binding = 2, .textureView = depth};
    entries[3] = {.binding = 3, .textureView = motion};
    entries[4] = {.binding = 4, .sampler = impl.sampler};
    entries[5] = {.binding = 5, .buffer = impl.params, .size = sizeof(ParamsUniform)};
    const WGPUBindGroupDescriptor group_descriptor{.layout = impl.layout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = wgpuDeviceCreateBindGroup(impl.device, &group_descriptor);
    std::array<WGPURenderPassColorAttachment, 2> colours{};
    for (std::size_t i = 0; i < 2; ++i) {
        colours[i] = {.view = i == 0 ? impl.views[write] : impl.shown_view,
                      .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                      .loadOp = WGPULoadOp_Clear,
                      .storeOp = WGPUStoreOp_Store,
                      .clearValue = {0.0, 0.0, 0.0, 1.0}};
    }
    const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = colours.size(), .colorAttachments = colours.data()};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
    impl.written = write;
    impl.valid = true;
    return frame.view != 0 ? impl.shown_view : impl.views[write];
}

} // namespace openrts::renderer
#endif
