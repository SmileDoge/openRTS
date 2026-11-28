#pragma once

// NVIDIA DLSS through Streamline (openRTS; docs/experimental-effects.md,
// "DLSS"): the frame's HDR, depth and motion copied into textures shared
// with a D3D11 device of its own, Streamline's DLSS run there, its output
// read back by wgpu for the tone curve. Windows, an RTX video card, the
// Streamline DLLs beside the game (sl.interposer.dll, sl.common.dll,
// sl.dlss.dll, nvngx_dlss.dll; LICENSE-EXCEPTIONS.md) and openRTS's
// wgpu-native on Vulkan or D3D12 (its D3D11 shared-texture import, as VR's).
// Without any of it available() is false and the rest is never called.

#include <cstdint>
#include <memory>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

namespace openrts::viewer {

// What DLSS does: off; DLAA (at the frame's own size); upscaling from a
// smaller internal size (the quality: 67, 58, 50, 33 % a side).
enum class DlssMode : int { off = 0, dlaa, quality, balanced, performance, ultra_performance };

struct DlssFrame {
    glm::mat4 projection{1.0f};             // this frame's without the jitter
    glm::mat4 view{1.0f};
    glm::mat4 previous_view_projection{1.0f}; // the last frame's, without the jitter
    glm::vec2 jitter{0.0f};                  // MotionVectors::jitter (pixels)
    glm::vec3 eye{0.0f};
    float near_plane = 0.1f, far_plane = 10000.0f, vertical_fov = 1.0f, aspect = 1.0f;
    bool reset = false; // no last frame (a cut)
};

class DlssUpscaler {
public:
    // Loads Streamline and DLSS on `device`'s video card; available() says
    // whether it worked, error() why not.
    DlssUpscaler(WGPUDevice device, WGPUAdapter adapter);
    ~DlssUpscaler();
    DlssUpscaler(const DlssUpscaler&) = delete;
    DlssUpscaler& operator=(const DlssUpscaler&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;

    // This frame: `mode` at the output size (the window's). Picks the
    // internal size (DLAA: the output's) and makes the shared textures the
    // frame is drawn into. false: not this frame (off, or they failed).
    bool prepare(DlssMode mode, std::uint32_t output_width, std::uint32_t output_height);
    // After prepare(): the size the scene is drawn at, and the shared
    // textures it draws into (the HDR frame, resolved from the MSAA, and
    // MotionVectors' motion), the output's size.
    [[nodiscard]] std::uint32_t render_width() const noexcept;
    [[nodiscard]] std::uint32_t render_height() const noexcept;
    [[nodiscard]] WGPUTextureView colour_view() const noexcept;
    [[nodiscard]] WGPUTextureView motion_view() const noexcept;
    // Records into `encoder` the camera's depth (the pre-pass) into its
    // shared texture (R32_FLOAT for D3D11). false: not this frame.
    bool record_inputs(WGPUCommandEncoder encoder, WGPUTextureView depth);
    // After the submission (`index` on `queue`) of what record_inputs()
    // recorded: DLSS on the D3D11 device (DLAA: the frame's own size);
    // returns the output's view for wgpu's next submissions (null on
    // failure). In step on the GPU through a shared fence (the CPU does not
    // wait); without one (a Vulkan driver without
    // VK_KHR_external_semaphore_win32) the CPU waits for wgpu, then D3D11.
    [[nodiscard]] WGPUTextureView evaluate(const DlssFrame& frame, WGPUQueue queue, std::uint64_t index);
    // Whether evaluate() keeps wgpu and D3D11 in step on the GPU.
    [[nodiscard]] bool gpu_sync() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::viewer
