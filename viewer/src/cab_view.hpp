#pragma once

// The 3-D cab of the player's engine: the cab shape (EngExtended Cabin,
// through a .repaint), its .sd, and the poses of its switch and gauge nodes
// for the cab state (docs/train-dll.md, "One cab for both kinds of DLL").
// RTS binds .sd Switches / Displays to the shape's nodes by name; a state is
// a frame of the node's own keys, spread evenly over its key frames.
//
// For the engineer on foot (engineer.hpp) it also keeps the cab shape's
// surfaces (CabCollision) and its interior doors, which open by their node's
// animation (InteriorDoors).

#include <array>
#include <cstdint>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <openrts/audio/interior_acoustics.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/dllhost/protocol.hpp>
#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/renderer/model_renderer.hpp>

#include "cab_screens.hpp"

namespace openrts::viewer {

// The cab shape's surfaces: the nearest distance level's triangles in shape
// coordinates, sorted into 0.25 m columns. `skip` marks nodes left out (open
// doors), by matrix index.
class CabCollision {
public:
    [[nodiscard]] static CabCollision build(const formats::ShapeModel& model);
    // A shape file's surfaces (null when it does not read).
    [[nodiscard]] static std::shared_ptr<CabCollision> load(const core::Vfs& vfs, const core::VirtualPath& shape);
    [[nodiscard]] bool empty() const noexcept { return triangles_.empty(); }
    // The box around every surface, in shape coordinates.
    [[nodiscard]] glm::vec3 lower() const noexcept { return lower_; }
    [[nodiscard]] glm::vec3 upper() const noexcept { return upper_; }
    // The highest surface facing up or down at (x, z) between `low` and `high`.
    [[nodiscard]] std::optional<float> floor(float x, float z, float low, float high,
                                             const std::vector<std::uint8_t>& skip) const;
    // Whether a surface crosses the segment a-b.
    [[nodiscard]] bool crosses(const glm::vec3& a, const glm::vec3& b, const std::vector<std::uint8_t>& skip) const;
    // The nearest surface along origin + t·direction (unit), t in [0, reach]:
    // its distance and node.
    struct Hit {
        float distance{};
        std::int32_t node = -1;
    };
    [[nodiscard]] std::optional<Hit> ray(const glm::vec3& origin, const glm::vec3& direction, float reach,
                                         const std::vector<std::uint8_t>& skip) const;
    // The ray against `nodes` only (a node of `rig` and what hangs on it),
    // that node posed at `frame` of the shape's animation: an open door
    // where it is drawn. The surfaces are read in the rest pose, so the ray
    // goes back into it.
    // What takes a node's rest-pose surfaces (and its children's) to where
    // they are drawn at `frame`, in shape coordinates.
    [[nodiscard]] static glm::mat4 pose_of(const formats::ShapeModel& rig, std::int32_t node, float frame);
    [[nodiscard]] std::optional<Hit> ray_posed(const formats::ShapeModel& rig, std::int32_t node, float frame,
                                               const std::vector<std::int32_t>& nodes, const glm::vec3& origin,
                                               const glm::vec3& direction, float reach) const;
    // The box around each node's own surfaces, by matrix index (lower > upper: none).
    void node_bounds(std::size_t nodes, std::vector<glm::vec3>& lower, std::vector<glm::vec3>& upper) const;
    // The corners of node `node`'s own surfaces (rest pose), every `step`-th
    // triangle's, appended to `out`.
    void node_points(std::int32_t node, std::vector<glm::vec3>& out, std::size_t step = 1) const;

private:
    struct Triangle {
        glm::vec3 a{}, b{}, c{};
        float normal_y{}; // of the unit normal
        std::int32_t node = -1;
    };
    template <class F> void visit(float x0, float z0, float x1, float z1, F&& f) const;
    // Each triangle once in the cells the segment a-b passes over (x, z).
    template <class F> void visit_segment(const glm::vec3& a, const glm::vec3& b, F&& f) const;
    std::vector<Triangle> triangles_;
    glm::vec3 lower_{}, upper_{};
    glm::vec2 origin_{};
    int columns_ = 0, rows_ = 0;
    std::vector<std::uint32_t> start_; // cell -> its first entry in items_ (columns_ * rows_ + 1)
    std::vector<std::uint32_t> items_;
    mutable std::vector<std::uint32_t> seen_;
    mutable std::uint32_t stamp_ = 0;
};

class CabView {
public:
    // The car's room: an engine's Cabin (its cab, or a motor car's salon),
    // else a passenger car's Interior (its salon). nullptr when it has none
    // with a description. `file` is the car's .eng / .wag.
    [[nodiscard]] static std::unique_ptr<CabView> create(const core::Vfs& vfs, const formats::WagonDefinition& car,
                                                         const core::VirtualPath& file);
    // Whether the car names a cab or a salon (create may still fail).
    [[nodiscard]] static bool has_room(const formats::WagonDefinition& car);

    // The model drawn: the Cabin / InteriorFile as named (an RTS .repaint
    // with its replaced images, or the shape).
    [[nodiscard]] const core::VirtualPath& shape() const noexcept { return shape_; }
    // The shape file itself (a .repaint's Shape): its folder holds the
    // cab's own files (TextureChanges, screens).
    [[nodiscard]] const core::VirtualPath& shape_file() const noexcept { return shape_file_; }
    // A passenger car's salon (Interior), not an engine's Cabin.
    [[nodiscard]] bool salon() const noexcept { return salon_; }
    // Where the cab shape sits in the car: EngExtended Cabin ( "x.s" 1 )
    // turns it round (the cab at the car's rear end, VL10k section b), a
    // salon's InteriorFile ( "x.s" 1 ) the same.
    [[nodiscard]] glm::mat4 placement() const noexcept;
    [[nodiscard]] bool turned() const noexcept { return turned_; }
    [[nodiscard]] const formats::CabDescription& description() const noexcept { return cab_; }
    // The LocomotiveSignalization lights' nodes in the cab shape (-1: not found), once bound.
    [[nodiscard]] const std::vector<std::int32_t>& signal_light_nodes() const noexcept { return signal_lights_; }
    // Finds the nodes once the cab model has loaded.
    void bind(const formats::ShapeModel& rig);
    [[nodiscard]] bool bound() const noexcept { return bound_; }
    // Bound, and its own reading of the shape (doors, chairs' floors) done.
    [[nodiscard]] bool loaded() const {
        return bound_ && (geometry_ || !loading_.valid() || loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    }
    // Poses for the cab state: a legacy DLL's outputs, or (null) the .sd's
    // initial states; and the doors as they are open.
    // `aspect`: the aspect the locomotive signal lights show without a DLL
    // (with one, its Cabin::Signal and ALSNOn decide).
    // The state one simulation step before `state`, to draw the switches'
    // frames and the arrows' values between the two (the DLL runs every
    // 0.05 s; drawn as they are, needles and levers move at 20 frames a
    // second). `alpha`: how far into the step the frame is drawn.
    struct Blend {
        const std::vector<float>* switch_frames = nullptr; // by switch id
        const std::vector<float>* display_values = nullptr; // by display id
        float alpha = 1.0f;
        // The switch the mouse holds: drawn where it is now, not between the
        // steps (its frame changes every frame, not every 50 ms).
        std::optional<std::uint32_t> live_switch;
    };
    // The realistic air's pressures (atm) for the gauges MaDaDev's cabs name
    // DTM (the brake pipe), DTC1 and DTC2 (the brake cylinders: the higher of
    // the train brake's and the independent brake's), DGR (the main
    // reservoir) and DURA (the equalising reservoir, the УР the driver's
    // valve holds, openRTS's or the DLL's taken; testers 2026-11-20: it
    // was the DLL's own and went apart from the others): with them those
    // needles show the model's air, not what the DLL draws of its own (its
    // needles lag and run ahead of F5's air; testers 2026-11-17). Other
    // gauges stay the DLL's.
    struct AirGauges {
        float pipe = 0.0f;
        float cylinder = 0.0f;
        float main_reservoir = 0.0f;
        float equalising = -1.0f; // < 0: no valve of the model here (the DLL's needle)
    };
    void adjustments(const dllhost::CarOutput* state, std::vector<renderer::NodeAdjustment>& out,
                     std::uint32_t aspect = 0, const Blend* blend = nullptr, const AirGauges* air = nullptr) const;
    // The displays' blinking (DisplayFade's second value) over `elapsed` s.
    void step_displays(float elapsed);
    // The driver's seat (Pos1 of the first chair), in the engine's shape coordinates.
    [[nodiscard]] glm::vec3 eye() const noexcept { return eye_; }
    // The SCREEN displays for this frame, in cab shape coordinates.
    // `planes`: the screens drawn (CabScreens::build).
    void screens(const dllhost::CarOutput* state, float elapsed, std::vector<ScreenBatch>& out,
                 std::vector<ScreenPlane>* planes = nullptr) {
        if (!screens_) return;
        // A screen display blinks too (DisplayFade, RTS 0x525DB0).
        std::vector<bool> off(cab_.displays.size(), false);
        for (std::size_t i = 0; i < off.size() && i < blink_clock_.size(); ++i)
            off[i] = cab_.displays[i].blink > 0.0f && blink_clock_[i] > cab_.displays[i].blink;
        screens_->build(state, elapsed, out, &off, planes);
    }
    // The SCREEN labels the last screens() call laid out (CabScreens::labels_seen); null: no screens.
    [[nodiscard]] const CabScreens* screen_layout() const noexcept { return screens_.get(); }
    [[nodiscard]] std::size_t switches_bound() const noexcept { return switch_nodes_; }
    // The frames of switch `index`'s node a position takes, as RTS counts
    // them for dragging (0x51C1C8): the node has the shape's frame rate x
    // AnimTime x (positions - 1) frames (the positions if that is 0), its
    // keys spread over them. 0: no animated node.
    [[nodiscard]] float switch_frames_per_state(std::size_t index) const noexcept;

    // The engineer on foot: the surfaces once they are read (null before),
    // the doors, and where a control is (its .sd Position, else its node).
    void poll();
    [[nodiscard]] const CabCollision* collision() const noexcept { return geometry_ ? &geometry_->collision : nullptr; }
    // Open: a doorway always; a door as it stands (before the shape's nodes
    // are read: as the .sd and the hand left it).
    [[nodiscard]] bool door_open(std::size_t door) const noexcept;
    // The door has a node of its own (it opens and closes; else an open doorway).
    [[nodiscard]] bool door_animated(std::size_t door) const noexcept {
        return door < doors_.size() && doors_[door].node >= 0;
    }
    // A doorway without a door (no node of its own in the shape, once its
    // nodes are read).
    [[nodiscard]] bool doorway(std::size_t door) const noexcept {
        return door < doors_.size() && geometry_ && doors_[door].node < 0;
    }
    [[nodiscard]] bool door_moving(std::size_t door) const noexcept;
    // How far a door is open now, 0 .. 1 (a doorway without a door: 1;
    // before the shape's nodes are read: as it was left).
    [[nodiscard]] float door_openness(std::size_t door) const noexcept {
        if (door >= doors_.size()) return 0.0f;
        if (!geometry_) return doors_[door].target ? 1.0f : 0.0f;
        return doors_[door].node < 0 ? 1.0f : doors_[door].open;
    }
    // Where it is going: open or shut.
    [[nodiscard]] bool door_wanted(std::size_t door) const noexcept {
        return door < doors_.size() && doors_[door_root(door)].target;
    }
    // The door a door follows through ParentObject (itself when it follows
    // none): working a following one works that one.
    [[nodiscard]] std::size_t door_root(std::size_t door) const noexcept;
    void toggle_door(std::size_t door);
    // Sends a door open or shut (it moves over its AnimTime): the train's
    // passenger doors (InteriorDoors with a GlobalID).
    void set_door(std::size_t door, bool open) {
        if (door < doors_.size()) doors_[door_root(door)].target = open;
    }
    void open_door(std::size_t door);
    void step_doors(float elapsed);
    // Nodes of open doors (and what hangs on them) for CabCollision.
    [[nodiscard]] const std::vector<std::uint8_t>& skip() const noexcept { return skip_; }
    [[nodiscard]] std::optional<glm::vec3> control_point(std::size_t switch_index) const;
    [[nodiscard]] std::optional<glm::vec3> display_point(std::size_t display_index) const;
    // Where a caption goes: the middle of the control's own surfaces (its node
    // and what hangs on it), else control_point / display_point. The .sd
    // Positions of many cabs are well off the controls; RTS never shows text
    // there (its Alt hint is a line in the middle of the screen, 0x40FC06).
    [[nodiscard]] std::optional<glm::vec3> control_label_point(std::size_t switch_index) const;
    [[nodiscard]] std::optional<glm::vec3> display_label_point(std::size_t display_index) const;
    // InteriorWindows: they open by their node's animation.
    [[nodiscard]] std::optional<std::size_t> find_window(std::string_view name) const;
    [[nodiscard]] bool window_open(std::size_t window) const noexcept;
    void toggle_window(std::size_t window);
    // The SMSTriggers codes of the doors and windows that began to open or
    // shut since the last call: RTS (0x438708 -> 0x46AF1C) posts each to
    // every sound of the car (its exterior and cab scripts, its libraries').
    [[nodiscard]] std::vector<std::uint32_t> take_sounds() { return std::exchange(sounds_, {}); }
    // The rooms (InteriorAreas) and how much sound passes between them
    // through the doors and windows as they are open now (docs/sound.md,
    // "Rooms and sound proofing"). Rooms: audio::InteriorAcoustics indices.
    [[nodiscard]] const audio::InteriorAcoustics& acoustics() const noexcept { return acoustics_; }
    [[nodiscard]] float sound_transmission(int from, int to) const;
    // What the mouse points at (cab shape coordinates): the switch or display
    // whose node (or a node under it) the ray meets first.
    struct Picked {
        bool control = true;   // a switch (else a display)
        std::size_t index{};   // in description().switches / displays
        float distance{};
    };
    [[nodiscard]] std::optional<Picked> pick(const glm::vec3& origin, const glm::vec3& direction, float reach) const;
    // The door (a node of its own) the ray meets before any other surface,
    // within `reach`, as it is drawn now (openRTS: a click opens or shuts
    // it; RTS works doors with Enter only).
    struct DoorPicked {
        std::size_t door{}; // in description().doors
        float distance{};
    };
    [[nodiscard]] std::optional<DoorPicked> pick_door(const glm::vec3& origin, const glm::vec3& direction, float reach) const;
    // OPENRTS_DOOR_PICK: from `eye` (the chair's seat and 0.75 m) at the middle of each door's own
    // surfaces as drawn now, what pick_door() finds (a line a door).
    [[nodiscard]] std::string door_pick_report(const glm::vec3& eye) const;

private:
    struct Bound {
        std::int32_t node = -1;
        float last_frame{};
    };
    struct Geometry {
        CabCollision collision;
        std::vector<std::int32_t> hierarchy;
        std::vector<glm::vec3> origins; // every node's origin in shape coordinates
        std::map<std::string, std::int32_t> nodes; // lower-case name -> matrix
        std::vector<float> last_frames;            // every node's last key frame
        std::uint32_t frame_rate = 30;             // the shape's animation, frames a second
        // Matrices, hierarchy and animations, to pose the controls' nodes for picking.
        std::shared_ptr<const formats::ShapeModel> rig;
    };
    struct Door {
        std::int32_t node = -1;
        float last_frame{};
        float open{};      // 0 closed .. 1 open
        bool target = false;
        std::vector<std::int32_t> nodes; // the door's node and its children
    };
    void rebuild_skip();
    void build_owners();
    // Doors that follow another (ParentObject) take its state.
    void follow_doors();
    core::VirtualPath shape_;
    core::VirtualPath shape_file_;
    bool salon_ = false;
    bool turned_ = false;
    formats::CabDescription cab_;
    audio::InteriorAcoustics acoustics_;
    std::vector<Bound> switches_; // by position in cab_.switches
    // By position in cab_.switches: the id whose state it is drawn with, its
    // root through ParentObject (itself when it follows none).
    std::vector<std::uint32_t> switch_source_;
    std::vector<Bound> displays_;
    std::vector<std::int32_t> signal_lights_; // LocomotiveSignalization SignalLights' nodes
    std::vector<std::int32_t> lamp_nodes_;    // LightSources' nodes, by the lamp's Name
    glm::vec3 eye_{0.0f, 2.5f, 0.0f};
    bool bound_ = false;
    std::vector<std::uint32_t> sounds_; // take_sounds()
    void sound(const std::array<std::uint16_t, 2>& codes, bool opening) {
        if (const auto code = codes[opening ? 0 : 1]; code != 0) sounds_.push_back(code);
    }
    std::size_t switch_nodes_ = 0;
    std::future<std::shared_ptr<Geometry>> loading_;
    std::shared_ptr<Geometry> geometry_;
    std::vector<Door> doors_; // by position in cab_.doors
    std::vector<Door> windows_; // by position in cab_.windows
    std::vector<std::uint8_t> skip_;
    // Every node's switch (index + 1) or display (-(index + 1)); 0: none.
    std::vector<std::int32_t> owner_;
    // The frame each switch's node was last drawn at (adjustments()), by
    // switch; < 0: at rest. Picking uses it: RTS finds a control by drawing
    // its node as it is (0x5290C8), so a lowered curtain is grabbed low.
    mutable std::vector<float> switch_frames_;
    std::vector<std::optional<glm::vec3>> control_centres_, display_centres_; // of their surfaces
    // Each switch's own nodes and the box around their surfaces (rest pose;
    // lower > upper: none), so a ray is tried against a posed switch's
    // surfaces only when it meets that box.
    std::vector<std::vector<std::uint32_t>> switch_owned_;
    std::vector<glm::vec3> switch_lower_, switch_upper_;
    std::unique_ptr<CabScreens> screens_;
    std::vector<std::vector<std::int32_t>> state_nodes_; // a DISPLAY's StateNodes, by display
    std::vector<float> blink_clock_;                     // by display (RTS display +72)
public:
    // The wipers running (the FreeAnimations' surfaces) where they are drawn
    // now, in shape coordinates: some of their corners (the raindrops'
    // wiping; a blade at rest on the glass wipes nothing): their triangles'
    // corners, three a triangle, in the same order from frame to frame.
    void wiper_points(std::vector<glm::vec3>& out) const;

private:
    // The .sd's AnimNodes a DLL runs (FreeAnimation: the wipers): while its
    // flag 0 is set they go round their key frames, AnimTime a round; let
    // go, a RETURNS one finishes the round back to frame 0, another stops
    // where it is (a pneumatic wiper with its cock shut: the ВЛ80Т's, the
    // ЧС7's; testers 2026-11-21: the wipers sounded and stood still).
    struct FreeAnim {
        std::vector<std::int32_t> nodes;
        std::vector<float> last; // each node's last key frame
        float time = 1.0f;
        bool returns = false;
        bool continuous = false; // CONTINUOUS: rounds while running; else to its end and back
        bool loop = false;       // LOOP: a CONTINUOUS one starts again at frame 0; else it goes back (ts.h's "goes down")
        bool down = false;       // going back towards frame 0
        float phase = 0.0f;      // 0..1 of a round
        bool moving = false;
    };
    std::vector<FreeAnim> free_anims_;
    mutable std::vector<std::uint8_t> free_anim_running_; // as the DLL left them (adjustments)
};

} // namespace openrts::viewer
