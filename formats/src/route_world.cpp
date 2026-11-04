#include <openrts/formats/route_world.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>

#include "text_tokenizer.hpp"

namespace openrts::formats {
namespace {

using detail::equals_ignore_case;
using detail::parse_double;
using detail::parse_integer;
using detail::read_text;
using detail::Tokenizer;

// Reads `RouteStart ( tile_x tile_z x z )` after its opening bracket.
[[nodiscard]] std::optional<RoutePosition> read_route_start(Tokenizer& tokens) {
    const auto tile_x = tokens.next();
    const auto tile_z = tokens.next();
    const auto x = tokens.next();
    const auto z = tokens.next();
    if (!tile_x || !tile_z || !x || !z) return std::nullopt;

    const auto parsed_tile_x = parse_integer<std::int32_t>(*tile_x);
    const auto parsed_tile_z = parse_integer<std::int32_t>(*tile_z);
    const auto parsed_x = parse_double(*x);
    const auto parsed_z = parse_double(*z);
    if (!parsed_tile_x || !parsed_tile_z || !parsed_x || !parsed_z) return std::nullopt;

    return RoutePosition{
        .tile_x = *parsed_tile_x,
        .tile_z = *parsed_tile_z,
        .x = *parsed_x,
        .y = 0.0,
        .z = *parsed_z,
    };
}

// Reads `Environment ( SpringClear ( file.env ) ... )` after its opening bracket.
void read_environments(Tokenizer& tokens, std::vector<RouteEnvironment>& environments) {
    while (const auto token = tokens.next()) {
        if (*token == ")") return;
        if (*token == "(") {
            tokens.skip_block();
            continue;
        }
        if (!tokens.open_block()) return;
        if (const auto file = tokens.next(); file && *file != ")") {
            environments.push_back({.condition = *token, .file = *file});
        }
        tokens.skip_block();
    }
}

// Reads one field of a `Static` block; the field's opening bracket is consumed.
void read_static_field(Tokenizer& tokens,
                       const std::string& field,
                       WorldObjectPlacement& object,
                       bool& has_shape,
                       bool& has_position) {
    if (equals_ignore_case(field, "UiD")) {
        if (const auto value = tokens.next()) {
            if (const auto uid = parse_integer<std::uint32_t>(*value)) object.uid = *uid;
        }
    } else if (equals_ignore_case(field, "FileName")) {
        if (const auto value = tokens.next()) {
            object.shape_file = *value;
            has_shape = true;
        }
    } else if (equals_ignore_case(field, "Position")) {
        const auto x = tokens.next();
        const auto y = tokens.next();
        const auto z = tokens.next();
        if (!x || !y || !z) return;
        const auto px = parse_double(*x);
        const auto py = parse_double(*y);
        const auto pz = parse_double(*z);
        if (!px || !py || !pz) return;
        object.position.x = *px;
        object.position.y = *py;
        object.position.z = *pz;
        has_position = true;
    } else if (equals_ignore_case(field, "CollideFlags")) {
        if (const auto value = tokens.next()) {
            if (const auto flags = parse_integer<std::uint32_t>(*value)) object.collide_flags = *flags;
        }
    } else if (equals_ignore_case(field, "StaticFlags")) {
        if (const auto value = tokens.next()) object.static_flags = static_cast<std::uint32_t>(std::strtoul(value->c_str(), nullptr, 16));
    } else if (object.kind == WorldObjectKind::transfer && (equals_ignore_case(field, "Width") || equals_ignore_case(field, "Height"))) {
        if (const auto value = tokens.next()) {
            if (const auto parsed = parse_double(*value)) {
                (equals_ignore_case(field, "Width") ? object.transfer_width : object.transfer_height) = static_cast<float>(*parsed);
            }
        }
    } else if (equals_ignore_case(field, "TrItemId")) {
        // ( database item ): 0 the track database, else the road one.
        const auto database = tokens.next();
        const auto item = tokens.next();
        if (database && item) {
            const auto db = parse_integer<std::uint32_t>(*database);
            const auto id = parse_integer<std::uint32_t>(*item);
            if (db && id) (*db == 0 && object.kind != WorldObjectKind::car_spawner ? object.rail_items : object.road_items).push_back(*id);
        }
    } else if (object.kind == WorldObjectKind::forest) {
        const auto pair = [&](std::array<float, 2>& out) {
            for (auto& value : out) {
                if (const auto token = tokens.next()) {
                    if (const auto parsed = parse_double(*token)) value = static_cast<float>(*parsed);
                }
            }
        };
        if (equals_ignore_case(field, "TreeTexture")) {
            if (const auto value = tokens.next()) object.tree_texture = *value;
        } else if (equals_ignore_case(field, "Area")) {
            pair(object.forest_area);
        } else if (equals_ignore_case(field, "ScaleRange")) {
            pair(object.scale_range);
        } else if (equals_ignore_case(field, "Population")) {
            if (const auto value = tokens.next()) object.population = static_cast<std::uint32_t>(std::strtoul(value->c_str(), nullptr, 10));
        } else if (equals_ignore_case(field, "TreeSize")) {
            pair(object.tree_size);
        }
    } else if (object.kind == WorldObjectKind::level_crossing) {
        const auto floats = [&](float* out, const std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                if (const auto token = tokens.next()) {
                    if (const auto parsed = parse_double(*token)) out[i] = static_cast<float>(*parsed);
                }
            }
        };
        if (equals_ignore_case(field, "LevelCrParameters")) {
            floats(object.crossing_parameters.data(), 2);
        } else if (equals_ignore_case(field, "CrashProbability")) {
            floats(&object.crash_probability, 1);
        } else if (equals_ignore_case(field, "LevelCrData")) {
            if (const auto value = tokens.next()) object.crossing_flags = static_cast<std::uint32_t>(std::strtoul(value->c_str(), nullptr, 16));
            if (const auto value = tokens.next()) object.crossing_tracks = static_cast<std::uint32_t>(std::strtoul(value->c_str(), nullptr, 10));
        } else if (equals_ignore_case(field, "LevelCrTiming")) {
            floats(object.crossing_timing.data(), 3);
        }
    } else if (object.kind == WorldObjectKind::car_spawner) {
        if (equals_ignore_case(field, "CarFrequency") || equals_ignore_case(field, "CarAvSpeed")) {
            if (const auto value = tokens.next()) {
                if (const auto parsed = parse_double(*value)) {
                    (equals_ignore_case(field, "CarFrequency") ? object.car_frequency : object.car_average_speed) = static_cast<float>(*parsed);
                }
            }
        }
    } else if (equals_ignore_case(field, "SignalSubObj")) {
        if (const auto value = tokens.next()) object.signal_sub_objects = static_cast<std::uint32_t>(std::strtoul(value->c_str(), nullptr, 16));
    } else if (equals_ignore_case(field, "QDirection")) {
        for (auto& component : object.rotation) {
            if (const auto value = tokens.next()) {
                if (const auto parsed = parse_double(*value)) component = static_cast<float>(*parsed);
            }
        }
    }
}

struct ObjectKeyword {
    const char* name;
    WorldObjectKind kind;
};

constexpr ObjectKeyword object_keywords[] = {
    {"Static", WorldObjectKind::static_object},
    {"TrackObj", WorldObjectKind::track},
    {"Gantry", WorldObjectKind::gantry},
    {"CollideObject", WorldObjectKind::collide_object},
    {"Signal", WorldObjectKind::signal},
    {"Speedpost", WorldObjectKind::speed_post},
    {"Transfer", WorldObjectKind::transfer},
    {"Forest", WorldObjectKind::forest},
    {"LevelCr", WorldObjectKind::level_crossing},
    {"Pickup", WorldObjectKind::pickup},
    {"CarSpawner", WorldObjectKind::car_spawner},
};

[[nodiscard]] std::optional<WorldObjectKind> object_kind(const std::string& keyword) {
    for (const auto& entry : object_keywords) {
        if (equals_ignore_case(keyword, entry.name)) return entry.kind;
    }
    return std::nullopt;
}

// Reads an object block after its opening bracket. Returns an object only when
// both its shape file and its position are present.
[[nodiscard]] std::optional<WorldObjectPlacement> read_object(Tokenizer& tokens,
                                                              const WorldTile& tile,
                                                              const WorldObjectKind kind) {
    WorldObjectPlacement object{.kind = kind};
    object.position.tile_x = tile.tile_x;
    object.position.tile_z = tile.tile_z;
    bool has_shape = false;
    bool has_position = false;

    for (int depth = 1; depth > 0;) {
        const auto field = tokens.next();
        if (!field) break;
        if (*field == "(") {
            ++depth;
            continue;
        }
        if (*field == ")") {
            --depth;
            continue;
        }
        if (depth != 1 || !tokens.open_block()) continue;

        if (kind == WorldObjectKind::signal && equals_ignore_case(*field, "SignalUnits")) {
            // SignalUnits ( n  SignalUnit ( sub  TrItemId ( db item ) ) ... )
            std::vector<std::string> path; // names of the blocks open inside SignalUnits
            std::string previous;
            WorldSignalUnit unit;
            bool expect_sub_object = false;
            std::vector<std::string> item_values;
            while (const auto token = tokens.next()) {
                if (*token == "(") {
                    path.push_back(previous);
                    expect_sub_object = equals_ignore_case(previous, "SignalUnit");
                    if (expect_sub_object) unit = {};
                    if (equals_ignore_case(previous, "TrItemId")) item_values.clear();
                    previous.clear();
                    continue;
                }
                if (*token == ")") {
                    if (path.empty()) break; // the end of SignalUnits
                    if (equals_ignore_case(path.back(), "TrItemId") && !item_values.empty()) {
                        if (const auto item = parse_integer<std::uint32_t>(item_values.back())) unit.track_item = *item;
                    } else if (equals_ignore_case(path.back(), "SignalUnit")) {
                        object.signal_units.push_back(unit);
                    }
                    path.pop_back();
                    continue;
                }
                if (expect_sub_object) {
                    if (const auto index = parse_integer<std::uint32_t>(*token)) unit.sub_object = *index;
                    expect_sub_object = false;
                } else if (!path.empty() && equals_ignore_case(path.back(), "TrItemId")) {
                    item_values.push_back(*token);
                }
                previous = *token;
            }
            continue;
        }
        read_static_field(tokens, *field, object, has_shape, has_position);
        tokens.skip_block();
    }

    if (!has_position) return std::nullopt;
    if (kind == WorldObjectKind::forest) {
        object.forest_complete = !object.tree_texture.empty() && object.population > 0 && object.tree_size[0] != 0.0f
            && object.tree_size[1] != 0.0f && (object.scale_range[0] != 0.0f || object.scale_range[1] != 0.0f);
        object.population = (std::min)(object.population, 10000u);
        return object;
    }
    if (kind == WorldObjectKind::car_spawner) return object;
    if (!has_shape) return std::nullopt;
    return object;
}

} // namespace

const char* to_string(const WorldObjectKind kind) noexcept {
    for (const auto& entry : object_keywords) {
        if (entry.kind == kind) return entry.name;
    }
    return "Unknown";
}

std::optional<std::pair<std::int32_t, std::int32_t>> parse_world_tile_name(const std::string_view name) {
    // w+XXXXXX+ZZZZZZ.w: the leading 'w' and two signed seven-character fields.
    const auto stem = std::filesystem::path(name).stem().string();
    if (stem.size() != 15 || (stem[0] != 'w' && stem[0] != 'W')) return std::nullopt;
    if (stem[8] != '+' && stem[8] != '-') return std::nullopt;

    const auto parse_field = [](std::string_view field) {
        if (!field.empty() && field.front() == '+') field.remove_prefix(1);
        return parse_integer<std::int32_t>(field);
    };
    const auto x = parse_field(std::string_view(stem).substr(1, 7));
    const auto z = parse_field(std::string_view(stem).substr(8, 7));
    if (!x || !z) return std::nullopt;
    return std::pair{*x, *z};
}

RouteDefinition parse_route_definition(const std::string_view trk_bytes) {
    RouteDefinition result;
    std::string error;
    const auto text = detail::decode_text(trk_bytes, error);
    if (!text) {
        result.error = error;
        return result;
    }

    Tokenizer tokens(*text);
    while (const auto key = tokens.next()) {
        if (equals_ignore_case(*key, "RouteID") && tokens.open_block()) {
            if (const auto id = tokens.next()) result.route_id = *id;
            tokens.skip_block();
        } else if (equals_ignore_case(*key, "RouteStart") && tokens.open_block()) {
            if (const auto start = read_route_start(tokens)) result.start = start;
            tokens.skip_block();
        } else if (equals_ignore_case(*key, "Environment") && tokens.open_block()) {
            read_environments(tokens, result.environments);
        } else if ((equals_ignore_case(*key, "SpeedLimit") || equals_ignore_case(*key, "TempRestrictedSpeed"))
                   && tokens.open_block()) {
            const bool limit = equals_ignore_case(*key, "SpeedLimit");
            if (const auto value = tokens.next()) {
                if (const auto parsed = parse_double(*value)) {
                    (limit ? result.speed_limit : result.temp_restricted_speed) = static_cast<float>(*parsed);
                }
            }
            tokens.skip_block();
        } else if (equals_ignore_case(*key, "Electrified") && tokens.open_block()) {
            if (const auto value = tokens.next()) result.electrified = std::strtoul(value->c_str(), nullptr, 16) != 0;
            tokens.skip_block();
        }
    }

    if (result.route_id.empty()) result.error = "Route file contains no RouteID.";
    return result;
}

WorldTile parse_world_tile(const std::string_view bytes, const std::string_view filename) {
    WorldTile result;
    const auto coordinate = parse_world_tile_name(filename);
    if (!coordinate) {
        result.error = "Invalid world tile filename.";
        return result;
    }
    result.tile_x = coordinate->first;
    result.tile_z = coordinate->second;

    std::string error;
    const auto text = detail::decode_text(bytes, error);
    if (!text) {
        result.error = error;
        return result;
    }

    Tokenizer tokens(*text);
    while (const auto key = tokens.next()) {
        const auto kind = object_kind(*key);
        if (!kind || !tokens.open_block()) continue;
        if (auto object = read_object(tokens, result, *kind)) result.objects.push_back(std::move(*object));
    }
    return result;
}

RouteDefinition load_route_definition(const std::filesystem::path& root) {
    const auto trk = root / (root.filename().string() + ".trk");
    std::ifstream input(trk, std::ios::binary);
    if (!input) return {.root = root, .error = "Cannot open file: " + trk.string()};
    auto result = parse_route_definition(std::string(std::istreambuf_iterator<char>(input), {}));
    result.root = root;
    return result;
}

WorldTile load_world_tile(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.error = "Cannot open file: " + file.string()};
    return parse_world_tile(std::string(std::istreambuf_iterator<char>(input), {}), file.filename().string());
}

RouteDefinition load_route_definition(const core::Vfs& vfs, const core::VirtualPath& route) {
    auto trk = route.resolve(std::string_view(std::string(route.filename()) + ".trk"));
    if (!trk || !vfs.exists(*trk)) {
        trk.reset();
        for (const auto& entry : vfs.list(route)) {
            if (!entry.directory && core::fold_case(entry.path.extension()) == ".trk") {
                trk = entry.path;
                break;
            }
        }
    }
    if (!trk) return {.root = route.to_filesystem(), .error = "No .trk file in " + route.str()};
    std::string error;
    const auto bytes = vfs.read(*trk, &error);
    if (!bytes) return {.root = route.to_filesystem(), .error = error};
    auto result = parse_route_definition(*bytes);
    result.root = route.to_filesystem();
    return result;
}

WorldTile load_world_tile(const core::Vfs& vfs, const core::VirtualPath& file) {
    std::string error;
    const auto bytes = vfs.read(file, &error);
    if (!bytes) return {.error = error};
    return parse_world_tile(*bytes, file.filename());
}

} // namespace openrts::formats
