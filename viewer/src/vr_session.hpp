#pragma once

// VR seated in the cab (docs/vr.md): the game's frames in a headset through
// OpenXR. The head's pose is laid over the camera the game makes (the
// engineer's seat, the mouse), each eye drawn by the scene's renderer into
// its own targets, its colour a D3D11 texture shared with wgpu's Vulkan
// device (openRTS's wgpu-native, third_party/wgpu-native) that D3D11 copies
// into the OpenXR swapchain on the GPU. The monitor shows the left eye and
// the panels. Windows, OPENRTS_HAS_XR only.

#ifdef OPENRTS_HAS_XR
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <glm/mat4x4.hpp>
#include <webgpu/webgpu.h>

namespace openrts::viewer {

class VrSession {
public:
    // The headset's session on wgpu's `device` (Vulkan, asked for
    // WGPUNativeFeature_VulkanExternalMemoryWin32); the eyes' colour in
    // `colour_format` (the window's: the shaders write display values);
    // `scale`: the eyes' resolution over the runtime's recommended one (the
    // runtime's compositor takes a larger image down: supersampling).
    // Null with `error`.
    [[nodiscard]] static std::unique_ptr<VrSession> start(WGPUInstance instance, WGPUDevice device, WGPUTextureFormat colour_format,
                                                          float scale, std::string& error);
    ~VrSession();
    VrSession(const VrSession&) = delete;
    VrSession& operator=(const VrSession&) = delete;

    // The runtime's events, then (while it shows us) the frame's start and
    // the eyes' places: true when the eyes are to be drawn now; false: draw
    // the window as usual (the frame, if begun, ended empty).
    bool begin_frame();
    // An eye this frame: its pose in the headset's space (head-to-space, the
    // runtime's, metres; x right, y up, -z ahead), its field (tangents of its
    // half angles, left and down negative), its targets.
    struct Eye {
        glm::mat4 pose{1.0f};
        float left{}, right{}, up{}, down{};
        std::uint32_t width{}, height{};
        WGPUTexture colour_texture = nullptr;
        WGPUTextureView colour = nullptr;
        WGPUTextureView depth = nullptr, msaa = nullptr, hdr = nullptr;
        WGPUTexture hdr_texture = nullptr;
    };
    [[nodiscard]] std::size_t eye_count() const noexcept;
    [[nodiscard]] const Eye& eye(std::size_t i) const;
    // Eye `i` drawn and submitted (`submission`): into the headset.
    void eye_done(std::size_t i, std::uint64_t submission);
    // The frame to the headset.
    void end_frame();
    // The view centred again: the head as it is now (its place, its turn
    // about the vertical) taken as the seat's (the cab's eye, looking ahead).
    void recenter();
    // The controller's recenter button pressed this frame (the left
    // controller's menu button, or a thumbstick's click).
    [[nodiscard]] bool recenter_pressed() const noexcept;
    // The left eye stretched over the window's `target` (the window's format).
    void mirror(WGPUCommandEncoder encoder, WGPUTextureView target);
    // The runtime ended the session (the headset's quit, its app closed).
    [[nodiscard]] bool ended() const noexcept;
    [[nodiscard]] const std::string& runtime() const noexcept;

private:
    VrSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::viewer
#endif
