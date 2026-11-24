#pragma once

// The inspector (F12): a tree of what is on the map around the camera (the
// world's objects by tile, the trains by car) and the chosen one's
// properties in a window of its own: where it is, its shape and the file it
// comes from, its materials. A material drawn FullBright can be lit as an
// ordinary one; that goes into a mod as the shape's `<shape>.openrts`
// (formats/shape_override.hpp), the shape itself left as it is.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/material.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/shape_override.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/renderer/wide_lines.hpp>

namespace openrts::renderer {
class ModelLibrary;
}

namespace openrts::viewer {

class TrainView;

// What the scene shows the inspector, each frame.
struct InspectorWorld {
    const std::vector<renderer::StreamedInstance>* instances = nullptr;
    renderer::ModelLibrary* library = nullptr;
    const core::Vfs* vfs = nullptr;
    formats::RoutePosition origin{};
    TrainView* trains = nullptr;
    std::filesystem::path root; // the installation (MODS_OPENRTS, openrts.ini)
};

class Inspector {
public:
    Inspector();

    void toggle() noexcept { open_ = !open_; }
    void set_open(const bool open) noexcept { open_ = open; }
    [[nodiscard]] bool open() const noexcept { return open_; }
    // Clicks in the scene choose an object instead of working it.
    [[nodiscard]] bool picking() const noexcept { return open_ && picking_; }
    // A click in the scene: the nearest object (world or train car) the ray
    // from `eye` along `direction` meets, by its shape's box.
    void pick(const glm::vec3& eye, const glm::vec3& direction, const InspectorWorld& world);
    // The two windows (ImGui).
    void draw(const InspectorWorld& world);
    // The chosen object's box, for the scene's helper lines.
    void add_selection_lines(const InspectorWorld& world, std::vector<renderer::WideLine>& lines) const;
    // The choice as text: open or not, what is chosen, its shape and
    // primitive states (--capture-inspect).
    [[nodiscard]] std::string report(const InspectorWorld& world) const;
    // The world object's lamp the lamps' editor holds on (to set it by day,
    // paused too): its tile, UiD and lamp.
    struct HeldLamp {
        std::int32_t tile_x{}, tile_z{};
        std::uint32_t uid{};
        std::size_t index{};
    };
    [[nodiscard]] std::optional<HeldLamp> held_lamp() const;
    // The lamps' gizmo in the world (inspector_lamps.cpp): a click on a
    // lamp's marker chooses it, on a handle of the chosen one starts
    // dragging it (true: the click was the gizmo's); while the button is
    // held the drag follows the ray; let go, it ends. `hover`: the handle
    // under the mouse, drawn brighter.
    bool begin_drag(const glm::vec3& eye, const glm::vec3& direction, const InspectorWorld& world);
    void drag(const glm::vec3& eye, const glm::vec3& direction, const InspectorWorld& world);
    void end_drag() noexcept { drag_.reset(); }
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    void hover(const std::optional<glm::vec3>& eye, const std::optional<glm::vec3>& direction);
    // "Go to": where the camera goes and what it looks at, taken once.
    [[nodiscard]] std::optional<std::pair<glm::vec3, glm::vec3>> take_fly_to() { return std::exchange(fly_to_, std::nullopt); }

private:
    struct Selection {
        enum class Kind : std::uint8_t { none, world, car } kind = Kind::none;
        // World: the object's tile and UiD (unique in its tile).
        std::int32_t tile_x{}, tile_z{};
        std::uint32_t uid{};
        // Car: its train and place in it.
        std::uint64_t train{};
        std::size_t car{};
    };
    // The chosen object as it is now: its shape, its transform, a name.
    struct Resolved {
        core::VirtualPath shape;
        glm::mat4 transform{1.0f};
        std::string name;
        const renderer::StreamedInstance* instance = nullptr;
    };

    [[nodiscard]] std::optional<Resolved> resolve(const InspectorWorld& world) const;
    void select(const Selection& selection) noexcept;
    void draw_tree(const InspectorWorld& world);
    void draw_properties(const InspectorWorld& world);
    void draw_materials(const InspectorWorld& world, const core::VirtualPath& shape);
    // openRTS's material of a part (PBR stage 4): as drawn, where each field
    // came from, the inspector's numbers (the shape's material file).
    void draw_pbr_material(const InspectorWorld& world, const core::VirtualPath& shape);
    formats::ShapeMaterialFile& material_file(const InspectorWorld& world, const core::VirtualPath& shape);
    void apply_material(const InspectorWorld& world, const core::VirtualPath& shape);
    void save_material(const InspectorWorld& world, const core::VirtualPath& shape);
    void fly_to(const InspectorWorld& world);
    // The shape's override being edited: read once when a shape is chosen.
    formats::ShapeOverride& edited(const InspectorWorld& world, const core::VirtualPath& shape);
    void apply_edit(const InspectorWorld& world, const core::VirtualPath& shape);
    void save_edit(const InspectorWorld& world, const core::VirtualPath& shape);
    // The lamps (inspector_lamps.cpp): a car's lights, a shape's lamps.
    [[nodiscard]] std::string lamp_key() const;
    void draw_lamps(const InspectorWorld& world, const core::VirtualPath& shape);
    void draw_car_lamps(const InspectorWorld& world);
    void draw_shape_lamps(const InspectorWorld& world, const core::VirtualPath& shape);
    void add_lamp_lines(const InspectorWorld& world, const core::VirtualPath& shape, const glm::mat4& transform,
                        std::vector<renderer::WideLine>& lines) const;

    bool open_ = false;
    bool picking_ = true;
    char filter_[96]{};
    char mod_[64]{};
    Selection selection_;
    bool scroll_to_selection_ = false;
    std::optional<std::pair<glm::vec3, glm::vec3>> fly_to_;
    // The override being edited, of `edited_shape_`; `dirty_`: not saved.
    std::string edited_shape_;
    formats::ShapeOverride edited_;
    bool dirty_ = false;
    std::string status_;
    // The shape's material entries being edited (PBR stage 4), of
    // `material_shape_`; the part chosen; `material_dirty_`: not saved.
    std::string material_shape_;
    formats::ShapeMaterialFile material_;
    std::uint32_t material_part_ = 0;
    bool material_dirty_ = false;
    std::string material_status_;
    // The lamps' editor: the chosen lamp and State, of the selection
    // `lamp_of_`; held on; `lamp_dirty_`: not saved.
    std::string lamp_of_;
    int lamp_ = 0, lamp_state_ = 0;
    bool lamp_hold_ = true;
    bool lamp_dirty_ = false;
    std::string lamp_status_;
    // The gizmo's handles as last drawn (add_lamp_lines): a lamp's marker
    // (choose it), the chosen one's arrows (move along the shape's x, y, z),
    // its middle (move in the view's plane), its beam's end (aim it), its
    // cone's rim (open or close it; a sphere's or a glow's size).
    struct Handle {
        enum class Kind : std::uint8_t { lamp, axis, free, aim, rim } kind = Kind::lamp;
        glm::vec3 at{};
        int axis = 0;        // Kind::axis: 0 x, 1 y, 2 z
        std::size_t lamp{};  // Kind::lamp
    };
    mutable std::vector<Handle> handles_;
    int hovered_ = -1;
    glm::vec3 eye_{};
    // The chosen lamp as drawn: `frame` its numbers' space to the scene
    // (a car's shape, turned for a light of the last car; a shape's), where
    // it is and the way it shines in the scene, its cone (tangents of its
    // half angles) or its size (a sphere, a glow), how far it is drawn.
    struct LampGeometry {
        glm::mat4 frame{1.0f};
        glm::mat4 place{1.0f};
        glm::vec3 at{}, beam{0.0f, 0.0f, 1.0f};
        bool cone = false;
        float tangent_x{}, tangent_y{}, reach{}, size{};
    };
    [[nodiscard]] std::optional<LampGeometry> lamp_geometry(const InspectorWorld& world, const core::VirtualPath& shape,
                                                            const glm::mat4& transform) const;
    struct Drag {
        Handle handle;
        glm::vec3 start{};      // where the ray met it
        glm::vec3 view{};       // the ray at the start: the drag plane's normal
        glm::mat4 frame{1.0f};  // the lamp's numbers' space to the scene
        glm::vec3 lamp_at{};    // the lamp, the scene
        glm::vec3 beam{};       // its way, the scene (unit)
        float tangent{};        // its cone's at the start
        float size{};           // its size at the start (a sphere, a glow)
        float along{};          // Kind::axis: where on its axis the ray met it
        std::vector<formats::LightDefinition> car;  // as it was (a car)
        std::vector<formats::ShapeLight> shape;     // as it was (a shape)
    };
    std::optional<Drag> drag_;
    void apply_shape_lamps(const InspectorWorld& world, const core::VirtualPath& shape, std::vector<formats::ShapeLight> lamps);
};

} // namespace openrts::viewer
