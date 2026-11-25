#pragma once

// NVIDIA DLSS through Streamline (openRTS; docs/experimental-effects.md,
// "DLSS"): the frame's HDR, depth and motion copied into textures shared
// with a D3D11 device of its own, Streamline's DLSS run there, its output
// read back by wgpu for the tone curve. Windows, an RTX video card, the
// Streamline DLLs beside the game (sl.interposer.dll, sl.common.dll,
// sl.dlss.dll, nvngx_dlss.dll; LICENSE-EXCEPTIONS.md) and openRTS's
// wgpu-native on Vulkan (its D3D11 shared-texture import, as VR's).
// Without any of it available() is false and the rest is never called.

#include <cstdint>
#include <memory>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

namespace openrts::viewer {

struct DlssFrame {
    std::uint32_t width = 0, height = 0;
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

    // Records into `encoder` the copies of the frame (`scene`: the resolved
    // HDR texture, `motion`: MotionVectors' texture, `depth`: the camera's
    // depth pre-pass) into the shared textures. false: not this frame.
    bool record_inputs(WGPUCommandEncoder encoder, WGPUTexture scene, WGPUTexture motion, WGPUTextureView depth,
                       std::uint32_t width, std::uint32_t height);
    // After the GPU finished what record_inputs() recorded: DLSS on the
    // D3D11 device (DLAA: the frame's own size), waited for; returns the
    // output's view for wgpu (null on failure).
    [[nodiscard]] WGPUTextureView evaluate(const DlssFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::viewer
