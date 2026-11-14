#pragma once

#ifndef OPENRTS_HAS_WGPU
#error "webgpu_context.hpp requires OPENRTS_ENABLE_WGPU and an installed wgpu-native SDK."
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

struct WgpuDeviceContextResult;

// The graphics API wgpu draws through. automatic: wgpu's own choice (on
// Windows Vulkan, else DirectX 12).
enum class GraphicsBackend { automatic, vulkan, d3d12, opengl };

// "auto", "vulkan", "dx12", "opengl" (as on the command line and in viewer_state.ini).
[[nodiscard]] std::string_view graphics_backend_key(GraphicsBackend backend) noexcept;
[[nodiscard]] std::optional<GraphicsBackend> parse_graphics_backend(std::string_view key) noexcept;
// The API a device runs on, for the interface: "Vulkan", "DirectX 12", "OpenGL", ...
[[nodiscard]] std::string_view adapter_backend_name(WGPUAdapter adapter);

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
    friend WgpuDeviceContextResult create_wgpu_device(GraphicsBackend backend);
    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
};

struct WgpuDeviceContextResult {
    WgpuDeviceContext context;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return static_cast<bool>(context); }
};

// Requests an adapter of that API and a device, without a surface (a windowed
// viewer creates its surface on this instance). A chosen API that has no
// adapter here falls back to the automatic choice, with a line on stderr.
[[nodiscard]] WgpuDeviceContextResult create_wgpu_device(GraphicsBackend backend);
// The automatic choice (tests, tools, captures).
[[nodiscard]] WgpuDeviceContextResult create_headless_wgpu_device();

// The multisample count the scene can be drawn with on this device:
// `wanted` (1, 2, 4 or 8) when every scene target format takes it (the
// screen's BGRA8 / RGBA8, the HDR frame's RGBA16Float, the Depth32Float
// depth), else 4. WebGPU guarantees 1 and 4; 2 and 8 are the adapter's own
// (the device asks for its format features where offered), tried here.
[[nodiscard]] std::uint32_t supported_sample_count(const WgpuDeviceContext& context, std::uint32_t wanted);

} // namespace openrts::renderer
