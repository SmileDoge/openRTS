#pragma once

// The experimental path's frame effects, shared by the model and terrain
// renderers (bind group 2 of their experimental pipelines):
//  - sun shadows: cascaded shadow maps, rendered before the main pass;
//  - every lamp: the scene's lights clustered by screen tile and depth (a
//    compute pass), instead of RTS's rings of nine;
//  - ambient occlusion: GTAO from a depth pre-pass, on the ambient light;
//  - aerial perspective: haze by distance and height, coloured by the sky
//    and lit towards the sun, instead of RTS's linear fog.
// RTS never had these; the RTS-compatible path does not use them.

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/task_pool.hpp>

namespace openrts::renderer {

struct SceneEffectSettings {
    bool shadows = true;
    bool clustered_lights = true;
    bool ambient_occlusion = true;
    bool aerial_perspective = true;
    float shadow_distance = 250.0f; // m, the last cascade's end (100..1000)
    std::uint32_t shadow_map_size = 2048; // texels a side of each cascade (1024, 2048, 4096)
    std::uint32_t shadow_cascades = 3;    // 2..4
    float shadow_softness = 1.0f;         // the filter's radius in texels (0.5..4)
    // The far cascades (the last two of 3 or 4, the last of 2) redrawn every
    // other frame, at once when their map no longer covers their slice.
    bool cache_far_cascades = true;
    // A map of its own around the shadow focus (the cab the engineer is in),
    // looked up before the cascades: sharp shadows on the desk.
    bool cab_shadows = true;
    // Lamps (headlights, the scenery's, the cab's) in full daylight, as a
    // part of their strength at night (0..1): an eye used to the sun hardly
    // sees a cab lamp. Their colours are made for the night (RTS lays a
    // lamp over the lighting with max(), so by day its full ambient hides it).
    float lamps_in_daylight = 0.25f;
    // Light the ground throws back onto surfaces turned down or sideways
    // (bogies, the underframe), as the ground's reflectance (0..0.5): the
    // ground taken as lit by the sun and the sky. 0: none.
    float ground_bounce = 0.2f;
    // Lamps throwing shadows (0..4, with the clustered lights): those
    // lighting the shadow focus (the cab, else around the view) most, by
    // their power, where they shine and how far they reach, so a floodlight
    // does not light the cab through its roof.
    std::uint32_t lamp_shadows = 3;
    bool show_occlusion = false;    // debug: the occlusion as the colour
    bool operator==(const SceneEffectSettings&) const = default;
};

// The frame's camera.
struct SceneEffectView {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f}; // reversed Z (depth.hpp)
    glm::vec3 eye{};
    float vertical_fov = 1.0f;
    float aspect = 1.0f;
    float near_plane = 0.05f;
    float far_plane = 2000.0f;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
};

// A sphere the shadows look at most closely (the cab): its own map, fitted
// to it and turned with `axis` (the cab's forward), so the map's texels stay
// put on the cab while the train runs straight.
struct ShadowFocus {
    glm::vec3 centre{};
    float radius = 0.0f;
    glm::vec3 axis{0.0f, 0.0f, 1.0f};
};

struct SceneEffectStats {
    std::uint32_t cascades = 0;
    std::uint32_t cascades_drawn = 0; // this frame (the others kept from before)
    std::uint32_t lights = 0; // clustered this frame
    bool focus = false;       // the focus map drawn this frame
    float focus_texel = 0.0f; // its texel (m)
    float near_texel = 0.0f;  // the first cascade's (m)
    float daylight = 0.0f;    // 0 night .. 1 full day, for the lamps
    float lamp_strength = 1.0f; // the lamps' strength this frame
    std::uint32_t lamp_shadows = 0; // lamps throwing shadows this frame
    std::uint32_t lamps_lighting = 0; // lamps lighting the place they are for (the ones chosen from)
    float lamp_radius = 0.0f;         // that place's radius (m)
    // The workers' passes (record_parallel) of the last frame, each from its
    // start to its command buffer finished (ms; 0: not drawn): the cascades
    // drawn again, the focus, the lamps together, the camera's depth pre-pass
    // with GTAO and the clusters.
    std::array<float, 4> record_cascade_ms{};
    float record_focus_ms = 0.0f;
    float record_lamps_ms = 0.0f;
    float record_camera_ms = 0.0f;
};

// The bind group layout of the effects (group 2), for pipelines that read them.
[[nodiscard]] WGPUBindGroupLayout create_scene_effects_layout(WGPUDevice device);

class SceneEffects {
public:
    static constexpr std::uint32_t max_cascades = 4;
    static constexpr std::uint32_t max_lights = 512;
    static constexpr std::uint32_t max_lamp_shadows = 8;

    explicit SceneEffects(WGPUDevice device);
    ~SceneEffects();
    SceneEffects(const SceneEffects&) = delete;
    SceneEffects& operator=(const SceneEffects&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    void set_settings(const SceneEffectSettings& settings);
    [[nodiscard]] const SceneEffectSettings& settings() const noexcept;

    // The frame's camera, lights and sun: cascades, the uniform and the
    // light lists (uploaded now). `active` false leaves every effect off
    // (the RTS-compatible path).
    void begin_frame(const SceneEffectView& view, const ModelLighting& lighting, bool active);
    // After begin_frame(), before recording: this frame's shadow focus
    // (nullopt: none). Its map is drawn in depth slot max_cascades + 1. It
    // also picks the lamps throwing shadows, drawn in depth slots
    // max_cascades + 2 on: onto `lamps` when given (in flight: where the
    // camera looks; their shadows fade out towards its edge), else onto the
    // focus, else around the view.
    void set_focus(const std::optional<ShadowFocus>& focus, const std::optional<ShadowFocus>& lamps = std::nullopt);
    // Records the passes before the main pass: each shadow cascade and the
    // camera's depth pre-pass through `draw` (which draws every depth-writing
    // surface for that pass), then GTAO and the light clusters.
    using DepthDraw = std::function<void(WGPURenderPassEncoder pass, std::uint32_t slot, const glm::mat4& view_projection,
                                         DepthPassKind kind)>;
    void record(WGPUCommandEncoder encoder, const DepthDraw& draw);
    // The same on `pool`'s threads: each shadow cascade, and the depth
    // pre-pass with GTAO and the clusters, into command buffers of their own
    // (recorded and finished in parallel); returns at once. `draw` is called
    // concurrently for different slots, so it must keep to its slot's data.
    // finish_parallel() waits and gives the command buffers in the order to
    // submit them (before the main pass); the caller releases them.
    void record_parallel(WGPUDevice device, TaskPool& pool, const DepthDraw& draw);
    [[nodiscard]] std::vector<WGPUCommandBuffer> finish_parallel(TaskPool& pool);
    // Group 2 of the experimental pipelines (valid also when every effect is off).
    [[nodiscard]] WGPUBindGroup bind_group() const noexcept;
    // Shadow casters wanted within this distance of the camera (0: none).
    [[nodiscard]] float shadow_range() const noexcept;
    // Whether this frame has depth passes (shadows or the occlusion's pre-pass).
    [[nodiscard]] bool wants_depth() const noexcept;
    [[nodiscard]] SceneEffectStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
