#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

// How the HDR frame becomes the screen's: none keeps the scene as it was
// drawn (the RTS look, values above 1 cut); Khronos PBR Neutral leaves the
// colours as authored up to near white and rolls only the highlights off;
// AgX (with its "punchy" look) as film would; ACES (Stephen Hill's fit of
// the RRT and sRGB ODT) contrasty and saturated, shifting hues in highlights;
// day_night: Khronos by day, AgX by night, crossfaded with the sun;
// aces_night: ACES by day, AgX by night, the same way.
enum class ToneCurve : std::uint32_t { none = 0, khronos_neutral = 1, agx = 2, aces = 3, day_night = 4, aces_night = 5 };

struct ToneSettings {
    ToneCurve curve = ToneCurve::day_night;
    float exposure = 1.0f;
    // 0 by day, 1 at night (from the sun's direct light); day_night and aces_night only.
    float night = 0.0f;
    // Exposure of the night curve: every filmic toe crushes the dark tones a
    // night in MSTS content lives in; about 2 keeps them as drawn.
    float night_exposure = 1.5f;
    // Saturation of the final picture, 1 as drawn (any curve, also none).
    float saturation = 1.0f;
    // The bloom's light (render's `bloom`) times this, added to the scene
    // before the exposure; 0: none. Not with the curve none (the RTS look).
    float bloom = 0.0f;
    // The light shafts' light (render's `shafts`, LightShaftRenderer) added
    // the same way, as they are; false: none.
    bool shafts = false;
    // The scene frame holds linear light (frame_encoding()); else gamma 2.2.
    bool linear_scene = false;
    // The exposure times the auto exposure's factor (render's `exposure`,
    // AutoExposure::state(); PBR stage 3).
    bool auto_exposure = false;
    // HDR output (hdr_output.hpp): the output is scRGB (linear, 1 = 80
    // nits); a white the SDR picture shows at 1 comes out at `paper_white`
    // nits and the highlights roll off to `peak` nits. 0: SDR output.
    float paper_white = 0.0f;
    float peak = 1000.0f;
};

// The HDR scene (scene_hdr(): drawn into hdr_format, gamma 2.2 encoded as
// before, values above 1 kept) to the presentation target: decode, exposure,
// curve, encode.
class ToneMapper {
public:
    ToneMapper(WGPUDevice device, WGPUTextureFormat output_format);
    ~ToneMapper();
    ToneMapper(const ToneMapper&) = delete;
    ToneMapper& operator=(const ToneMapper&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // One full-screen pass from `scene` (sampled) into `output`, with the
    // bloom's light (BloomRenderer::build; null: none), the light shafts'
    // (LightShaftRenderer::render, the frame's size; null: none) and the
    // auto exposure's state (AutoExposure::state(); null: none).
    void render(WGPUCommandEncoder encoder, WGPUTextureView scene, WGPUTextureView output, const ToneSettings& settings,
                WGPUTextureView bloom = nullptr, WGPUTextureView shafts = nullptr, WGPUBuffer exposure = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
