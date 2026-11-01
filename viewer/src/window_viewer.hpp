#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <optional>

#include "debug_view.hpp"
#include "viewer_scene.hpp"

namespace openrts::viewer {

// Opens a window and runs the interactive loop for `scene` until the window is
// closed. Returns the process exit code.
struct WindowOptions {
    renderer::ShadingPath shading = renderer::ShadingPath::rts_compatible;
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
};

[[nodiscard]] int run_window_viewer(ViewerScene& scene, const WindowOptions& options);

} // namespace openrts::viewer
#endif
