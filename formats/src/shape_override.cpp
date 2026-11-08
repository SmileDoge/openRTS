#include <openrts/formats/shape_override.hpp>

#include <algorithm>
#include <cstdio>

#include <openrts/formats/shape_binary.hpp>

#include "text_tree.hpp"

namespace openrts::formats {
namespace {

using detail::TextNode;

[[nodiscard]] std::string file_name_lower(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
    return detail::lower_ascii(path);
}

} // namespace

const ShapeOverride::PrimState* ShapeOverride::find(const std::uint32_t index) const noexcept {
    for (const auto& state : prim_states) {
        if (state.index == index) return &state;
    }
    return nullptr;
}

void ShapeOverride::set_light_material(const std::uint32_t index, std::string image, const std::optional<std::int32_t> light_material) {
    auto it = std::find_if(prim_states.begin(), prim_states.end(), [&](const PrimState& state) { return state.index == index; });
    if (!light_material) {
        if (it != prim_states.end()) prim_states.erase(it);
        return;
    }
    if (it == prim_states.end()) {
        prim_states.push_back({.index = index});
        it = std::prev(prim_states.end());
    }
    it->image = file_name_lower(image);
    it->light_material = light_material;
    std::sort(prim_states.begin(), prim_states.end(), [](const PrimState& a, const PrimState& b) { return a.index < b.index; });
}

ShapeOverride parse_shape_override(std::string_view text) {
    ShapeOverride result;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    TextNode root;
    if (!detail::read_text_tree(text, root)) return result;
    const auto* top = root.child("openrtsshapeoverride");
    if (!top) return result;
    for (const auto& node : top->children) {
        if (node.name != "primstate" || node.values.empty()) continue;
        const double index = node.number(0, -1.0);
        if (index < 0.0) continue;
        ShapeOverride::PrimState state{.index = static_cast<std::uint32_t>(index)};
        if (const auto* image = node.child("image"); image && !image->values.empty()) state.image = file_name_lower(image->values[0]);
        if (const auto* light = node.child("lightmaterial"); light && !light->values.empty()) {
            state.light_material = static_cast<std::int32_t>(light->number(0, -5.0));
        }
        if (state.light_material) result.prim_states.push_back(std::move(state));
    }
    return result;
}

std::string write_shape_override(const ShapeOverride& data) {
    std::string out = "OpenRTSShapeOverride ( 1\n";
    for (const auto& state : data.prim_states) {
        char line[96];
        std::snprintf(line, sizeof line, "\tPrimState ( %u", state.index);
        out += line;
        if (!state.image.empty()) out += " Image ( \"" + state.image + "\" )";
        if (state.light_material) {
            std::snprintf(line, sizeof line, " LightMaterial ( %d )", *state.light_material);
            out += line;
        }
        out += " )\n";
    }
    out += ")\n";
    return out;
}

std::string primitive_image(const ShapeModel& model, const std::uint32_t index) {
    if (index >= model.primitive_states.size()) return {};
    const auto& state = model.primitive_states[index];
    if (state.texture_count == 0 || state.texture_index >= model.textures.size()) return {};
    const auto image = model.textures[state.texture_index].image_index;
    if (image >= model.images.size()) return {};
    return file_name_lower(model.images[image].generic_string());
}

std::vector<std::int32_t> apply_shape_override(ShapeModel& model, const ShapeOverride& data) {
    std::vector<std::int32_t> before(model.primitive_states.size(), -5);
    for (std::size_t i = 0; i < model.primitive_states.size(); ++i) {
        const auto vertex = model.primitive_states[i].vertex_state_index;
        if (vertex >= 0 && static_cast<std::size_t>(vertex) < model.vertex_states.size())
            before[i] = model.vertex_states[static_cast<std::size_t>(vertex)].light_material_index;
    }
    for (const auto& change : data.prim_states) {
        if (!change.light_material || change.index >= model.primitive_states.size()) continue;
        if (!change.image.empty() && primitive_image(model, change.index) != change.image) continue;
        auto& state = model.primitive_states[change.index];
        if (state.vertex_state_index < 0 || static_cast<std::size_t>(state.vertex_state_index) >= model.vertex_states.size()) continue;
        auto own = model.vertex_states[static_cast<std::size_t>(state.vertex_state_index)];
        own.light_material_index = *change.light_material;
        model.vertex_states.push_back(own);
        state.vertex_state_index = static_cast<std::int32_t>(model.vertex_states.size() - 1);
    }
    return before;
}

} // namespace openrts::formats
