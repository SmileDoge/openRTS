#include <openrts/formats/route_grass.hpp>

#include <algorithm>
#include <cctype>

#include "text_tokenizer.hpp"
#include "text_tree.hpp"

namespace openrts::formats {

namespace {

std::string lower_utf8(std::string text) {
    // ASCII lowered; Cyrillic left as written (names are matched as the
    // route writes them, lowered ASCII on both sides).
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string unquote(std::string text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') return text.substr(1, text.size() - 2);
    return text;
}

} // namespace

RouteGrass parse_route_grass(const std::string_view text) {
    RouteGrass out;
    detail::TextNode root;
    if (!detail::read_text_tree(text, root)) {
        out.error = "no blocks";
        return out;
    }
    for (const auto& node : root.children) {
        if (node.name == "nograssinseason") {
            out.no_grass_seasons.insert(static_cast<int>(node.number(0, -1.0)));
        } else if (node.name == "grassdistancefromtrack") {
            out.from_track = static_cast<float>(node.number(0, 0.0));
        } else if (node.name == "grassdistancefromroad") {
            out.from_road = static_cast<float>(node.number(0, 0.0));
        } else if (node.name == "terraintextures") {
            for (const auto& ground : node.children) {
                if (ground.name != "terraintexture") continue;
                const auto* file = ground.child("texturefile");
                if (!file || file->values.empty()) continue;
                GroundGrass grass;
                if (const auto* height = ground.child("grassheight")) grass.height = static_cast<float>(height->number(0, 1.0));
                const auto* uses = ground.child("usegrasstextures");
                float density = 0.0f;
                if (uses) {
                    for (const auto& use : uses->children) {
                        if (use.name != "grasstexture") continue;
                        if (const auto* spread = use.child("texturespread")) density += static_cast<float>(spread->number(0, 0.0));
                    }
                }
                if (!uses || density <= 0.0f) continue; // no grass on it
                grass.density = density;
                out.grounds[lower_utf8(unquote(file->values[0]))] = grass;
            }
        }
    }
    return out;
}

std::optional<RouteGrass> load_route_grass(const core::Vfs& vfs, const core::VirtualPath& route) {
    const auto file = route.resolve(std::string(route.filename()) + ".rtst");
    if (!file || !vfs.exists(*file)) return std::nullopt;
    const auto bytes = vfs.read(*file);
    if (!bytes) return std::nullopt;
    std::string error;
    const auto text = detail::decode_text(*bytes, error);
    if (!text) return std::nullopt;
    auto grass = parse_route_grass(*text);
    if (!grass.ok()) return std::nullopt;
    return grass;
}

} // namespace openrts::formats
