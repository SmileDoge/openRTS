#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <array>
#include <optional>
#include <vector>
#include <utility>

#include <openrts/renderer/webgpu_context.hpp>

#include "debug_view.hpp"
#include "viewer_scene.hpp"
#include "window_mode.hpp"

namespace openrts::viewer {

// Opens a window and runs the interactive loop for `scene` until the window is
// closed. Returns the process exit code.
struct WindowOptions {
    renderer::ShadingPath shading = renderer::ShadingPath::rts_compatible;
    // --effects shadows,clusters,ao,aerial | none: the experimental path's
    // effects for this run, instead of the panel's remembered ones.
    std::optional<std::string> effects;
    // --light-range <m>: the experimental path's light range for this run (0: as RTS).
    std::optional<float> light_range;
    // --headlight-reach <factor>: the trains' headlights' reach for this run (1: as RTS).
    std::optional<float> headlight_reach;
    // --lamps-daylight <0..1>: the lamps' strength in full daylight for this run.
    std::optional<float> lamps_daylight;
    // --ground-bounce <0..0.5>: the ground's reflectance for this run.
    std::optional<float> ground_bounce;
    // --lamp-shadows <0..8>: lamps throwing shadows for this run.
    std::optional<int> lamp_shadows;
    // --shadow-settings distance,size,cascades,softness: this run's shadows.
    std::optional<std::string> shadow_settings;
    // --parallel-recording on|off: this run's choice.
    std::optional<bool> parallel_recording;
    // --paused: the world starts paused (Pause resumes it): nothing moves,
    // not even a flashing signal, so once the scene has loaded (a big
    // station: some 45 s) captures repeat pixel for pixel.
    bool start_paused = false;
    // --multi-draw on|off: this run's multi-draw indirect (the remembered
    // "multi_draw" otherwise, on by default).
    std::optional<bool> multi_draw;
    // --shadow-cache on|off: this run's far-cascade caching.
    std::optional<bool> shadow_cache;
    // --cab-shadows on|off: this run's shadow map around the cab.
    std::optional<bool> cab_shadows;
    std::int32_t time_of_day = 12 * 3600; // seconds
    float time_speed = 1.0f;
    renderer::rts::Season season = renderer::rts::Season::summer;
    renderer::rts::Weather weather = renderer::rts::Weather::clear;
    bool vsync = true;
    // The FPS limit: 30..500 frames per second, 0 none (--fps-limit; else
    // the remembered "fps_limit", the start menu's and the panel's setting).
    int fps_limit = 0;
    // The window: in a window, borderless or full screen (--window-mode;
    // else the remembered "window_mode"; window_mode.hpp). A capture and the
    // start menu are always a window.
    std::optional<WindowMode> window_mode;
    bool batching = true;
    std::uint32_t msaa = 4; // samples (--msaa 1|4); 4 as RTS's "4x MSAA +Alpha"
    bool sound = true;
    // --sound-offline: the sounds run without a device, nothing is heard
    // (automated checks with OPENRTS_SOUND_TRACE).
    bool sound_offline = false;
    float sound_volume = 0.5f; // master volume, 0..1
    // What is open (root, mode, content), recorded in saved debug views.
    DebugView identity;
    // Camera to restore after the scene is attached (--view).
    std::optional<DebugView> view;
    // A menu (openrts_game): the scene draws the whole interface, no viewer panel.
    bool menu = false;
    // HDR frame with a tone curve (AgX in the experimental path), --hdr.
    bool hdr = false;
    // --backend auto|vulkan|dx12|opengl: this run's graphics API, instead of
    // the remembered one (viewer_state.ini `backend`).
    std::optional<renderer::GraphicsBackend> backend;
    // --reshade-depth on|off: keep the depth for ReShade this run (else remembered).
    std::optional<bool> reshade_depth;
    std::string title = "openRTS shape viewer";
    // A frame to a file and exit (--capture <file.ppm>): the window off the
    // screen, without focus and panels, the frame taken `capture_at` seconds
    // after the start; the camera, when given, at eye and target in car
    // `capture_car`'s shape space (x right, y up, z forward).
    std::string capture_path;
    float capture_at = 10.0f;
    std::optional<std::array<float, 6>> capture_camera;
    std::size_t capture_car = 0;
    // Cab switches set before the capture (--capture-switch <id> <state>,
    // repeatable): from half of capture_at on, half a second apart and in
    // order, so the DLL has the time to act.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> capture_switches;
    // The engineer in the cab's seat for the capture (--capture-cab), at
    // half of capture_at with the switches.
    bool capture_cab = false;
    // The car menu for the capture (--capture-menu <item>): opened at half of
    // capture_at with the camera where --capture-camera puts it, item
    // `capture_menu` picked a moment later (0: none); the capture prints it.
    // More --capture-menu items: the menu is opened again for each, one
    // after another before the capture (an uncoupling, then a coupling).
    std::optional<int> capture_menu;
    std::vector<int> capture_menu_more;
    // --capture-inspect: at 0.5 of capture_at the inspector (F12) opens and
    // a click at the picture's middle chooses what is there; the capture
    // prints the inspector's choice.
    bool capture_inspect = false;
    // --capture-uncouple <index>: at 0.6 of capture_at the driven train is
    // uncoupled before car `index` (from 0), as the Train window's button does.
    std::optional<std::size_t> capture_uncouple;
    // --capture-drive <index>: at 0.8 of capture_at train `index` (in the
    // trains' order, from 0) becomes the driven one, as the Train window's
    // "drive" does.
    std::optional<std::size_t> capture_drive;
    // --capture-board <index> (repeatable): from 0.3 of capture_at, one
    // after another, the engineer stands on the ground at a cab door of car
    // `index` (from 0) of the driven train, then Enter (to that cab, its door
    // opened) and Ctrl+Enter (up the steps) are pressed; each takes 0.6 of
    // capture_at shared out.
    std::vector<std::size_t> capture_board;
    // --capture-passage <index>: at 0.3 of capture_at the engineer stands in
    // the cab or salon of car `index` (from 0) of the driven train, at its
    // passage door to the next car, the door opened (the next car's room
    // seen through it); from 0.6 to 0.75 he walks on through it.
    std::optional<std::size_t> capture_passage;
    // --capture-ok: a scenario's messages closed as they come (their OK).
    bool capture_ok = false;
    // --capture-report: from 0.5 of capture_at, every 2 s, the first item
    // of the scenario's report menu (F8) picked, each menu once.
    bool capture_report = false;
    // --capture-tape: at 0.2 of capture_at the tape starts (Ctrl+Num5) and
    // its window opens (Ctrl+F9); it rolls until the capture ends, when the
    // window's tape is printed.
    bool capture_tape = false;
};

[[nodiscard]] int run_window_viewer(ViewerScene& scene, const WindowOptions& options);

} // namespace openrts::viewer
#endif
