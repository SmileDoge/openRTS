#include <openrts/formats/cargo_file.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "text_tokenizer.hpp"
#include "text_tree.hpp"

namespace openrts::formats {
namespace {

using detail::TextNode;

[[nodiscard]] const TextNode* find(const TextNode& node, const std::string_view name) {
    return node.child(name);
}

[[nodiscard]] std::vector<const TextNode*> all(const TextNode& node, const std::string_view name) {
    std::vector<const TextNode*> found;
    for (const auto& child : node.children) {
        if (child.name == name) found.push_back(&child);
    }
    return found;
}

[[nodiscard]] float number(const TextNode* node, const std::size_t index, const float fallback = 0.0f) {
    return node ? static_cast<float>(node->number(index, fallback)) : fallback;
}

[[nodiscard]] std::string text(const TextNode* node) {
    return node && !node->values.empty() ? node->values.front() : std::string{};
}

[[nodiscard]] bool wildcard(const std::string_view pattern, const std::string_view name) {
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || detail::equals_ignore_case(pattern.substr(p, 1), name.substr(n, 1)))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

[[nodiscard]] int literal_weight(const std::string_view pattern) {
    int weight = 0;
    for (const char c : pattern) {
        if (c != '*' && c != '?') ++weight;
    }
    return weight;
}

struct Match {
    int weight = -1;
    std::size_t index = 0;
    bool found = false;
};

void consider(Match& match, const int weight, const std::size_t index) {
    if (weight < 0) return;
    if (!match.found || weight > match.weight || (weight == match.weight && index >= match.index)) {
        match = Match{weight, index, true};
    }
}

void add_default(std::vector<CargoType>& cargoes, const std::string_view id, const float density) {
    if (std::any_of(cargoes.begin(), cargoes.end(), [&](const CargoType& c) { return detail::equals_ignore_case(c.id, id); })) {
        return;
    }
    CargoType cargo;
    cargo.id = std::string{id};
    cargo.name = std::string{id};
    cargo.density = density;
    cargoes.push_back(std::move(cargo));
}

void complete_catalog(CargoCatalog& catalog) {
    add_default(catalog.cargoes, "GRAVEL", 1600.0f);
    add_default(catalog.cargoes, "CEMENT", 1400.0f);
    add_default(catalog.cargoes, "IRON_ORE", 2500.0f);
    add_default(catalog.cargoes, "ORE", 2000.0f);
    add_default(catalog.cargoes, "SLATE", 1700.0f);
    add_default(catalog.cargoes, "OIL", 850.0f);
    add_default(catalog.cargoes, "DIESEL", 830.0f);
    add_default(catalog.cargoes, "GASOLINE", 740.0f);
    add_default(catalog.cargoes, "MAZUT", 950.0f);
    add_default(catalog.cargoes, "ALCOHOL", 790.0f);
    add_default(catalog.cargoes, "CHEMICAL", 1000.0f);
    add_default(catalog.cargoes, "WATER", 1000.0f);
    add_default(catalog.cargoes, "GAS", 500.0f);
    add_default(catalog.cargoes, "FOOD", 500.0f);
    add_default(catalog.cargoes, "MAIL", 200.0f);
    add_default(catalog.cargoes, "LUGGAGE", 200.0f);
    add_default(catalog.cargoes, "WOOD", 600.0f);
    add_default(catalog.cargoes, "TIMBER", 600.0f);
    add_default(catalog.cargoes, "STEEL", 1800.0f);
    add_default(catalog.cargoes, "METAL", 1800.0f);
    add_default(catalog.cargoes, "VEHICLES", 150.0f);
    add_default(catalog.cargoes, "CARS", 150.0f);
}

[[nodiscard]] CargoType read_cargo(const TextNode& node) {
    CargoType cargo;
    cargo.id = text(find(node, "id"));
    cargo.name = text(find(node, "name"));
    cargo.texture = text(find(node, "texture"));
    cargo.loading_type = text(find(node, "loadingtype"));
    cargo.density = number(find(node, "density"), 0, 0.0f);
    return cargo;
}

void read_wagon_name(const TextNode& node, CargoWagonType& wagon) {
    for (const auto& child : node.children) {
        if (child.name == "filename") {
            if (!child.values.empty()) wagon.filenames.push_back(child.values.front());
        } else if (child.name == "pattern") {
            if (!child.values.empty()) wagon.patterns.push_back(child.values.front());
        } else if (!child.values.empty()) {
            wagon.filenames.push_back(child.values.front());
        }
    }
}

[[nodiscard]] CargoWagonType read_wagon(const TextNode& node) {
    CargoWagonType wagon;
    wagon.name = text(find(node, "name"));
    for (const auto* name : all(node, "wagonname")) read_wagon_name(*name, wagon);
    for (const auto* type : all(node, "loadingtype")) {
        if (!type->values.empty()) wagon.loading_types.push_back(type->values.front());
    }
    if (const auto* areas = find(node, "loadingareas")) {
        for (const auto* area : all(*areas, "loadingarea")) {
            if (const auto* box = find(*area, "boundingbox"); box && box->values.size() >= 6) {
                std::array<float, 6> bounds{};
                for (std::size_t i = 0; i < 6; ++i) bounds[i] = static_cast<float>(box->number(i, 0.0));
                wagon.areas.push_back(bounds);
            }
        }
    }
    if (const auto* points = find(node, "intakepoints")) {
        for (const auto& child : points->children) {
            if (child.name != "intakepoint" && child.name != "dumppoint") continue;
            if (child.values.size() < 4) continue;
            CargoIntake intake;
            intake.dump = child.name == "dumppoint";
            for (std::size_t i = 0; i < 4; ++i) intake.values[i] = static_cast<float>(child.number(i, 0.0));
            wagon.intakes.push_back(intake);
        }
    }
    wagon.capacity = number(find(node, "capacity"), 0, 0.0f);
    wagon.maximum_weight = number(find(node, "maximumweight"), 0, 0.0f);
    wagon.dump_speed = number(find(node, "dumpspeed"), 0, 0.0f);
    wagon.dump_max_velocity = number(find(node, "dumpmaxvelocity"), 0, 0.0f);
    return wagon;
}

} // namespace

std::string_view cargo_mode(const std::string_view cargo_id) {
    using namespace std::string_view_literals;
    static constexpr std::array pile{"COAL"sv, "SAND"sv, "COKE"sv, "SLATE"sv, "GRAIN"sv,
                                     "GRAVEL"sv, "CEMENT"sv, "IRON_ORE"sv, "ORE"sv, "FERTILIZER"sv};
    static constexpr std::array liquid{"MAZUT"sv, "WATER"sv, "DIESEL"sv, "CHEMICAL"sv,
                                       "GASOLINE"sv, "OIL"sv, "ALCOHOL"sv};
    static constexpr std::array box{"FOOD"sv, "MAIL"sv, "LUGGAGE"sv};
    static constexpr std::array stack{"WOOD"sv, "STEEL"sv, "METAL"sv, "TIMBER"sv};
    static constexpr std::array vehicle{"CARS"sv, "VEHICLES"sv};

    for (const auto id : pile) {
        if (detail::equals_ignore_case(cargo_id, id)) return "PILE"sv;
    }
    for (const auto id : liquid) {
        if (detail::equals_ignore_case(cargo_id, id)) return "LIQUID"sv;
    }
    for (const auto id : box) {
        if (detail::equals_ignore_case(cargo_id, id)) return "BOX"sv;
    }
    for (const auto id : stack) {
        if (detail::equals_ignore_case(cargo_id, id)) return "STACK"sv;
    }
    for (const auto id : vehicle) {
        if (detail::equals_ignore_case(cargo_id, id)) return "VEHICLE"sv;
    }
    if (detail::equals_ignore_case(cargo_id, "GAS"sv)) return "GAS"sv;
    return {};
}

const CargoType* CargoCatalog::cargo(const std::string_view id) const {
    for (const auto& entry : cargoes) {
        if (detail::equals_ignore_case(entry.id, id)) return &entry;
    }
    return nullptr;
}

const CargoWagonType* CargoCatalog::wagon_for(const std::string_view file_name) const {
    Match match;
    for (std::size_t i = 0; i < wagons.size(); ++i) {
        const auto& wagon = wagons[i];
        for (const auto& name : wagon.filenames) {
            if (detail::equals_ignore_case(name, file_name)) consider(match, 100000, i);
        }
        for (const auto& pattern : wagon.patterns) {
            if (wildcard(pattern, file_name)) consider(match, literal_weight(pattern), i);
        }
    }
    return match.found ? &wagons[match.index] : nullptr;
}

bool cargo_accepts(const CargoWagonType& wagon, const CargoType& cargo) {
    std::string_view mode = cargo_mode(cargo.id);
    if (!cargo.loading_type.empty()) mode = cargo.loading_type;
    if (mode.empty()) return false;
    if (detail::equals_ignore_case(mode, "CONTAINER")) return false;
    for (const auto& type : wagon.loading_types) {
        if (detail::equals_ignore_case(type, mode)) return true;
    }
    return false;
}

float cargo_capacity(const CargoWagonType& wagon, const CargoType& cargo, const float width, const float height,
                     const float length) {
    const float volume = std::max(0.0f, width) * std::max(0.0f, height) * std::max(0.0f, length);
    if (!(volume > 0.0f)) return 0.0f;
    float box = 0.0f;
    for (const auto& area : wagon.areas) {
        box += std::max(0.0f, area[3] - area[0]) * std::max(0.0f, area[4] - area[1]) * std::max(0.0f, area[5] - area[2]);
    }
    const float hold = box > 0.0f ? box : std::max(0.0f, wagon.capacity) * volume;
    const float weight_limit = std::max(0.0f, wagon.maximum_weight) * volume;
    const float by_density = hold * std::max(0.0f, cargo.density);
    return weight_limit > 0.0f ? std::min(by_density, weight_limit) : by_density;
}

CargoCatalog parse_cargo_catalog(const std::string_view bytes) {
    CargoCatalog catalog;
    std::string decode_error;
    const auto text_value = detail::decode_text(bytes, decode_error);
    if (!text_value) {
        catalog.error = std::move(decode_error);
        return catalog;
    }
    TextNode root;
    if (!detail::read_text_tree(*text_value, root)) {
        catalog.error = "the cargo file has no blocks";
        return catalog;
    }
    if (const auto* types = find(root, "cargotypes")) {
        for (const auto* node : all(*types, "cargo")) catalog.cargoes.push_back(read_cargo(*node));
    }
    if (const auto* types = find(root, "wagontypes")) {
        for (const auto* node : all(*types, "wagontype")) catalog.wagons.push_back(read_wagon(*node));
    }
    complete_catalog(catalog);
    return catalog;
}

CargoCatalog load_cargo_catalog(const core::Vfs& vfs) {
    for (const auto name : {"TRAINS/TRAINSET/DEFAULT/RTScargoCustom.dat", "TRAINS/TRAINSET/DEFAULT/RTScargo.dat"}) {
        const auto path = core::VirtualPath::parse(name);
        if (!path || !vfs.exists(*path)) continue;
        std::string error;
        const auto bytes = vfs.read(*path, &error);
        if (!bytes) {
            CargoCatalog catalog;
            catalog.error = std::move(error);
            return catalog;
        }
        return parse_cargo_catalog(*bytes);
    }
    return {};
}

} // namespace openrts::formats
