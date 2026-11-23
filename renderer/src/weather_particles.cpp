#include <openrts/renderer/weather_particles.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include "shader_code.hpp"

namespace openrts::renderer {

namespace {

struct Uniform {
    glm::mat4 view_projection;
    glm::mat4 view;
    float eye[4];      // xyz the eye, w the time
    float flow[4];     // xyz the wind (m/s, world), w 1: snow
    float eye_vel[4];  // xyz the camera's velocity, w the projection's y scale
    float light[4];    // rgb the light (display), a 1: the frame is linear
    float params[4];   // x the box's half width, y its half height, z the count, w the viewport's height
    glm::mat4 to_room;
    float room_lower[4]; // w 1: there is a room
    float room_upper[4];
    float origin[4];     // the origin's shift, within the box
};

WGPUStringView name(const char* text) {
    return {text, std::strlen(text)};
}

} // namespace

struct WeatherParticleRenderer::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBuffer uniform = nullptr;
    WGPUBindGroup group = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    ~Impl() {
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (group) wgpuBindGroupRelease(group);
        if (uniform) wgpuBufferRelease(uniform);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }
};

std::uint32_t WeatherParticleRenderer::count(const float amount, const bool snow) noexcept {
    const float n = (snow ? 40000.0f : 60000.0f) * std::clamp(amount, 0.0f, 2.0f);
    return static_cast<std::uint32_t>(n);
}

WeatherParticleRenderer::WeatherParticleRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    WGPUBindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.minBindingSize = sizeof(Uniform);
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = 1, .entries = &entry};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(Uniform)};
    impl.uniform = wgpuDeviceCreateBuffer(device, &buffer);
    WGPUBindGroupEntry bound{};
    bound.binding = 0;
    bound.buffer = impl.uniform;
    bound.size = sizeof(Uniform);
    const WGPUBindGroupDescriptor group{.layout = impl.layout, .entryCount = 1, .entries = &bound};
    impl.group = wgpuDeviceCreateBindGroup(device, &group);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = name(detail::shader_code("world/weather_particles"))};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto module = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
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
    const WGPUBlendState over{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_Zero, .dstFactor = WGPUBlendFactor_One},
    };
    const WGPUColorTargetState target{.format = target_format, .blend = &over, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{.module = module, .entryPoint = name("fs_main"), .targetCount = 1, .targets = &target};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = pipeline_layout,
        .vertex = {.module = module, .entryPoint = name("vs_main")},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = scene_multisample(),
        .fragment = &fragment,
    };
    impl.pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(module);
}

WeatherParticleRenderer::~WeatherParticleRenderer() = default;

bool WeatherParticleRenderer::ready() const noexcept {
    return impl_->pipeline && impl_->group;
}

void WeatherParticleRenderer::render(const WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view,
                                     const WeatherParticles& weather) {
    auto& impl = *impl_;
    const auto n = count(weather.amount, weather.snow);
    if (!ready() || n == 0 || weather.viewport_height <= 0.0f) return;
    Uniform u{};
    u.view_projection = projection * view;
    u.view = view;
    u.eye[0] = weather.eye.x;
    u.eye[1] = weather.eye.y;
    u.eye[2] = weather.eye.z;
    u.eye[3] = weather.time;
    u.flow[0] = weather.wind.x;
    u.flow[1] = weather.wind.y;
    u.flow[2] = weather.wind.z;
    u.flow[3] = weather.snow ? 1.0f : 0.0f;
    u.eye_vel[0] = weather.eye_velocity.x;
    u.eye_vel[1] = weather.eye_velocity.y;
    u.eye_vel[2] = weather.eye_velocity.z;
    u.eye_vel[3] = projection[1][1];
    for (int k = 0; k < 3; ++k) u.light[k] = std::clamp(weather.light[k], 0.0f, 1.0f);
    u.light[3] = frame_linear() ? 1.0f : 0.0f;
    u.params[0] = weather.snow ? 18.0f : 22.0f;
    u.params[1] = weather.snow ? 10.0f : 14.0f;
    // The viewport's aspect (projection's x over y scale, inverted).
    u.params[2] = projection[0][0] != 0.0f ? projection[1][1] / projection[0][0] : 1.0f;
    u.params[3] = weather.viewport_height;
    u.to_room = weather.to_room;
    {
        const double box[3] = {2.0 * u.params[0], 2.0 * u.params[1], 2.0 * u.params[0]};
        for (int k = 0; k < 3; ++k) u.origin[k] = static_cast<float>(std::fmod(weather.origin_shift[k], box[k]));
    }
    if (weather.has_room) {
        for (int k = 0; k < 3; ++k) {
            u.room_lower[k] = weather.room_lower[k];
            u.room_upper[k] = weather.room_upper[k];
        }
        u.room_lower[3] = 1.0f;
    }
    const auto queue = wgpuDeviceGetQueue(impl.device);
    if (!queue) return;
    wgpuQueueWriteBuffer(queue, impl.uniform, 0, &u, sizeof(u));
    wgpuQueueRelease(queue);
    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, n * 6u, 1, 0, 0);
}

} // namespace openrts::renderer
#endif
