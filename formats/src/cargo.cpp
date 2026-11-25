#include <openrts/formats/cargo.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "cargo_internal.hpp"
#include "rts_text.hpp"
#include "text_tokenizer.hpp"
#include "text_tree.hpp"

namespace openrts::formats {

namespace {

using detail::lower_ascii;
using detail::TextNode;

[[nodiscard]] bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// `*` any run, `?` one character; both lower case.
[[nodiscard]] bool wildcard(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// A value as RTS reads numbers with units (50t, 5.5t).
[[nodiscard]] float quantity(const TextNode* node, std::size_t index, float fallback = 0.0f) {
    if (!node || index >= node->values.size()) return fallback;
    return static_cast<float>(detail::rts_quantity(lower_ascii(node->values[index])));
}

[[nodiscard]] float number(const TextNode* node, std::size_t index, float fallback = 0.0f) {
    if (!node || index >= node->values.size()) return fallback;
    return static_cast<float>(detail::rts_atof(node->values[index]));
}

[[nodiscard]] std::string text(const TextNode* node, std::size_t index = 0) {
    return node && index < node->values.size() ? node->values[index] : std::string{};
}

[[nodiscard]] CargoBox box_of(const TextNode* node, std::size_t first = 0) {
    CargoBox box;
    for (std::size_t k = 0; k < 3; ++k) {
        const float a = number(node, first + k), b = number(node, first + 3 + k);
        box.low[k] = (std::min)(a, b);
        box.high[k] = (std::max)(a, b);
    }
    return box;
}

// Every LoadingType of a block (its own children only).
[[nodiscard]] std::uint32_t loading_of(const TextNode& node) {
    std::uint32_t bits = 0;
    for (const auto& child : node.children) {
        if (child.name != "loadingtype") continue;
        for (const auto& value : child.values) bits |= cargo_loading_bit(value);
    }
    return bits;
}

// A tree of the whole file (UTF-16 or UTF-8).
[[nodiscard]] bool tree_of(std::string_view bytes, TextNode& root, std::string* error) {
    std::string decode_error;
    const auto decoded = detail::decode_text(bytes, decode_error);
    if (!decoded) {
        if (error) *error = decode_error;
        return false;
    }
    detail::read_text_tree(*decoded, root);
    return true;
}

// Every node named `name` at any depth, in order.
void collect(const TextNode& node, std::string_view name, std::vector<const TextNode*>& out) {
    for (const auto& child : node.children) {
        if (child.name == name) {
            out.push_back(&child);
        } else {
            collect(child, name, out);
        }
    }
}

[[nodiscard]] std::uint32_t bits_of(const TextNode* node) {
    // A bitmask as written (1, 3, 0x3).
    return node && !node->values.empty() ? static_cast<std::uint32_t>(std::strtoul(node->values[0].c_str(), nullptr, 0)) : 0u;
}

[[nodiscard]] CargoLoadingAnimation read_animation(const TextNode& node) {
    CargoLoadingAnimation animation;
    if (const auto* bits = node.child("loadingunloading")) animation.loading_unloading = bits_of(bits);
    animation.side = lower_ascii(text(node.child("loadingside")));
    animation.areas = bits_of(node.child("forareas"));
    animation.intakes = bits_of(node.child("forintakepoints"));
    if (const auto* nodes = node.child("nodes")) {
        // ( n name ... ): the count as written, then the names.
        for (std::size_t i = 1; i < nodes->values.size(); ++i) animation.nodes.push_back(nodes->values[i]);
        if (nodes->values.size() == 1 && !detail::parse_double(nodes->values[0])) animation.nodes.push_back(nodes->values[0]);
    }
    if (const auto* bits = node.child("forloadingtype")) {
        for (const auto& value : bits->values) animation.loading |= cargo_loading_bit(value);
    }
    if (const auto* frame = node.child("animframe")) animation.frame = static_cast<int>(number(frame, 0, -1.0f));
    if (const auto* time = node.child("animtime"); time && number(time, 0) > 0.0f) animation.time = number(time, 0);
    return animation;
}

[[nodiscard]] WagonCargoDescription read_wagon(const TextNode& node) {
    WagonCargoDescription wagon;
    wagon.name = text(node.child("name"));
    if (const auto* names = node.child("wagonname")) {
        for (const auto& child : names->children) {
            for (const auto& value : child.values) {
                if (child.name == "pattern") wagon.patterns.push_back(lower_ascii(value));
                if (child.name == "filename") wagon.filenames.push_back(lower_ascii(value));
            }
        }
    }
    wagon.loading = loading_of(node);
    if (const auto* intakes = node.child("intakepoints")) {
        for (const auto& child : intakes->children) {
            if (child.name == "intakepoint") {
                wagon.intakes.push_back({{number(&child, 0), number(&child, 1), number(&child, 2)}, number(&child, 3)});
            } else if (child.name == "dumppoint") {
                wagon.dumps.push_back({{number(&child, 0), number(&child, 1), number(&child, 2)}, number(&child, 3)});
            }
        }
    }
    if (const auto* areas = node.child("loadingareas")) {
        for (const auto& child : areas->children) {
            if (child.name != "loadingarea") continue;
            CargoLoadingArea area;
            area.box = box_of(child.child("boundingbox"));
            area.parent_node = text(child.child("parentnode"));
            if (const auto* pivot = child.child("areapivot")) area.pivot = std::array<float, 3>{number(pivot, 0), number(pivot, 1), number(pivot, 2)};
            area.dump_side = lower_ascii(text(child.child("dumpfromside")));
            area.loading = loading_of(child);
            if (const auto* invisible = child.child("invisible")) area.invisible = invisible->values.empty() || number(invisible, 0) != 0.0f;
            for (const auto& sub : child.children) {
                if (sub.name != "loadinganimation") continue;
                auto animation = read_animation(sub);
                if (!animation.areas) animation.areas = 1u << wagon.areas.size();
                wagon.animations.push_back(std::move(animation));
            }
            wagon.areas.push_back(std::move(area));
        }
    }
    for (const auto& child : node.children) {
        if (child.name == "loadinganimation") wagon.animations.push_back(read_animation(child));
    }
    wagon.capacity = number(node.child("capacity"), 0);
    wagon.maximum_weight = number(node.child("maximumweight"), 0);
    wagon.dump_speed = number(node.child("dumpspeed"), 0);
    if (const auto* velocity = node.child("dumpmaxvelocity")) wagon.dump_max_velocity = number(velocity, 0);
    if (const auto* bounding = node.child("useboundingboxasarea")) wagon.use_bounding_box = bounding->values.empty() || number(bounding, 0) != 0.0f;
    return wagon;
}

[[nodiscard]] std::string file_stem_lower(std::string_view name) {
    auto lower = lower_ascii(name);
    if (const auto slash = lower.find_last_of("/\\"); slash != std::string::npos) lower.erase(0, slash + 1);
    return lower;
}

} // namespace

std::uint32_t cargo_loading_bit(std::string_view name) {
    struct Entry {
        std::string_view name;
        std::uint32_t bit;
    };
    static constexpr Entry entries[] = {{"pile", cargo_pile},   {"liquid", cargo_liquid}, {"container", cargo_container},
                                        {"stack", cargo_stack}, {"gas", cargo_gas},       {"box", cargo_box},
                                        {"energy", cargo_energy}, {"dump", cargo_dump},   {"fridge", cargo_fridge}};
    for (const auto& entry : entries) {
        if (same(entry.name, name)) return entry.bit;
    }
    return 0;
}

CargoKinds::CargoKinds() {
    // rts1.exe 0x45B930 (codes), 0x50CD6C (densities), 0x50D1F0 (piles).
    // Diesel has no density there (a tank of it weighed nothing); openRTS
    // gives it 840 kg/m³.
    const auto add = [&](std::uint16_t code, const char* id, const char* name, float density, float pile, std::uint32_t loading) {
        kinds_.push_back({.code = code,
                          .id = id,
                          .name = name,
                          .density = density,
                          .pile = pile,
                          .loading = loading,
                          .texture = std::string("RTSCargo_") + id});
    };
    add(1, "diesel", "Diesel", 840.0f, 1.0f, cargo_liquid);
    add(2, "gasoline", "Gasoline", 700.0f, 1.0f, cargo_liquid);
    add(3, "alcohol", "Alcohol", 790.0f, 1.0f, cargo_liquid);
    add(4, "gas", "Gas", 1150.0f, 1.0f, cargo_gas);
    add(5, "electricity", "Electricity", 0.0f, 1.0f, cargo_energy);
    add(8, "sand", "Sand", 1500.0f, 0.7f, cargo_pile);
    add(9, "water", "Water", 1000.0f, 1.0f, cargo_liquid);
    add(10, "coal", "Coal", 1500.0f, 0.5f, cargo_pile);
    add(50, "gravel", "Gravel", 1750.0f, 0.3f, cargo_pile);
    add(51, "grain", "Grain", 770.0f, 0.4f, cargo_pile);
    add(52, "slate", "Slate", 1450.0f, 0.6f, cargo_pile);
    add(53, "ore", "Ore", 1200.0f, 0.2f, cargo_pile);
    add(54, "iron_ore", "Iron ore", 3400.0f, 0.2f, cargo_pile);
    add(55, "fertilizer", "Mineral fertilizer", 1300.0f, 0.25f, cargo_pile);
    add(56, "cement", "Cement", 1510.0f, 0.1f, cargo_pile);
    add(57, "metal", "Metal", 3400.0f, 1.0f, cargo_stack);
    add(58, "oil", "Oil", 850.0f, 1.0f, cargo_liquid);
    add(59, "chemical", "Chemicals", 1840.0f, 1.0f, cargo_liquid);
    add(60, "mazut", "Mazut", 910.0f, 1.0f, cargo_liquid);
    add(300, "mail", "Mail", 0.0f, 1.0f, cargo_box);
    add(301, "food", "Food", 0.0f, 1.0f, cargo_box | cargo_fridge);
    add(302, "luggage", "Luggage", 0.0f, 1.0f, cargo_box);
    add(303, "message", "Message", 0.0f, 1.0f, cargo_box);
    add(304, "container", "Container", 0.0f, 1.0f, cargo_container);
}

const CargoKind* CargoKinds::find(std::uint16_t code) const {
    for (const auto& kind : kinds_) {
        if (kind.code == code) return &kind;
    }
    return nullptr;
}

const CargoKind* CargoKinds::find(std::string_view id) const {
    for (const auto& kind : kinds_) {
        if (same(kind.id, id)) return &kind;
    }
    return nullptr;
}

std::size_t CargoKinds::add_own(std::string_view bytes, std::string* error) {
    TextNode root;
    if (!tree_of(bytes, root, error)) return 0;
    std::vector<const TextNode*> cargos;
    collect(root, "cargo", cargos);
    std::size_t added = 0;
    std::uint16_t next = 1000;
    for (const auto& kind : kinds_) {
        if (kind.code >= next) next = static_cast<std::uint16_t>(kind.code + 1);
    }
    for (const auto* node : cargos) {
        const auto id = text(node->child("id"));
        if (id.empty() || find(id)) continue;
        CargoKind kind;
        kind.code = next++;
        kind.id = id;
        kind.name = text(node->child("name"));
        if (kind.name.empty()) kind.name = id;
        kind.russian_name = text(node->child("russianname"));
        kind.density = number(node->child("density"), 0);
        kind.pile = number(node->child("pile"), 0, 0.5f);
        kind.loading = loading_of(*node);
        if (!kind.loading) kind.loading = cargo_pile;
        kind.texture = text(node->child("texture"));
        if (kind.texture.empty()) kind.texture = "oRTSCargo_" + lower_ascii(id);
        if (const auto dot = kind.texture.rfind('.'); dot != std::string::npos) kind.texture.erase(dot);
        kind.own = true;
        kinds_.push_back(std::move(kind));
        ++added;
    }
    return added;
}

CargoKinds load_cargo_kinds(const core::Vfs& vfs) {
    CargoKinds kinds;
    if (const auto path = core::VirtualPath::parse("TRAINS/TRAINSET/DEFAULT/openRTSCargo.dat")) {
        if (const auto bytes = vfs.read(*path)) kinds.add_own(*bytes);
    }
    return kinds;
}

bool WagonCargoDescription::matches(std::string_view file_name) const {
    const auto file = file_stem_lower(file_name);
    for (const auto& filename : filenames) {
        if (filename == file) return true;
    }
    for (const auto& pattern : patterns) {
        if (wildcard(pattern, file)) return true;
    }
    return false;
}

const WagonCargoDescription* CargoDescriptions::find(std::string_view file_name) const {
    for (const auto& wagon : wagons) {
        if (wagon.matches(file_name)) return &wagon;
    }
    return nullptr;
}

void parse_cargo_descriptions(std::string_view bytes, CargoDescriptions& into) {
    TextNode root;
    std::string error;
    if (!tree_of(bytes, root, &error)) {
        if (into.error.empty()) into.error = error;
        return;
    }
    std::vector<const TextNode*> wagons;
    collect(root, "wagontype", wagons);
    for (const auto* node : wagons) into.wagons.push_back(read_wagon(*node));
}

CargoDescriptions load_cargo_descriptions(const core::Vfs& vfs) {
    CargoDescriptions result;
    for (const char* name : {"TRAINS/TRAINSET/DEFAULT/RTSCargo.dat", "TRAINS/TRAINSET/DEFAULT/RTSCargoCustom.dat"}) {
        const auto path = core::VirtualPath::parse(name);
        if (!path) continue;
        if (const auto bytes = vfs.read(*path)) parse_cargo_descriptions(*bytes, result);
    }
    return result;
}

std::optional<WagonCargoDescription> parse_wagon_cargo_description(std::string_view wag_bytes) {
    TextNode root;
    if (!tree_of(wag_bytes, root, nullptr)) return std::nullopt;
    std::vector<const TextNode*> found;
    collect(root, "wagoncargodescription", found);
    if (found.empty()) return std::nullopt;
    return read_wagon(*found.front());
}

std::array<float, 3> CargoObject::extent() const noexcept {
    if (size) return {size->high[0] - size->low[0], size->high[1] - size->low[1], size->high[2] - size->low[2]};
    const float side = volume > 0.0f ? std::cbrt(volume) : 1.0f;
    return {side, side, side};
}

void parse_cargo_objects(std::string_view bytes, std::vector<CargoObject>& into, std::string* error) {
    TextNode root;
    if (!tree_of(bytes, root, error)) return;
    std::vector<const TextNode*> objects;
    collect(root, "cargoobject", objects);
    for (const auto* node : objects) {
        CargoObject object;
        object.id = text(node->child("id"));
        if (object.id.empty()) continue;
        object.name = text(node->child("name"));
        if (object.name.empty()) object.name = object.id;
        object.description = text(node->child("description"));
        object.loading = loading_of(*node);
        object.cargo = lower_ascii(text(node->child("cargotype")));
        object.shape = text(node->child("shape"));
        if (const auto* size = node->child("size"); size && size->values.size() >= 6) object.size = box_of(size);
        object.volume = number(node->child("volume"), 0);
        object.stackable = static_cast<std::uint32_t>((std::max)(0.0f, number(node->child("stackable"), 0)));
        object.mass = quantity(node->child("mass"), 0);
        object.mass_maximum = quantity(node->child("massmaximum"), 0);
        const auto same_id = std::find_if(into.begin(), into.end(), [&](const CargoObject& o) { return same(o.id, object.id); });
        if (same_id != into.end()) {
            *same_id = std::move(object);
        } else {
            into.push_back(std::move(object));
        }
    }
}

std::vector<CargoObject> load_cargo_objects(const core::Vfs& vfs, const std::optional<core::VirtualPath>& route) {
    std::vector<CargoObject> result;
    if (const auto path = core::VirtualPath::parse("TRAINS/OBJECTS/RTSObjects.dat")) {
        if (const auto bytes = vfs.read(*path)) parse_cargo_objects(*bytes, result);
    }
    if (route) {
        if (const auto path = route->resolve(std::string_view("RTSObjects.dat"))) {
            if (const auto bytes = vfs.read(*path)) parse_cargo_objects(*bytes, result);
        }
    }
    return result;
}

std::uint32_t pickup_loading(std::uint16_t type) {
    switch (type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 6:
    case 13:
        return cargo_pile;
    case 5:
    case 7:
    case 16:
        return cargo_liquid | cargo_gas;
    case 8:
        return cargo_box | cargo_stack;
    case 11:
        return cargo_energy;
    case 12:
        return cargo_container | cargo_stack | cargo_box;
    case 14:
        return cargo_pile | cargo_stack;
    case 15:
        return cargo_stack | cargo_box | cargo_fridge;
    default:
        return 0;
    }
}

const char* pickup_type_name(std::uint16_t type) {
    switch (type) {
    case 1: return "Grain loader";
    case 2: return "Coal loader";
    case 3: return "Gravel loader";
    case 4: return "Sand loader";
    case 5: return "Water rack";
    case 6: return "Coal fuel loader";
    case 7: return "Diesel rack";
    case 8: return "Covered car platform";
    case 10: return "Sand box";
    case 11: return "Power socket";
    case 12: return "Loading crane";
    case 13: return "Bulk unloading trestle";
    case 14: return "Bulk crane";
    case 15: return "Covered car platform";
    case 16: return "Liquid loading rack";
    default: return "Pickup";
    }
}

namespace detail {

PickupDescription read_pickup_description(const TextNode& node) {
    PickupDescription pickup;
    if (const auto* type = node.child("pickuptype")) {
        pickup.type = static_cast<std::uint16_t>((std::max)(0.0f, number(type, 0)));
        pickup.type_value = number(type, 1);
    }
    if (const auto* speed = node.child("speedrange")) {
        pickup.speed_min = number(speed, 0);
        pickup.speed_max = number(speed, 1);
    }
    if (const auto* capacity = node.child("pickupcapacity")) {
        pickup.capacity = number(capacity, 0);
        pickup.speed = number(capacity, 1);
    }
    pickup.cargo = lower_ascii(text(node.child("cargotype")));
    pickup.length = number(node.child("pickuplength"), 0);
    pickup.margin = number(node.child("placementmargin"), 0);
    if (const auto* points = node.child("pickuppoints"); points && !points->values.empty()) {
        // ( n x y z ... ): the count as written, the triples after it.
        const auto count = static_cast<std::size_t>((std::max)(0.0f, number(points, 0)));
        for (std::size_t i = 0; i < count && 1 + i * 3 + 2 < points->values.size(); ++i) {
            pickup.points.push_back({number(points, 1 + i * 3), number(points, 2 + i * 3), number(points, 3 + i * 3)});
        }
    }
    if (const auto* area = node.child("pickuparea"); area && area->values.size() >= 6) pickup.area = box_of(area);
    if (const auto* table = node.child("cargotable")) {
        for (const auto& item : table->children) {
            if (item.name != "item" || item.values.size() < 3) continue;
            PickupDescription::Item entry;
            entry.loading = cargo_loading_bit(item.values[0]);
            entry.cargo = lower_ascii(item.values[1]);
            entry.object = item.values[2];
            entry.count = item.values.size() > 3 ? static_cast<int>(number(&item, 3)) : -1;
            entry.mass = item.values.size() > 4 ? quantity(&item, 4) : -1.0f;
            // A bulk item names no object (its third value repeats the kind).
            if (entry.loading & (cargo_pile | cargo_liquid | cargo_gas)) entry.object.clear();
            pickup.table.push_back(std::move(entry));
        }
    }
    pickup.crane_type = static_cast<int>(number(node.child("pickupcranetype"), 0));
    pickup.crane_oriented = static_cast<int>(number(node.child("pickupcraneoriented"), 0));
    if (const auto* nodes = node.child("pickupcraneshapenodes")) {
        PickupDescription::CraneNodes crane;
        crane.bridge = text(nodes->child("bridge"));
        crane.cabin = text(nodes->child("cabin"));
        crane.winch = text(nodes->child("winch"));
        crane.hook = text(nodes->child("hook"));
        crane.hook_height = number(nodes->child("hookinitialheight"), 0);
        pickup.crane_nodes = std::move(crane);
    }
    return pickup;
}

} // namespace detail

} // namespace openrts::formats
