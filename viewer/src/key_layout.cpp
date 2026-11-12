#include "key_layout.hpp"

#include <cstdlib>
#include <cstring>

#include "ui_text.hpp"

namespace openrts::viewer {
namespace {

KeyLayout current = KeyLayout::openrts;
bool hints = true;
bool panels = false;
float sensitivity = 1.0f;
float head = default_head_motion;
simulation::RailJointLayout joints = simulation::RailJointLayout::openrts;
simulation::BodyMotionModel bodies = simulation::BodyMotionModel::openrts;

} // namespace

KeyLayout key_layout() noexcept { return current; }

void set_key_layout(const KeyLayout layout) noexcept { current = layout; }

bool key_hints() noexcept { return hints; }

void set_key_hints(const bool shown) noexcept { hints = shown; }

bool panels_shown() noexcept { return panels; }

void set_panels_shown(const bool shown) noexcept { panels = shown; }

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

float head_motion() noexcept { return head; }

void set_head_motion(const float amount) noexcept {
    head = amount > 0.0f ? (amount < max_head_motion ? amount : max_head_motion) : 0.0f;
}

float parse_head_motion(const char* text) noexcept {
    if (!text) return default_head_motion;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || !(value >= 0.0f)) return default_head_motion;
    return value < max_head_motion ? value : max_head_motion;
}

simulation::RailJointLayout rail_joint_layout() noexcept { return joints; }

void set_rail_joint_layout(const simulation::RailJointLayout layout) noexcept { joints = layout; }

simulation::RailJointLayout parse_rail_joint_layout(const char* text) noexcept {
    return text && std::strcmp(text, "rts") == 0 ? simulation::RailJointLayout::rts : simulation::RailJointLayout::openrts;
}

simulation::BodyMotionModel body_motion_model() noexcept { return bodies; }

void set_body_motion_model(const simulation::BodyMotionModel model) noexcept { bodies = model; }

simulation::BodyMotionModel parse_body_motion_model(const char* text) noexcept {
    return text && std::strcmp(text, "rts") == 0 ? simulation::BodyMotionModel::rts : simulation::BodyMotionModel::openrts;
}

const char* window_key_name() { return current == KeyLayout::rts ? "Num0" : tr("'-'"); }

const char* head_out_key_name() { return current == KeyLayout::rts ? "Num1" : tr("'='"); }

const char* car_menu_key_name() { return current == KeyLayout::rts ? "Num3" : tr("'`'"); }

const char* flashlight_key_name() { return current == KeyLayout::rts ? "Num2" : tr("Shift+'`'"); }

const char* tape_key_name() { return current == KeyLayout::rts ? "Ctrl+Num5" : tr("Ctrl+'`'"); }

} // namespace openrts::viewer
