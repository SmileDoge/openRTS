#pragma once

#ifndef OPENRTS_HAS_WGPU
#error "webgpu_context.hpp requires OPENRTS_ENABLE_WGPU and an installed wgpu-native SDK."
#endif

#include <string>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

struct WgpuDeviceContextResult;

class WgpuDeviceContext {
public:
    WgpuDeviceContext() = default;
    ~WgpuDeviceContext();

    WgpuDeviceContext(const WgpuDeviceContext&) = delete;
    WgpuDeviceContext& operator=(const WgpuDeviceContext&) = delete;
    WgpuDeviceContext(WgpuDeviceContext&& other) noexcept;
    WgpuDeviceContext& operator=(WgpuDeviceContext&& other) noexcept;

    [[nodiscard]] WGPUDevice device() const noexcept { return device_; }
    [[nodiscard]] WGPUInstance instance() const noexcept { return instance_; }
    [[nodiscard]] WGPUAdapter adapter() const noexcept { return adapter_; }
    [[nodiscard]] explicit operator bool() const noexcept { return device_ != nullptr; }

private:
    friend WgpuDeviceContextResult create_headless_wgpu_device();
    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
};

struct WgpuDeviceContextResult {
    WgpuDeviceContext context;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return static_cast<bool>(context); }
};

// Requests a default adapter and device without a surface. Intended for asset
// upload validation; a windowed viewer will own a surface separately.
[[nodiscard]] WgpuDeviceContextResult create_headless_wgpu_device();

} // namespace openrts::renderer
