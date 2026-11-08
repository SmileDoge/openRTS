#include "key_layout.hpp"

#include <cstdlib>

#include "ui_text.hpp"

namespace openrts::viewer {
namespace {

KeyLayout current = KeyLayout::openrts;
bool hints = true;
float sensitivity = 1.0f;

} // namespace

KeyLayout key_layout() noexcept { return current; }

void set_key_layout(const KeyLayout layout) noexcept { current = layout; }

bool key_hints() noexcept { return hints; }

void set_key_hints(const bool shown) noexcept { hints = shown; }

float mouse_sensitivity() noexcept { return sensitivity; }

void set_mouse_sensitivity(const float factor) noexcept {
    sensitivity = factor >= min_mouse_sensitivity ? (factor <= max_mouse_sensitivity ? factor : max_mouse_sensitivity) : min_mouse_sensitivity;
}

float parse_mouse_sensitivity(const char* text) noexcept {
    if (!text) return 1.0f;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || !(value > 0.0f)) return 1.0f;
    return value < min_mouse_sensitivity ? min_mouse_sensitivity : value > max_mouse_sensitivity ? max_mouse_sensitivity : value;
}

const char* window_key_name() { return current == KeyLayout::rts ? "Num0" : tr("'-'"); }

const char* head_out_key_name() { return current == KeyLayout::rts ? "Num1" : tr("'='"); }

const char* car_menu_key_name() { return current == KeyLayout::rts ? "Num3" : tr("'`'"); }

const char* flashlight_key_name() { return current == KeyLayout::rts ? "Num2" : tr("Shift+'`'"); }

} // namespace openrts::viewer
