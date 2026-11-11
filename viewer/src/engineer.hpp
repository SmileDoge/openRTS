#pragma once

// The player on foot, RTS's engineer: seated in a cab chair, standing and
// walking about the cab shape, or outside on the ground.
//
// RTS (rts1.exe): config.ini `engineer.Height` (1.8, the man) and
// `engineer.Height1` (0.8). Seated, the eye is Height1 above the chair's
// Pos1 (0x43778C: y = Pos1.y + Height1 - Height, the eye Height above that);
// Enter stands him up at Pos2 (his feet at Pos2.y), Ctrl+Enter at Pos3
// (InteriorChairs). In a cab he walks on the .sd InteriorPlanes and the
// planes of its open doors, as RTS (0x434C58): a step goes where a plane
// lies under it, up to 0.4 Height above his feet; off them he slides along
// the edge of the plane he stands on. The planes are the author's: chairs
// and desks are off them, and nothing else stops him. His eye is Height
// above the plane (it often lies under the visible floor: the VL10k's by
// 0.29 m). A cab without planes: on the cab shape's own surfaces (floor,
// walls, desks, doors), the eye 0.85 Height above his feet. Outside: on
// the ground among the world's objects' boxes, and on the cars' outsides
// (RTS mode 6: walkways, steps, ladders, ledges, doors; walk_rules.hpp),
// the eye Height above his feet; trajectories run as passages.

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
#include "walk_rules.hpp"

namespace openrts::viewer {

// A car's outside he walks on (RTS mode 6) this frame: its shape .sd's
// ESD_Exterior_Details (ExteriorPlanes, ExteriorLedges, ExteriorLadders,
// ExteriorDoors) in the car's shape frame.
struct CarOutside {
    const formats::ExteriorDetails* details = nullptr;
    glm::mat4 transform{1.0f};                       // the car's shape -> the scene
    std::function<bool(std::size_t door)> door_open; // an ExteriorDoor open now
    // Its cab holds this point (car frame) on a plane (RTS 0x434C58): the
    // way in through a door's plane (RTS 0x4322F0); none: no cab.
    std::function<bool(const glm::vec3&)> cab_holds;
    float width = 3.0f;
};

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
    // RTS's walking among the world's objects' ESD_Complex boxes (0x4322F0,
    // mode 1): a step from `from` to `to` (scene), his height.
    std::function<BoxStep(const glm::vec3& from, const glm::vec3& to, float height)> boxes;
};

class Engineer {
public:
    // on_car: on a car's outside (RTS mode 6), his feet in its shape's frame.
    enum class Mode : std::uint8_t { off, seated, standing, outside, on_car };
    // What he did that the scene carries out (take_event()).
    struct Event {
        enum class Kind : std::uint8_t {
            none,
            into_cab,    // from the car's outside through ExteriorDoor `door`, at `at` (car frame)
            onto_car,    // from the cab out onto the car's outside by its door `door`, at `at` (cab frame);
                         // `by_path`: along the car's ExteriorDoor of the same name (Ctrl+Enter, RTS 0x431270)
            next_car,    // over to the car at the end `side` (+1 the shape's front), at `at` (car frame);
                         // `or_off`: none there, down to the ground
            outer_door,  // Enter at the car's ExteriorDoor `door`: open or shut it
        };
        Kind kind = Kind::none;
        std::size_t door{};
        int side{};
        glm::vec3 at{};
        bool by_path = false;
        bool or_off = false;
    };
    [[nodiscard]] Event take_event() noexcept { return std::exchange(event_, Event{}); }

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
    // Going along a trajectory (a ladder, a door's way, a climb onto a box).
    [[nodiscard]] bool passing() const noexcept { return passage_.active(); }

    // The ladder of `car` within reach of the eye (RTS 0x414D24 / 0x422EBC:
    // the nearest whose Position is within its Radius and in view), and how
    // far it is.
    [[nodiscard]] static std::optional<std::pair<std::size_t, float>> ladder_in_reach(const CarOutside& car,
                                                                                     const glm::vec3& eye,
                                                                                     const glm::vec3& forward);
    // What Ctrl+Enter at that ladder does from where he stands.
    [[nodiscard]] static std::string ladder_prompt(const CarOutside& car, std::size_t ladder, const glm::vec3& feet_car);
    // Up (or down) `ladder` of `car` from the ground (RTS 0x431F88): on the
    // car's outside, going along it.
    void climb(const CarOutside& car, std::size_t ladder);
    // Out of the cab onto the car's outside at `at` (car frame); along the
    // car's ExteriorDoor `door` backwards when it is given (RTS 0x431270).
    void step_onto_car(const CarOutside& car, const glm::vec3& at, std::optional<std::size_t> door = std::nullopt);
    // Into `cab` by its door `door`, standing at `at` (cab frame).
    void enter_cab(const CabView& cab, std::size_t door, const glm::vec3& at);
    // A cab's planes hold `at` (cab frame), RTS 0x434C58: a plane (or an
    // open door's) up to 0.4 `height` above it.
    [[nodiscard]] static bool cab_holds(const CabView& cab, const glm::vec3& at, float height);
    // Down to the ground from `car`'s outside at `at` (car frame), falling.
    void get_off_car(const CarOutside& car, const glm::vec3& at);
    // On `car`'s outside: the arrows walk him there (RTS 0x434444), Enter
    // works an outer door, Ctrl+Enter takes a ladder or goes through a door.
    void update_on_car(const CarOutside& car, const EngineerControls& controls, const Outdoors& outdoors, float elapsed,
                       ViewerCamera& camera);
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
    enum class Target : std::uint8_t { none, stand, sit, door, get_off, get_on, seat, window };
    struct Choice {
        Target target = Target::none;
        std::size_t index{};
    };
    [[nodiscard]] Choice choose(const CabView& cab, const glm::mat4& car) const;
    void walk_inside(const CabView& cab, const glm::vec3& step);
    void walk_planes(const CabView& cab, const glm::vec3& step);
    void walk_on_car(const CarOutside& car, const glm::vec3& step, const Outdoors& outdoors);
    // His feet to `support`: up at once, down falling (RTS 0x4121BC: 9.8 m/s²).
    void settle(float support, float elapsed);
    // The passage ended: where it leads (RTS 0x435654 by the Connects word).
    void end_passage(const CarOutside* car, const Outdoors& outdoors);
    // The head turned by the mouse relative to a body (`base` its heading).
    void look_relative(float base, ViewerCamera& camera);
    // Where he stands at `spot` (a chair's Pos3): at the height of the plane
    // under it (one up to 0.4 Height above it, or the nearest within 0.3 m),
    // else on the cab shape's floor there.
    [[nodiscard]] glm::vec3 stand_at(const CabView& cab, glm::vec3 spot) const;
    // His eye above his feet: Height on the planes, 0.85 Height on the shape.
    [[nodiscard]] float rise(const CabView& cab) const noexcept;
    [[nodiscard]] bool passable(const CabView& cab, const glm::vec3& from, glm::vec3& to) const;
    [[nodiscard]] std::optional<std::size_t> open_door_near(const CabView& cab, bool exterior, float reach = 1.4f) const;
    [[nodiscard]] std::optional<std::size_t> chair_window(const CabView& cab) const;
    void get_off(const CabView& cab, std::size_t door, const glm::mat4& car, const Outdoors& outdoors);
    void walk_outside(const glm::vec3& step, const Outdoors& outdoors, float elapsed);
    // The arrows' step this frame, along the ground where the head looks (`yaw`).
    [[nodiscard]] glm::vec3 walk_step(const EngineerControls& controls, float yaw, float elapsed) const;
    // On the ground: a passage going on, or the arrows' step.
    void move_outside(const EngineerControls& controls, const Outdoors& outdoors, float elapsed, const ViewerCamera& camera);
    void get_on(CabView& cab, std::size_t door);
    [[nodiscard]] glm::vec3 local_eye(const CabView& cab) const;

    Mode mode_ = Mode::off;
    std::size_t chair_ = 0;
    glm::vec3 feet_{};            // cab shape coordinates inside, scene coordinates outside, the car's on its outside
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
    // Seated in a salon's seat (InteriorSeats) instead of a chair.
    std::optional<std::size_t> seat_;
    // Where he looks from and to this frame (cab frame): a seat or a window
    // is picked so, the one most in the middle of the view.
    glm::vec3 eye_cab_{}, look_cab_{0.0f, 0.0f, -1.0f};
    glm::vec3 up_feet_{};         // where he stands then (Pos2; on the shape, on its floor)
    glm::vec3 lean_{}, lean_goal_{}; // the wheel's offset of the head at the chair, cab space
    bool rebase_ = false;         // keep the camera's heading on entering a car
    std::optional<std::size_t> pass_;
    std::string prompt_;
    Passage passage_;
    std::string passage_end_;               // the Connects word it leads to ("": he stays where it ends)
    std::size_t passage_door_ = SIZE_MAX;   // the car's ExteriorDoor it goes through
    OutsideFooting last_;                   // on the car's outside: what he stood on last (RTS dword_61D6B0)
    float fall_ = 0.0f;                     // m/s down (RTS flt_61D418)
    float fall_target_ = 0.0f;              // on the car: the height he falls to
    std::optional<float> box_top_;          // on the ground: the box's top he stands on
    Event event_;
};

} // namespace openrts::viewer
