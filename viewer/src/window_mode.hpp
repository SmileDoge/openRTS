#pragma once

#include <optional>
#include <string_view>

// How the game's window is shown: in a window; in a window without a frame
// over the whole monitor (Alt+Tab is quick); or full screen (the monitor's
// own mode, minimised when another window takes the focus). Remembered in
// viewer_state.ini as window_mode, the start menu's Settings and the game
// panel's Graphics; --window-mode for one run. Alt+Enter in the game
// switches between the window and the full screen kind chosen last. The
// start menu itself stays a window.
namespace openrts::viewer {

enum class WindowMode : unsigned char { windowed, borderless, fullscreen };
inline constexpr int window_mode_count = 3;

[[nodiscard]] std::optional<WindowMode> parse_window_mode(std::string_view key) noexcept;
// The key it is remembered and given on the command line as.
[[nodiscard]] const char* window_mode_key(WindowMode mode) noexcept;
// Its name for the settings (translated).
[[nodiscard]] const char* window_mode_name(WindowMode mode);

} // namespace openrts::viewer
