#include <openrts/formats/environment.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

#include "text_tokenizer.hpp"

namespace openrts::formats {
namespace {

using detail::equals_ignore_case;
using detail::parse_double;
using detail::parse_integer;
using detail::Tokenizer;

constexpr std::int32_t seconds_per_day = 86400;

// Generic node of the bracketed text format: `name ( values... children... )`.
struct TextNode {
    std::string name;
    std::vector<std::string> values;
    std::vector<TextNode> children;

    [[nodiscard]] const TextNode* child(const std::string_view wanted) const {
        const auto it = std::find_if(children.begin(), children.end(), [&](const TextNode& node) {
            return equals_ignore_case(node.name, wanted);
        });
        return it == children.end() ? nullptr : &*it;
    }

    [[nodiscard]] std::vector<const TextNode*> all(const std::string_view wanted) const {
        std::vector<const TextNode*> result;
        for (const auto& node : children) {
            if (equals_ignore_case(node.name, wanted)) result.push_back(&node);
        }
        return result;
    }

    [[nodiscard]] std::string value(const std::size_t index = 0) const {
        return index < values.size() ? values[index] : std::string{};
    }
};

// Reads the contents of a block whose opening bracket was consumed.
void read_node_contents(Tokenizer& tokens, TextNode& node) {
    std::optional<std::string> pending;
    while (const auto token = tokens.next()) {
        if (*token == "(") {
            TextNode child{.name = pending.value_or(std::string{})};
            pending.reset();
            read_node_contents(tokens, child);
            node.children.push_back(std::move(child));
            continue;
        }
        if (pending) node.values.push_back(std::move(*pending));
        pending.reset();
        if (*token == ")") return;
        pending = *token;
    }
    if (pending) node.values.push_back(std::move(*pending));
}

[[nodiscard]] TextNode read_document(const std::string_view text) {
    Tokenizer tokens(text);
    TextNode root;
    read_node_contents(tokens, root);
    return root;
}

[[nodiscard]] double number_of(const TextNode* node, const std::size_t index = 0, const double fallback = 0.0) {
    if (!node) return fallback;
    return parse_double(node->value(index)).value_or(fallback);
}

[[nodiscard]] std::int32_t integer_of(const TextNode* node, const std::size_t index = 0) {
    return static_cast<std::int32_t>(number_of(node, index));
}

[[nodiscard]] std::uint32_t colour_of(const TextNode* node) {
    if (!node) return 0;
    return parse_integer<std::uint32_t>(node->value(), 16).value_or(0);
}

[[nodiscard]] EnvShader read_anim_shader(const TextNode* anim) {
    EnvShader result;
    if (!anim) return result;

    result.frame_length = static_cast<float>(number_of(anim->child("world_anim_shader_framelen")));
    if (const auto* frames = anim->child("world_anim_shader_frames")) {
        for (const auto* frame : frames->all("world_anim_shader_frame")) {
            EnvAnimFrame parsed;
            if (const auto* tiles = frame->child("world_anim_shader_frame_uvtiles")) {
                parsed.uv_tiles[0] = static_cast<float>(number_of(tiles, 0, 1.0));
                parsed.uv_tiles[1] = static_cast<float>(number_of(tiles, 1, 1.0));
                parsed.has_uv_tiles = true;
            }
            if (const auto* stamp = frame->child("world_anim_shader_frame_uvstamp")) {
                for (std::size_t k = 0; k < 4; ++k) parsed.uv_stamp[k] = static_cast<float>(number_of(stamp, k));
                parsed.has_uv_stamp = true;
            }
            if (const auto* scroll = frame->child("world_anim_shader_frame_uvscroll")) {
                parsed.uv_scroll[0] = static_cast<float>(number_of(scroll, 0));
                parsed.uv_scroll[1] = static_cast<float>(number_of(scroll, 1));
                parsed.has_uv_scroll = true;
            }
            result.frames.push_back(parsed);
        }
    }

    if (const auto* shader = anim->child("world_shader")) {
        result.shader_name = shader->value();
        if (const auto* slots = shader->child("terrain_texslots")) {
            if (const auto* slot = slots->child("terrain_texslot")) {
                result.texture = slot->value(0);
                result.texture_addressing = static_cast<std::uint32_t>(number_of(slot, 1, 1.0));
            }
        }
    }
    return result;
}

void read_fade(const TextNode& layer_node, SkyLayer& layer) {
    const auto* fade_in = layer_node.child("world_sky_layer_fadein");
    const auto* fade_out = layer_node.child("world_sky_layer_fadeout");
    // RTS only keeps the fade window when a fade-out is given.
    if (!fade_out) return;
    if (fade_in) {
        layer.fade_in_start = parse_time_of_day(fade_in->value(0));
        layer.fade_in_end = parse_time_of_day(fade_in->value(1));
    }
    layer.fade_out_start = parse_time_of_day(fade_out->value(0));
    layer.fade_out_end = parse_time_of_day(fade_out->value(1));

    if (layer.fade_in_start > layer.fade_out_start) layer.fade_in_start -= seconds_per_day;
    if (layer.fade_in_end > layer.fade_out_start) layer.fade_in_end -= seconds_per_day;
    if (layer.fade_out_start > layer.fade_out_end) layer.fade_out_end += seconds_per_day;
}

[[nodiscard]] SkyLayer read_layer(const TextNode& node) {
    SkyLayer layer;
    read_fade(node, layer);
    if (const auto* top = node.child("world_sky_layer_top")) {
        layer.top_faces = static_cast<std::uint16_t>(integer_of(top->child("world_sky_layer_top_nfaces")));
        layer.top_radius = integer_of(top->child("world_sky_layer_top_radius"));
        layer.top_height = integer_of(top->child("world_sky_layer_top_height"));
    }
    if (const auto* edge = node.child("world_sky_layer_edge")) {
        if (const auto* steps = edge->child("world_sky_layer_edge_steps")) {
            const auto heights = steps->all("world_sky_layer_edge_step_height");
            const auto radii = steps->all("world_sky_layer_edge_step_radius");
            for (std::size_t i = 0; i < heights.size() && i < radii.size(); ++i) {
                layer.edge_steps.push_back({.height = integer_of(heights[i]), .radius = integer_of(radii[i])});
            }
        }
    }
    layer.shader = read_anim_shader(node.child("world_anim_shader"));
    return layer;
}

[[nodiscard]] SkySatellite read_satellite(const TextNode& node) {
    SkySatellite satellite;
    satellite.low_scale = static_cast<std::uint16_t>(integer_of(node.child("world_sky_satellite_low_scale")));
    satellite.high_scale = static_cast<std::uint16_t>(integer_of(node.child("world_sky_satellite_high_scale")));
    satellite.rise_position = static_cast<std::uint16_t>(integer_of(node.child("world_sky_satellite_rise_position")));
    if (const auto* rise = node.child("world_sky_satellite_rise_time")) satellite.rise_time = parse_time_of_day(rise->value());
    if (const auto* set = node.child("world_sky_satellite_set_time")) satellite.set_time = parse_time_of_day(set->value());
    if (satellite.rise_time > satellite.set_time) satellite.rise_time -= seconds_per_day;

    constexpr const char* direct[] = {"world_sky_satellite_dir_rise_colour", "world_sky_satellite_dir_high_colour",
                                      "world_sky_satellite_dir_set_colour"};
    constexpr const char* ambient[] = {"world_sky_satellite_amb_rise_colour", "world_sky_satellite_amb_high_colour",
                                       "world_sky_satellite_amb_set_colour"};
    for (std::size_t i = 0; i < 3; ++i) {
        satellite.direct_colours[i] = colour_of(node.child(direct[i]));
        satellite.ambient_colours[i] = colour_of(node.child(ambient[i]));
    }
    satellite.light = integer_of(node.child("world_sky_satellite_light")) != 0;
    satellite.shader = read_anim_shader(node.child("world_anim_shader"));
    return satellite;
}

} // namespace

std::int32_t parse_time_of_day(const std::string& value) {
    std::int32_t result = 0;
    std::int32_t scale = 3600;
    std::size_t start = 0;
    while (start <= value.size() && scale >= 1) {
        const auto end = value.find(':', start);
        const auto part = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        result += scale * parse_integer<std::int32_t>(part).value_or(0);
        if (end == std::string::npos) break;
        start = end + 1;
        scale /= 60;
    }
    return result;
}

EnvironmentDefinition load_environment(const std::filesystem::path& file) {
    std::string error;
    const auto text = detail::read_text(file, error);
    if (!text) return {.error = error};
    return parse_environment(*text);
}

EnvironmentDefinition load_environment(const core::Vfs& vfs, const core::VirtualPath& file) {
    std::string error;
    const auto bytes = vfs.read(file, &error);
    if (!bytes) return {.error = error};
    return parse_environment(*bytes);
}

EnvironmentDefinition parse_environment(const std::string_view bytes) {
    EnvironmentDefinition result;
    std::string error;
    const auto text = detail::decode_text(bytes, error);
    if (!text) {
        result.error = error;
        return result;
    }

    const auto document = read_document(*text);
    const auto* world = document.child("world");
    if (!world) {
        result.error = "Environment file has no world block.";
        return result;
    }

    result.fog_distance = static_cast<float>(number_of(world->child("world_fog_distance")));
    result.fog_day_colour = colour_of(world->child("world_fog_day_colour"));
    result.fog_night_colour = colour_of(world->child("world_fog_night_colour"));

    const auto* sky = world->child("world_sky");
    if (!sky) {
        result.error = "Environment file has no world_sky block.";
        return result;
    }
    result.layers_behind_satellites =
        static_cast<std::uint16_t>(integer_of(sky->child("world_sky_nlayers_behind_satellites")));
    if (const auto* layers = sky->child("world_sky_layers")) {
        for (const auto* layer : layers->all("world_sky_layer")) result.sky_layers.push_back(read_layer(*layer));
    }
    if (const auto* satellites = sky->child("world_sky_satellites")) {
        for (const auto* satellite : satellites->all("world_sky_satellite")) {
            result.satellites.push_back(read_satellite(*satellite));
        }
    }
    // RTS (0x55859C): when the first satellite does not light the scene, every
    // later lighting satellite is swapped into slot 0 in turn; the last one wins.
    auto& satellites = result.satellites;
    if (satellites.size() > 1 && !satellites.front().light) {
        for (std::size_t i = 1; i < satellites.size(); ++i) {
            if (satellites[i].light) std::swap(satellites.front(), satellites[i]);
        }
    }

    if (const auto* ambient = world->child("world_environment_ambient_light")) result.ambient_light = colour_of(ambient);

    if (const auto* water = world->child("world_water")) {
        EnvWater parsed;
        parsed.wave_height = static_cast<float>(number_of(water->child("world_water_wave_height")));
        parsed.wave_speed = static_cast<float>(number_of(water->child("world_water_wave_speed")));
        if (const auto* layers = water->child("world_water_layers")) {
            // As many as the count says, or as there are (0x55859C).
            const auto declared = static_cast<std::size_t>((std::max)(0, integer_of(layers)));
            for (const auto* layer : layers->all("world_water_layer")) {
                if (parsed.layers.size() >= declared) break;
                EnvWaterLayer read;
                read.height = static_cast<float>(number_of(layer->child("world_water_layer_height")));
                read.sky_reflection = layer->child("world_water_layer_sky_reflection") != nullptr;
                read.shader = read_anim_shader(layer->child("world_anim_shader"));
                parsed.layers.push_back(std::move(read));
            }
        }
        result.water = std::move(parsed);
    }
    if (const auto* precipitation = world->child("world_precipitation")) {
        EnvPrecipitation parsed;
        if (const auto* type = precipitation->child("world_precipitation_type")) {
            std::string value = type->value();
            for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            parsed.snow = value.find("snow") != std::string::npos;
        }
        if (const auto* density = precipitation->child("world_precipitation_density")) {
            parsed.density = static_cast<std::uint32_t>(number_of(density, 0, 1024.0));
        }
        if (const auto* speed = precipitation->child("world_precipitation_speed")) parsed.speed = static_cast<float>(number_of(speed, 0, 5.0));
        parsed.shader = read_anim_shader(precipitation->child("world_anim_shader"));
        result.precipitation = std::move(parsed);
    }
    if (const auto* wind = world->child("world_wind")) {
        if (const auto* layers = wind->child("world_wind_layers")) {
            const auto declared = static_cast<std::size_t>((std::max)(0, integer_of(layers)));
            for (const auto* layer : layers->all("world_wind_layer")) {
                if (result.wind.size() >= declared) break;
                EnvWindLayer read;
                read.max_height = static_cast<float>(number_of(layer->child("world_wind_layer_maxheight")));
                if (const auto* direction = layer->child("world_wind_layer_direction")) {
                    for (std::size_t k = 0; k < 3; ++k) read.direction[k] = static_cast<float>(number_of(direction, k));
                }
                read.speed = static_cast<float>(number_of(layer->child("world_wind_layer_speed")));
                read.turbulence = static_cast<float>(number_of(layer->child("world_wind_layer_turbulencep")));
                result.wind.push_back(read);
            }
        }
    }
    return result;
}

} // namespace openrts::formats
