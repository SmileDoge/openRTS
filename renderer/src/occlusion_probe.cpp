#include <openrts/renderer/occlusion_probe.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>

#include <algorithm>
#include <cstring>

#include <glm/vec4.hpp>
#include <webgpu/wgpu.h> // wgpuDevicePoll

namespace openrts::renderer {
namespace {

constexpr const char* probe_shader = R"(
@vertex
fn vs_main(@location(0) position: vec4<f32>) -> @builtin(position) vec4<f32> {
    return position;
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(0.0);
}
)";

[[nodiscard]] WGPUStringView text(const char* value) { return {.data = value, .length = WGPU_STRLEN}; }

constexpr float half_size = 2.5f;  // pixels
constexpr float depth_pull = 1.04f; // reversed Z: depth ~ 1 / distance
constexpr std::size_t slot_count = 3;

struct Slot {
    WGPUBuffer readback = nullptr;
    std::uint32_t count = 0;
    std::uint64_t tag = 0;
    bool busy = false;
};

} // namespace

struct OcclusionProbe::Impl {
    WGPUDevice device = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUQuerySet queries = nullptr;
    WGPUBuffer resolved = nullptr;
    WGPUBuffer vertices = nullptr;
    std::array<Slot, slot_count> slots{};
    std::vector<glm::vec4> corners; // this frame's squares, 6 vertices each
    std::uint64_t tag = 0;
    Slot* current = nullptr;
    bool has_result = false;
    std::uint64_t result_tag = 0;
    std::vector<bool> result;

    static void on_mapped(WGPUMapAsyncStatus status, WGPUStringView, void* slot_pointer, void* impl_pointer) {
        auto& slot = *static_cast<Slot*>(slot_pointer);
        auto& self = *static_cast<Impl*>(impl_pointer);
        slot.busy = false;
        if (status != WGPUMapAsyncStatus_Success) return;
        self.result.assign(slot.count, true);
        if (const auto* data = static_cast<const std::uint64_t*>(
                wgpuBufferGetConstMappedRange(slot.readback, 0, slot.count * sizeof(std::uint64_t)))) {
            for (std::uint32_t i = 0; i < slot.count; ++i) self.result[i] = data[i] != 0;
        }
        wgpuBufferUnmap(slot.readback);
        self.result_tag = slot.tag;
        self.has_result = true;
    }

    ~Impl() {
        const bool pending = std::any_of(slots.begin(), slots.end(), [](const Slot& s) { return s.busy; });
        if (pending && device) wgpuDevicePoll(device, true, nullptr);
        for (auto& slot : slots) {
            if (slot.readback) wgpuBufferRelease(slot.readback);
        }
        if (vertices) wgpuBufferRelease(vertices);
        if (resolved) wgpuBufferRelease(resolved);
        if (queries) wgpuQuerySetRelease(queries);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
    }
};

OcclusionProbe::OcclusionProbe(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    auto& self = *impl_;
    self.device = device;
    const WGPUQuerySetDescriptor query_descriptor{.type = WGPUQueryType_Occlusion, .count = max_points};
    self.queries = wgpuDeviceCreateQuerySet(device, &query_descriptor);
    const WGPUBufferDescriptor resolved{.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc,
                                        .size = max_points * sizeof(std::uint64_t)};
    self.resolved = wgpuDeviceCreateBuffer(device, &resolved);
    const WGPUBufferDescriptor vertices{.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                        .size = max_points * 6 * sizeof(glm::vec4)};
    self.vertices = wgpuDeviceCreateBuffer(device, &vertices);
    for (auto& slot : self.slots) {
        const WGPUBufferDescriptor readback{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                                            .size = max_points * sizeof(std::uint64_t)};
        slot.readback = wgpuDeviceCreateBuffer(device, &readback);
    }

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = text(probe_shader)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 0, .bindGroupLayouts = nullptr};
    const auto layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
    const WGPUVertexAttribute attribute{.format = WGPUVertexFormat_Float32x4, .offset = 0, .shaderLocation = 0};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(glm::vec4),
        .attributeCount = 1,
        .attributes = &attribute,
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
    const WGPUColorTargetState target{.format = target_format, .writeMask = WGPUColorWriteMask_None};
    const WGPUFragmentState fragment{.module = shader, .entryPoint = text("fs_main"), .targetCount = 1, .targets = &target};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {.module = shader, .entryPoint = text("vs_main"), .bufferCount = 1, .buffers = &vertex_layout},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None},
        .depthStencil = &depth,
        .multisample = scene_multisample(),
        .fragment = &fragment,
    };
    self.pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(layout);
    wgpuShaderModuleRelease(shader);
}

OcclusionProbe::~OcclusionProbe() = default;

bool OcclusionProbe::ready() const noexcept {
    return impl_->pipeline && impl_->queries && impl_->resolved && impl_->vertices;
}

WGPUQuerySet OcclusionProbe::query_set() const noexcept { return impl_->queries; }

void OcclusionProbe::request(const std::vector<Point>& points, const std::uint64_t tag, const glm::vec2 viewport) {
    auto& self = *impl_;
    self.corners.clear();
    self.tag = tag;
    if (viewport.x <= 0.0f || viewport.y <= 0.0f) return;
    const glm::vec2 half{half_size * 2.0f / viewport.x, half_size * 2.0f / viewport.y};
    const auto count = (std::min)(points.size(), std::size_t{max_points});
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = points[i];
        const float z = (std::min)(p.depth * depth_pull, 1.0f);
        const glm::vec4 a{p.ndc.x - half.x, p.ndc.y - half.y, z, 1.0f};
        const glm::vec4 b{p.ndc.x + half.x, p.ndc.y - half.y, z, 1.0f};
        const glm::vec4 c{p.ndc.x + half.x, p.ndc.y + half.y, z, 1.0f};
        const glm::vec4 d{p.ndc.x - half.x, p.ndc.y + half.y, z, 1.0f};
        self.corners.insert(self.corners.end(), {a, b, c, a, c, d});
    }
}

void OcclusionProbe::draw(const WGPURenderPassEncoder pass) {
    auto& self = *impl_;
    self.current = nullptr;
    if (!ready() || self.corners.empty()) return;
    for (auto& slot : self.slots) {
        if (!slot.busy) {
            self.current = &slot;
            break;
        }
    }
    if (!self.current) return;
    const auto count = static_cast<std::uint32_t>(self.corners.size() / 6);
    self.current->count = count;
    self.current->tag = self.tag;
    if (const auto queue = wgpuDeviceGetQueue(self.device)) {
        wgpuQueueWriteBuffer(queue, self.vertices, 0, self.corners.data(), self.corners.size() * sizeof(glm::vec4));
        wgpuQueueRelease(queue);
    }
    wgpuRenderPassEncoderSetPipeline(pass, self.pipeline);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, self.vertices, 0, self.corners.size() * sizeof(glm::vec4));
    for (std::uint32_t i = 0; i < count; ++i) {
        wgpuRenderPassEncoderBeginOcclusionQuery(pass, i);
        wgpuRenderPassEncoderDraw(pass, 6, 1, i * 6, 0);
        wgpuRenderPassEncoderEndOcclusionQuery(pass);
    }
    self.corners.clear();
}

void OcclusionProbe::resolve(const WGPUCommandEncoder encoder) {
    auto& self = *impl_;
    if (!self.current) return;
    const auto bytes = static_cast<std::uint64_t>(self.current->count) * sizeof(std::uint64_t);
    wgpuCommandEncoderResolveQuerySet(encoder, self.queries, 0, self.current->count, self.resolved, 0);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, self.resolved, 0, self.current->readback, 0, bytes);
}

void OcclusionProbe::submitted() {
    auto& self = *impl_;
    if (!self.current) return;
    auto& slot = *self.current;
    slot.busy = true;
    const WGPUBufferMapCallbackInfo callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = Impl::on_mapped,
        .userdata1 = &slot,
        .userdata2 = &self,
    };
    wgpuBufferMapAsync(slot.readback, WGPUMapMode_Read, 0, static_cast<std::uint64_t>(slot.count) * sizeof(std::uint64_t),
                       callback);
    self.current = nullptr;
}

bool OcclusionProbe::take(std::uint64_t& tag, std::vector<bool>& visible) {
    auto& self = *impl_;
    if (!self.has_result) return false;
    self.has_result = false;
    tag = self.result_tag;
    visible = self.result;
    return true;
}

} // namespace openrts::renderer
#endif
