#include "window_mode.hpp"

#include "ui_text.hpp"

namespace openrts::viewer {

std::optional<WindowMode> parse_window_mode(const std::string_view key) noexcept {
    if (key == "windowed" || key == "window") return WindowMode::windowed;
    if (key == "borderless") return WindowMode::borderless;
    if (key == "fullscreen") return WindowMode::fullscreen;
    return std::nullopt;
}

const char* window_mode_key(const WindowMode mode) noexcept {
    switch (mode) {
    case WindowMode::borderless: return "borderless";
    case WindowMode::fullscreen: return "fullscreen";
    case WindowMode::windowed: break;
    }
    return "windowed";
}

const char* window_mode_name(const WindowMode mode) {
    switch (mode) {
    case WindowMode::borderless: return tr("Borderless window");
    case WindowMode::fullscreen: return tr("Full screen");
    case WindowMode::windowed: break;
    }
    return tr("In a window");
}

} // namespace openrts::viewer
