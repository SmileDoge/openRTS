#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <webgpu/webgpu.h>

#include <openrts/audio/audio_engine.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/rts_sky.hpp>

#include "camera.hpp"

namespace openrts::viewer {

struct FrameView {
    glm::vec3 eye{};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    float vertical_fov = 0.0f;
    float aspect = 1.0f;
    float viewport_height = 720.0f; // pixels
};

// GPU objects a scene may create resources with.
struct SceneGpu {
    WGPUDevice device = nullptr;
    WGPUTextureFormat target_format = WGPUTextureFormat_Undefined;
    renderer::ModelRenderer& models;
    // Sound output, or nullptr when sound is off or no device is available.
    audio::AudioEngine* audio = nullptr;
};

// World clock driven by the viewer.
struct SceneClock {
    std::int32_t time_of_day = 12 * 3600; // seconds
    float elapsed = 0.0f;                  // frame time in seconds
    renderer::rts::Season season = renderer::rts::Season::summer;
    renderer::rts::Weather weather = renderer::rts::Weather::clear;
};

// A service label drawn over the 3D view at a point of the scene (junction
// ids, signal heads); text may have several lines.
struct WorldLabel {
    glm::vec3 position{};
    std::string text;
    std::uint32_t colour = 0xffffffffu; // 0xAABBGGRR (ImGui)
};

// A line between two points of the scene drawn over the 3D view (screen
// space, `thickness` pixels), e.g. a signal to its track.
struct WorldLine {
    glm::vec3 a{};
    glm::vec3 b{};
    std::uint32_t colour = 0xffffffffu; // 0xAABBGGRR (ImGui)
    float thickness = 2.0f;
};

// Keys the scene reacts to this frame (edges are "pressed this frame").
struct SceneInput {
    bool step_up = false;        // Up arrow held (shape scenes: animation frames)
    bool step_down = false;      // Down arrow held
    bool throw_junction = false; // J
    bool toggle_follow = false;  // V
    bool toggle_track = false;   // T
    bool toggle_labels = false;  // L: junction and signal labels
    // Train driving (edges).
    bool place_train = false;      // C
    bool throttle_up = false;      // Home
    bool throttle_down = false;    // End
    bool reverser_forward = false; // Page Up
    bool reverser_back = false;    // Page Down
    bool brake_more = false;       // ]
    bool brake_less = false;       // [
    bool brake_less_held = false;  // [ held (brake valve position I)
    bool independent_more = false; // .
    bool independent_less = false; // ,
    bool toggle_ep = false;        // K: ЭПТ on/off
    bool turn_around = false;      // X: drive from the other end of the train
    bool toggle_track_control = false; // Ctrl+Alt+F7 (RTS): the Track control window
    bool place_names = false;     // F7 (RTS): platform and siding names along the track
    // RTS's information keys (docs/reverse/dispatcher.md).
    bool car_info = false;        // F5: a page of the train's cars; again: the next page
    bool car_info_shift = false;  // Shift held with F5: the train as a whole
    bool limits_panel = false;    // Ctrl+F5: speed limits and the kilometre
    bool stations_panel = false;  // F6: the stations ahead
    bool signal_names = false;    // Ctrl+F7: signal names near the camera
};

// Lighting and background colour the scene wants for the current frame.
struct SceneEnvironment {
    renderer::ModelLighting lighting{
        .sun_diffuse = glm::vec4{1.0f},
        .sun_ambient = glm::vec4{0.5f, 0.5f, 0.5f, 1.0f},
        .sun_direction = renderer::rts_sun_direction(135.0f, 45.0f),
    };
    // Mid grey, so that black (missing-texture) materials stay visible.
    glm::vec4 clear_colour{0.35f, 0.35f, 0.35f, 1.0f};
    bool time_of_day_applies = false;
};

// What the viewer shows. A scene is loaded on the CPU first so that content
// errors are reported before any window or GPU device is created.
class ViewerScene {
public:
    virtual ~ViewerScene() = default;

    [[nodiscard]] virtual std::string_view title() const = 0;
    // Creates GPU resources and positions the camera. Returns an error message
    // on failure, empty on success.
    [[nodiscard]] virtual std::string attach(const SceneGpu& gpu, ViewerCamera& camera) = 0;
    // Releases GPU resources; called before the renderer and device go away.
    virtual void detach() = 0;
    virtual void update(renderer::ModelRenderer& renderer, ViewerCamera& camera, const SceneClock& clock,
                        const SceneInput& input) = 0;
    [[nodiscard]] virtual SceneEnvironment environment() const { return {}; }
    // Draws behind everything else (sky); called before the models are rendered.
    virtual void draw_background(WGPURenderPassEncoder, const FrameView&) {}
    virtual void submit(renderer::ModelRenderer& renderer, const FrameView& frame) = 0;
    // Draws after the models (debug overlays such as the track network).
    virtual void draw_foreground(WGPURenderPassEncoder, const FrameView&) {}
    // Route coordinates of the scene's local origin (route scenes only), for
    // saving and restoring debug views in absolute coordinates.
    [[nodiscard]] virtual std::optional<formats::RoutePosition> origin() const { return std::nullopt; }
    // Adds scene-specific lines to the debug overlay (ImGui builds only).
    virtual void draw_overlay(const ViewerCamera& camera) const = 0;
};

struct SceneLoadResult {
    std::unique_ptr<ViewerScene> scene;
    std::string error;
};

// Content a scene reads: the VFS over the installation, with hot reload when
// `watching` (changes are taken with Vfs::poll_changes each frame).
struct SceneContent {
    std::shared_ptr<core::Vfs> vfs;
    bool watching = false;
    std::string consist;         // route scenes: consist to load, put on a path start
    std::string path;            // route scenes: the path for the consist (file name, id or name); the first by default
    bool track_control = false;  // route scenes: open the Track control window at start
};

[[nodiscard]] SceneLoadResult load_shape_scene(SceneContent content, const core::VirtualPath& shape);
[[nodiscard]] SceneLoadResult load_route_scene(SceneContent content, const core::VirtualPath& route);

} // namespace openrts::viewer
#endif
