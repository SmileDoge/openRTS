#include <openrts/renderer/depth_export.hpp>
#include <openrts/renderer/gpu_pass_timer.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include "shader_code.hpp"

namespace openrts::renderer {
namespace {

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

} // namespace

struct DepthExport::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    void release_texture() {
        if (view) wgpuTextureViewRelease(view);
        if (texture) wgpuTextureRelease(texture);
        view = nullptr;
        texture = nullptr;
    }

    ~Impl() {
        release_texture();
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }
};

DepthExport::DepthExport(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    WGPUBindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.texture.sampleType = WGPUTextureSampleType_Depth;
    entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    entry.texture.multisampled = true;
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = 1, .entries = &entry};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(detail::shader_code("post/depth_export"))};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const WGPUDepthStencilState depth{
        .format = depth_format,
        .depthWriteEnabled = WGPUOptionalBool_True,
        .depthCompare = WGPUCompareFunction_Always,
        .stencilFront = {.compare = WGPUCompareFunction_Always},
        .stencilBack = {.compare = WGPUCompareFunction_Always},
    };
    const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_main"), .targetCount = 0, .targets = nullptr};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = pipeline_layout,
        .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main")},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = {.count = 1, .mask = ~0u},
        .fragment = &fragment,
    };
    impl.pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
}

DepthExport::~DepthExport() = default;

bool DepthExport::ready() const noexcept { return impl_->pipeline && impl_->layout; }

WGPUTexture DepthExport::texture() const noexcept { return impl_->texture; }

void DepthExport::record(const WGPUCommandEncoder encoder, const WGPUTextureView source, const std::uint32_t width,
                         const std::uint32_t height) {
    auto& impl = *impl_;
    if (!ready() || !source || width == 0 || height == 0) return;
    if (!impl.texture || impl.width != width || impl.height != height) {
        impl.release_texture();
        const WGPUTextureDescriptor descriptor{
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = width, .height = height, .depthOrArrayLayers = 1},
            .format = depth_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        impl.texture = wgpuDeviceCreateTexture(impl.device, &descriptor);
        if (!impl.texture) return;
        impl.view = wgpuTextureCreateView(impl.texture, nullptr);
        impl.width = width;
        impl.height = height;
    }
    const WGPUBindGroupEntry entry{.binding = 0, .textureView = source};
    const WGPUBindGroupDescriptor group_descriptor{.layout = impl.layout, .entryCount = 1, .entries = &entry};
    const auto group = wgpuDeviceCreateBindGroup(impl.device, &group_descriptor);
    const WGPURenderPassDepthStencilAttachment depth{
        .view = impl.view,
        .depthLoadOp = WGPULoadOp_Clear,
        .depthStoreOp = WGPUStoreOp_Store,
        .depthClearValue = depth_clear_value,
        .depthReadOnly = false,
        .stencilLoadOp = WGPULoadOp_Undefined,
        .stencilStoreOp = WGPUStoreOp_Undefined,
        .stencilReadOnly = true,
    };
    const auto timestamps = render_pass_timestamps(GpuPass::depth_copy);
    const WGPURenderPassDescriptor pass_descriptor{
        .colorAttachmentCount = 0, .depthStencilAttachment = &depth, .timestampWrites = timestamps ? &*timestamps : nullptr};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    // Sixteen bands, a draw each, two triangles a band. ReShade's generic
    // depth add-on ignores a depth buffer drawn with three vertices or
    // fewer, and a frame whose only depth buffer had eight draws or fewer
    // (examples/09-depth/generic_depth_addon.cpp, on_present): the RTS path
    // has no other single-sample depth buffer for it.
    constexpr std::uint32_t bands = 16;
    for (std::uint32_t band = 0; band < bands; ++band) wgpuRenderPassEncoderDraw(pass, 6, 1, band * 6, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

} // namespace openrts::renderer
#endif
