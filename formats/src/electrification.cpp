#include <openrts/formats/electrification.hpp>

#include <cctype>
#include <cstdlib>
#include <optional>

#include "text_tree.hpp"

namespace openrts::formats {
namespace {

using detail::TextNode;

[[nodiscard]] std::uint32_t uint_value(const TextNode* node, const std::uint32_t fallback = 0) {
    if (!node || node->values.empty()) return fallback;
    return static_cast<std::uint32_t>(std::strtoul(node->values[0].c_str(), nullptr, 10));
}
[[nodiscard]] int int_value(const TextNode* node, const int fallback = 0) {
    if (!node || node->values.empty()) return fallback;
    return std::atoi(node->values[0].c_str());
}
[[nodiscard]] float float_value(const TextNode* node, const float fallback = 0.0f) {
    if (!node || node->values.empty()) return fallback;
    return static_cast<float>(node->number(0, fallback));
}
[[nodiscard]] std::uint32_t hex_value(const std::string& text) {
    return static_cast<std::uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
}
[[nodiscard]] std::string text_value(const TextNode* node) {
    if (!node || node->values.empty()) return {};
    std::string out = node->values[0];
    for (std::size_t i = 1; i < node->values.size(); ++i) out += " " + node->values[i];
    return out;
}
// Position ( tile_x tile_z x y z ).
[[nodiscard]] std::optional<RoutePosition> position_value(const TextNode* node) {
    if (!node || node->values.size() < 5) return std::nullopt;
    return RoutePosition{.tile_x = std::atoi(node->values[0].c_str()),
                         .tile_z = std::atoi(node->values[1].c_str()),
                         .x = node->number(2),
                         .y = node->number(3),
                         .z = node->number(4)};
}

[[nodiscard]] std::optional<std::string> decode(const std::string_view bytes, std::string& error, TextNode& root) {
    auto text = detail::decode_text(bytes, error);
    if (!text) return std::nullopt;
    if (!detail::read_text_tree(*text, root)) {
        error = "nothing to read";
        return std::nullopt;
    }
    return text;
}

// The block `name` at the top or one level down (some files wrap the lists).
[[nodiscard]] const TextNode* find(const TextNode& root, const std::string_view name) {
    if (const auto* node = root.child(name)) return node;
    for (const auto& child : root.children) {
        if (const auto* node = child.child(name)) return node;
    }
    return nullptr;
}

} // namespace

ElectricTypes parse_electric_types(const std::string_view bytes) {
    ElectricTypes out;
    TextNode root;
    if (!decode(bytes, out.error, root)) return out;
    if (const auto* list = find(root, "electricsupplytypes")) {
        for (const auto& node : list->children) {
            if (node.name != "electricsupplytype") continue;
            ElectricSupplyType t;
            t.id = uint_value(node.child("id"));
            t.name = text_value(node.child("name"));
            if (const auto* c = node.child("colour"); c && !c->values.empty()) t.colour = hex_value(c->values[0]);
            t.voltage = float_value(node.child("voltage"));
            t.frequency = float_value(node.child("frequency"));
            t.voltage_drop = float_value(node.child("voltagedrop"));
            // RTS keeps a type only with an id and a voltage (0x491668).
            if (t.id != 0 && t.voltage != 0.0f) out.supply_types.push_back(std::move(t));
        }
    }
    if (const auto* list = find(root, "catenarytypes")) {
        for (const auto& node : list->children) {
            if (node.name != "catenarytype") continue;
            CatenaryType t;
            t.id = uint_value(node.child("id"));
            t.name = text_value(node.child("name"));
            if (const auto* c = node.child("colour"); c && !c->values.empty()) t.colour = hex_value(c->values[0]);
            if (const auto* p = node.child("position")) {
                t.position[0] = static_cast<float>(p->number(0));
                t.position[1] = static_cast<float>(p->number(1));
            }
            t.type = uint_value(node.child("type"));
            t.num_wires = uint_value(node.child("numwires"));
            t.burn_q = float_value(node.child("burnq"));
            if (t.id != 0) out.catenary_types.push_back(std::move(t));
        }
    }
    if (const auto* list = find(root, "electricsubstations")) {
        for (const auto& node : list->children) {
            if (node.name != "electricsubstation") continue;
            ElectricSubstation s;
            s.id = uint_value(node.child("id"));
            s.name = text_value(node.child("name"));
            s.electro_type = uint_value(node.child("electrotype"));
            if (const auto p = position_value(node.child("position"))) s.position = *p;
            s.phases = uint_value(node.child("phasenum"));
            s.power = float_value(node.child("power"));
            s.voltage = float_value(node.child("voltage"));
            if (const auto* d = node.child("powerdecline")) {
                s.power_decline[0] = static_cast<float>(d->number(0));
                s.power_decline[1] = static_cast<float>(d->number(1));
            }
            if (s.id != 0) out.substations.push_back(std::move(s));
        }
    }
    return out;
}

ElectroDatabase parse_electro_database(const std::string_view bytes) {
    ElectroDatabase out;
    TextNode root;
    if (!decode(bytes, out.error, root)) return out;
    if (const auto* list = find(root, "electrosections")) {
        for (const auto& node : list->children) {
            if (node.name != "electrosection") continue;
            ElectroSection s;
            s.id = uint_value(node.child("id"));
            s.name = text_value(node.child("name"));
            s.basic_catenary_type = uint_value(node.child("basiccatenarytype"));
            if (const auto* sources = node.child("powersources")) {
                for (const auto& p : sources->children) {
                    if (p.name != "powersource") continue;
                    ElectroPowerSource source;
                    source.substation = uint_value(p.child("substation"));
                    // RTS stops at a source without a substation (0x491DC8).
                    if (source.substation == 0) break;
                    source.phase = uint_value(p.child("phase"));
                    source.active = uint_value(p.child("active"));
                    source.feeder = position_value(p.child("feederpos"));
                    source.use_substation_position = int_value(p.child("usesubstationpos")) != 0;
                    source.feeder_q = float_value(p.child("feederq"), 100.0f);
                    s.sources.push_back(std::move(source));
                }
            }
            if (const auto* current = node.child("currentsource")) {
                const int index = int_value(current, -1);
                if (index >= 0 && index < static_cast<int>(s.sources.size())) {
                    s.current_source = index;
                    // As RTS: the chosen source switched in, every source enabled.
                    for (std::size_t j = 0; j < s.sources.size(); ++j) {
                        auto& a = s.sources[j].active;
                        a = (static_cast<int>(j) == index ? (a | 1u) : (a & ~1u)) | 2u;
                    }
                }
            }
            if (s.id != 0 && !s.sources.empty()) out.sections.push_back(std::move(s));
        }
    }
    if (const auto* list = find(root, "sectionelectrification")) {
        for (const auto& node : list->children) {
            if (node.name != "section" || node.values.empty()) continue;
            NodeElectrification n;
            n.node = static_cast<std::uint32_t>(std::strtoul(node.values[0].c_str(), nullptr, 10));
            if (n.node == 0) continue;
            if (const auto* e = node.child("electricity"); e && !e->values.empty()) {
                const auto count = static_cast<std::size_t>(std::strtoul(e->values[0].c_str(), nullptr, 10));
                for (std::size_t i = 0; i < count && 1 + i * 3 + 2 < e->values.size(); ++i) {
                    const auto at = 1 + i * 3;
                    n.electricity.push_back({.section = static_cast<std::uint32_t>(std::strtoul(e->values[at].c_str(), nullptr, 10)),
                                             .start = static_cast<float>(e->number(at + 1)),
                                             .flags = hex_value(e->values[at + 2])});
                }
            }
            if (const auto* c = node.child("catenary"); c && !c->values.empty()) {
                const auto count = static_cast<std::size_t>(std::strtoul(c->values[0].c_str(), nullptr, 10));
                for (std::size_t i = 0; i < count && 1 + i * 5 + 4 < c->values.size(); ++i) {
                    const auto at = 1 + i * 5;
                    n.catenary.push_back({.type = static_cast<std::uint32_t>(std::strtoul(c->values[at].c_str(), nullptr, 10)),
                                          .start = static_cast<float>(c->number(at + 1)),
                                          .length = static_cast<float>(c->number(at + 2)),
                                          .unknown = static_cast<float>(c->number(at + 3)),
                                          .flags = hex_value(c->values[at + 4])});
                }
            }
            if (!n.electricity.empty() || !n.catenary.empty()) out.nodes.push_back(std::move(n));
        }
    }
    return out;
}

namespace {

// `<route>/<RouteID><extension>`, else the only file of the folder with it.
[[nodiscard]] std::optional<core::VirtualPath> route_file(const core::Vfs& vfs, const core::VirtualPath& route,
                                                         const std::string& route_id, const std::string_view extension) {
    if (!route_id.empty()) {
        if (const auto candidate = route.resolve(route_id + std::string(extension)); candidate && vfs.exists(*candidate)) {
            return candidate;
        }
    }
    std::optional<core::VirtualPath> found;
    for (const auto& entry : vfs.list(route)) {
        if (entry.directory) continue;
        std::string name(entry.path.filename());
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name.size() > extension.size() && name.ends_with(extension)) {
            if (found) return std::nullopt; // more than one: none is the route's
            found = entry.path;
        }
    }
    return found;
}

} // namespace

RouteElectrification load_route_electrification(const core::Vfs& vfs, const core::VirtualPath& route) {
    RouteElectrification out;
    const auto definition = load_route_definition(vfs, route);
    out.electrified = definition.ok() && definition.electrified;
    const std::string id = definition.ok() ? definition.route_id : std::string{};
    std::string error;
    if (const auto file = route_file(vfs, route, id, ".electrotype")) {
        if (const auto bytes = vfs.read(*file, &error)) {
            out.types = parse_electric_types(*bytes);
            out.has_types = out.types.ok();
            if (!out.types.ok()) out.error = std::string(file->filename()) + ": " + out.types.error;
        } else {
            out.error = error;
        }
    }
    if (const auto file = route_file(vfs, route, id, ".electrodb")) {
        if (const auto bytes = vfs.read(*file, &error)) {
            out.database = parse_electro_database(*bytes);
            out.has_database = out.database.ok();
            if (!out.database.ok()) out.error = std::string(file->filename()) + ": " + out.database.error;
        } else {
            out.error = error;
        }
    }
    return out;
}

} // namespace openrts::formats
