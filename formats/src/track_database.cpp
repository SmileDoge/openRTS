#include <openrts/formats/track_database.hpp>

#include <cmath>

#include <algorithm>

#include "text_tokenizer.hpp"

namespace openrts::formats {
namespace {

using detail::equals_ignore_case;
using detail::Tokenizer;

// Reads the values of the current block up to its closing bracket; nested
// blocks are skipped.
[[nodiscard]] std::vector<std::string> values(Tokenizer& tokens) {
    std::vector<std::string> result;
    while (const auto token = tokens.next()) {
        if (*token == ")") break;
        if (*token == "(") {
            tokens.skip_block();
            continue;
        }
        result.push_back(*token);
    }
    return result;
}

[[nodiscard]] double number_at(const std::vector<std::string>& list, const std::size_t index) {
    if (index >= list.size()) return 0.0;
    return detail::parse_double(list[index]).value_or(0.0);
}

[[nodiscard]] std::uint32_t hex_at(const std::vector<std::string>& list, const std::size_t index) {
    if (index >= list.size()) return 0;
    return detail::parse_integer<std::uint32_t>(list[index], 16).value_or(0);
}

[[nodiscard]] std::int32_t int_at(const std::vector<std::string>& list, const std::size_t index) {
    return static_cast<std::int32_t>(number_at(list, index));
}

[[nodiscard]] std::uint32_t uint_at(const std::vector<std::string>& list, const std::size_t index) {
    const auto value = number_at(list, index);
    return value < 0 ? 0u : static_cast<std::uint32_t>(value);
}

[[nodiscard]] float float_at(const std::vector<std::string>& list, const std::size_t index) {
    return static_cast<float>(number_at(list, index));
}

// Calls `child(name)` for each named block until the enclosing bracket closes;
// `child` must consume the block including its closing bracket. Bare words
// (counts) are passed to `word`.
template <typename Child, typename Word>
void blocks(Tokenizer& tokens, Child child, Word word) {
    std::string name;
    bool pending = false;
    while (const auto token = tokens.next()) {
        if (*token == ")") return;
        if (*token != "(") {
            if (pending) word(name);
            name = *token;
            pending = true;
            continue;
        }
        pending = false;
        child(name);
    }
}

template <typename Child>
void blocks(Tokenizer& tokens, Child child) {
    blocks(tokens, child, [](const std::string&) {});
}

void read_vector_node(Tokenizer& tokens, TrackNode& node) {
    node.kind = TrackNodeKind::vector;
    blocks(tokens, [&](const std::string& name) {
        if (equals_ignore_case(name, "TrVectorSections")) {
            const auto list = values(tokens);
            const auto count = uint_at(list, 0);
            constexpr std::size_t fields = 16;
            for (std::uint32_t i = 0; i < count && 1 + (i + 1) * fields <= list.size(); ++i) {
                const auto at = 1 + i * fields;
                node.sections.push_back({
                    .section = uint_at(list, at),
                    .shape = uint_at(list, at + 1),
                    .world_tile_x = int_at(list, at + 2),
                    .world_tile_z = int_at(list, at + 3),
                    .world_uid = uint_at(list, at + 4),
                    .flags1 = uint_at(list, at + 5),
                    .flags2 = uint_at(list, at + 6),
                    // at + 7: a hex string ("00"), not interpreted
                    .tile_x = int_at(list, at + 8),
                    .tile_z = int_at(list, at + 9),
                    .x = float_at(list, at + 10),
                    .y = float_at(list, at + 11),
                    .z = float_at(list, at + 12),
                    .pitch = float_at(list, at + 13),
                    .heading = float_at(list, at + 14),
                    .roll = float_at(list, at + 15),
                });
            }
        } else if (equals_ignore_case(name, "TrItemRefs")) {
            blocks(tokens, [&](const std::string& ref) {
                const auto list = values(tokens);
                if (equals_ignore_case(ref, "TrItemRef") && !list.empty()) node.items.push_back(uint_at(list, 0));
            });
        } else {
            tokens.skip_block();
        }
    });
}

[[nodiscard]] TrackNodeUid read_uid(const std::vector<std::string>& list) {
    return {
        .world_tile_x = int_at(list, 0),
        .world_tile_z = int_at(list, 1),
        .world_uid = uint_at(list, 2),
        // 3: flag
        .tile_x = int_at(list, 4),
        .tile_z = int_at(list, 5),
        .x = float_at(list, 6),
        .y = float_at(list, 7),
        .z = float_at(list, 8),
        .pitch = float_at(list, 9),
        .heading = float_at(list, 10),
        .roll = float_at(list, 11),
    };
}

void read_pins(Tokenizer& tokens, TrackNode& node) {
    // `TrPins ( in out TrPin ( node direction ) ... )`
    std::vector<std::string> counts;
    blocks(
        tokens,
        [&](const std::string& name) {
            const auto list = values(tokens);
            if (equals_ignore_case(name, "TrPin")) node.pins.push_back({uint_at(list, 0), uint_at(list, 1)});
        },
        [&](const std::string& word) { counts.push_back(word); });
    // The bare words before the first TrPin are the in and out counts.
    if (counts.size() >= 2) {
        node.in_pin_count = static_cast<std::uint32_t>(number_at(counts, 0));
        node.out_pin_count = static_cast<std::uint32_t>(number_at(counts, 1));
    } else if (counts.size() == 1) {
        node.in_pin_count = static_cast<std::uint32_t>(number_at(counts, 0));
    }
}

void read_node(Tokenizer& tokens, TrackDatabase& database) {
    const auto index_token = tokens.next();
    if (!index_token) return;
    TrackNode node{.index = static_cast<std::uint32_t>(detail::parse_double(*index_token).value_or(0.0))};
    blocks(tokens, [&](const std::string& name) {
        if (equals_ignore_case(name, "TrVectorNode")) {
            read_vector_node(tokens, node);
        } else if (equals_ignore_case(name, "TrJunctionNode")) {
            node.kind = TrackNodeKind::junction;
            const auto list = values(tokens);
            node.junction_first = uint_at(list, 0);
            node.junction_shape = uint_at(list, 1);
            node.junction_last = uint_at(list, 2);
        } else if (equals_ignore_case(name, "TrEndNode")) {
            node.kind = TrackNodeKind::end;
            tokens.skip_block();
        } else if (equals_ignore_case(name, "UiD")) {
            node.uid = read_uid(values(tokens));
        } else if (equals_ignore_case(name, "TrPins")) {
            read_pins(tokens, node);
        } else {
            tokens.skip_block();
        }
    });
    if (node.index == 0) return;
    if (database.nodes.size() <= node.index) database.nodes.resize(node.index + 1);
    database.nodes[node.index] = std::move(node);
}

[[nodiscard]] TrackItemKind item_kind(const std::string& keyword) {
    struct Entry {
        const char* keyword;
        TrackItemKind kind;
    };
    static constexpr Entry entries[] = {
        {"EmptyItem", TrackItemKind::empty},
        {"SignalItem", TrackItemKind::signal},
        {"SpeedPostItem", TrackItemKind::speed_post},
        {"PlatformItem", TrackItemKind::platform},
        {"SidingItem", TrackItemKind::siding},
        {"LevelCrItem", TrackItemKind::level_crossing},
        {"CrossoverItem", TrackItemKind::crossover},
        {"PickupItem", TrackItemKind::pickup},
        {"CarSpawnerItem", TrackItemKind::car_spawner},
        {"SoundRegionItem", TrackItemKind::sound_region},
        {"HazzardItem", TrackItemKind::hazard},
    };
    for (const auto& entry : entries) {
        if (equals_ignore_case(keyword, entry.keyword)) return entry.kind;
    }
    return TrackItemKind::other;
}

void read_item(Tokenizer& tokens, const std::string& keyword, TrackDatabase& database) {
    TrackItem item{.kind = item_kind(keyword), .keyword = keyword};
    bool has_id = false;
    blocks(tokens, [&](const std::string& name) {
        if (equals_ignore_case(name, "TrSignalDirs")) {
            blocks(tokens, [&](const std::string& entry) {
                const auto list = values(tokens);
                if (!equals_ignore_case(entry, "TrSignalDir")) return;
                item.signal_directions.push_back(
                    {uint_at(list, 0), {uint_at(list, 1), uint_at(list, 2), uint_at(list, 3)}});
            });
            return;
        }
        const auto list = values(tokens);
        if (equals_ignore_case(name, "TrItemId")) {
            item.id = uint_at(list, 0);
            has_id = true;
        } else if (equals_ignore_case(name, "TrItemSData")) {
            item.distance = float_at(list, 0);
            item.data_flags = hex_at(list, 1);
        } else if (equals_ignore_case(name, "TrItemRData")) {
            item.has_position = true;
            item.x = float_at(list, 0);
            item.y = float_at(list, 1);
            item.z = float_at(list, 2);
            item.tile_x = int_at(list, 3);
            item.tile_z = int_at(list, 4);
        } else if (equals_ignore_case(name, "TrItemPData") && !item.has_position) {
            item.has_position = true;
            item.x = float_at(list, 0);
            item.z = float_at(list, 1);
            item.tile_x = int_at(list, 2);
            item.tile_z = int_at(list, 3);
        } else if (equals_ignore_case(name, "TrSignalType")) {
            item.signal_flags = hex_at(list, 0);
            item.signal_direction = uint_at(list, 1);
            item.signal_angle = float_at(list, 2);
            if (list.size() > 3) item.name = list[3];
        } else if (equals_ignore_case(name, "SpeedpostTrItemData")) {
            for (std::size_t i = 0; i < list.size(); ++i) item.speed_post_data.push_back(float_at(list, i));
        } else if (equals_ignore_case(name, "PlatformName") || equals_ignore_case(name, "SidingName")) {
            if (!list.empty()) item.name = list[0];
        } else if (equals_ignore_case(name, "Station")) {
            if (!list.empty()) item.station = list[0];
        }
    });
    if (!has_id) return;
    if (database.items.size() <= item.id) database.items.resize(item.id + 1);
    database.items[item.id] = std::move(item);
}

} // namespace

const char* to_string(const TrackItemKind kind) noexcept {
    switch (kind) {
    case TrackItemKind::empty: return "empty";
    case TrackItemKind::signal: return "signal";
    case TrackItemKind::speed_post: return "speed post";
    case TrackItemKind::platform: return "platform";
    case TrackItemKind::siding: return "siding";
    case TrackItemKind::level_crossing: return "level crossing";
    case TrackItemKind::crossover: return "crossover";
    case TrackItemKind::pickup: return "pickup";
    case TrackItemKind::car_spawner: return "car spawner";
    case TrackItemKind::sound_region: return "sound region";
    case TrackItemKind::hazard: return "hazard";
    case TrackItemKind::other: return "other";
    }
    return "other";
}

std::optional<SpeedPost> speed_post(const TrackItem& item) {
    if (item.kind != TrackItemKind::speed_post || item.speed_post_data.empty()) return std::nullopt;
    const auto& v = item.speed_post_data;
    SpeedPost post;
    post.flags = static_cast<std::uint32_t>(v[0]);
    std::size_t at = 1;
    const auto next = [&]() { return at < v.size() ? v[at++] : 0.0f; };
    const bool limit = (post.flags & 2u) != 0;
    const bool one = (post.flags & 1u) != 0;
    if (limit) {
        post.limit = true;
        post.resume = one;
        if (!one) post.speed = next();
        if ((post.flags & 0x10u) != 0) post.number = next();
    } else {
        post.warning = one;
        if ((post.flags & 8u) != 0) post.speed = next();
        post.number = next();
    }
    post.angle = next();
    post.speed = std::abs(post.speed);
    // km/h, or mph with flag 0x100 (RTS multiplies by 0.4469444 and 0.2777778).
    if (!post.resume) post.speed *= (post.flags & 0x100u) != 0 ? 0.44694444f : 0.27777778f;
    if ((post.flags & 0x80u) == 0 && (post.flags & 0x60u) != 0) {
        post.passenger = (post.flags & 0x20u) != 0;
        post.freight = (post.flags & 0x40u) != 0;
    }
    return post;
}

const TrackNode* TrackDatabase::node(const std::uint32_t index) const {
    if (index == 0 || index >= nodes.size() || nodes[index].index != index) return nullptr;
    return &nodes[index];
}

TrackDatabase parse_track_database(const std::string_view bytes) {
    TrackDatabase database;
    std::string error;
    const auto text = detail::decode_text(bytes, error);
    if (!text) {
        database.error = error;
        return database;
    }
    Tokenizer tokens(*text);
    std::string name;
    bool found = false;
    while (const auto token = tokens.next()) {
        if (*token == ")") continue;
        if (*token != "(") {
            name = *token;
            continue;
        }
        if (!equals_ignore_case(name, "TrackDB")) {
            tokens.skip_block();
            continue;
        }
        found = true;
        blocks(tokens, [&](const std::string& section) {
            if (equals_ignore_case(section, "Serial")) {
                database.serial = uint_at(values(tokens), 0);
            } else if (equals_ignore_case(section, "TrackNodes")) {
                blocks(tokens, [&](const std::string& entry) {
                    if (equals_ignore_case(entry, "TrackNode")) {
                        read_node(tokens, database);
                    } else {
                        tokens.skip_block();
                    }
                });
            } else if (equals_ignore_case(section, "TrItemTable")) {
                blocks(tokens, [&](const std::string& entry) { read_item(tokens, entry, database); });
            } else {
                tokens.skip_block();
            }
        });
    }
    if (!found) database.error = "No TrackDB block.";
    return database;
}

TrackDatabase load_track_database(const core::Vfs& vfs, const core::VirtualPath& route) {
    auto tdb = route.resolve(std::string_view(std::string(route.filename()) + ".tdb"));
    if (!tdb || !vfs.exists(*tdb)) {
        tdb.reset();
        for (const auto& entry : vfs.list(route)) {
            if (!entry.directory && core::fold_case(entry.path.extension()) == ".tdb") {
                tdb = entry.path;
                break;
            }
        }
    }
    if (!tdb) return {.error = "No .tdb in " + route.str()};
    std::string error;
    const auto bytes = vfs.read(*tdb, &error);
    if (!bytes) return {.error = error};
    return parse_track_database(*bytes);
}

} // namespace openrts::formats
