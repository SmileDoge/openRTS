#include <openrts/renderer/auto_exposure.hpp>
#include <openrts/renderer/gpu_pass_timer.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <array>
#include <cstring>

#include <webgpu/wgpu.h> // wgpuDevicePoll
#include "shader_code.hpp"

namespace openrts::renderer {
namespace {

[[nodiscard]] WGPUStringView text(const char* value) {
    return {.data = value, .length = WGPU_STRLEN};
}

struct Params {
    std::uint32_t size[2]{};
    std::uint32_t step = 1;
    std::uint32_t linear = 1;
    float elapsed{};
    float strength{};
    float down{};
    float up{};
    float speed{};
    float key{};
    std::uint32_t reset{};
    std::uint32_t pad{};
};
static_assert(sizeof(Params) == 48);

} // namespace

struct AutoExposure::Impl {
    WGPUDevice device = nullptr;
    WGPUShaderModule shader = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUComputePipeline build = nullptr;
    WGPUComputePipeline adapt = nullptr;
    WGPUBuffer params = nullptr;
    WGPUBuffer histogram = nullptr;
    WGPUBuffer state = nullptr;
    bool reset = true;
    // The panel's copy (copy_for_panel / submitted / latest).
    WGPUBuffer panel_buffer = nullptr;
    bool panel_copied = false;
    bool panel_busy = false;
    AutoExposureState panel_state;

    ~Impl() {
        for (const auto buffer : {params, histogram, state, panel_buffer}) {
            if (buffer) wgpuBufferRelease(buffer);
        }
        for (const auto pipeline : {build, adapt}) {
            if (pipeline) wgpuComputePipelineRelease(pipeline);
        }
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (shader) wgpuShaderModuleRelease(shader);
    }
};

AutoExposure::AutoExposure(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = text(detail::shader_code("post/auto_exposure"))};
    const WGPUShaderModuleDescriptor shader{.nextInChain = &source.chain};
    impl.shader = wgpuDeviceCreateShaderModule(device, &shader);
    std::array<WGPUBindGroupLayoutEntry, 4> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Compute;
    entries[0].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Compute;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = sizeof(Params);
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Compute;
    entries[2].buffer.type = WGPUBufferBindingType_Storage;
    entries[2].buffer.minBindingSize = 128 * 4;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Compute;
    entries[3].buffer.type = WGPUBufferBindingType_Storage;
    entries[3].buffer.minBindingSize = 16;
    const WGPUBindGroupLayoutDescriptor layout{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    for (const auto& [pipeline, entry] : {std::pair{&impl.build, "build"}, std::pair{&impl.adapt, "adapt"}}) {
        const WGPUComputePipelineDescriptor descriptor{.layout = pipeline_layout, .compute = {.module = impl.shader, .entryPoint = text(entry)}};
        *pipeline = wgpuDeviceCreateComputePipeline(device, &descriptor);
    }
    wgpuPipelineLayoutRelease(pipeline_layout);
    const WGPUBufferDescriptor params{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(Params)};
    impl.params = wgpuDeviceCreateBuffer(device, &params);
    const WGPUBufferDescriptor histogram{.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, .size = 128 * 4};
    impl.histogram = wgpuDeviceCreateBuffer(device, &histogram);
    const WGPUBufferDescriptor state{.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst, .size = 16};
    impl.state = wgpuDeviceCreateBuffer(device, &state);
    const WGPUBufferDescriptor panel{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, .size = 16};
    impl.panel_buffer = wgpuDeviceCreateBuffer(device, &panel);
    // Zeroed: no histogram yet, the state not valid (the factor read as 1
    // by the tone curve until the first adapt).
    if (const auto queue = wgpuDeviceGetQueue(device)) {
        const std::array<std::uint32_t, 128> zero{};
        if (impl.histogram) wgpuQueueWriteBuffer(queue, impl.histogram, 0, zero.data(), sizeof(zero));
        const float initial[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        if (impl.state) wgpuQueueWriteBuffer(queue, impl.state, 0, initial, sizeof(initial));
        wgpuQueueRelease(queue);
    }
}

AutoExposure::~AutoExposure() = default;

bool AutoExposure::ready() const noexcept {
    const auto& impl = *impl_;
    return impl.build && impl.adapt && impl.params && impl.histogram && impl.state && impl.layout;
}

void AutoExposure::reset() noexcept { impl_->reset = true; }

WGPUBuffer AutoExposure::state() const noexcept { return impl_->state; }

void AutoExposure::measure(const WGPUCommandEncoder encoder, const WGPUTextureView scene, const std::uint32_t width, const std::uint32_t height,
                           const bool linear, const float elapsed, const AutoExposureSettings& settings) {
    auto& impl = *impl_;
    if (!ready() || !scene || width == 0 || height == 0) return;
    Params p;
    p.size[0] = width;
    p.size[1] = height;
    p.step = (std::max)(1u, width / 640u); // about 640 samples a row
    p.linear = linear ? 1u : 0u;
    p.elapsed = std::clamp(elapsed, 0.0f, 1.0f);
    p.strength = std::clamp(settings.strength, 0.0f, 1.0f);
    p.down = std::clamp(settings.down, 0.0f, 3.0f);
    p.up = std::clamp(settings.up, 0.0f, 3.0f);
    p.speed = std::clamp(settings.speed, 0.1f, 5.0f);
    p.key = auto_exposure_key;
    p.reset = impl.reset ? 1u : 0u;
    impl.reset = false;
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.params, 0, &p, sizeof(p));
        wgpuQueueRelease(queue);
    }
    std::array<WGPUBindGroupEntry, 4> entries{};
    entries[0].binding = 0;
    entries[0].textureView = scene;
    entries[1].binding = 1;
    entries[1].buffer = impl.params;
    entries[1].size = sizeof(Params);
    entries[2].binding = 2;
    entries[2].buffer = impl.histogram;
    entries[2].size = 128 * 4;
    entries[3].binding = 3;
    entries[3].buffer = impl.state;
    entries[3].size = 16;
    const WGPUBindGroupDescriptor group_descriptor{.layout = impl.layout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = wgpuDeviceCreateBindGroup(impl.device, &group_descriptor);
    const auto timestamps = compute_pass_timestamps(GpuPass::exposure);
    const WGPUComputePassDescriptor descriptor{.timestampWrites = timestamps ? &*timestamps : nullptr};
    const auto pass = wgpuCommandEncoderBeginComputePass(encoder, &descriptor);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, impl.build);
    const auto samples_x = (width + p.step - 1) / p.step, samples_y = (height + p.step - 1) / p.step;
    wgpuComputePassEncoderDispatchWorkgroups(pass, (samples_x + 15) / 16, (samples_y + 15) / 16, 1);
    wgpuComputePassEncoderSetPipeline(pass, impl.adapt);
    wgpuComputePassEncoderDispatchWorkgroups(pass, 1, 1, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

void AutoExposure::copy_for_panel(const WGPUCommandEncoder encoder) {
    auto& impl = *impl_;
    if (!ready() || !impl.panel_buffer || impl.panel_busy || impl.panel_copied) return;
    wgpuCommandEncoderCopyBufferToBuffer(encoder, impl.state, 0, impl.panel_buffer, 0, 16);
    impl.panel_copied = true;
}

void AutoExposure::submitted() {
    auto& impl = *impl_;
    if (!impl.panel_copied) return;
    impl.panel_copied = false;
    impl.panel_busy = true;
    const WGPUBufferMapCallbackInfo callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = [](const WGPUMapAsyncStatus status, WGPUStringView, void* data, void*) {
            auto& owner = *static_cast<Impl*>(data);
            owner.panel_busy = false;
            if (status != WGPUMapAsyncStatus_Success) return;
            float values[4]{};
            if (const auto* bytes = wgpuBufferGetConstMappedRange(owner.panel_buffer, 0, 16)) std::memcpy(values, bytes, sizeof(values));
            wgpuBufferUnmap(owner.panel_buffer);
            owner.panel_state = {.adapted = values[0], .factor = values[1], .measured = values[2], .valid = values[3] > 0.5f};
        },
        .userdata1 = &impl,
    };
    wgpuBufferMapAsync(impl.panel_buffer, WGPUMapMode_Read, 0, 16, callback);
}

AutoExposureState AutoExposure::latest() const noexcept { return impl_->panel_state; }

AutoExposureState AutoExposure::read_state(const WGPUInstance instance) const {
    const auto& impl = *impl_;
    AutoExposureState out;
    if (!ready()) return out;
    const WGPUBufferDescriptor read{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, .size = 16};
    const auto buffer = wgpuDeviceCreateBuffer(impl.device, &read);
    if (!buffer) return out;
    const auto queue = wgpuDeviceGetQueue(impl.device);
    const auto encoder = wgpuDeviceCreateCommandEncoder(impl.device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, impl.state, 0, buffer, 0, 16);
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    bool done = false, mapped = false;
    struct State {
        bool* done;
        bool* mapped;
    } state{&done, &mapped};
    const WGPUBufferMapCallbackInfo callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = [](const WGPUMapAsyncStatus status, WGPUStringView, void* data, void*) {
            auto* s = static_cast<State*>(data);
            *s->mapped = status == WGPUMapAsyncStatus_Success;
            *s->done = true;
        },
        .userdata1 = &state,
    };
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, 16, callback);
    for (int i = 0; i < 1000 && !done; ++i) {
        wgpuDevicePoll(impl.device, true, nullptr);
        if (instance) wgpuInstanceProcessEvents(instance);
    }
    if (mapped) {
        float values[4]{};
        std::memcpy(values, wgpuBufferGetConstMappedRange(buffer, 0, 16), sizeof(values));
        out = {.adapted = values[0], .factor = values[1], .measured = values[2], .valid = values[3] > 0.5f};
        wgpuBufferUnmap(buffer);
    }
    wgpuBufferRelease(buffer);
    wgpuQueueRelease(queue);
    return out;
}

} // namespace openrts::renderer
#endif
