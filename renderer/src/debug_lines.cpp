#include <openrts/renderer/debug_lines.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include <array>
#include <cstddef>

namespace openrts::renderer {
namespace {

constexpr const char* line_shader = R"(
struct Frame {
    view_projection: mat4x4<f32>,
};
@group(0) @binding(0) var<uniform> frame: Frame;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
};

@vertex
fn vs_main(@location(0) position: vec3<f32>, @location(1) colour: vec4<f32>) -> VertexOut {
    var out: VertexOut;
    out.position = frame.view_projection * vec4<f32>(position, 1.0);
    out.colour = colour;
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    return input.colour;
}
)";

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

[[nodiscard]] WGPURenderPipeline create_pipeline(const WGPUDevice device, const WGPUPipelineLayout layout,
                                                 const WGPUShaderModule shader, const WGPUTextureFormat target_format,
                                                 const bool depth_test) {
    const std::array<WGPUVertexAttribute, 2> attributes{{
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(DebugVertex, position), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Unorm8x4, .offset = offsetof(DebugVertex, colour), .shaderLocation = 1},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(DebugVertex),
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
        .depthCompare = depth_test ? depth_nearer_or_equal : WGPUCompareFunction_Always,
        .stencilFront = keep,
        .stencilBack = keep,
        .stencilReadMask = ~0u,
        .stencilWriteMask = ~0u,
    };
    const WGPUColorTargetState target{.format = target_format, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{
        .module = shader,
        .entryPoint = wgsl_name("fs_main"),
        .targetCount = 1,
        .targets = &target,
    };
    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main"), .bufferCount = 1, .buffers = &vertex_layout},
        .primitive = {.topology = WGPUPrimitiveTopology_LineList},
        .depthStencil = &depth,
        .multisample = {.count = 1, .mask = ~0u},
        .fragment = &fragment,
    };
    return wgpuDeviceCreateRenderPipeline(device, &descriptor);
}

struct LineBuffer {
    WGPUBuffer buffer = nullptr;
    std::uint64_t capacity = 0; // bytes
    std::uint32_t count = 0;    // vertices

    void release() {
        if (buffer) wgpuBufferRelease(buffer);
        buffer = nullptr;
        capacity = 0;
        count = 0;
    }

    void upload(const WGPUDevice device, const std::vector<DebugVertex>& vertices) {
        count = static_cast<std::uint32_t>(vertices.size() & ~std::size_t{1});
        if (count == 0) return;
        const auto bytes = static_cast<std::uint64_t>(count) * sizeof(DebugVertex);
        if (bytes > capacity) {
            if (buffer) wgpuBufferRelease(buffer);
            capacity = bytes * 2;
            const WGPUBufferDescriptor descriptor{.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                                  .size = capacity};
            buffer = wgpuDeviceCreateBuffer(device, &descriptor);
        }
        if (const auto queue = wgpuDeviceGetQueue(device)) {
            wgpuQueueWriteBuffer(queue, buffer, 0, vertices.data(), bytes);
            wgpuQueueRelease(queue);
        }
    }
};

} // namespace

struct DebugLineRenderer::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline tested = nullptr;
    WGPURenderPipeline through = nullptr;
    WGPUBuffer uniform = nullptr;
    WGPUBindGroup binding = nullptr;
    LineBuffer lines;
    LineBuffer overlay;

    ~Impl() {
        lines.release();
        overlay.release();
        if (binding) wgpuBindGroupRelease(binding);
        if (uniform) wgpuBufferRelease(uniform);
        if (tested) wgpuRenderPipelineRelease(tested);
        if (through) wgpuRenderPipelineRelease(through);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }
};

DebugLineRenderer::DebugLineRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
    WGPUBindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Vertex;
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.minBindingSize = sizeof(glm::mat4);
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = 1, .entries = &entry};
    impl_->layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);

    const WGPUBufferDescriptor uniform_descriptor{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                                                  .size = sizeof(glm::mat4)};
    impl_->uniform = wgpuDeviceCreateBuffer(device, &uniform_descriptor);
    WGPUBindGroupEntry binding{};
    binding.binding = 0;
    binding.buffer = impl_->uniform;
    binding.size = sizeof(glm::mat4);
    const WGPUBindGroupDescriptor group{.layout = impl_->layout, .entryCount = 1, .entries = &binding};
    impl_->binding = wgpuDeviceCreateBindGroup(device, &group);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(line_shader)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1,
                                                                  .bindGroupLayouts = &impl_->layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    impl_->tested = create_pipeline(device, pipeline_layout, shader, target_format, true);
    impl_->through = create_pipeline(device, pipeline_layout, shader, target_format, false);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
}

DebugLineRenderer::~DebugLineRenderer() = default;

bool DebugLineRenderer::ready() const noexcept {
    return impl_->tested && impl_->through && impl_->binding;
}

void DebugLineRenderer::set_lines(const std::vector<DebugVertex>& lines) {
    impl_->lines.upload(impl_->device, lines);
}

void DebugLineRenderer::set_overlay(const std::vector<DebugVertex>& lines) {
    impl_->overlay.upload(impl_->device, lines);
}

void DebugLineRenderer::render(const WGPURenderPassEncoder pass, const glm::mat4& view_projection) {
    if (!ready()) return;
    if (const auto queue = wgpuDeviceGetQueue(impl_->device)) {
        wgpuQueueWriteBuffer(queue, impl_->uniform, 0, &view_projection, sizeof(view_projection));
        wgpuQueueRelease(queue);
    }
    wgpuRenderPassEncoderSetBindGroup(pass, 0, impl_->binding, 0, nullptr);
    if (impl_->lines.count > 0) {
        wgpuRenderPassEncoderSetPipeline(pass, impl_->tested);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, impl_->lines.buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, impl_->lines.count, 1, 0, 0);
    }
    if (impl_->overlay.count > 0) {
        wgpuRenderPassEncoderSetPipeline(pass, impl_->through);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, impl_->overlay.buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, impl_->overlay.count, 1, 0, 0);
    }
}

} // namespace openrts::renderer
#endif
