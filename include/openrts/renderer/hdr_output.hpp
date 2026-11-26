#pragma once

// HDR output (--hdr-output, docs/experimental-effects.md "HDR output"):
// the swap chain in RGBA16Float, which DirectX 12 presents as scRGB (linear
// Rec.709, 1 = 80 nits) to a display with Windows's HDR on. The tone curve
// writes the scene's light there (ToneSettings::paper_white, peak); the
// panels, drawn for an 8-bit sRGB target, are drawn into one of their own
// and laid over at the paper white (UiCompositor).

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

// On for this run (set before the surface and the overlay are made).
inline bool& hdr_output() {
    static bool on = false;
    return on;
}
// The paper white (an SDR white, the panels' white), nits; the display's
// peak, nits. Read each frame.
inline float& hdr_paper_white() {
    static float nits = 200.0f;
    return nits;
}
inline float& hdr_peak() {
    static float nits = 1000.0f;
    return nits;
}
// The panels' target in HDR output.
inline constexpr WGPUTextureFormat hdr_ui_format = WGPUTextureFormat_RGBA8Unorm;

class UiCompositor {
public:
    UiCompositor(WGPUDevice device, WGPUTextureFormat output_format);
    ~UiCompositor();
    UiCompositor(const UiCompositor&) = delete;
    UiCompositor& operator=(const UiCompositor&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // The panels' texture at the output's size (made again when it changes).
    [[nodiscard]] WGPUTextureView target(std::uint32_t width, std::uint32_t height);
    // The panels (premultiplied, display-encoded) over `output` at the paper white.
    void compose(WGPUCommandEncoder encoder, WGPUTextureView output, float paper_white);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The HDR calibration's patterns over the whole output (scRGB):
// mode 1 the peak (a square at `peak` nits on a field at 10 000), mode 2
// the paper white (greys and a white at `paper` nits, a bar at the peak).
class HdrCalibration {
public:
    HdrCalibration(WGPUDevice device, WGPUTextureFormat output_format);
    ~HdrCalibration();
    HdrCalibration(const HdrCalibration&) = delete;
    HdrCalibration& operator=(const HdrCalibration&) = delete;
    void draw(WGPUCommandEncoder encoder, WGPUTextureView output, int mode, float peak, float paper, std::uint32_t width,
              std::uint32_t height);

private:
    WGPUDevice device_ = nullptr;
    WGPUBindGroupLayout layout_ = nullptr;
    WGPURenderPipeline pipeline_ = nullptr;
    WGPUBuffer uniform_ = nullptr;
};

} // namespace openrts::renderer
#endif
