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
};

} // namespace openrts::viewer
