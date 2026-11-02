#include <openrts/renderer/wide_lines.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>

#include <array>
#include <cmath>
#include <cstddef>

#include <glm/vec4.hpp>

namespace openrts::renderer {
namespace {

// Vertices come in clip space: the strip is built on the CPU each frame.
constexpr const char* wide_line_shader = R"(
struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
};

@vertex
fn vs_main(@location(0) position: vec4<f32>, @location(1) colour: vec4<f32>) -> VertexOut {
    var out: VertexOut;
    out.position = position;
    out.colour = colour;
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    return input.colour;
}
)";

struct StripVertex {
    glm::vec4 position;
    std::uint32_t colour;
};

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

// The near clip: points closer than this (clip w, metres) are cut off.
constexpr float near_w = 0.02f;
// Pulled nearer by this share of the distance (reversed Z: depth ~ 1 / distance).
constexpr float depth_pull = 1.02f;
// At most 100 000 lines a frame (12 MB of vertices).
constexpr std::size_t max_vertices = 600000;

} // namespace

struct WideLineRenderer::Impl {
    WGPUDevice device = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer buffer = nullptr;
    std::uint64_t capacity = 0;
    std::vector<StripVertex> vertices;

    ~Impl() {
        if (buffer) wgpuBufferRelease(buffer);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
    }
};

WideLineRenderer::WideLineRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(wide_line_shader)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 0, .bindGroupLayouts = nullptr};
    const auto layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);

    const std::array<WGPUVertexAttribute, 2> attributes{{
        {.format = WGPUVertexFormat_Float32x4, .offset = offsetof(StripVertex, position), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Unorm8x4, .offset = offsetof(StripVertex, colour), .shaderLocation = 1},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(StripVertex),
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
    const WGPUBlendState blend{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_SrcAlpha,
                  .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One,
                  .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
    };
    const WGPUColorTargetState target{.format = target_format, .blend = &blend, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{
        .module = shader,
        .entryPoint = wgsl_name("fs_main"),
        .targetCount = 1,
        .targets = &target,
    };
    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main"), .bufferCount = 1, .buffers = &vertex_layout},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None},
        .depthStencil = &depth,
        .multisample = scene_multisample(),
        .fragment = &fragment,
    };
    impl_->pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(layout);
    wgpuShaderModuleRelease(shader);
}

WideLineRenderer::~WideLineRenderer() = default;

bool WideLineRenderer::ready() const noexcept { return impl_->pipeline != nullptr; }

void WideLineRenderer::render(const WGPURenderPassEncoder pass, const std::vector<WideLine>& lines,
                              const glm::mat4& view_projection, const glm::vec2 viewport) {
    if (!ready() || lines.empty() || viewport.x <= 0.0f || viewport.y <= 0.0f) return;
    auto& out = impl_->vertices;
    out.clear();
    for (const auto& line : lines) {
        // A helper overlay never needs more; it keeps the buffer far below
        // the device's limit whatever a caller passes.
        if (out.size() >= max_vertices) break;
        auto a = view_projection * glm::vec4{line.a, 1.0f};
        auto b = view_projection * glm::vec4{line.b, 1.0f};
        if (a.w < near_w && b.w < near_w) continue;
        if (a.w < near_w) a = a + (b - a) * ((near_w - a.w) / (b.w - a.w));
        if (b.w < near_w) b = b + (a - b) * ((near_w - b.w) / (a.w - b.w));
        // The strip's half width across the line on the screen, in NDC.
        const glm::vec2 sa{a.x / a.w * viewport.x, a.y / a.w * viewport.y};
        const glm::vec2 sb{b.x / b.w * viewport.x, b.y / b.w * viewport.y};
        glm::vec2 along = sb - sa;
        const float length = std::sqrt(along.x * along.x + along.y * along.y);
        along = length > 1e-6f ? along / length : glm::vec2{1.0f, 0.0f};
        const glm::vec2 across{-along.y * line.thickness / viewport.x, along.x * line.thickness / viewport.y};
        const auto corner = [&](const glm::vec4& c, const float side) {
            return StripVertex{glm::vec4{c.x + across.x * side * c.w, c.y + across.y * side * c.w,
                                         (std::min)(c.z * depth_pull, c.w), c.w},
                               line.colour};
        };
        const auto a0 = corner(a, -1.0f), a1 = corner(a, 1.0f), b0 = corner(b, -1.0f), b1 = corner(b, 1.0f);
        out.insert(out.end(), {a0, a1, b1, a0, b1, b0});
    }
    if (out.empty()) return;
    const auto bytes = static_cast<std::uint64_t>(out.size() * sizeof(StripVertex));
    if (bytes > impl_->capacity) {
        if (impl_->buffer) wgpuBufferRelease(impl_->buffer);
        impl_->capacity = bytes * 2;
        const WGPUBufferDescriptor descriptor{.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                              .size = impl_->capacity};
        impl_->buffer = wgpuDeviceCreateBuffer(impl_->device, &descriptor);
    }
    if (const auto queue = wgpuDeviceGetQueue(impl_->device)) {
        wgpuQueueWriteBuffer(queue, impl_->buffer, 0, out.data(), bytes);
        wgpuQueueRelease(queue);
    }
    wgpuRenderPassEncoderSetPipeline(pass, impl_->pipeline);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, impl_->buffer, 0, bytes);
    wgpuRenderPassEncoderDraw(pass, static_cast<std::uint32_t>(out.size()), 1, 0, 0);
}

} // namespace openrts::renderer
#endif
