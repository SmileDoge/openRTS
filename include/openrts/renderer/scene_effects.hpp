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

#include <openrts/renderer/cloud_shadow.hpp>
#include <openrts/renderer/sky_light.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/task_pool.hpp>

namespace openrts::renderer {

// The cab's and salon's lamps in full daylight keep at least this part of
// their night strength, whatever SceneEffectSettings::lamps_in_daylight.
inline constexpr float cab_lamps_in_daylight = 0.5f;

struct SceneEffectSettings {
    bool shadows = true;
    bool clustered_lights = true;
    bool ambient_occlusion = true;
    // The occlusion's quality. High: two slices of six steps each way at the
    // frame's size, then a 5 x 5 blur. Low: a quarter of the pixels (half
    // the size each way), one slice of four steps, joined back up to the
    // frame's size by depth: several times cheaper (on a GTX 1650 in a cab
    // high took 60 fps down to 10), coarser and softer.
    bool occlusion_low = false;
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
    // Lamps (headlights, the scenery's) in full daylight, as a part of their
    // strength at night (0..1): an eye used to the sun hardly sees them.
    // Their colours are made for the night (RTS lays a lamp over the
    // lighting with max(), so by day its full ambient hides it). The cab's
    // and salon's lamps at least cab_lamps_in_daylight of it (since
    // 2026-11-15: indoors, out of the sun; at 0 they did nothing by day).
    float lamps_in_daylight = 0.25f;
    // Light the ground throws back onto surfaces turned down or sideways
    // (bogies, the underframe), as the ground's reflectance (0..0.5): the
    // ground taken as lit by the sun and the sky. 0: none.
    float ground_bounce = 0.2f;
    // Light indoors by day (0..1; the experimental path): in the cab or salon
    // the eye is in (ModelLighting::room) the sun come in through the
    // windows and the sky's light, thrown about the room, on every surface.
    // Without it the shadows left a cab several times darker than outside
    // (testers, 2026-11-16: "кабина темновата из-за теней"); RTS, without
    // shadows, lights the cab as if it had no roof. 0: none.
    float indoor_light = 0.5f;
    // PBR stage 2, the sky's own light (docs/experimental-effects.md, "The
    // sky's light"), to tune it: its diffuse part and its reflections as
    // factors (0..2; 1: as the sky gives them), the sky's reflection on the
    // surfaces of the cab or salon the eye is in (0..1, their windows let
    // only some of it in), and the least of the sky's light near the eye in
    // a tunnel (0..0.5, what its mouths let in).
    float sky_light_strength = 1.0f;
    float sky_reflection_strength = 1.0f;
    float sky_reflection_indoors = 0.35f;
    float sky_light_tunnel = 0.1f;
    // Lamps throwing shadows (0..4, with the clustered lights): those
    // lighting the shadow focus (the cab, else around the view) most, by
    // their power, where they shine and how far they reach, so a floodlight
    // does not light the cab through its roof.
    std::uint32_t lamp_shadows = 3;
    // Bloom (BloomRenderer, the HDR frame): the light of the lamps, signals
    // and headlights and of what is over 1 (the sun) spread round them;
    // strength 0..3, 1 as made; bloom_textures: at night the textures'
    // brightest too (lamps' bulbs, lit windows, signs; BloomRenderer). It
    // takes the camera's depth pre-pass (for the lights hidden behind
    // things), drawn for it without the ambient occlusion too.
    bool bloom = true;
    float bloom_strength = 1.0f;
    bool bloom_textures = true;
    // Light shafts (LightShaftRenderer, the HDR frame): the sun's beams in
    // the air through what shades it; strength 0..4, 1 as made. They take
    // the camera's depth pre-pass too. Off by default (openRTS's own).
    bool light_shafts = false;
    float light_shafts_strength = 1.0f;
    // Relief by the materials' height maps (parallax occlusion mapping,
    // docs/pbr-materials.md): the image shifted where the eye's ray meets
    // the relief, up to `parallax_distance` m from the eye (faded out over
    // its last quarter), `parallax_steps` steps at most along a ray seen
    // edgewise. openRTS's own; only materials with a height map have it.
    bool parallax = true;
    float parallax_distance = 30.0f;
    int parallax_steps = 32;
    bool show_occlusion = false;    // debug: the occlusion as the colour
    // Debug (PBR stage 4, docs/pbr-materials.md): the models' material as
    // the colour: 0 the picture, 1 the base colour, 2 roughness, 3 metallic,
    // 4 the normal (after its map), 5 the occlusion (the map's and GTAO's),
    // 6 the emission. Not kept.
    int material_view = 0;
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
    std::uint32_t lamp_shadows_drawn = 0; // of them, maps drawn this frame (the rest kept)
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
    // The sun's shadow cache this frame: on, the cascades drawn whole into
    // it, its pieces drawn, and the share of the cascades' texels they cover.
    bool still_cache = false;
    std::uint32_t still_full = 0;
    std::uint32_t still_pieces = 0;
    float still_share = 0.0f;
    // Of the cascades' working maps and the cab's map, those kept from the
    // last frame (nothing in them changed).
    std::uint32_t maps_kept = 0;
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
    // `clouds`: openRTS's clouds' shadow (NativeSky::cloud_shadow), with the
    // sun's. `sky`: the sky's light (SkyLight::view; PBR stage 2), read by
    // the physically based light when it is on.
    void begin_frame(const SceneEffectView& view, const ModelLighting& lighting, bool active, const CloudShadow& clouds = {},
                     const SkyLightView& sky = {});
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
    // `casters`: all of them, or (the sun's shadow cache) the still ones or
    // the moving ones (ModelRenderer::set_moving; the terrain is still).
    using DepthDraw = std::function<void(WGPURenderPassEncoder pass, std::uint32_t slot, const glm::mat4& view_projection,
                                         DepthPassKind kind, DepthCasters casters)>;
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
    // The camera's depth pre-pass of this frame (single-sample, the frame's
    // size), when it is drawn (the ambient occlusion or the bloom); else null.
    [[nodiscard]] WGPUTextureView camera_depth() const noexcept;
    // The owner (ModelExtraLight::owner) of the lamp a depth slot draws the
    // shadow of, 0 for the other slots: its own model is left out.
    [[nodiscard]] std::uint32_t depth_slot_owner(std::uint32_t slot) const noexcept;
    [[nodiscard]] SceneEffectStats stats() const noexcept;
    // The sun's shadow cache checked (--shadow-cache-check): every cascade
    // also drawn whole into a scratch array each frame, and the texels that
    // differ from the cached one counted on the GPU.
    void set_shadow_cache_check(bool on);
    // Between begin_frame() / set_focus() and record(): the frame's moving
    // signature (ModelRenderer::moving_signature). With it a cascade whose
    // cache did not change and whose window stayed keeps its working map,
    // and the cab's map stays while its matrix does (drawn again at least
    // every 30 frames): nothing is drawn while nothing moves. `outside`:
    // the signature without the cab, for the cascades while the cab's map
    // covers the cab (looked up first there); `signature` with it.
    void set_moving_signature(std::uint64_t signature, std::uint64_t outside);
    // After record() / record_parallel(), in the frame's main encoder (after
    // the workers' buffers): the comparison.
    void record_check(WGPUCommandEncoder encoder);
    // After the frame's submit: every 120 frames a line of the counts (read
    // back, waiting for the GPU), else empty.
    [[nodiscard]] std::string shadow_cache_check_report(WGPUInstance instance);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
