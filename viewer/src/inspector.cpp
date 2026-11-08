#include "inspector.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>

#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>

#include "train_view.hpp"
#include "ui_text.hpp"
#include "viewer_state.hpp"

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

namespace fs = std::filesystem;

constexpr const char* default_mod = "openrts_edits";

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

[[nodiscard]] std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

[[nodiscard]] std::string file_name(const core::VirtualPath& path) { return std::string(path.filename()); }

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

[[nodiscard]] fs::path from_utf8(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

[[nodiscard]] const char* kind_name(const formats::WorldObjectKind kind) {
    using K = formats::WorldObjectKind;
    switch (kind) {
    case K::static_object: return tr("object");
    case K::track: return tr("track");
    case K::gantry: return tr("catenary mast");
    case K::collide_object: return tr("obstacle");
    case K::signal: return tr("signal");
    case K::speed_post: return tr("speed post");
    case K::transfer: return tr("ground texture");
    case K::forest: return tr("forest");
    case K::level_crossing: return tr("level crossing");
    case K::pickup: return tr("pickup");
    case K::car_spawner: return tr("road traffic");
    }
    return "?";
}

// MSTS's light materials (formats::ShapeVertexState).
[[nodiscard]] std::string light_name(const std::int32_t material) {
    switch (material) {
    case -1: return tr("vertex colour");
    case -5: return tr("normal");
    case -6: return tr("shiny 25");
    case -7: return tr("shiny 750");
    case -8: return "FullBright";
    case -9: return tr("circumference");
    case -11: return tr("half bright");
    case -12: return tr("dark shade");
    default: return std::to_string(material);
    }
}

[[nodiscard]] const char* role_name(const TrainView::Role role) {
    switch (role) {
    case TrainView::Role::driven: return tr("driven");
    case TrainView::Role::ai: return tr("test driver");
    case TrainView::Role::standing: return tr("standing");
    }
    return "?";
}

// Where the ray from `eye` along `direction` enters the box `bounds` of a
// shape placed by `transform` (the ray's parameter); `inside`: the eye is
// in it (then where it leaves).
struct Hit {
    float t{};
    bool inside = false;
};
[[nodiscard]] std::optional<Hit> ray_box(const glm::vec3& eye, const glm::vec3& direction, const glm::mat4& transform,
                                         const renderer::ModelBounds& bounds) {
    if (bounds.radius <= 0.0f) return std::nullopt;
    const glm::mat4 inverse = glm::inverse(transform);
    const glm::vec3 origin{inverse * glm::vec4{eye, 1.0f}};
    const glm::vec3 way{inverse * glm::vec4{direction, 0.0f}};
    float near_t = 0.0f, far_t = std::numeric_limits<float>::max();
    bool inside = true;
    for (int axis = 0; axis < 3; ++axis) {
        const float lo = bounds.min[axis], hi = bounds.max[axis];
        if (origin[axis] < lo || origin[axis] > hi) inside = false;
        if (std::abs(way[axis]) < 1e-9f) {
            if (origin[axis] < lo || origin[axis] > hi) return std::nullopt;
            continue;
        }
        float a = (lo - origin[axis]) / way[axis];
        float b = (hi - origin[axis]) / way[axis];
        if (a > b) std::swap(a, b);
        near_t = (std::max)(near_t, a);
        far_t = (std::min)(far_t, b);
        if (near_t > far_t) return std::nullopt;
    }
    if (far_t < 0.0f) return std::nullopt;
    return Hit{.t = inside ? far_t : near_t, .inside = inside};
}

// Adds `mod` to the installation's openrts.ini [Mods] (after the others,
// numbered when they are); false when it was there already.
bool list_mod(const fs::path& root, const std::string& mod) {
    const auto file = root / "openrts.ini";
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            std::ostringstream all;
            all << in.rdbuf();
            text = all.str();
        }
    }
    const bool crlf = text.find("\r\n") != std::string::npos;
    std::vector<std::string> lines;
    {
        std::string line;
        std::istringstream in(text);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    std::optional<std::size_t> section, last;
    int highest = 0;
    bool numbered = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (const auto comment = line.find(';'); comment != std::string::npos) line = trim(line.substr(0, comment));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            if (section && !last) last = section;
            if (lower(trim(line.substr(1, line.size() - 2))) == "mods") {
                section = i;
                last.reset();
            } else if (section) {
                break;
            }
            continue;
        }
        if (!section) continue;
        last = i;
        std::string value = line;
        if (const auto equals = line.find('='); equals != std::string::npos) {
            numbered = true;
            try {
                highest = (std::max)(highest, std::stoi(trim(line.substr(0, equals))));
            } catch (...) {
            }
            value = trim(line.substr(equals + 1));
        }
        if (lower(value) == lower(mod)) return false;
    }
    const std::string entry = numbered ? std::to_string(highest + 1) + " = " + mod : mod;
    if (section) {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>((last ? *last : *section) + 1), entry);
    } else {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.emplace_back();
        lines.emplace_back("[Mods]");
        lines.push_back(entry);
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const auto& line : lines) out << line << (crlf ? "\r\n" : "\n");
    return true;
}

} // namespace

Inspector::Inspector() {
    const auto mod = remembered("inspector_mod").value_or(default_mod);
    std::snprintf(mod_, sizeof mod_, "%s", mod.c_str());
}

void Inspector::select(const Selection& selection) noexcept {
    selection_ = selection;
    scroll_to_selection_ = true;
}

std::optional<Inspector::Resolved> Inspector::resolve(const InspectorWorld& world) const {
    if (selection_.kind == Selection::Kind::world && world.instances) {
        for (const auto& instance : *world.instances) {
            if (instance.uid != selection_.uid || instance.world_tile_x != selection_.tile_x || instance.world_tile_z != selection_.tile_z)
                continue;
            return Resolved{.shape = instance.source_path, .transform = instance.transform, .name = file_name(instance.source_path), .instance = &instance};
        }
    }
    if (selection_.kind == Selection::Kind::car && world.trains) {
        const auto cars = world.trains->inspected_cars(selection_.train);
        if (selection_.car < cars.size()) {
            const auto& car = cars[selection_.car];
            return Resolved{.shape = car.shape, .transform = car.transform.value_or(glm::mat4{1.0f}), .name = car.name};
        }
    }
    return std::nullopt;
}

void Inspector::pick(const glm::vec3& eye, const glm::vec3& direction, const InspectorWorld& world) {
    if (!world.library) return;
    std::optional<std::pair<Hit, Selection>> best;
    const auto consider = [&](const Hit& hit, const Selection& selection) {
        // A box the eye is in (a big building, a ground texture) only when
        // nothing else is in the way.
        if (!best || (best->first.inside && !hit.inside) || (best->first.inside == hit.inside && hit.t < best->first.t))
            best = std::pair{hit, selection};
    };
    if (world.instances) {
        for (const auto& instance : *world.instances) {
            if (instance.source_path.empty()) continue;
            auto* model = world.library->find(instance.source_path);
            if (!model) continue;
            if (const auto hit = ray_box(eye, direction, instance.transform, model->bounds())) {
                consider(*hit, {.kind = Selection::Kind::world, .tile_x = instance.world_tile_x, .tile_z = instance.world_tile_z, .uid = instance.uid});
            }
        }
    }
    if (world.trains) {
        for (const auto& train : world.trains->summaries()) {
            const auto cars = world.trains->inspected_cars(train.id);
            for (const auto& car : cars) {
                if (!car.transform || car.shape.empty()) continue;
                auto* model = world.library->find(car.shape);
                if (!model) continue;
                if (const auto hit = ray_box(eye, direction, *car.transform, model->bounds())) {
                    consider(*hit, {.kind = Selection::Kind::car, .train = train.id, .car = car.index});
                }
            }
        }
    }
    if (best) select(best->second);
}

void Inspector::add_selection_lines(const InspectorWorld& world, std::vector<renderer::WideLine>& lines) const {
    if (!open_ || !world.library) return;
    const auto chosen = resolve(world);
    if (!chosen) return;
    auto* model = world.library->find(chosen->shape);
    if (!model) return;
    const auto bounds = model->bounds();
    if (bounds.radius <= 0.0f) return;
    glm::vec3 corners[8];
    for (int k = 0; k < 8; ++k) {
        const glm::vec3 local{(k & 1) ? bounds.max.x : bounds.min.x, (k & 2) ? bounds.max.y : bounds.min.y,
                              (k & 4) ? bounds.max.z : bounds.min.z};
        corners[k] = glm::vec3{chosen->transform * glm::vec4{local, 1.0f}};
    }
    static constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : edges) lines.push_back({corners[edge[0]], corners[edge[1]], 0xff20e0ffu, 2.5f});
}

std::string Inspector::report(const InspectorWorld& world) const {
    std::string text = open_ ? "open" : "shut";
    const auto chosen = resolve(world);
    if (!chosen) return text + ", nothing chosen";
    text += ", " + std::string(selection_.kind == Selection::Kind::car ? "car " : "object ") + chosen->name + " (" + chosen->shape.str() + ")";
    if (chosen->instance) text += std::string(", ") + formats::to_string(chosen->instance->kind) + " UiD " + std::to_string(chosen->instance->uid);
    if (const auto* model = world.library ? world.library->find(chosen->shape) : nullptr) {
        std::size_t fullbright = 0;
        for (const auto& primitive : model->primitives())
            if (primitive.light_material == -8) ++fullbright;
        text += ", " + std::to_string(model->primitives().size()) + " primitive states (" + std::to_string(fullbright) + " FullBright)";
    }
    return text;
}

void Inspector::fly_to(const InspectorWorld& world) {
    const auto chosen = resolve(world);
    if (!chosen || !world.library) return;
    const auto* model = world.library->find(chosen->shape);
    const auto bounds = model ? model->bounds() : renderer::ModelBounds{};
    const glm::vec3 centre{chosen->transform * glm::vec4{bounds.center, 1.0f}};
    const float scale = glm::length(glm::vec3{chosen->transform[0]});
    const float reach = (std::max)(bounds.radius * scale, 2.0f) * 1.6f + 4.0f;
    fly_to_ = std::pair{centre + glm::normalize(glm::vec3{1.0f, 0.55f, 1.0f}) * reach, centre};
}

formats::ShapeOverride& Inspector::edited(const InspectorWorld& world, const core::VirtualPath& shape) {
    if (edited_shape_ != shape.key()) {
        edited_shape_ = shape.key();
        edited_ = world.vfs ? renderer::find_shape_override(*world.vfs, shape) : formats::ShapeOverride{};
        dirty_ = false;
        status_.clear();
    }
    return edited_;
}

void Inspector::apply_edit(const InspectorWorld& world, const core::VirtualPath& shape) {
    renderer::hold_shape_override(shape, edited_);
    if (world.library) world.library->notify_changes({renderer::shape_override_path(shape)});
}

void Inspector::save_edit(const InspectorWorld& world, const core::VirtualPath& shape) {
    std::string mod = trim(mod_);
    if (mod.empty()) mod = default_mod;
    if (mod.find_first_of("/\\:") != std::string::npos || mod == "." || mod == "..") {
        status_ = tr("The mod's name is a folder name, without slashes.");
        return;
    }
    const auto virtual_file = renderer::shape_override_path(shape).str();
    const fs::path relative = fs::path("MODS_OPENRTS") / from_utf8(mod) / from_utf8(virtual_file);
    const fs::path file = world.root / relative;
    std::error_code error;
    if (edited_.empty()) {
        fs::remove(file, error);
    } else {
        fs::create_directories(file.parent_path(), error);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << formats::write_shape_override(edited_);
        if (!out) {
            status_ = trf("Could not write %s", utf8(relative).c_str());
            return;
        }
    }
    const bool added = list_mod(world.root, mod);
    remember("inspector_mod", mod);
    dirty_ = false;
    status_ = edited_.empty() ? trf("Removed: %s", utf8(relative).c_str()) : trf("Saved: %s", utf8(relative).c_str());
    if (added) status_ += std::string("\n") + tr("The mod was added to openrts.ini: it is mounted from the next start (here the change is on already).");
}

void Inspector::draw(const InspectorWorld& world) {
#ifdef OPENRTS_HAS_IMGUI
    if (!open_) return;
    draw_tree(world);
    if (open_) draw_properties(world);
#else
    (void)world;
#endif
}

void Inspector::draw_tree(const InspectorWorld& world) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui::SetNextWindowPos({16.0f, 96.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({380.0f, 560.0f}, ImGuiCond_FirstUseEver);
    const std::string title = std::string(tr("Inspector: the map's objects")) + "###inspector_tree";
    if (!ImGui::Begin(title.c_str(), &open_)) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox(tr("Choose with a click in the world"), &picking_);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##inspector_filter", tr("Filter by name"), filter_, sizeof filter_);
    const std::string filter = lower(trim(filter_));
    const auto shown = [&](const std::string& name) { return filter.empty() || lower(name).find(filter) != std::string::npos; };
    ImGui::Separator();
    ImGui::BeginChild("##inspector_tree_list");
    // The world: the tiles around the camera, their objects by kind and name.
    if (world.instances) {
        std::map<std::pair<std::int32_t, std::int32_t>, std::vector<const renderer::StreamedInstance*>> tiles;
        std::size_t count = 0;
        for (const auto& instance : *world.instances) {
            if (instance.source_path.empty() || !shown(file_name(instance.source_path))) continue;
            tiles[{instance.world_tile_x, instance.world_tile_z}].push_back(&instance);
            ++count;
        }
        const bool chosen_world = selection_.kind == Selection::Kind::world;
        if (scroll_to_selection_ && chosen_world) ImGui::SetNextItemOpen(true);
        const std::string label = trf("World (%zu objects)", count) + "###inspector_world";
        if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (auto& [tile, objects] : tiles) {
                std::sort(objects.begin(), objects.end(), [](const auto* a, const auto* b) {
                    if (a->kind != b->kind) return a->kind < b->kind;
                    return a->source_path.key() < b->source_path.key();
                });
                const bool here = chosen_world && selection_.tile_x == tile.first && selection_.tile_z == tile.second;
                if (scroll_to_selection_ && here) ImGui::SetNextItemOpen(true);
                const std::string tile_label = trf("Tile %d, %d (%zu)", tile.first, tile.second, objects.size()) + "###tile"
                    + std::to_string(tile.first) + "_" + std::to_string(tile.second);
                if (!ImGui::TreeNode(tile_label.c_str())) continue;
                for (const auto* object : objects) {
                    const bool selected = here && selection_.uid == object->uid;
                    const std::string item = file_name(object->source_path) + "  [" + kind_name(object->kind) + "]##obj"
                        + std::to_string(tile.first) + "_" + std::to_string(tile.second) + "_" + std::to_string(object->uid);
                    if (ImGui::Selectable(item.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        selection_ = {.kind = Selection::Kind::world, .tile_x = tile.first, .tile_z = tile.second, .uid = object->uid};
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fly_to(world);
                    }
                    if (selected && scroll_to_selection_) ImGui::SetScrollHereY(0.4f);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
    }
    // The trains, their cars in order.
    if (world.trains) {
        const auto trains = world.trains->summaries();
        const bool chosen_car = selection_.kind == Selection::Kind::car;
        if (scroll_to_selection_ && chosen_car) ImGui::SetNextItemOpen(true);
        const std::string label = trf("Trains (%zu)", trains.size()) + "###inspector_trains";
        if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& train : trains) {
                const bool here = chosen_car && selection_.train == train.id;
                if (scroll_to_selection_ && here) ImGui::SetNextItemOpen(true);
                const std::string train_label = train.name + "  (" + role_name(train.role) + ", " + trf("%zu cars", train.cars) + ")###train"
                    + std::to_string(train.id);
                if (!ImGui::TreeNode(train_label.c_str())) continue;
                const auto cars = world.trains->inspected_cars(train.id);
                for (const auto& car : cars) {
                    if (!shown(car.name)) continue;
                    const bool selected = here && selection_.car == car.index;
                    const std::string item = std::to_string(car.index + 1) + ". " + car.name + "##car" + std::to_string(train.id) + "_"
                        + std::to_string(car.index);
                    if (ImGui::Selectable(item.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        selection_ = {.kind = Selection::Kind::car, .train = train.id, .car = car.index};
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fly_to(world);
                    }
                    if (selected && scroll_to_selection_) ImGui::SetScrollHereY(0.4f);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
    }
    scroll_to_selection_ = false;
    ImGui::EndChild();
    ImGui::End();
#else
    (void)world;
#endif
}

void Inspector::draw_properties(const InspectorWorld& world) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui::SetNextWindowPos({412.0f, 96.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({520.0f, 560.0f}, ImGuiCond_FirstUseEver);
    const std::string title = std::string(tr("Inspector: properties")) + "###inspector_properties";
    if (!ImGui::Begin(title.c_str(), &open_)) {
        ImGui::End();
        return;
    }
    const auto chosen = resolve(world);
    if (!chosen) {
        ImGui::TextWrapped("%s", tr("Nothing is chosen: choose an object in the tree, or click it in the world."));
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(chosen->name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Go to"))) fly_to(world);
    const auto* model = world.library ? world.library->find(chosen->shape) : nullptr;
    if (ImGui::BeginTable("##inspector_props", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(tr("Property"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthStretch);
        const auto row = [](const char* name, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", value.c_str());
        };
        char text[256];
        if (const auto* instance = chosen->instance) {
            row(tr("Kind"), kind_name(instance->kind));
            row("UiD", std::to_string(instance->uid));
            std::snprintf(text, sizeof text, "%d, %d", instance->world_tile_x, instance->world_tile_z);
            row(tr("World tile"), text);
            const auto& p = instance->position;
            std::snprintf(text, sizeof text, "%s %d, %d: %.2f, %.2f, %.2f", tr("tile"), p.tile_x, p.tile_z, p.x, p.y, p.z);
            row(tr("Position"), text);
            std::snprintf(text, sizeof text, "0x%X / 0x%X", instance->collide_flags, instance->static_flags);
            row(tr("Collide / static flags"), text);
        } else if (selection_.kind == Selection::Kind::car && world.trains) {
            const auto cars = world.trains->inspected_cars(selection_.train);
            if (selection_.car < cars.size()) {
                const auto& car = cars[selection_.car];
                std::string train_name;
                for (const auto& train : world.trains->summaries())
                    if (train.id == selection_.train) train_name = train.name;
                std::snprintf(text, sizeof text, "%s, %zu / %zu", train_name.c_str(), car.index + 1, cars.size());
                row(tr("Train, car"), text);
                row(tr("Kind"), car.engine ? tr("locomotive (.eng)") : tr("car (.wag)"));
                row(tr("File"), car.file);
                std::snprintf(text, sizeof text, "%.1f t", car.mass / 1000.0f);
                row(tr("Mass"), text);
                std::snprintf(text, sizeof text, "%.2f x %.2f x %.2f m", car.width, car.height, car.length);
                row(tr("Size (W x H x L)"), text);
                if (!car.library.empty()) row(tr("DLL"), car.library);
            }
        }
        row(tr("Shape"), chosen->shape.str());
        if (world.vfs) {
            if (const auto origin = world.vfs->origin(chosen->shape)) {
                row(tr("Shape from"), origin->source_name + "  (" + utf8(origin->location) + ")");
            }
            if (const auto origin = world.vfs->origin(renderer::shape_override_path(chosen->shape))) {
                row(tr("Override from"), origin->source_name);
            }
        }
        if (model) {
            const auto b = model->bounds();
            std::snprintf(text, sizeof text, "%.2f x %.2f x %.2f m", b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z);
            row(tr("Box (X x Y x Z)"), text);
            std::string levels = std::to_string((std::max)(model->level_distances().size(), std::size_t{1}));
            if (!model->level_distances().empty()) {
                levels += ":";
                for (const auto distance : model->level_distances()) levels += " " + std::to_string(static_cast<int>(distance));
                levels += " m";
            }
            row(tr("Distance levels"), levels);
        } else {
            row(tr("Model"), tr("not loaded"));
        }
        ImGui::EndTable();
    }
    draw_materials(world, chosen->shape);
    ImGui::End();
#else
    (void)world;
#endif
}

void Inspector::draw_materials(const InspectorWorld& world, const core::VirtualPath& shape) {
#ifdef OPENRTS_HAS_IMGUI
    if (!ImGui::CollapsingHeader(tr("Materials"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto* model = world.library ? world.library->find(shape) : nullptr;
    if (!model) {
        ImGui::TextDisabled("%s", tr("The shape is not loaded."));
        return;
    }
    auto& over = edited(world, shape);
    ImGui::TextWrapped("%s", tr("FullBright materials ignore the sun and the lamps; ticked, one is lit as an ordinary material (only this shape, kept in a mod)."));
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    const float height = (std::min)(ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(model->primitives().size()) + 1.5f), 280.0f);
    if (ImGui::BeginTable("##inspector_materials", 5, flags, {0.0f, height})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn(tr("Texture"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Shader"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Light"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn(tr("Lit as ordinary"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();
        for (const auto& primitive : model->primitives()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", primitive.index);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(primitive.image.empty() ? "-" : primitive.image.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(primitive.shader.c_str());
            ImGui::TableSetColumnIndex(3);
            const auto* entry = over.find(primitive.index);
            const auto now = entry && entry->light_material ? *entry->light_material : primitive.shape_light_material;
            std::string light = light_name(now);
            if (now != primitive.shape_light_material) light += " (" + light_name(primitive.shape_light_material) + ")";
            ImGui::TextUnformatted(light.c_str());
            ImGui::TableSetColumnIndex(4);
            if (primitive.shape_light_material != -8 && !entry) {
                ImGui::TextDisabled("-");
                continue;
            }
            bool lit = entry && entry->light_material && *entry->light_material == -5;
            const std::string id = "##lit" + std::to_string(primitive.index);
            if (ImGui::Checkbox(id.c_str(), &lit)) {
                over.set_light_material(primitive.index, primitive.image, lit ? std::optional<std::int32_t>{-5} : std::nullopt);
                dirty_ = true;
                apply_edit(world, shape);
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextUnformatted(tr("Mod (MODS_OPENRTS):"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##inspector_mod", mod_, sizeof mod_);
    ImGui::BeginDisabled(!dirty_);
    if (ImGui::Button(tr("Save to the mod"))) save_edit(world, shape);
    ImGui::SameLine();
    if (ImGui::Button(tr("Undo the changes"))) {
        renderer::hold_shape_override(shape, std::nullopt);
        if (world.library) world.library->notify_changes({renderer::shape_override_path(shape)});
        edited_shape_.clear();
    }
    ImGui::EndDisabled();
    if (dirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.3f, 1.0f}, "%s", tr("not saved"));
    }
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
#else
    (void)world;
    (void)shape;
#endif
}

} // namespace openrts::viewer
