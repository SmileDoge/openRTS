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

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    // The box around each node's own surfaces, by matrix index (lower > upper: none).
    void node_bounds(std::size_t nodes, std::vector<glm::vec3>& lower, std::vector<glm::vec3>& upper) const;

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
    // nullptr when the engine has no cab shape with a description.
    [[nodiscard]] static std::unique_ptr<CabView> create(const core::Vfs& vfs, const formats::WagonDefinition& engine,
                                                         const core::VirtualPath& file);

    [[nodiscard]] const core::VirtualPath& shape() const noexcept { return shape_; }
    // Where the cab shape sits in the car: EngExtended Cabin ( "x.s" 1 )
    // turns it round (the cab at the car's rear end, VL10k section b).
    [[nodiscard]] glm::mat4 placement() const noexcept;
    [[nodiscard]] bool turned() const noexcept { return turned_; }
    [[nodiscard]] const formats::CabDescription& description() const noexcept { return cab_; }
    // Finds the nodes once the cab model has loaded.
    void bind(const formats::ShapeModel& rig);
    [[nodiscard]] bool bound() const noexcept { return bound_; }
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
    };
    void adjustments(const dllhost::CarOutput* state, std::vector<renderer::NodeAdjustment>& out,
                     std::uint32_t aspect = 0, const Blend* blend = nullptr) const;
    // The displays' blinking (DisplayFade's second value) over `elapsed` s.
    void step_displays(float elapsed);
    // The driver's seat (Pos1 of the first chair), in the engine's shape coordinates.
    [[nodiscard]] glm::vec3 eye() const noexcept { return eye_; }
    // The SCREEN displays for this frame, in cab shape coordinates.
    void screens(const dllhost::CarOutput* state, float elapsed, std::vector<ScreenBatch>& out) {
        if (!screens_) return;
        // A screen display blinks too (DisplayFade, RTS 0x525DB0).
        std::vector<bool> off(cab_.displays.size(), false);
        for (std::size_t i = 0; i < off.size() && i < blink_clock_.size(); ++i)
            off[i] = cab_.displays[i].blink > 0.0f && blink_clock_[i] > cab_.displays[i].blink;
        screens_->build(state, elapsed, out, &off);
    }
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
    void toggle_door(std::size_t door);
    // Sends a door open or shut (it moves over its AnimTime): the train's
    // passenger doors (InteriorDoors with a GlobalID).
    void set_door(std::size_t door, bool open) {
        if (door < doors_.size()) doors_[door].target = open;
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
    core::VirtualPath shape_;
    bool turned_ = false;
    formats::CabDescription cab_;
    audio::InteriorAcoustics acoustics_;
    std::vector<Bound> switches_; // by position in cab_.switches
    std::vector<Bound> displays_;
    std::vector<std::int32_t> signal_lights_; // LocomotiveSignalization SignalLights' nodes
    std::vector<std::int32_t> lamp_nodes_;    // LightSources' nodes, by the lamp's Name
    glm::vec3 eye_{0.0f, 2.5f, 0.0f};
    bool bound_ = false;
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
};

} // namespace openrts::viewer
