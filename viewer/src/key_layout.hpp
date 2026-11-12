#pragma once

#include <openrts/simulation/rail_joints.hpp>

// Which keys the viewer's own engineer actions use. RTS's put the cab
// window and the head out on the numeric keypad (Num0 / Num1, rts1.exe
// 0x4121BC); openRTS's take keys every keyboard has and no installed cab
// assigns (the cabs' AssignKey use letters, digits, Space and ; ' [ ] , . /
// \ < >). The rest (arrows, Shift, Enter, Ctrl+Enter, Alt) is RTS's in both.
// Remembered in viewer_state.ini as key_layout.
namespace openrts::viewer {

enum class KeyLayout : unsigned char { rts, openrts };

[[nodiscard]] KeyLayout key_layout() noexcept;
void set_key_layout(KeyLayout layout) noexcept;

// Display names of the current layout's keys (translated).
[[nodiscard]] const char* window_key_name();
[[nodiscard]] const char* head_out_key_name();
// The car menu (RTS: Num3; openRTS: the ` key, left of 1).
[[nodiscard]] const char* car_menu_key_name();
// RTS's flashlight: Num2; openRTS's Shift+` (the backslash is the ED4M's and
// ER2's cab key, the tilde no cab's).
[[nodiscard]] const char* flashlight_key_name();
// The tape (RTS: Ctrl+Num5; openRTS: Ctrl+`). Num5, which locomotive it
// records, is RTS's layout's only.
[[nodiscard]] const char* tape_key_name();

// The key hints at the bottom of the screen (what Enter does where the
// engineer stands, the window keys, a cab door's): shown unless turned off
// in the start menu's Settings or the panel's General (remembered as
// key_hints).
[[nodiscard]] bool key_hints() noexcept;
void set_key_hints(bool shown) noexcept;

// How fast the mouse turns the view (the right button's look, the orbit): a
// factor on the usual step, 0.2..3, set in the start menu's Settings or the
// panel's General (remembered as mouse_sensitivity). The step also narrows
// with the view's zoom (ViewerCamera::rotate).
inline constexpr float min_mouse_sensitivity = 0.2f;
inline constexpr float max_mouse_sensitivity = 3.0f;
[[nodiscard]] float mouse_sensitivity() noexcept;
void set_mouse_sensitivity(float factor) noexcept;
// The remembered value as a factor (1 when none or unreadable).
[[nodiscard]] float parse_mouse_sensitivity(const char* text) noexcept;

// The driver's head in the cab (openRTS, head_motion.hpp): how far it nods
// with the train's acceleration, braking and the rail joints and leans in
// curves; 0 as RTS (the camera carried by the body's sway alone), 1 as
// tuned, up to 3. Set in the start menu's Settings or the panel's General
// (remembered as head_motion).
inline constexpr float max_head_motion = 3.0f;
[[nodiscard]] float head_motion() noexcept;
void set_head_motion(float amount) noexcept;
// The remembered value (1 when none or unreadable).
[[nodiscard]] float parse_head_motion(const char* text) noexcept;

// The rail joints the wheels knock on (openrts/simulation/rail_joints.hpp):
// openRTS's layout unless RTS's is chosen in the start menu's Settings or
// the panel's General (remembered as rail_joints: "openrts" or "rts").
[[nodiscard]] simulation::RailJointLayout rail_joint_layout() noexcept;
void set_rail_joint_layout(simulation::RailJointLayout layout) noexcept;
// The remembered value ("rts": RTS's, anything else openRTS's).
[[nodiscard]] simulation::RailJointLayout parse_rail_joint_layout(const char* text) noexcept;

} // namespace openrts::viewer
