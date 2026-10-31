#include <openrts/renderer/webgpu_context.hpp>

#include <chrono>
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

} // namespace

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

WgpuDeviceContextResult create_headless_wgpu_device() {
    WgpuDeviceContext context;
    const WGPUInstanceDescriptor instance_descriptor{};
    context.instance_ = wgpuCreateInstance(&instance_descriptor);
    if (!context.instance_) return {.error = "wgpuCreateInstance returned null."};

    AdapterRequest adapter_request;
    const WGPURequestAdapterOptions adapter_options{
        .nextInChain = nullptr,
        .featureLevel = WGPUFeatureLevel_Core,
        .powerPreference = WGPUPowerPreference_Undefined,
        .forceFallbackAdapter = false,
        .backendType = WGPUBackendType_Undefined,
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
    // Timestamp queries are optional (GPU frame time in the viewer).
    const WGPUFeatureName timestamps = WGPUFeatureName_TimestampQuery;
    const bool has_timestamps = wgpuAdapterHasFeature(context.adapter_, timestamps);
    const WGPUDeviceDescriptor device_descriptor{
        .requiredFeatureCount = has_timestamps ? 1u : 0u,
        .requiredFeatures = has_timestamps ? &timestamps : nullptr,
    };
    wgpuAdapterRequestDevice(context.adapter_, &device_descriptor, device_callback);
    if (!wait_for_request(context.instance_, device_request)
        || device_request.status != WGPURequestDeviceStatus_Success || !device_request.device)
        return {.error = "WebGPU device request failed or timed out."};
    context.device_ = device_request.device;
    return {.context = std::move(context)};
}

} // namespace openrts::renderer
