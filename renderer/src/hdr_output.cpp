#include <openrts/renderer/hdr_output.hpp>

#include <algorithm>
#include <array>

#include "shader_code.hpp"

namespace openrts::renderer {

namespace {
[[nodiscard]] WGPUStringView name(const char* text) { return {.data = text, .length = WGPU_STRLEN}; }
} // namespace

struct UiCompositor::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer uniform = nullptr;
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    std::uint32_t width = 0, height = 0;

    void release_target() {
        if (view) wgpuTextureViewRelease(view);
        if (texture) wgpuTextureRelease(texture);
        view = nullptr;
        texture = nullptr;
    }
    ~Impl() {
        release_target();
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (uniform) wgpuBufferRelease(uniform);
    }
};

UiCompositor::UiCompositor(const WGPUDevice device, const WGPUTextureFormat output_format) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = 16;
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = 16};
    impl.uniform = wgpuDeviceCreateBuffer(device, &buffer);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = name(detail::shader_code("post/hdr_ui"))};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const WGPUBlendState blend{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
    };
    const WGPUColorTargetState target{.format = output_format, .blend = &blend, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{.module = shader, .entryPoint = name("fs_main"), .targetCount = 1, .targets = &target};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = pipeline_layout,
        .vertex = {.module = shader, .entryPoint = name("vs_main")},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .multisample = {.count = 1, .mask = ~0u},
        .fragment = &fragment,
    };
    impl.pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
}

UiCompositor::~UiCompositor() = default;

bool UiCompositor::ready() const noexcept { return impl_->pipeline && impl_->layout && impl_->uniform; }

WGPUTextureView UiCompositor::target(const std::uint32_t width, const std::uint32_t height) {
    auto& impl = *impl_;
    if (impl.view && impl.width == width && impl.height == height) return impl.view;
    impl.release_target();
    const WGPUTextureDescriptor descriptor{
        .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = (std::max)(width, 1u), .height = (std::max)(height, 1u), .depthOrArrayLayers = 1},
        .format = hdr_ui_format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    impl.texture = wgpuDeviceCreateTexture(impl.device, &descriptor);
    if (impl.texture) impl.view = wgpuTextureCreateView(impl.texture, nullptr);
    impl.width = width;
    impl.height = height;
    return impl.view;
}

void UiCompositor::compose(const WGPUCommandEncoder encoder, const WGPUTextureView output, const float paper_white) {
    auto& impl = *impl_;
    if (!ready() || !impl.view || !output) return;
    const float scale[4] = {paper_white / 80.0f, 0.0f, 0.0f, 0.0f};
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.uniform, 0, scale, sizeof(scale));
        wgpuQueueRelease(queue);
    }
    std::array<WGPUBindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].textureView = impl.view;
    entries[1].binding = 1;
    entries[1].buffer = impl.uniform;
    entries[1].size = 16;
    const WGPUBindGroupDescriptor group_descriptor{.layout = impl.layout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = wgpuDeviceCreateBindGroup(impl.device, &group_descriptor);
    const WGPURenderPassColorAttachment colour{
        .view = output, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Load, .storeOp = WGPUStoreOp_Store};
    const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

HdrCalibration::HdrCalibration(const WGPUDevice device, const WGPUTextureFormat output_format) : device_(device) {
    WGPUBindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.minBindingSize = 32;
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = 1, .entries = &entry};
    layout_ = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = 32};
    uniform_ = wgpuDeviceCreateBuffer(device, &buffer);
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = name(detail::shader_code("post/hdr_calibration"))};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout_};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const WGPUColorTargetState target{.format = output_format, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{.module = shader, .entryPoint = name("fs_main"), .targetCount = 1, .targets = &target};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = pipeline_layout,
        .vertex = {.module = shader, .entryPoint = name("vs_main")},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .multisample = {.count = 1, .mask = ~0u},
        .fragment = &fragment,
    };
    pipeline_ = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
}

HdrCalibration::~HdrCalibration() {
    if (pipeline_) wgpuRenderPipelineRelease(pipeline_);
    if (layout_) wgpuBindGroupLayoutRelease(layout_);
    if (uniform_) wgpuBufferRelease(uniform_);
}

void HdrCalibration::draw(const WGPUCommandEncoder encoder, const WGPUTextureView output, const int mode, const float peak, const float paper,
                          const std::uint32_t width, const std::uint32_t height) {
    if (!pipeline_ || !layout_ || !uniform_ || !output || mode <= 0) return;
    const float values[8] = {static_cast<float>(mode), peak, paper, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f};
    if (const auto queue = wgpuDeviceGetQueue(device_)) {
        wgpuQueueWriteBuffer(queue, uniform_, 0, values, sizeof(values));
        wgpuQueueRelease(queue);
    }
    WGPUBindGroupEntry entry{};
    entry.binding = 0;
    entry.buffer = uniform_;
    entry.size = 32;
    const WGPUBindGroupDescriptor group_descriptor{.layout = layout_, .entryCount = 1, .entries = &entry};
    const auto group = wgpuDeviceCreateBindGroup(device_, &group_descriptor);
    const WGPURenderPassColorAttachment colour{
        .view = output, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store};
    const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

} // namespace openrts::renderer
