#pragma once

// Thunderstorms, openRTS's own (RTS has none): lightning strikes round the
// eye, each a few strokes of light. Where and when they strike is a field
// hashed from route cells and time slots, so every client of a game sees the
// same strike at the same place and moment without a word on the network.
// A strike's channel to the ground is drawn as glowing strips
// (LightningRenderer), its light shows on the clouds (NativeSkyFrame::flash)
// and on the scene (ModelLighting::flash). See docs/weather.md.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

// Strikes a minute within lightning_radius of the eye, on average: the storm
// gathers them (some minutes more, some fewer, more on one side). Read each frame.
inline float& lightning_rate() {
    static float rate = 6.0f;
    return rate;
}

// How bright their light is on the clouds, the land and the channel (0..3,
// 1 as made). Read each frame.
inline float& lightning_brightness() {
    static float brightness = 1.0f;
    return brightness;
}

// A strike asked for in the viewer panel, to try the light and the thunder:
// its distance ahead of the eye (m), to the ground, strength 1. The scene
// takes it on its next frame under a storm (this client only).
inline std::optional<float>& lightning_test_request() {
    static std::optional<float> distance;
    return distance;
}

inline constexpr float lightning_radius = 12000.0f; // m round the eye
inline constexpr double lightning_duration = 1.2;   // s a strike lasts at most
inline constexpr float lightning_cell = 2000.0f;     // m: the field's cells
inline constexpr double lightning_slot = 10.0;       // s: a cell strikes at most once a slot

struct LightningStrike {
    std::uint64_t id{};    // the same strike on every client
    glm::dvec2 ground{};   // where it strikes, route metres (x east, z north)
    double start{};        // s of the day it begins
    bool to_ground = true; // a channel to the ground; else a flash within the cloud
    std::uint32_t seed{};  // its shape and strokes
    float strength = 1.0f; // 0.5..1.5
};

// How many strikes the storm gathers near `at` (route metres) at `time` (s
// of the day): 0.2..2.6, about 1 on average; the gathering drifts on.
[[nodiscard]] float lightning_gathering(glm::dvec2 at, double time);

// The strikes alight at `time` (s of the day) within `radius` m of `eye`
// (route metres), `rate` a minute within lightning_radius; the newest first.
[[nodiscard]] std::vector<LightningStrike> lightning_strikes(glm::dvec2 eye, double time, float rate,
                                                             float radius = lightning_radius);

// A strike's light `age` s after it began: 0 before and after it, its
// strokes up to about its strength (the first return stroke the brightest).
[[nodiscard]] float lightning_flash(const LightningStrike& strike, double age);
// Its light as a frame `span` s long ending at `age` shows it: the
// brightest within the frame, so a stroke of a few tens of milliseconds
// shows near its peak on some frame even at 30 frames a second (sampled at
// one instant, most strokes fell between the frames).
[[nodiscard]] float lightning_flash_seen(const LightningStrike& strike, double age, double span);

// A piece of its channel, metres from the ground point (y up), and its width
// and brightness at each end (1: the main channel; branches thinner and dimmer,
// the top fading into the cloud).
struct LightningSegment {
    glm::vec3 a{};
    glm::vec3 b{};
    float width = 1.0f;
    float brightness_a = 1.0f;
    float brightness_b = 1.0f;
};

// The channel of a strike to the ground from `height` m above it (the cloud's
// base) down, with its branches; none within the cloud.
[[nodiscard]] std::vector<LightningSegment> lightning_channel(const LightningStrike& strike, float height);

#ifdef OPENRTS_HAS_WGPU
// One strip of a channel as drawn: its ends in scene coordinates, the core's
// width in pixels (a halo round it six times as wide) and the light at each
// end (the frame's encoding, values above 1 kept in the HDR frame).
struct LightningStrip {
    glm::vec3 a{};
    float width = 1.5f;
    glm::vec3 b{};
    float spare = 0.0f;
    glm::vec4 light_a{};
    glm::vec4 light_b{};
};

// Draws the frame's channels in the scene pass after the sky: added, depth-
// tested without writing depth; a channel beyond the far plane is kept on it
// (in front of the sky, behind whatever is drawn).
class LightningRenderer {
public:
    LightningRenderer(WGPUDevice device, WGPUTextureFormat target_format);
    ~LightningRenderer();
    LightningRenderer(const LightningRenderer&) = delete;
    LightningRenderer& operator=(const LightningRenderer&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    void set_strips(std::vector<LightningStrip> strips);
    [[nodiscard]] bool empty() const noexcept;
    void render(WGPURenderPassEncoder pass, const glm::mat4& view_projection, glm::vec2 viewport);
    // The same into the bloom's source (BloomRenderer::begin_sources: one sample, HDR).
    void render_bloom(WGPURenderPassEncoder pass, const glm::mat4& view_projection, glm::vec2 viewport);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
