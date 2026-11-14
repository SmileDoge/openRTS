#include <openrts/renderer/webgpu_context.hpp>

#include <webgpu/wgpu.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace openrts::renderer {
namespace {

struct AdapterRequest {
    WGPUAdapter adapter = nullptr;
    WGPURequestAdapterStatus status = WGPURequestAdapterStatus_Unknown;
    bool complete = false;
};

struct DeviceRequest {
    WGPUDevice device = nullptr;
    WGPURequestDeviceStatus status = WGPURequestDeviceStatus_Unknown;
    bool complete = false;
};

void on_adapter(const WGPURequestAdapterStatus status, const WGPUAdapter adapter, WGPUStringView,
                void* userdata, void*) {
    auto& request = *static_cast<AdapterRequest*>(userdata);
    request.status = status;
    request.adapter = adapter;
    request.complete = true;
}

// Validation and out-of-memory errors nobody caught: printed, so a broken
// pipeline or binding says what is wrong instead of failing silently.
void on_uncaptured_error(WGPUDevice const*, const WGPUErrorType type, const WGPUStringView message, void*, void*) {
    std::fprintf(stderr, "WebGPU error %d: %.*s\n", static_cast<int>(type),
                 static_cast<int>(message.data ? (message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length) : 0),
                 message.data ? message.data : "");
    std::fflush(stderr);
}

void on_device(const WGPURequestDeviceStatus status, const WGPUDevice device, WGPUStringView,
               void* userdata, void*) {
    auto& request = *static_cast<DeviceRequest*>(userdata);
    request.status = status;
    request.device = device;
    request.complete = true;
}

template <typename Request>
[[nodiscard]] bool wait_for_request(const WGPUInstance instance, Request& request) {
    constexpr auto timeout = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!request.complete && std::chrono::steady_clock::now() < deadline) {
        wgpuInstanceProcessEvents(instance);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return request.complete;
}

WGPUInstanceBackend instance_backends(const GraphicsBackend backend) {
    switch (backend) {
    case GraphicsBackend::vulkan: return WGPUInstanceBackend_Vulkan;
    case GraphicsBackend::d3d12: return WGPUInstanceBackend_DX12;
    case GraphicsBackend::opengl: return WGPUInstanceBackend_GL;
    case GraphicsBackend::automatic: break;
    }
    return WGPUInstanceBackend_All;
}

WGPUBackendType adapter_backend(const GraphicsBackend backend) {
    switch (backend) {
    case GraphicsBackend::vulkan: return WGPUBackendType_Vulkan;
    case GraphicsBackend::d3d12: return WGPUBackendType_D3D12;
    case GraphicsBackend::opengl: return WGPUBackendType_OpenGL;
    case GraphicsBackend::automatic: break;
    }
    return WGPUBackendType_Undefined;
}

} // namespace

std::string_view graphics_backend_key(const GraphicsBackend backend) noexcept {
    switch (backend) {
    case GraphicsBackend::vulkan: return "vulkan";
    case GraphicsBackend::d3d12: return "dx12";
    case GraphicsBackend::opengl: return "opengl";
    case GraphicsBackend::automatic: break;
    }
    return "auto";
}

std::optional<GraphicsBackend> parse_graphics_backend(const std::string_view key) noexcept {
    if (key == "auto") return GraphicsBackend::automatic;
    if (key == "vulkan") return GraphicsBackend::vulkan;
    if (key == "dx12" || key == "d3d12") return GraphicsBackend::d3d12;
    if (key == "opengl" || key == "gl") return GraphicsBackend::opengl;
    return std::nullopt;
}

std::string_view adapter_backend_name(const WGPUAdapter adapter) {
    if (!adapter) return "-";
    WGPUAdapterInfo info{};
    wgpuAdapterGetInfo(adapter, &info);
    const auto type = info.backendType;
    wgpuAdapterInfoFreeMembers(info);
    switch (type) {
    case WGPUBackendType_Vulkan: return "Vulkan";
    case WGPUBackendType_D3D12: return "DirectX 12";
    case WGPUBackendType_D3D11: return "DirectX 11";
    case WGPUBackendType_OpenGL: return "OpenGL";
    case WGPUBackendType_OpenGLES: return "OpenGL ES";
    case WGPUBackendType_Metal: return "Metal";
    default: return "?";
    }
}

WgpuDeviceContext::~WgpuDeviceContext() {
    if (device_) wgpuDeviceRelease(device_);
    if (adapter_) wgpuAdapterRelease(adapter_);
    if (instance_) wgpuInstanceRelease(instance_);
}

WgpuDeviceContext::WgpuDeviceContext(WgpuDeviceContext&& other) noexcept
    : instance_(other.instance_), adapter_(other.adapter_), device_(other.device_) {
    other.instance_ = nullptr;
    other.adapter_ = nullptr;
    other.device_ = nullptr;
}

WgpuDeviceContext& WgpuDeviceContext::operator=(WgpuDeviceContext&& other) noexcept {
    if (this == &other) return *this;
    if (device_) wgpuDeviceRelease(device_);
    if (adapter_) wgpuAdapterRelease(adapter_);
    if (instance_) wgpuInstanceRelease(instance_);
    instance_ = other.instance_;
    adapter_ = other.adapter_;
    device_ = other.device_;
    other.instance_ = nullptr;
    other.adapter_ = nullptr;
    other.device_ = nullptr;
    return *this;
}

WgpuDeviceContextResult create_headless_wgpu_device() { return create_wgpu_device(GraphicsBackend::automatic); }

WgpuDeviceContextResult create_wgpu_device(const GraphicsBackend backend) {
    // One API at a time, never an instance of every backend: such an
    // instance makes wgpu open DXGI, Direct3D 12 and OpenGL even when it ends
    // on Vulkan, and a hook on all of them (ReShade installed for the game)
    // crashed in dxgi.dll there.
    const auto try_backend = [](const GraphicsBackend single) -> WgpuDeviceContextResult {
        WgpuDeviceContext context;
        WGPUInstanceExtras extras{};
        extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
        extras.backends = instance_backends(single);
        const WGPUInstanceDescriptor instance_descriptor{.nextInChain = &extras.chain};
        context.instance_ = wgpuCreateInstance(&instance_descriptor);
        if (!context.instance_) return {.error = "wgpuCreateInstance returned null."};

        AdapterRequest adapter_request;
        const WGPURequestAdapterOptions adapter_options{
            .nextInChain = nullptr,
            .featureLevel = WGPUFeatureLevel_Core,
            .powerPreference = WGPUPowerPreference_HighPerformance,
            .forceFallbackAdapter = false,
            .backendType = adapter_backend(single),
            .compatibleSurface = nullptr,
        };
        const WGPURequestAdapterCallbackInfo adapter_callback{
            .nextInChain = nullptr,
            .mode = WGPUCallbackMode_AllowProcessEvents,
            .callback = on_adapter,
            .userdata1 = &adapter_request,
            .userdata2 = nullptr,
        };
        wgpuInstanceRequestAdapter(context.instance_, &adapter_options, adapter_callback);
        if (!wait_for_request(context.instance_, adapter_request)
            || adapter_request.status != WGPURequestAdapterStatus_Success || !adapter_request.adapter)
            return {.error = "WebGPU adapter request failed or timed out."};
        context.adapter_ = adapter_request.adapter;

        DeviceRequest device_request;
        const WGPURequestDeviceCallbackInfo device_callback{
            .nextInChain = nullptr,
            .mode = WGPUCallbackMode_AllowProcessEvents,
            .callback = on_device,
            .userdata1 = &device_request,
            .userdata2 = nullptr,
        };
        // Optional features: timestamp queries (GPU frame time in the
        // viewer), a first instance in indirect draws (the model renderer's
        // multi-draw calls), BC compressed textures (DXT1 ACE as they are),
        // the adapter's own format features (MSAA 2x and 8x).
        std::vector<WGPUFeatureName> features;
        for (const auto feature : {WGPUFeatureName_TimestampQuery, WGPUFeatureName_IndirectFirstInstance,
                                   WGPUFeatureName_TextureCompressionBC,
                                   static_cast<WGPUFeatureName>(WGPUNativeFeature_TextureAdapterSpecificFormatFeatures)}) {
            if (wgpuAdapterHasFeature(context.adapter_, feature)) features.push_back(feature);
        }
        const WGPUDeviceDescriptor device_descriptor{
            .requiredFeatureCount = features.size(),
            .requiredFeatures = features.empty() ? nullptr : features.data(),
            .uncapturedErrorCallbackInfo = {.callback = on_uncaptured_error},
        };
        wgpuAdapterRequestDevice(context.adapter_, &device_descriptor, device_callback);
        if (!wait_for_request(context.instance_, device_request)
            || device_request.status != WGPURequestDeviceStatus_Success || !device_request.device)
            return {.error = "WebGPU device request failed or timed out."};
        context.device_ = device_request.device;
        return {.context = std::move(context)};
    };

    if (backend != GraphicsBackend::automatic) {
        auto result = try_backend(backend);
        if (result.ok()) return result;
        const auto key = graphics_backend_key(backend);
        std::fprintf(stderr, "No %.*s device (%s); falling back to the automatic choice.%c", static_cast<int>(key.size()), key.data(),
                     result.error.c_str(), 10);
        std::fflush(stderr);
    }
    // Automatic: Vulkan, else Direct3D 12, else OpenGL.
    std::string error = "No WebGPU adapter.";
    for (const auto single : {GraphicsBackend::vulkan, GraphicsBackend::d3d12, GraphicsBackend::opengl}) {
        if (single == backend) continue;
        auto result = try_backend(single);
        if (result.ok()) return result;
        error = result.error;
    }
    return {.error = error};
}

std::uint32_t supported_sample_count(const WgpuDeviceContext& context, const std::uint32_t wanted) {
    if (wanted <= 1) return 1;
    if (wanted != 2 && wanted != 8) return 4;
    const auto device = context.device();
    if (!device || !wgpuDeviceHasFeature(device, static_cast<WGPUFeatureName>(WGPUNativeFeature_TextureAdapterSpecificFormatFeatures)))
        return 4;
    // Each scene target format at that count, in a validation error scope.
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    for (const auto format : {WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGBA8Unorm, WGPUTextureFormat_RGBA16Float,
                              WGPUTextureFormat_Depth32Float}) {
        const WGPUTextureDescriptor descriptor{
            .usage = WGPUTextureUsage_RenderAttachment,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = 4, .height = 4, .depthOrArrayLayers = 1},
            .format = format,
            .mipLevelCount = 1,
            .sampleCount = wanted,
        };
        if (const auto texture = wgpuDeviceCreateTexture(device, &descriptor)) wgpuTextureRelease(texture);
    }
    struct Outcome {
        bool done = false;
        bool failed = true;
    } outcome;
    const WGPUPopErrorScopeCallbackInfo pop{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = [](const WGPUPopErrorScopeStatus status, const WGPUErrorType type, WGPUStringView, void* userdata, void*) {
            auto& o = *static_cast<Outcome*>(userdata);
            o.failed = status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError;
            o.done = true;
        },
        .userdata1 = &outcome,
    };
    wgpuDevicePopErrorScope(device, pop);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!outcome.done && std::chrono::steady_clock::now() < deadline) {
        wgpuDevicePoll(device, false, nullptr);
        wgpuInstanceProcessEvents(context.instance());
        if (!outcome.done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return outcome.done && !outcome.failed ? wanted : 4;
}

} // namespace openrts::renderer
