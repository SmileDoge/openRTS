#include <openrts/renderer/light_shafts.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <array>
#include <string>

#include <openrts/renderer/gpu_pass_timer.hpp>
#include <openrts/renderer/scene_effects.hpp>
#include "shader_code.hpp"

namespace openrts::renderer {
namespace {

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

struct ShaftUniform {
    glm::mat4 inv_view_projection{1.0f};
    glm::mat4 to_room{1.0f};
    glm::vec4 eye{};
    glm::vec4 room_lower{};
    glm::vec4 room_upper{};
    glm::vec4 sizes{};
    glm::vec4 march{};
};

constexpr WGPUTextureFormat shaft_format = WGPUTextureFormat_RGBA16Float;

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
    void ensure(const WGPUDevice device, const std::uint32_t w, const std::uint32_t h, const char* label) {
        if (texture && width == w && height == h) return;
        release();
        const WGPUTextureDescriptor descriptor{
            .label = wgsl_name(label),
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = w, .height = h, .depthOrArrayLayers = 1},
            .format = shaft_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        texture = wgpuDeviceCreateTexture(device, &descriptor);
        if (texture) view = wgpuTextureCreateView(texture, nullptr);
        width = w;
        height = h;
    }
};

} // namespace

struct LightShaftRenderer::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;         // group 0 of every pass
    WGPUBindGroupLayout effects_layout = nullptr; // group 1 of the march (the effects')
    WGPURenderPipeline march = nullptr;
    WGPURenderPipeline blur_x = nullptr;
    WGPURenderPipeline blur_y = nullptr;
    WGPURenderPipeline up = nullptr;
    WGPUBuffer uniform = nullptr;
    Image a, b, full;

    ~Impl() {
        a.release();
        b.release();
        full.release();
        for (auto* p : {march, blur_x, blur_y, up})
            if (p) wgpuRenderPipelineRelease(p);
        if (uniform) wgpuBufferRelease(uniform);
        if (effects_layout) wgpuBindGroupLayoutRelease(effects_layout);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }

    WGPURenderPipeline pipeline(const std::string& code, const char* entry, const bool with_effects) {
        const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = {.data = code.c_str(), .length = code.size()}};
        const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain, .label = wgsl_name("light shafts")};
        const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
        if (!shader) return nullptr;
        const WGPUBindGroupLayout layouts[] = {layout, effects_layout};
        const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = with_effects ? 2u : 1u, .bindGroupLayouts = layouts};
        const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
        const WGPUColorTargetState target{.format = shaft_format, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name(entry), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .label = wgsl_name(entry),
            .layout = pipeline_layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main")},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u},
            .fragment = &fragment,
        };
        const auto made = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        wgpuPipelineLayoutRelease(pipeline_layout);
        wgpuShaderModuleRelease(shader);
        return made;
    }

    [[nodiscard]] WGPUBindGroup group(const WGPUTextureView depth, const WGPUTextureView input) const {
        std::array<WGPUBindGroupEntry, 3> entries{};
        entries[0].binding = 0;
        entries[0].buffer = uniform;
        entries[0].size = sizeof(ShaftUniform);
        entries[1].binding = 1;
        entries[1].textureView = depth;
        entries[2].binding = 2;
        entries[2].textureView = input;
        const WGPUBindGroupDescriptor descriptor{.layout = layout, .entryCount = entries.size(), .entries = entries.data()};
        return wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    void pass(const WGPUCommandEncoder encoder, const WGPUTextureView target, const WGPURenderPipeline pipeline, const WGPUBindGroup own,
              const WGPUBindGroup effects, const bool timed) const {
        const WGPURenderPassColorAttachment colour{
            .view = target,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = {0.0, 0.0, 0.0, 0.0},
        };
        const auto timestamps = timed ? render_pass_timestamps(GpuPass::light_shafts) : std::nullopt;
        const WGPURenderPassDescriptor descriptor{
            .colorAttachmentCount = 1, .colorAttachments = &colour, .timestampWrites = timestamps ? &*timestamps : nullptr};
        const auto encoded = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        wgpuRenderPassEncoderSetPipeline(encoded, pipeline);
        wgpuRenderPassEncoderSetBindGroup(encoded, 0, own, 0, nullptr);
        if (effects) wgpuRenderPassEncoderSetBindGroup(encoded, 1, effects, 0, nullptr);
        wgpuRenderPassEncoderDraw(encoded, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(encoded);
        wgpuRenderPassEncoderRelease(encoded);
    }
};

LightShaftRenderer::LightShaftRenderer(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(ShaftUniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    impl.effects_layout = create_scene_effects_layout(device);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(ShaftUniform)};
    impl.uniform = wgpuDeviceCreateBuffer(device, &buffer);
    if (!impl.layout || !impl.effects_layout || !impl.uniform) return;
    const std::string march = std::string(detail::shader_code("common/effects_struct")) + detail::shader_code("effects/light_shafts_common") + detail::shader_code("effects/light_shafts_march");
    const std::string filter = std::string(detail::shader_code("effects/light_shafts_common")) + detail::shader_code("effects/light_shafts_filter");
    impl.march = impl.pipeline(march, "fs_march", true);
    impl.blur_x = impl.pipeline(filter, "fs_blur_x", false);
    impl.blur_y = impl.pipeline(filter, "fs_blur_y", false);
    impl.up = impl.pipeline(filter, "fs_up", false);
}

LightShaftRenderer::~LightShaftRenderer() = default;

WGPUTexture LightShaftRenderer::texture() const noexcept {
    return impl_->full.texture;
}

bool LightShaftRenderer::ready() const noexcept {
    const auto& impl = *impl_;
    return impl.march && impl.blur_x && impl.blur_y && impl.up && impl.uniform;
}

WGPUTextureView LightShaftRenderer::render(const WGPUCommandEncoder encoder, const WGPUBindGroup effects, const WGPUTextureView depth,
                                           const LightShaftFrame& frame) {
    auto& impl = *impl_;
    if (!ready() || !effects || !depth || frame.width == 0 || frame.height == 0) return nullptr;
    const std::uint32_t half_w = (std::max)((frame.width + 1) / 2, 1u), half_h = (std::max)((frame.height + 1) / 2, 1u);
    impl.a.ensure(impl.device, half_w, half_h, "light shafts a");
    impl.b.ensure(impl.device, half_w, half_h, "light shafts b");
    impl.full.ensure(impl.device, frame.width, frame.height, "light shafts");
    if (!impl.a.view || !impl.b.view || !impl.full.view) return nullptr;

    const float strength = std::clamp(frame.strength, 0.0f, 4.0f);
    const float fog_end = frame.fog_end > 1.0f ? frame.fog_end : 3000.0f;
    ShaftUniform u;
    u.inv_view_projection = glm::inverse(frame.projection * frame.view);
    u.eye = glm::vec4{frame.eye, frame.room ? 1.0f : 0.0f};
    // Outdoors a dust of 1/1500 per metre and half of RTS's haze; in a room
    // 0.3 per metre (a beam through a window shows over a metre).
    const float outdoor = strength * (1.0f / 1500.0f + 0.5f / fog_end);
    const float indoor = strength * 0.3f;
    if (frame.room) {
        u.to_room = frame.room->to_room;
        u.room_lower = glm::vec4{frame.room->lower, indoor};
        u.room_upper = glm::vec4{frame.room->upper, outdoor};
    } else {
        u.room_lower = glm::vec4{0.0f, 0.0f, 0.0f, 0.0f};
        u.room_upper = glm::vec4{0.0f, 0.0f, 0.0f, outdoor};
    }
    u.sizes = glm::vec4{static_cast<float>(half_w), static_cast<float>(half_h), static_cast<float>(frame.width),
                        static_cast<float>(frame.height)};
    u.march = glm::vec4{(std::max)(frame.shadow_distance, 10.0f), 1500.0f, strength > 0.0f ? 1.0f : 0.0f, 0.0f};
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.uniform, 0, &u, sizeof(u));
        wgpuQueueRelease(queue);
    }
    const auto march_group = impl.group(depth, impl.b.view);
    const auto blur_x_group = impl.group(depth, impl.a.view);
    const auto blur_y_group = impl.group(depth, impl.b.view);
    const auto up_group = impl.group(depth, impl.a.view);
    impl.pass(encoder, impl.a.view, impl.march, march_group, effects, true);
    impl.pass(encoder, impl.b.view, impl.blur_x, blur_x_group, nullptr, false);
    impl.pass(encoder, impl.a.view, impl.blur_y, blur_y_group, nullptr, false);
    impl.pass(encoder, impl.full.view, impl.up, up_group, nullptr, false);
    for (auto* g : {march_group, blur_x_group, blur_y_group, up_group})
        if (g) wgpuBindGroupRelease(g);
    return impl.full.view;
}

} // namespace openrts::renderer
#endif
