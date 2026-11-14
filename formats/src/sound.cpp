#include <openrts/formats/sound.hpp>

#include <algorithm>
#include <array>

#include "text_tree.hpp"

// Parsing follows rts1.exe (0x4DD0EC scripts, 0x4DF894 actions, 0x4578C0
// keyword search), see docs/sound.md:
// - the text is lower-cased, and Comment / Skip / _Skip blocks are removed;
// - a keyword matches only exactly, followed by "(" (no misspellings);
// - fields of a group or stream are found by searching its text in document
//   order, so the first "Volume ( )" anywhere inside counts;
// - counts written before lists limit how many entries are read;
// - a trigger's action is the first action keyword in a fixed order.

namespace openrts::formats {
namespace {

using detail::TextNode;

[[nodiscard]] std::uint32_t unsigned_value(const TextNode& node, std::size_t index, std::uint32_t fallback = 0) {
    const auto value = node.number(index, -1.0);
    return value < 0.0 ? fallback : static_cast<std::uint32_t>(value);
}

[[nodiscard]] float float_value(const TextNode& node, std::size_t index, float fallback = 0.0f) {
    return static_cast<float>(node.number(index, fallback));
}

// First block named `name` inside `node` (or `node` itself), in document order.
[[nodiscard]] const TextNode* find(const TextNode& node, std::string_view name, bool include_self = false) {
    if (include_self && node.name == name) return &node;
    for (const auto& child : node.children) {
        if (child.name == name) return &child;
        if (const auto* inner = find(child, name)) return inner;
    }
    return nullptr;
}

// Count written before a list, limiting the entries read (RTS reads at most
// that many and stops early when fewer are present).
[[nodiscard]] std::size_t limit(const TextNode& node, std::size_t available) {
    return (std::min)(available, static_cast<std::size_t>(unsigned_value(node, 0)));
}

// Numbers after the count: `On ( 2 45 46 )`, `Sources ( 2 0 1 )`.
[[nodiscard]] std::vector<std::uint32_t> counted_list(const TextNode& node) {
    std::vector<std::uint32_t> result;
    const auto count = limit(node, node.values.empty() ? 0 : node.values.size() - 1);
    for (std::size_t i = 1; i <= count; ++i) result.push_back(unsigned_value(node, i));
    return result;
}

constexpr std::array<std::pair<std::string_view, SoundVariable>, 13> variable_names{{
    {"speed", SoundVariable::speed},
    {"variable1", SoundVariable::variable1},
    {"variable2", SoundVariable::variable2},
    {"variable3", SoundVariable::variable3},
    {"variable4", SoundVariable::variable4},
    {"variable5", SoundVariable::variable5},
    {"variable6", SoundVariable::variable6},
    {"variable7", SoundVariable::variable7},
    {"variable8", SoundVariable::variable8},
    {"distance", SoundVariable::distance},
    {"distance_travelled", SoundVariable::distance_travelled},
    {"acceleration", SoundVariable::acceleration},
    {"radius", SoundVariable::radius},
}};

[[nodiscard]] SoundVariable variable_named(std::string_view name) {
    for (const auto& [key, variable] : variable_names) {
        if (key == name) return variable;
    }
    return SoundVariable::unknown;
}

// Curve controller: speedcontrolled, variable1controlled, ...,
// distancetravelledcontrolled (no underscore), radiuscontrolled.
[[nodiscard]] SoundVariable controller_named(const std::string& name) {
    for (const auto& [key, variable] : variable_names) {
        std::string compact;
        for (const char character : key) {
            if (character != '_') compact.push_back(character);
        }
        if (name == compact + "controlled") return variable;
    }
    return SoundVariable::unknown;
}

struct Reader {
    SoundScript& script;

    void unknown(const std::string& name) {
        if (std::find(script.unknown.begin(), script.unknown.end(), name) == script.unknown.end()) {
            script.unknown.push_back(name);
        }
    }

    [[nodiscard]] SoundActivation activation(const TextNode* node) {
        SoundActivation result;
        if (!node) return result;
        result.present = true;
        result.cab_camera = find(*node, "cabcam") != nullptr;
        result.passenger_camera = find(*node, "passengercam") != nullptr;
        result.external_camera = find(*node, "externalcam") != nullptr;
        if (const auto* distance = find(*node, "distance")) result.distance = float_value(*distance, 0);
        return result;
    }

    [[nodiscard]] SoundCurve curve(const TextNode& node) {
        SoundCurve result;
        const auto control = node.values.empty() ? std::string{} : node.values.front();
        result.control = controller_named(control);
        if (result.control == SoundVariable::unknown) unknown("curve:" + control);
        if (const auto* points = find(node, "curvepoints")) {
            const auto count = limit(*points, points->values.empty() ? 0 : (points->values.size() - 1) / 2);
            for (std::size_t i = 0; i < count; ++i) {
                result.points.emplace_back(float_value(*points, 1 + 2 * i), float_value(*points, 2 + 2 * i));
            }
        }
        if (const auto* granularity = find(node, "granularity")) result.granularity = float_value(*granularity, 0);
        return result;
    }

    // Action of a block (a trigger, or one entry of TriggerActions).
    void actions(const TextNode& block, std::vector<SoundAction>& out) {
        if (const auto* list = find(block, "triggeractions", true)) {
            const auto count = limit(*list, list->children.size());
            for (std::size_t i = 0; i < count; ++i) actions(list->children[i], out);
            return;
        }
        SoundAction result;
        struct Keyword {
            std::string_view name;
            SoundActionKind kind;
        };
        static constexpr Keyword with_value[]{
            {"setstreamvolume", SoundActionKind::set_stream_volume},
            {"disabletrigger", SoundActionKind::disable_trigger},
            {"enabletrigger", SoundActionKind::enable_trigger},
            {"disablestream", SoundActionKind::disable_stream},
            {"suspendstream", SoundActionKind::suspend_stream},
            {"enablestream", SoundActionKind::enable_stream},
            {"haltplayback", SoundActionKind::halt_playback},
            {"posttrigger", SoundActionKind::post_trigger},
            {"increasevariablevalue", SoundActionKind::increase_variable},
            {"setvariablevalue", SoundActionKind::set_variable},
        };
        for (const auto& [name, kind] : with_value) {
            const auto* node = find(block, name, true);
            if (!node) continue;
            result.kind = kind;
            if (kind == SoundActionKind::set_stream_volume) {
                result.value = float_value(*node, 0, 1.0f);
            } else if (kind == SoundActionKind::set_variable || kind == SoundActionKind::increase_variable) {
                result.variable = variable_named(node->values.empty() ? std::string{} : node->values.front());
                result.value = float_value(*node, 1);
            } else if (kind != SoundActionKind::halt_playback) {
                result.index = unsigned_value(*node, 0);
            }
            out.push_back(std::move(result));
            return;
        }

        if (const auto* fade = find(block, "fade", true)) result.fade = (std::max)(float_value(*fade, 0), 0.0f);
        if (find(block, "releaselooprelease", true)) {
            result.kind = SoundActionKind::release_loop_release;
        } else if (find(block, "releaseloopreleasewithjump", true)) {
            result.kind = SoundActionKind::release_loop_release_with_jump;
        } else {
            const TextNode* play = nullptr;
            static constexpr Keyword players[]{
                {"playoneshot", SoundActionKind::play_one_shot},
                {"startloop", SoundActionKind::start_loop},
                {"startlooprelease", SoundActionKind::start_loop_release},
            };
            for (const auto& [name, kind] : players) {
                if ((play = find(block, name, true))) {
                    result.kind = kind;
                    break;
                }
            }
            if (play) {
                std::vector<const TextNode*> files;
                for (const auto& child : play->children) {
                    if (child.name == "file") files.push_back(&child);
                }
                // A player inside the player (MaDaDev: `StartLoopRelease ( 1
                // StartLoopRelease ( 1 File ( "tifon.wav" -1 ) ... ) )`, the
                // 2ЭС4К's typhon and whistle, 20 scripts of the install):
                // its files. RTS plays them (PROBABLE: its File search goes
                // deeper; not traced).
                for (const TextNode* inner = play; files.empty() && inner;) {
                    const TextNode* next = nullptr;
                    for (const auto& child : inner->children) {
                        if (child.name == "playoneshot" || child.name == "startloop" || child.name == "startlooprelease") {
                            next = &child;
                            break;
                        }
                    }
                    inner = next;
                    if (!inner) break;
                    for (const auto& child : inner->children) {
                        if (child.name == "file") files.push_back(&child);
                    }
                }
                const auto count = limit(*play, files.size());
                for (std::size_t i = 0; i < count; ++i) {
                    if (files[i]->values.empty()) continue;
                    result.files.push_back({.name = files[i]->values.front(),
                                            .parameter = static_cast<int>(files[i]->number(1, -1.0))});
                }
            }
            if (const auto* method = find(block, "selectionmethod", true); method && !method->values.empty()) {
                result.selection = method->values.front() == "randomselection" ? SoundSelection::random
                                                                                : SoundSelection::sequential;
            }
        }
        if (result.kind == SoundActionKind::none) {
            for (const auto& child : block.children) unknown("action:" + child.name);
        }
        out.push_back(std::move(result));
    }

    void min_max(const TextNode* node, float& minimum, float& maximum) {
        if (!node) return;
        minimum = float_value(*node, 0);
        maximum = float_value(*node, 1, minimum);
    }

    [[nodiscard]] SoundTrigger trigger(const TextNode& node) {
        SoundTrigger result{.name = node.name};
        if (node.name == "initial_trigger") {
            result.kind = SoundTriggerKind::initial;
        } else if (node.name == "discrete_trigger") {
            result.kind = SoundTriggerKind::discrete;
            result.code = unsigned_value(node, 0);
        } else if (node.name == "variable_trigger") {
            result.kind = SoundTriggerKind::variable;
            const auto spec = node.values.empty() ? std::string{} : node.values.front();
            for (const auto& [name, variable] : variable_names) {
                const auto base = std::string(name);
                if (spec == base + "_dec_past" || spec == base + "_inc_past") {
                    result.variable = variable;
                    result.increase = spec.ends_with("_inc_past");
                }
            }
            result.threshold = float_value(node, 1);
            if (result.variable == SoundVariable::unknown) unknown("variable:" + spec);
        } else if (node.name == "random_trigger") {
            result.kind = SoundTriggerKind::random;
            min_max(find(node, "delay_min_max"), result.minimum, result.maximum);
            min_max(find(node, "volume_min_max"), result.volume_minimum, result.volume_maximum);
        } else if (node.name == "event_trigger") {
            result.kind = SoundTriggerKind::event;
            const auto event = node.values.empty() ? std::string{} : node.values.front();
            result.event = event == "wagononjoint" ? SoundEvent::wagon_on_joint
                : event == "wagononjunction"       ? SoundEvent::wagon_on_junction
                : event == "wagononanyjoint"       ? SoundEvent::wagon_on_any_joint
                                                   : SoundEvent::unknown;
            result.event_parameter = static_cast<std::int32_t>(node.number(1, 0.0));
        } else {
            unknown("trigger:" + node.name);
            return result; // kind unknown: keeps its index, does nothing
        }
        actions(node, result.actions);
        return result;
    }

    [[nodiscard]] SoundStream stream(const TextNode& node) {
        SoundStream result;
        if (const auto* priority = find(node, "priority")) result.priority = static_cast<int>(priority->number(0));
        if (const auto* volume = find(node, "volume")) result.volume = float_value(*volume, 0, 1.0f);
        if (const auto* triggers = find(node, "triggers")) {
            const auto count = limit(*triggers, triggers->children.size());
            for (std::size_t i = 0; i < count; ++i) result.triggers.push_back(trigger(triggers->children[i]));
        }
        if (const auto* curve_node = find(node, "volumecurve")) result.volume_curve = curve(*curve_node);
        if (const auto* curve_node = find(node, "frequencycurve")) result.frequency_curve = curve(*curve_node);
        if (const auto* sources = find(node, "sources")) result.sources = counted_list(*sources);
        if (const auto* attenuation = find(node, "attenuation")) {
            result.attenuation = SoundAttenuation{
                .scale = float_value(*attenuation, 0, 1.0f),
                .distance = float_value(*attenuation, 1),
                .type = static_cast<int>(attenuation->number(2, 1.0)),
            };
        }
        result.allow_on_window_camera = find(node, "allowstreamonwindowcamera") != nullptr;
        return result;
    }

    [[nodiscard]] SoundGroup group(const TextNode& node) {
        SoundGroup result{.level = static_cast<int>(node.number(0))};
        result.activation = activation(find(node, "activation"));
        result.deactivation = activation(find(node, "deactivation"));
        if (const auto* volume = find(node, "volume")) result.volume = float_value(*volume, 0, 1.0f);
        result.stereo = find(node, "stereo") != nullptr;
        if (const auto* sources = find(node, "soundsources")) {
            const auto count = limit(*sources, sources->values.empty() ? 0 : (sources->values.size() - 1) / 4);
            for (std::size_t i = 0; i < count; ++i) {
                const auto base = 1 + 4 * i;
                result.sources.push_back({float_value(*sources, base), float_value(*sources, base + 1),
                                          float_value(*sources, base + 2), float_value(*sources, base + 3, 1.0f)});
            }
        }
        if (const auto* pairs = find(node, "discretetriggerpairs")) {
            for (const auto& pair : pairs->children) {
                if (pair.name != "pair") continue;
                SoundTriggerPair parsed;
                if (const auto* list = find(pair, "on")) parsed.on = counted_list(*list);
                if (const auto* list = find(pair, "off")) parsed.off = counted_list(*list);
                if (const auto* list = find(pair, "halt")) parsed.halt = counted_list(*list);
                result.trigger_pairs.push_back(std::move(parsed));
            }
        }
        if (const auto* list = find(node, "nondistributabletriggers")) result.non_distributable = counted_list(*list);
        if (const auto* streams = find(node, "streams")) {
            std::vector<const TextNode*> entries;
            for (const auto& child : streams->children) {
                if (child.name == "stream") entries.push_back(&child);
            }
            const auto count = limit(*streams, entries.size());
            for (std::size_t i = 0; i < count; ++i) result.streams.push_back(stream(*entries[i]));
        }
        static constexpr std::string_view known[]{"activation", "deactivation", "volume", "stereo", "soundsources",
                                                  "discretetriggerpairs", "nondistributabletriggers", "streams"};
        for (const auto& child : node.children) {
            if (std::find(std::begin(known), std::end(known), child.name) == std::end(known)) unknown(child.name);
        }
        return result;
    }
};

// ScalabiltyGroup blocks (the MSTS spelling) anywhere in the text, in order.
void collect_groups(const TextNode& node, std::vector<const TextNode*>& out) {
    for (const auto& child : node.children) {
        if (child.name == "scalabiltygroup") {
            out.push_back(&child);
        } else {
            collect_groups(child, out);
        }
    }
}

} // namespace

const SoundGroup* SoundScript::select_group(const SoundOwner owner) const {
    switch (owner) {
    case SoundOwner::world:
    case SoundOwner::environment:
        return groups.empty() ? nullptr : &groups.front();
    case SoundOwner::player_train:
        for (const auto& group : groups) {
            if (group.level == 7) return &group;
        }
        return groups.empty() ? nullptr : &groups.front();
    case SoundOwner::train:
        for (const auto& group : groups) {
            if (group.level == 5 || group.level == 0) return &group;
        }
        return nullptr;
    case SoundOwner::car:
        for (const auto& group : groups) {
            if (group.level == 0) return &group;
        }
        return nullptr;
    }
    return nullptr;
}

std::vector<core::VirtualPath> sound_file_candidates(const core::VirtualPath& script, const std::string_view file) {
    std::vector<core::VirtualPath> result;
    if (const auto beside = script.parent().resolve(file)) result.push_back(*beside);
    if (const auto global = core::VirtualPath::parse("SOUND")->resolve(file)) {
        if (std::find(result.begin(), result.end(), *global) == result.end()) result.push_back(*global);
    }
    return result;
}

SoundScript parse_sound_script(const std::string_view bytes) {
    SoundScript script;
    std::string error;
    auto text = detail::decode_text(bytes, error);
    if (!text) return {.error = error};
    // RTS lower-cases the whole file (CharLowerBuffW) before parsing.
    *text = detail::lower_ascii(*text);
    TextNode root;
    detail::read_text_tree(*text, root);

    Reader reader{script};
    std::vector<const TextNode*> groups;
    collect_groups(root, groups);
    for (const auto* group : groups) script.groups.push_back(reader.group(*group));
    if (const auto* table = find(root, "namedtriggerstable")) {
        // RTS 0x4DD0EC: the file's first NamedTriggersTable, for every group;
        // its tokens in pairs, a name then its code (_wtoi), names
        // lower-cased with the file.
        for (std::size_t i = 0; i + 1 < table->values.size(); i += 2) {
            const auto& name = table->values[i];
            if (name.empty()) break; // RTS: "Empty name in named triggers table"
            const auto number = detail::parse_double(table->values[i + 1]);
            script.named_triggers.emplace_back(name, number ? static_cast<std::uint32_t>(static_cast<std::int64_t>(*number)) : 0u);
        }
    }
    // No groups: the script is silent (RTS does not require Tr_SMS either).
    return script;
}

SoundScript load_sound_script(const core::Vfs& vfs, const core::VirtualPath& file) {
    std::string error;
    const auto bytes = vfs.read(file, &error);
    if (!bytes) return {.error = error};
    return parse_sound_script(*bytes);
}

WorldSoundTile parse_world_sounds(const std::string_view bytes, const std::string_view filename) {
    WorldSoundTile result;
    const auto tile = parse_world_tile_name(filename);
    if (!tile) return {.error = "World sound file name is not a tile name: " + std::string(filename)};
    result.tile_x = tile->first;
    result.tile_z = tile->second;

    std::string error;
    auto text = detail::decode_text(bytes, error);
    if (!text) return {.error = error};
    TextNode root;
    detail::read_text_tree(*text, root);
    const auto* file = root.child("tr_worldsoundfile");
    if (!file) return {.error = "World sound file has no Tr_Worldsoundfile block."};
    for (const auto& node : file->children) {
        if (node.name != "soundsource") continue;
        WorldSoundSource source{.position = {.tile_x = result.tile_x, .tile_z = result.tile_z}};
        if (const auto* position = node.child("position")) {
            source.position.x = position->number(0);
            source.position.y = position->number(1);
            source.position.z = position->number(2);
        }
        if (const auto* name = node.child("filename"); name && !name->values.empty()) source.file = name->values.front();
        if (const auto* uid = node.child("uid")) source.uid = static_cast<std::uint32_t>(uid->number(0));
        if (!source.file.empty()) result.sources.push_back(std::move(source));
    }
    return result;
}

WorldSoundTile load_world_sounds(const core::Vfs& vfs, const core::VirtualPath& file) {
    std::string error;
    const auto bytes = vfs.read(file, &error);
    if (!bytes) return {.error = error};
    return parse_world_sounds(*bytes, file.filename());
}

} // namespace openrts::formats
