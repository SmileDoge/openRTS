#pragma once

#include <string>
#include <string_view>

#include <openrts/simulation/body_motion.hpp>
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
// E on foot in openRTS's keys: what Enter does where the engineer stands
// (a door, a window, the chair, a switch), Shift+E what Ctrl+Enter does
// (climb in, go out, go through, a ladder); with nothing for Enter at hand
// the car menu. Seated E is the cab's, as W A S D are (testers 2026-11-20).
// A prompt's keys as the player presses them: on foot in openRTS's keys
// Ctrl+Enter is Shift+E and Enter is E, else as written.
[[nodiscard]] std::string engineer_keys(std::string_view prompt, bool on_foot);
// The prompt offers something for Enter alone (not only Ctrl+Enter).
[[nodiscard]] bool offers_enter(std::string_view prompt) noexcept;
// RTS's flashlight: Num2; openRTS's Shift+` (the backslash is the ED4M's and
// ER2's cab key, the tilde no cab's).
[[nodiscard]] const char* flashlight_key_name();
// The tape (RTS: Ctrl+Num5; openRTS: Ctrl+`). Num5, which locomotive it
// records, is RTS's layout's only.
[[nodiscard]] const char* tape_key_name();
// The world's time on the screen (RTS: Num4, 0x402C88; openRTS: Shift+-).
[[nodiscard]] const char* clock_key_name();
// The boarding at a scenario's stop (RTS: Ctrl+Num7; openRTS: Ctrl+=).
[[nodiscard]] const char* board_key_name();
// On foot outside: collide with the world's objects or walk through them
// (the Train window's checkbox): Num/ in RTS's keys, Shift+'=' in openRTS's.
[[nodiscard]] const char* walk_collision_key_name();

// The sound while the game's window is in the background (another window
// in front, or minimised). RTS falls silent then: its DirectSound buffers
// have no DSBCAPS_GLOBALFOCUS (rts1.exe 0x4AA178, flags 0xAA), and one
// misses a vigilance device's whistle and gets the brakes (testers,
// 2026-11-14). openRTS plays on, unless set quieter or off in the start
// menu's Settings or the panel's General (remembered as background_sound:
// "play", "quiet", "off"). The trains and their DLLs run on either way.
enum class BackgroundSound : unsigned char { play, quiet, off };
[[nodiscard]] BackgroundSound background_sound() noexcept;
void set_background_sound(BackgroundSound sound) noexcept;
[[nodiscard]] BackgroundSound parse_background_sound(const char* text) noexcept;
[[nodiscard]] const char* background_sound_key(BackgroundSound sound) noexcept;
// The volume's factor in the background: 1, a third (about -10 dB), 0.
[[nodiscard]] float background_sound_gain(BackgroundSound sound) noexcept;

// The key hints at the bottom of the screen (what Enter does where the
// engineer stands, the window keys, a cab door's): shown unless turned off
// in the start menu's Settings or the panel's General (remembered as
// key_hints).
[[nodiscard]] bool key_hints() noexcept;
void set_key_hints(bool shown) noexcept;

// Walking on W A S D as well as the arrows while the engineer is on his
// feet (standing in a cab, outside, on a car): those four letters then
// work no cab key. Seated in a chair they are the cab's again (testers,
// 2026-11-18). On unless turned off in the panel's General (remembered as
// wasd_walk).
[[nodiscard]] bool wasd_walking() noexcept;
void set_wasd_walking(bool on) noexcept;

// The КЛУБ's keys on the numeric keypad while NumLock is on (testers
// 2026-11-28): the digits, П on "*", К on "/", ВВОД on Enter; the keypad's
// own keys (the window, the cameras) wait while it is. Off unless turned on
// in the panel's General (remembered as klub_numpad).
[[nodiscard]] bool klub_numpad() noexcept;
void set_klub_numpad(bool on) noexcept;
// NumLock is on (Windows; elsewhere false).
[[nodiscard]] bool num_lock_on() noexcept;
// Both: the keypad is the КЛУБ's now (in a cab with its keys).
[[nodiscard]] inline bool klub_numpad_active() noexcept { return klub_numpad() && num_lock_on(); }

// The openRTS panel and the DLL cab window: hidden until Esc shows them
// (testers, 2026-11-12: they cluttered the top of the picture); Esc hides
// them again. Each start begins with them hidden.
[[nodiscard]] bool panels_shown() noexcept;
void set_panels_shown(bool shown) noexcept;

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

// How smoothly the view follows the mouse: each frame's move spread over
// this many milliseconds (mouse_look.hpp), 0..50, 12 by default
// (remembered as mouse_smoothing_ms). At sensitivity 1 a count of the mouse
// turns the view about half a degree, a dozen pixels on the screen, and
// slow moves came out in jumps (2026-11-20). 0: at once.
inline constexpr float max_mouse_smoothing = 50.0f;
inline constexpr float default_mouse_smoothing = 12.0f;
[[nodiscard]] float mouse_smoothing() noexcept;
void set_mouse_smoothing(float milliseconds) noexcept;
// The remembered value (default_mouse_smoothing when none or unreadable).
[[nodiscard]] float parse_mouse_smoothing(const char* text) noexcept;

// The mouse lock (the wheel's click, or the panel's General; remembered as
// mouse_lock): the cursor hidden and the view turning with every move of
// the mouse, no button held; a dot in the screen's middle clicks the cab's
// controls and the doors. Off while the openRTS panel (Esc) or the start
// menu is shown.
[[nodiscard]] bool mouse_lock() noexcept;
void set_mouse_lock(bool on) noexcept;

// The driver's head in the cab (openRTS, head_motion.hpp): how far it nods
// with the train's acceleration, braking and the rail joints and leans in
// curves; 0 as RTS (the camera carried by the body's sway alone), 1 as
// tuned, up to 3; 0.25 by default (at 1 the neck felt too loose). Set in the
// start menu's Settings or the panel's General (remembered as head_motion).
inline constexpr float max_head_motion = 3.0f;
inline constexpr float default_head_motion = 0.25f;
[[nodiscard]] float head_motion() noexcept;
void set_head_motion(float amount) noexcept;
// The remembered value (default_head_motion when none or unreadable).
[[nodiscard]] float parse_head_motion(const char* text) noexcept;

// The rail joints the wheels knock on (openrts/simulation/rail_joints.hpp):
// openRTS's layout unless RTS's is chosen in the start menu's Settings or
// the panel's General (remembered as rail_joints: "openrts" or "rts").
[[nodiscard]] simulation::RailJointLayout rail_joint_layout() noexcept;
void set_rail_joint_layout(simulation::RailJointLayout layout) noexcept;
// The remembered value ("rts": RTS's, anything else openRTS's).
[[nodiscard]] simulation::RailJointLayout parse_rail_joint_layout(const char* text) noexcept;

// How the car bodies move on their springs (openrts/simulation/body_motion.hpp):
// openRTS's model unless RTS's is chosen in the start menu's Settings or the
// panel's General (remembered as body_motion: "openrts" or "rts").
[[nodiscard]] simulation::BodyMotionModel body_motion_model() noexcept;
void set_body_motion_model(simulation::BodyMotionModel model) noexcept;
// openRTS's body motion: how hard switches push the bodies aside (0..2, 1
// by default; remembered as switch_throws).
[[nodiscard]] float switch_throws() noexcept;
void set_switch_throws(float factor) noexcept;
// The remembered value ("rts": RTS's, anything else openRTS's).
[[nodiscard]] simulation::BodyMotionModel parse_body_motion_model(const char* text) noexcept;

} // namespace openrts::viewer
