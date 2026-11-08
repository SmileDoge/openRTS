#pragma once

// The player on foot, RTS's engineer: seated in a cab chair, standing and
// walking about the cab shape, or outside on the ground.
//
// RTS (rts1.exe): config.ini `engineer.Height` (1.8, the man) and
// `engineer.Height1` (0.8). Seated, the eye is Height1 above the chair's
// Pos1 (0x43778C: y = Pos1.y + Height1 - Height, the eye Height above that);
// Enter stands him up at Pos2, Ctrl+Enter at Pos3 (InteriorChairs). RTS
// walks him on the .sd InteriorPlanes, which lie about 0.3 m under a cab's
// visible floor; openRTS walks him on the cab shape's own surfaces (its
// floor, walls, desks and doors) with the eye 0.85 Height above his feet,
// which puts it where RTS has it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/core/vfs.hpp>

#include "cab_view.hpp"
#include "camera.hpp"

namespace openrts::viewer {

struct EngineerControls {
    bool forward = false, back = false, left = false, right = false; // the arrow keys, held
    bool run = false;          // Shift held
    bool slow = false;         // End held: a fifth of the pace, with Shift twice the run (RTS 0x4121BC)
    bool action = false;       // Enter: stand up, sit down, a door, get off or on
    bool action_ctrl = false;  // Ctrl with that Enter: stand up at the chair's Pos3
    bool window = false;       // Num0: open or close the chair's window (RTS 0x4121BC)
    bool head_out = false;     // Num1: put the head out of the open window, or back
};

// The world outside the cab, from the scene: what the engineer stands on at
// a scene point (the highest surface up to a step above it: the ground, and
// with object collisions on, platforms, ballast, bridges) and whether
// something (a train, a wall) is in the way of a step.
struct Outdoors {
    std::function<std::optional<float>(const glm::vec3&)> floor;
    std::function<bool(const glm::vec3& from, const glm::vec3& to)> blocked;
};

class Engineer {
public:
    enum class Mode : std::uint8_t { off, seated, standing, outside };

    // engineer.Height / Height1 from RTS's config.ini (UTF-16 or ANSI text).
    void configure(std::string_view config);
    // Where one stands outside to climb in through each exterior door of
    // `cab`, in cab coordinates (Mode::outside's get-on spots), by door.
    [[nodiscard]] static std::vector<std::pair<std::size_t, glm::vec3>> get_on_points(const CabView& cab);
    // What Enter does at that door from outside.
    [[nodiscard]] static std::string get_on_prompt(const CabView& cab, std::size_t door);
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] bool active() const noexcept { return mode_ != Mode::off; }
    [[nodiscard]] bool inside() const noexcept { return mode_ == Mode::seated || mode_ == Mode::standing; }
    // The chair he sits in or stands by (the start's, the last one taken).
    [[nodiscard]] std::size_t chair() const noexcept { return chair_; }
    // At the chair, risen to its Pos2 (Enter in the chair, RTS's chair action
    // 3): still bound to it, the arrows do not walk; Enter sits him back down.
    [[nodiscard]] bool risen() const noexcept { return mode_ == Mode::seated && up_; }
    // The wheel at the chair, off the controls: the head nearer (`notches` >
    // 0) along `along` (cab space, where the cursor points) or back, at most
    // 0.6 m from where it is, smoothly.
    void lean(const glm::vec3& along, float notches);
    // Into the cab, seated in chair `chair` (0: the driver).
    void sit(std::size_t chair);
    // On foot outside at `feet` (scene coordinates), as RTS starts an
    // activity beside the player's car.
    void stand_outside(const glm::vec3& feet);
    // On foot in `cab` by chair `chair`, at its Pos3 (where Ctrl+Enter stands
    // him): how RTS starts the engineer in the cab (0x41B230: a current
    // chair, not seated, at Pos3); Enter there sits him down. No chair: seated
    // at the cab's eye, as before.
    void stand_by(const CabView& cab, std::size_t chair);
    void off() noexcept;

    // `car`: the cab car's body this frame (cab shape -> scene). Moves the
    // engineer and puts the camera at his eye; the mouse turns his head.
    // On foot with no cab to go with (no train of his): the same walk
    // outside, the arrows along the ground where the head looks.
    void update_on_foot(const EngineerControls& controls, const Outdoors& outdoors, float elapsed, ViewerCamera& camera);
    void update(CabView& cab, const EngineerControls& controls, const glm::mat4& car, const Outdoors& outdoors,
                float elapsed, ViewerCamera& camera);
    // The scene's origin moved by `delta` (scene coordinates of a fixed point grew by it).
    void shift(const glm::vec3& delta) noexcept;
    [[nodiscard]] glm::vec3 feet() const noexcept { return feet_; }
    // He walked into an open door to the next car (WAGON_FRONT / WAGON_BACK):
    // the door, once; the scene finds the car and calls arrive().
    [[nodiscard]] std::optional<std::size_t> take_pass() noexcept { return std::exchange(pass_, std::nullopt); }
    // Standing just inside door `door` of `cab` (it opens).
    void arrive(CabView& cab, std::size_t door);
    [[nodiscard]] bool heading_out() const noexcept { return head_out_; }
    // What Enter does here (empty: nothing), for the prompt on the screen.
    [[nodiscard]] const std::string& prompt() const noexcept { return prompt_; }
    [[nodiscard]] float height() const noexcept { return height_; }
    // Walking and running paces, m/s (config.ini engineer.velocity / engineer.velocity1).
    [[nodiscard]] float walk_speed() const noexcept { return walk_speed_; }
    [[nodiscard]] float run_speed() const noexcept { return run_speed_; }
    void set_speeds(float walk, float run) noexcept;

private:
    enum class Target : std::uint8_t { none, stand, sit, door, get_off, get_on };
    struct Choice {
        Target target = Target::none;
        std::size_t index{};
    };
    [[nodiscard]] Choice choose(const CabView& cab, const glm::mat4& car) const;
    void walk_inside(const CabView& cab, const glm::vec3& step);
    [[nodiscard]] bool passable(const CabView& cab, const glm::vec3& from, glm::vec3& to) const;
    [[nodiscard]] std::optional<std::size_t> open_door_near(const CabView& cab, bool exterior, float reach = 1.4f) const;
    [[nodiscard]] std::optional<std::size_t> chair_window(const CabView& cab) const;
    void get_off(const CabView& cab, std::size_t door, const glm::mat4& car, const Outdoors& outdoors);
    void walk_outside(const glm::vec3& step, const Outdoors& outdoors);
    void get_on(CabView& cab, std::size_t door);
    [[nodiscard]] glm::vec3 local_eye(const CabView& cab) const;

    Mode mode_ = Mode::off;
    std::size_t chair_ = 0;
    glm::vec3 feet_{};            // cab shape coordinates inside, scene coordinates outside
    float height_ = 1.81f;        // engineer.Height
    float seated_eye_ = 0.81f;    // engineer.Height1
    float walk_speed_ = 1.6f;     // engineer.velocity (RTS default 1.6 m/s, read_config)
    float run_speed_ = 5.0f;      // engineer.velocity1, with Shift (RTS default 5 m/s)
    // The head turned by the mouse: relative to the car inside.
    bool camera_set_ = false;
    float look_yaw_{}, look_pitch_{}, last_yaw_{};
    // Sitting down and standing up take a moment (RTS moves the camera over 2 s).
    glm::vec3 blend_from_{};
    float blend_ = 1.0f;
    bool blocked_ = false;        // the last step ran into something
    bool head_out_ = false;       // seated, the head out of the chair's window
    bool up_ = false;             // seated: risen at the chair (Pos2), risen()
    glm::vec3 up_feet_{};         // where he stands then (Pos2 on the floor)
    glm::vec3 lean_{}, lean_goal_{}; // the wheel's offset of the head at the chair, cab space
    bool rebase_ = false;         // keep the camera's heading on entering a car
    std::optional<std::size_t> pass_;
    std::string prompt_;
};

} // namespace openrts::viewer
