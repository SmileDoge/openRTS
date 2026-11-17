#pragma once

// Auto exposure (PBR stage 3, docs/experimental-effects.md, "Auto
// exposure"): the eye getting used to the light. Each frame, on the GPU,
// the HDR frame's light is measured as a histogram of the log2 of its
// luminance (every few pixels, the middle of the picture weighing three
// times the edges), its mean taken without the darkest and the brightest
// tenth, and the adapted level follows it over time, towards light faster
// than towards the dark, as an eye does. The tone curve multiplies the
// exposure the player set (its correction now) by
//   2 ^ clamp(strength · (key − adapted), −down, up),
// key the log2 luminance of a typical day scene of RTS content (exposure 1
// there). A strength under 1 keeps the scenes' own character: a tunnel
// stays darker than a sunny field, only less so. Nothing leaves the GPU
// (read_state() for captures).

#include <cstdint>
#include <memory>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

struct AutoExposureSettings {
    bool on = true;          // on since 2026-11-17 (the experimental renderer with HDR and a tone curve only)
    // The user's tuning of 2026-11-17 (a cab's dark corner and its windows,
    // the sky's openness no longer following the view): 0.15 / 1.5 / 1 before.
    float strength = 0.21f;  // 0..1: how far the exposure follows the light (1: all the way to the key)
    float down = 3.0f;       // the most it darkens a bright scene, stops (0..3)
    float up = 3.0f;         // the most it brightens a dark one, stops (0..3; the night curve already brightens a night twice)
    float speed = 1.0f;      // a factor on the adaptation's pace (towards light 3 stops/s-ish, towards dark 1)
    bool operator==(const AutoExposureSettings&) const = default;
};

// The log2 luminance a typical day scene of RTS content measures (the
// render check's open stretch at noon), where the auto exposure leaves the
// exposure as set.
inline constexpr float auto_exposure_key = -3.0f;

// What the GPU holds: the adapted log2 luminance, the exposure factor, the
// last frame's measured log2 luminance.
struct AutoExposureState {
    float adapted{};
    float factor = 1.0f;
    float measured{};
    bool valid = false;
};

#ifdef OPENRTS_HAS_WGPU
class AutoExposure {
public:
    explicit AutoExposure(WGPUDevice device);
    ~AutoExposure();
    AutoExposure(const AutoExposure&) = delete;
    AutoExposure& operator=(const AutoExposure&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // Measures `scene` (the HDR frame, `linear`: linear light, else gamma
    // 2.2) and adapts by `elapsed` seconds; outside any pass, before the
    // tone curve reads state().
    void measure(WGPUCommandEncoder encoder, WGPUTextureView scene, std::uint32_t width, std::uint32_t height, bool linear,
                 float elapsed, const AutoExposureSettings& settings);
    // The next measure() starts from the frame's light at once (turned on,
    // a new scene).
    void reset() noexcept;
    // The state the tone curve reads (vec4: adapted, factor, measured, valid).
    [[nodiscard]] WGPUBuffer state() const noexcept;
    // Read back (blocking; captures and reports only).
    [[nodiscard]] AutoExposureState read_state(WGPUInstance instance) const;
    // For the panel, without waiting: the state copied in this frame's
    // `encoder` (when the last copy has been read), mapped once the frame
    // is submitted (submitted()); latest() is the last one read.
    void copy_for_panel(WGPUCommandEncoder encoder);
    void submitted();
    [[nodiscard]] AutoExposureState latest() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
