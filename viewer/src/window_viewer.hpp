#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>

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
};

[[nodiscard]] int run_window_viewer(ViewerScene& scene, const WindowOptions& options);

} // namespace openrts::viewer
#endif
