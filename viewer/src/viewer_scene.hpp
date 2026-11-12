#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <string_view>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <webgpu/webgpu.h>

#include <openrts/audio/audio_engine.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/activity.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/net/connection.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/rts_sky.hpp>
#include <openrts/renderer/scene_effects.hpp>

#include "key_repeat.hpp"

#include "camera.hpp"

namespace openrts::viewer {

struct FrameView {
    glm::vec3 eye{};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    float vertical_fov = 0.0f;
    float view_zoom = 1.0f; // ViewerCamera::view_zoom (Ctrl and the wheel)
    float aspect = 1.0f;
    float viewport_height = 720.0f; // pixels
    float viewport_width = 1280.0f;  // pixels
    // The experimental path's effects (SceneEffects::bind_group) for the
    // terrain; the distance within which shadow casters out of view are
    // wanted (0: none).
    WGPUBindGroup effects = nullptr;
    float shadow_range = 0.0f;
    // Workers the scene may spread its culling over (nullptr: none).
    renderer::TaskPool* pool = nullptr;
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
    double time_of_day_exact = 12 * 3600.0; // the same with its fraction (the sky)
    float elapsed = 0.0f;                  // frame time in seconds
    float time_speed = 1.0f;               // the clock's speed (0: time stopped; elapsed still runs)
    renderer::rts::Season season = renderer::rts::Season::summer;
    renderer::rts::Weather weather = renderer::rts::Weather::clear;
    // The window is in the background (frame_limiter.hpp: at most 30
    // frames/s): the streaming makes up for the fewer frames.
    bool background = false;
};

// A service label drawn over the 3D view at a point of the scene (junction
// ids, signal heads); text may have several lines.
struct WorldLabel {
    glm::vec3 position{};
    std::string text;
    std::uint32_t colour = 0xffffffffu; // 0xAABBGGRR (ImGui)
    // Hidden when the scene in front covers it (read from the depth buffer a
    // frame late); off for captions on the very surface they name.
    bool occludable = true;
    // Who the label is, for the late occlusion answer; 0: its text and place
    // (a label riding a moving train needs a key of its own).
    std::size_t key{};
};

// A line between two points of the scene, `thickness` pixels wide, e.g. a
// signal to its track; drawn in the scene pass, hidden behind what is in front.
struct WorldLine {
    glm::vec3 a{};
    glm::vec3 b{};
    std::uint32_t colour = 0xffffffffu; // 0xAABBGGRR (ImGui)
    float thickness = 2.0f;
};

// Keys the scene reacts to this frame (edges are "pressed this frame").

struct SceneInput {
    // The eye the train's lights are seen from (--capture-camera, whose
    // camera is set after the scene drives its own); otherwise the camera's.
    std::optional<glm::vec3> light_eye;
    bool step_up = false;        // Up arrow held (shape scenes: animation frames)
    bool step_down = false;      // Down arrow held
    bool throw_junction = false; // J
    bool toggle_follow = false;  // V
    bool toggle_track = false;   // T
    bool toggle_labels = false;  // L: junction and signal labels
    bool car_menu = false;       // Num3 / `: the car menu at a coupling or bogie (edge)
    bool flashlight = false;     // Num2 / Shift+`: RTS's flashlight on or off (edge)
    bool tape = false;           // Ctrl+Num5 / Ctrl+`: the tape on or off (edge)
    bool tape_which = false;     // Num5: which locomotive the tape records (edge)
    bool inspector = false;      // F12: the inspector open or shut (edge)
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
    // A train without a DLL (openRTS's default logic; a DLL's cab has its own keys).
    bool horn = false;             // Space held
    bool sander = false;           // X held
    bool bell = false;             // B: the bell (whistle) on / off
    bool pantographs = false;      // P: pantographs down / up again
    bool doors_left = false;       // Y: the left doors open / shut
    bool doors_right = false;      // U: the right doors open / shut
    bool toggle_track_control = false; // Ctrl+Alt+F7 (RTS): the Track control window
    bool place_names = false;     // F7 (RTS): platform and siding names along the track
    // RTS's information keys (docs/reverse/dispatcher.md).
    bool car_info = false;        // F5: a page of the train's cars; again: the next page
    bool car_info_shift = false;  // Shift held with F5: the train as a whole
    bool limits_panel = false;    // Ctrl+F5: speed limits and the kilometre
    bool stations_panel = false;  // F6: the stations ahead
    bool car_view = false;        // F3: orbit the train's first car, then its last, then back
    bool signal_names = false;    // Ctrl+F7: signal names near the camera
    bool activity_journal = false; // F11 (RTS): the scenario's messages
    bool activity_menu = false;    // F8 (RTS): the scenario's menu (reports)
    bool tape_window = false;      // Ctrl+F9 (openRTS): the tape window open or shut
    // The engineer on foot (engineer.hpp), with the cab keys or without.
    bool walk_forward = false, walk_back = false, walk_left = false, walk_right = false; // arrows held
    bool walk_run = false;             // Shift held
    bool walk_slow = false;            // End held
    bool engineer_action = false;      // Enter
    bool engineer_action_ctrl = false; // Ctrl held with that Enter
    bool cab_labels = false;           // Alt held: the captions of the cab's controls
    bool cab_window = false;           // Num0: the chair's window
    bool head_out = false;             // Num1: the head out of it
    // The mouse over the 3-D view (not over a panel, not turning the view):
    // where it points (normalised device coordinates), a left click, the wheel.
    bool pointer = false;
    float pointer_x{}, pointer_y{};
    bool click = false;
    bool release = false;              // the left button let go (a held cab button returns)
    bool held = false;                 // the left button is down (a cab control is dragged)
    float cursor_x{}, cursor_y{};      // the cursor in window pixels (y down)
    bool click_shift = false;
    float scroll{};
    std::vector<KeyEvent> keys;   // every key press and release this frame (not repeats)
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
    renderer::CloudShadow cloud_shadow; // openRTS's clouds on the ground (none: map null)
};

// The start's loading screen: shown until the world around the camera and
// the train (its cab, for a start in it) are loaded.
struct SceneLoading {
    bool done = true;
    // No time passes meanwhile (a start in the cab: the locomotive's DLLs
    // run from the moment the cab is shown, not while it loads).
    bool hold_time = false;
    float progress = 1.0f;  // 0..1, never going back
    const char* stage = ""; // English, for tr()
    std::string title;      // the route and the train
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
    // In the main pass after the opaque models, before the translucent ones
    // (glass): helpers that show through glass but hide behind solid models.
    virtual void draw_before_translucent(WGPURenderPassEncoder, const FrameView&) {}
    // Depth only, for the experimental path's shadow cascades and depth
    // pre-pass (SceneEffects): what the scene draws besides the models (the
    // terrain).
    virtual void draw_depth(WGPURenderPassEncoder, std::uint32_t /*slot*/, const glm::mat4& /*view_projection*/,
                            renderer::DepthPassKind /*kind*/) {}
    // Called once before the depth passes, which may run on several threads.
    virtual void prepare_depth() {}
    // Outside any pass, just before the main pass begins (after the
    // experimental path's depth passes): what the main pass reads.
    virtual void before_main_pass(WGPUCommandEncoder, const FrameView&) {}
    // After the translucent models (glass), before the cab: where RTS draws
    // its lights (0x408574's light loop between object passes 2 and 3).
    virtual void draw_after_translucent(WGPURenderPassEncoder, const FrameView&) {}
    // The occlusion queries the main pass is begun with (nullptr: none).
    [[nodiscard]] virtual WGPUQuerySet occlusion_query_set() const { return nullptr; }
    // After the main pass ended, before the encoder is finished; and after
    // the frame was submitted.
    virtual void after_main_pass(WGPUCommandEncoder) {}
    virtual void frame_submitted() {}
    // Route coordinates of the scene's local origin (route scenes only), for
    // saving and restoring debug views in absolute coordinates.
    [[nodiscard]] virtual std::optional<formats::RoutePosition> origin() const { return std::nullopt; }
    // Adds scene-specific lines to the debug overlay (ImGui builds only).
    virtual void draw_overlay(const ViewerCamera& camera) const = 0;
    // The scene takes the keyboard (a cab's own keys): the camera and the
    // default driving keys stay off.
    [[nodiscard]] virtual bool keyboard_captured() const { return false; }
    // The scene is done: its window closes (the start menu, updated, starts again).
    [[nodiscard]] virtual bool wants_close() const { return false; }
    // Esc closes something of the scene's this frame (the car menu, a
    // placement in Track control): it does not show or hide the panels then.
    [[nodiscard]] virtual bool escape_taken() const { return false; }
    // The mouse wheel works the scene (the 3-D cab) instead of zooming.
    [[nodiscard]] virtual bool takes_scroll() const { return false; }
    // Switch `id` of the driven train's cab to `state` (--capture-switch).
    virtual void set_cab_switch(std::uint32_t, std::uint32_t) {}
    // Where it is now: State and SetState (the capture's report).
    [[nodiscard]] virtual std::optional<std::pair<std::uint32_t, std::uint32_t>> cab_switch_state(std::uint32_t) const { return std::nullopt; }
    // What the world has around (water, forests, crossings, cars, rain,
    // smoke), a line for the capture's report.
    [[nodiscard]] virtual std::string world_report() const { return {}; }
    // A time of day the scene sets (an RTS server's clock), taken once.
    [[nodiscard]] virtual std::optional<double> take_time_of_day() { return std::nullopt; }
    // The game is paused from outside (a network server): no time passes.
    [[nodiscard]] virtual bool paused() const { return false; }
    // The start's loading, as the last update left it.
    [[nodiscard]] virtual SceneLoading loading() const { return {}; }
    // Car `car` of the driven train, shape space to world (--capture-camera).
    [[nodiscard]] virtual std::optional<glm::mat4> train_car_transform(std::size_t) const { return std::nullopt; }
    // The engineer into the driven train's cab (--capture-cab).
    virtual void enter_cab() {}
    // --capture-menu: pick an item of the open car menu; the menu as text.
    virtual void choose_car_menu_item(int) {}
    // --capture-uncouple: uncouple the driven train before car `index` (from 0).
    virtual void uncouple_driven(std::size_t) {}
    // --capture-drive: train `index` (from 0, in order) becomes the driven one.
    virtual void drive_unit(std::size_t) {}
    // --capture-board: the engineer on the ground at a cab door of car
    // `index` (from 0) of the driven train.
    virtual void stand_at_cab_door(std::size_t) {}
    // --capture-passage: the engineer at the passage door of car `index`.
    virtual void stand_at_passage(std::size_t) {}
    [[nodiscard]] virtual std::string car_menu_report() const { return {}; }
    // --capture-ok: the scenario's open message closed (its OK).
    virtual void close_activity_message() {}
    // --capture-report: the first item of the scenario's report menu picked
    // (each menu once).
    virtual void report_activity() {}
    // The inspector's choice as text (--capture-inspect).
    [[nodiscard]] virtual std::string inspector_report() const { return {}; }
    // The tape window's tape as text (--capture-tape).
    [[nodiscard]] virtual std::string tape_report() const { return {}; }
    // What the shadows look at most closely this frame (after submit): the
    // cab the engineer is in.
    [[nodiscard]] virtual std::optional<renderer::ShadowFocus> shadow_focus() const { return std::nullopt; }
    // In flight: the sphere the lamps' shadows cover, where the camera looks
    // (nullopt: the shadow focus, or around the view).
    [[nodiscard]] virtual std::optional<renderer::ShadowFocus> lamp_shadow_focus(const ViewerCamera&) const { return std::nullopt; }
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
    int start_state = -1;        // route scenes: the DLLs' start (--start cold|on|running)
    // Route scenes: a saved situation to put the trains back from
    // (--situation: a file, or a name in the route's situation folder).
    std::filesystem::path situation;
    // Route scenes: where the player starts (--spawn): 0 as before (the
    // camera at the route start), 1 the cab, 2 beside the cab car, 3 above
    // the train.
    int start_view = 0;
    // Route scenes: save the situation under this name once the train
    // stands on the track (--save-situation).
    std::string save_situation;
    // The installation folder: RTS's own files (RTS\config.ini) are not in the VFS.
    std::filesystem::path root;
    // Route scenes: an RTS startup snapshot to set the world from
    // (--mp-snapshot, docs/net.md): junctions, signal aspects, trains.
    std::shared_ptr<const net::Snapshot> snapshot;
    // Route scenes: a live RTS server (--mp-connect) the snapshot came from;
    // the scene watches its game (NetSession). `early`: packages that came
    // after the snapshot's end.
    std::shared_ptr<net::RtsConnection> connection;
    std::vector<net::ReceivedPackage> early;
    // Route scenes: logged in to an openRTS server as the dispatcher's
    // console (--mp-console): Track control and the console's window.
    bool console = false;
    // Route scenes: an RTS scenario to start (--activity,
    // docs/activities.md) and the folder of its own files (RTS_ACT/<name>).
    std::shared_ptr<const formats::Activity> activity;
    core::VirtualPath activity_folder;
};

[[nodiscard]] SceneLoadResult load_shape_scene(SceneContent content, const core::VirtualPath& shape);
[[nodiscard]] SceneLoadResult load_route_scene(SceneContent content, const core::VirtualPath& route);

} // namespace openrts::viewer
#endif
