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
    // --lamp-shadows <0..4>: lamps throwing shadows for this run.
    std::optional<int> lamp_shadows;
    // --shadow-settings distance,size,cascades,softness: this run's shadows.
    std::optional<std::string> shadow_settings;
    // --parallel-recording on|off: this run's choice.
    std::optional<bool> parallel_recording;
    // --shadow-cache on|off: this run's far-cascade caching.
    std::optional<bool> shadow_cache;
    // --cab-shadows on|off: this run's shadow map around the cab.
    std::optional<bool> cab_shadows;
    std::int32_t time_of_day = 12 * 3600; // seconds
    float time_speed = 1.0f;
    renderer::rts::Season season = renderer::rts::Season::summer;
    renderer::rts::Weather weather = renderer::rts::Weather::clear;
    bool vsync = true;
    bool batching = true;
    std::uint32_t msaa = 4; // samples (--msaa 1|4); 4 as RTS's "4x MSAA +Alpha"
    bool sound = true;
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
    // --capture-uncouple <index>: at 0.6 of capture_at the driven train is
    // uncoupled before car `index` (from 0), as the Train window's button does.
    std::optional<std::size_t> capture_uncouple;
};

[[nodiscard]] int run_window_viewer(ViewerScene& scene, const WindowOptions& options);

} // namespace openrts::viewer
#endif
