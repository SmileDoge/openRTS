#include <openrts/formats/signal_config.hpp>

#include <algorithm>

#include "text_tree.hpp"

namespace openrts::formats {
namespace {

using detail::lower_ascii;
using detail::TextNode;

constexpr std::pair<std::string_view, SignalAspect> aspect_names[] = {
    {"stop", SignalAspect::stop},
    {"stop_and_proceed", SignalAspect::stop_and_proceed},
    {"restricting", SignalAspect::restricting},
    {"approach_1", SignalAspect::approach_1},
    {"approach_2", SignalAspect::approach_2},
    {"approach_3", SignalAspect::approach_3},
    {"clear_1", SignalAspect::clear_1},
    {"clear_2", SignalAspect::clear_2},
    {"block_obstructed", SignalAspect::block_obstructed},
    {"failed", SignalAspect::failed},
};

[[nodiscard]] std::string value(const TextNode& node, std::size_t index) {
    return index < node.values.size() ? node.values[index] : std::string{};
}

[[nodiscard]] bool has_flag(const TextNode& node, std::string_view flag) {
    const auto* flags = node.child("signalflags");
    if (!flags) return false;
    for (const auto& word : flags->values) {
        if (lower_ascii(word) == flag) return true;
    }
    return false;
}

void read_type(const TextNode& node, SignalConfig& config) {
    SignalType type;
    type.display_name = value(node, 0);
    type.name = signal_name_key(type.display_name);
    if (const auto* function = node.child("signalfntype")) {
        const auto name = lower_ascii(value(*function, 0));
        type.function = name == "shunting" ? SignalFunction::shunting
            : name == "distance"           ? SignalFunction::distance
            : name == "repeater"           ? SignalFunction::repeater
            : name == "info"               ? SignalFunction::info
                                           : SignalFunction::normal;
        if (name != "normal" && name != "shunting" && name != "distance" && name != "repeater" && name != "info") {
            config.warnings.push_back("signal type " + type.display_name + ": unknown SignalFnType " + name);
        }
    }
    if (const auto* texture = node.child("signallighttex")) type.light_texture = value(*texture, 0);
    type.semaphore = has_flag(node, "semaphore");
    if (const auto* lights = node.child("signallights")) {
        for (const auto& light : lights->children) {
            if (light.name != "signallight") continue;
            SignalLight entry{.index = static_cast<std::uint32_t>(light.number(0)), .colour = value(light, 1)};
            if (const auto* position = light.child("position")) {
                for (std::size_t i = 0; i < 3; ++i) entry.position[i] = static_cast<float>(position->number(i));
            }
            if (const auto* radius = light.child("radius")) entry.radius = static_cast<float>(radius->number(0));
            type.lights.push_back(std::move(entry));
        }
    }
    if (const auto* states = node.child("signaldrawstates")) {
        for (const auto& state : states->children) {
            if (state.name != "signaldrawstate") continue;
            SignalDrawState entry{.index = static_cast<std::uint32_t>(state.number(0)), .name = value(state, 1)};
            if (const auto* lights = state.child("drawlights")) {
                for (const auto& light : lights->children) {
                    if (light.name != "drawlight") continue;
                    entry.lights.push_back({.light = static_cast<std::uint32_t>(light.number(0)), .flashing = has_flag(light, "flashing")});
                }
            }
            if (const auto* position = state.child("semaphorepos")) entry.semaphore_position = static_cast<float>(position->number(0));
            type.draw_states.push_back(std::move(entry));
        }
    }
    if (const auto* aspects = node.child("signalaspects")) {
        for (const auto& aspect : aspects->children) {
            if (aspect.name != "signalaspect") continue;
            const auto parsed = signal_aspect_from_name(value(aspect, 0));
            if (!parsed) {
                config.warnings.push_back("signal type " + type.display_name + ": unknown aspect " + value(aspect, 0));
                continue;
            }
            SignalAspectEntry entry{.aspect = *parsed, .draw_state = value(aspect, 1)};
            if (const auto* mph = aspect.child("speedmph")) entry.speed = static_cast<float>(mph->number(0) * 1609.0 / 3600.0);
            if (const auto* kph = aspect.child("speedkph")) entry.speed = static_cast<float>(kph->number(0) / 3.6);
            type.aspects.push_back(std::move(entry));
        }
    }
    if (const auto* ahead = node.child("signalnumclearahead")) type.num_clear_ahead = static_cast<std::uint32_t>(ahead->number(0));
    if (const auto* flash = node.child("sigflashduration")) {
        type.flash_on = static_cast<float>(flash->number(0));
        type.flash_off = static_cast<float>(flash->number(1));
    }
    if (const auto* info = node.child("semaphoreinfo")) type.semaphore_info = static_cast<float>(info->number(0));
    for (auto& aspect : type.aspects) aspect.draw_state_index = type.draw_state_index(aspect.draw_state);
    config.types.push_back(std::move(type));
}

void read_shape(const TextNode& node, SignalConfig& config) {
    SignalShape shape{.file = signal_name_key(value(node, 0)), .description = value(node, 1)};
    if (const auto* subs = node.child("signalsubobjs")) {
        for (const auto& sub : subs->children) {
            if (sub.name != "signalsubobj") continue;
            SignalSubObject entry{.index = static_cast<std::uint32_t>(sub.number(0)), .name = value(sub, 1), .description = value(sub, 2)};
            if (const auto* kind = sub.child("sigsubtype")) {
                const auto name = lower_ascii(value(*kind, 0));
                if (name == "signal_head") entry.kind = SignalSubObjectKind::signal_head;
                else if (name == "decor") entry.kind = SignalSubObjectKind::decor;
                else if (name == "number_plate") entry.kind = SignalSubObjectKind::number_plate;
                else if (name == "gradient_plate") entry.kind = SignalSubObjectKind::gradient_plate;
                else if (name.starts_with("user")) {
                    entry.kind = SignalSubObjectKind::user;
                    entry.user = static_cast<std::uint32_t>(std::atoi(name.c_str() + 4));
                }
            }
            if (const auto* type = sub.child("sigsubstype")) entry.signal_type = signal_name_key(value(*type, 0));
            entry.optional = has_flag(sub, "optional");
            entry.default_on = has_flag(sub, "default");
            entry.back_facing = has_flag(sub, "back_facing");
            entry.junction_link = has_flag(sub, "jn_link");
            if (const auto* links = sub.child("sigsubjnlinkif")) {
                for (std::size_t i = 1; i < links->values.size(); ++i) entry.junction_links.push_back(static_cast<std::uint32_t>(links->number(i)));
            }
            shape.sub_objects.push_back(std::move(entry));
        }
    }
    config.shapes.push_back(std::move(shape));
}

} // namespace

std::string signal_name_key(const std::string_view name) {
    std::string result(name);
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto& c = result[i];
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x80) {
            c = static_cast<char>(std::tolower(b));
        } else if (b == 0xD0 && i + 1 < result.size()) {
            // U+0410..U+042F -> U+0430..U+044F; U+0400..U+040F -> U+0450..U+045F.
            auto& next = result[i + 1];
            const auto n = static_cast<unsigned char>(next);
            if (n >= 0x90 && n <= 0x9F) {
                next = static_cast<char>(n + 0x20);
            } else if (n >= 0xA0 && n <= 0xAF) {
                c = static_cast<char>(0xD1);
                next = static_cast<char>(n - 0x20);
            } else if (n >= 0x80 && n <= 0x8F) {
                c = static_cast<char>(0xD1);
                next = static_cast<char>(n + 0x10);
            }
            ++i;
        }
    }
    return result;
}

std::optional<SignalAspect> signal_aspect_from_name(const std::string_view name) {
    const auto lower = lower_ascii(name);
    for (const auto& [text, aspect] : aspect_names) {
        if (lower == text) return aspect;
    }
    return std::nullopt;
}

const char* to_string(const SignalAspect aspect) noexcept {
    static constexpr const char* names[] = {"STOP", "STOP_AND_PROCEED", "RESTRICTING", "APPROACH_1", "APPROACH_2",
                                            "APPROACH_3", "CLEAR_1", "CLEAR_2", "BLOCK_OBSTRUCTED", "FAILED"};
    const auto index = static_cast<std::size_t>(aspect);
    return index < std::size(names) ? names[index] : "?";
}

int SignalType::draw_state_index(const std::string_view state) const {
    const auto lower = lower_ascii(state);
    for (std::size_t i = 0; i < draw_states.size(); ++i) {
        if (lower_ascii(draw_states[i].name) == lower) return static_cast<int>(i);
    }
    return -1;
}

const SignalType* SignalConfig::find_type(const std::string_view name) const {
    const auto lower = signal_name_key(name);
    for (const auto& type : types) {
        if (type.name == lower) return &type;
    }
    return nullptr;
}

const SignalShape* SignalConfig::find_shape(const std::string_view file) const {
    const auto lower = signal_name_key(file);
    for (const auto& shape : shapes) {
        if (shape.file == lower) return &shape;
    }
    return nullptr;
}

const SignalLightColour* SignalConfig::find_colour(const std::string_view name) const {
    const auto lower = lower_ascii(name);
    for (const auto& colour : colours) {
        if (lower_ascii(colour.name) == lower) return &colour;
    }
    return nullptr;
}

const SignalLightTexture* SignalConfig::find_texture(const std::string_view name) const {
    const auto lower = lower_ascii(name);
    for (const auto& texture : light_textures) {
        if (lower_ascii(texture.name) == lower) return &texture;
    }
    return nullptr;
}

SignalConfig parse_signal_config(const std::string_view bytes) {
    SignalConfig config;
    std::string error;
    const auto text = detail::decode_text(bytes, error);
    if (!text) return {.error = error};
    TextNode root;
    if (!detail::read_text_tree(*text, root)) return {.error = "sigcfg.dat has no blocks"};
    for (const auto& block : root.children) {
        if (block.name == "lighttextures") {
            for (const auto& texture : block.children) {
                if (texture.name != "lighttex") continue;
                SignalLightTexture entry{.name = value(texture, 0), .file = value(texture, 1)};
                for (std::size_t i = 0; i < 4; ++i) entry.uv[i] = static_cast<float>(texture.number(2 + i, i < 2 ? 0.0 : 1.0));
                config.light_textures.push_back(std::move(entry));
            }
        } else if (block.name == "lightstab") {
            for (const auto& entry : block.children) {
                if (entry.name != "lightstabentry") continue;
                SignalLightColour colour{.name = value(entry, 0)};
                if (const auto* argb = entry.child("colour")) {
                    for (std::size_t i = 0; i < 4; ++i) colour.argb[i] = static_cast<std::uint8_t>(std::clamp(argb->number(i), 0.0, 255.0));
                }
                config.colours.push_back(std::move(colour));
            }
        } else if (block.name == "signaltypes") {
            for (const auto& type : block.children) {
                if (type.name == "signaltype") read_type(type, config);
            }
        } else if (block.name == "signalshapes") {
            for (const auto& shape : block.children) {
                if (shape.name == "signalshape") read_shape(shape, config);
            }
        } else if (block.name == "scriptfiles") {
            for (const auto& file : block.children) {
                if (file.name == "scriptfile") config.script_files.push_back(value(file, 0));
            }
        } else if (block.name == "rts_shuntingautomode") {
            config.shunting_auto_mode = block.number(0) != 0.0;
        }
    }
    if (config.types.empty()) config.error = "sigcfg.dat has no signal types";
    return config;
}

SignalConfig load_signal_config(const core::Vfs& vfs, const core::VirtualPath& route) {
    const auto path = route.resolve(std::string_view("sigcfg.dat"));
    if (!path) return {.error = "bad route path"};
    std::string error;
    const auto bytes = vfs.read(*path, &error);
    if (!bytes) return {.error = "sigcfg.dat: " + error};
    return parse_signal_config(*bytes);
}

} // namespace openrts::formats
