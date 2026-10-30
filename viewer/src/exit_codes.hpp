#pragma once

namespace openrts::viewer::exit_code {

inline constexpr int success = 0;
inline constexpr int content_failed = 2;   // shape, route or ACE file could not be loaded
inline constexpr int device_failed = 3;    // no WebGPU adapter/device
inline constexpr int upload_failed = 4;    // headless texture upload failed
inline constexpr int platform_failed = 5;  // GLFW unavailable or failed to initialise
inline constexpr int window_failed = 6;
inline constexpr int surface_failed = 7;
inline constexpr int renderer_failed = 8;  // renderer, overlay or scene GPU setup failed

} // namespace openrts::viewer::exit_code
