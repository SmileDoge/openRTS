#include <openrts/renderer/motion_vectors.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include "shader_code.hpp"

namespace openrts::renderer {
namespace {

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

struct ParamsUniform {
    glm::mat4 inverse_view_projection;
    glm::mat4 previous_view_projection;
    glm::mat4 steady_view_projection;
    float train_shift[4];
    float size[4];
};

// The Halton sequence's `index`th number in `base` (0..1).
[[nodiscard]] float halton(std::uint32_t index, const std::uint32_t base) {
    float result = 0.0f, fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

constexpr WGPUTextureFormat motion_format = WGPUTextureFormat_RGBA16Float;

} // namespace

struct MotionVectors::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer params = nullptr;
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    std::uint32_t width = 0, height = 0;
    bool valid = false; // a last frame to go back to
    glm::mat4 previous_view_projection{1.0f};
    glm::vec3 previous_eye{0.0f};
    std::optional<glm::vec3> previous_train;
    std::uint32_t frame = 0;
    std::uint32_t phases = 8;

    void release_texture() {
        if (view) wgpuTextureViewRelease(view);
        if (texture) wgpuTextureRelease(texture);
        view = nullptr;
        texture = nullptr;
        width = height = 0;
    }

    void ensure(const std::uint32_t w, const std::uint32_t h) {
        if (w == width && h == height && view) return;
        release_texture();
        const WGPUTextureDescriptor descriptor{
            // CopySrc: the upscalers may take a copy.
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = w, .height = h, .depthOrArrayLayers = 1},
            .format = motion_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        texture = wgpuDeviceCreateTexture(device, &descriptor);
        if (texture) view = wgpuTextureCreateView(texture, nullptr);
        width = w;
        height = h;
        valid = false;
    }

    ~Impl() {
        release_texture();
        if (params) wgpuBufferRelease(params);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }
};

MotionVectors::MotionVectors(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    for (std::uint32_t i : {0u, 1u}) {
        entries[i] = {.binding = i, .visibility = WGPUShaderStage_Fragment};
        entries[i].texture.sampleType = WGPUTextureSampleType_Depth;
        entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
    }
    entries[2] = {.binding = 2, .visibility = WGPUShaderStage_Fragment};
    entries[2].buffer.type = WGPUBufferBindingType_Uniform;
    entries[2].buffer.minBindingSize = sizeof(ParamsUniform);
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(ParamsUniform)};
    impl.params = wgpuDeviceCreateBuffer(device, &buffer);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL},
                                      .code = wgsl_name(detail::shader_code("post/motion_vectors"))};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const WGPUColorTargetState target{.format = motion_format, .writeMask = WGPUColorWriteMask_All};
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

MotionVectors::~MotionVectors() = default;

bool MotionVectors::ready() const noexcept {
    return impl_->pipeline && impl_->layout && impl_->params;
}

void MotionVectors::reset() noexcept {
    impl_->valid = false;
}

glm::vec2 MotionVectors::jitter() const noexcept {
    const auto index = impl_->frame % (std::max)(impl_->phases, 1u) + 1;
    return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

void MotionVectors::set_jitter_phases(const std::uint32_t phases) noexcept {
    impl_->phases = (std::max)(phases, 1u);
}

glm::mat4 MotionVectors::jittered(const glm::mat4& projection, const std::uint32_t width, const std::uint32_t height) const noexcept {
    if (width == 0 || height == 0) return projection;
    const auto j = jitter();
    glm::mat4 out = projection;
    // A shift of the clip space's x, y by w times the offset (in NDC).
    out[2][0] += j.x * 2.0f / static_cast<float>(width) * (projection[2][3] != 0.0f ? -projection[2][3] : 1.0f);
    out[2][1] += j.y * 2.0f / static_cast<float>(height) * (projection[2][3] != 0.0f ? -projection[2][3] : 1.0f);
    return out;
}

glm::mat4 MotionVectors::last_view_projection() const noexcept {
    return impl_->previous_view_projection;
}

bool MotionVectors::has_last() const noexcept {
    return impl_->valid;
}

WGPUTextureView MotionVectors::view() const noexcept {
    return impl_->view;
}

WGPUTexture MotionVectors::texture() const noexcept {
    return impl_->texture;
}

WGPUTextureView MotionVectors::compute(const WGPUCommandEncoder encoder, const WGPUTextureView depth, const WGPUTextureView moving_depth,
                                       const MotionFrame& frame, const WGPUTextureView target) {
    auto& impl = *impl_;
    ++impl.frame;
    if (!ready() || !depth || frame.width == 0 || frame.height == 0) {
        impl.valid = false;
        return nullptr;
    }
    impl.ensure(frame.width, frame.height);
    if (!impl.view) return nullptr;
    // A jump of the camera (a cut, the origin moved): no last frame. The
    // trains' pixels move by the player's train's own move (nothing when
    // the camera goes round a standing train).
    if (glm::length(frame.eye - impl.previous_eye) > 50.0f) impl.valid = false;
    glm::vec3 shift{0.0f};
    if (moving_depth && frame.train && impl.previous_train) {
        shift = *frame.train - *impl.previous_train;
        if (glm::length(shift) > 50.0f) shift = glm::vec3{0.0f};
    }
    const ParamsUniform params{
        .inverse_view_projection = glm::inverse(frame.view_projection),
        .previous_view_projection = impl.previous_view_projection,
        .steady_view_projection = frame.steady_view_projection,
        .train_shift = {shift.x, shift.y, shift.z, impl.valid ? 1.0f : 0.0f},
        .size = {static_cast<float>(frame.width), static_cast<float>(frame.height), 1.0f / static_cast<float>(frame.width),
                 1.0f / static_cast<float>(frame.height)},
    };
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.params, 0, &params, sizeof(params));
        wgpuQueueRelease(queue);
    }
    std::array<WGPUBindGroupEntry, 3> entries{};
    entries[0] = {.binding = 0, .textureView = depth};
    // Without the trains' depth the scene's stands in (every pixel then
    // reads as a train's, moved by no shift: as still as the rest).
    entries[1] = {.binding = 1, .textureView = moving_depth ? moving_depth : depth};
    entries[2] = {.binding = 2, .buffer = impl.params, .size = sizeof(ParamsUniform)};
    const WGPUBindGroupDescriptor group_descriptor{.layout = impl.layout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = wgpuDeviceCreateBindGroup(impl.device, &group_descriptor);
    const WGPURenderPassColorAttachment colour{
        .view = target ? target : impl.view,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
    impl.valid = true;
    impl.previous_view_projection = frame.steady_view_projection;
    impl.previous_eye = frame.eye;
    impl.previous_train = frame.train;
    return target ? target : impl.view;
}

} // namespace openrts::renderer
#endif
