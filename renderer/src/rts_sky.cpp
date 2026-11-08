#include <openrts/renderer/rts_sky.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <glm/common.hpp>

namespace openrts::renderer::rts {
namespace {

constexpr double pi = 3.141592653589793;

// Tiles with Z at or above this are on RTS's "northern" branch of the
// azimuth formula.
constexpr std::int32_t northern_tile_z = 12384;

[[nodiscard]] std::uint8_t channel(const std::uint32_t argb, const int shift) {
    return static_cast<std::uint8_t>((argb >> shift) & 0xFF);
}

// Weighted sum of three colours per channel, stored as RTS does: values from
// 256 up saturate to 255, smaller ones (including negative) are truncated to a byte.
[[nodiscard]] std::uint32_t blend_colours(const std::uint32_t (&colours)[3], const double (&weights)[3]) {
    std::uint32_t result = 0xFF000000u;
    for (const int shift : {0, 8, 16}) {
        const auto value = static_cast<std::int64_t>(channel(colours[0], shift) * weights[0]
                                                     + channel(colours[1], shift) * weights[1]
                                                     + channel(colours[2], shift) * weights[2]);
        const auto byte = value < 256 ? static_cast<std::uint8_t>(value) : std::uint8_t{255};
        result |= static_cast<std::uint32_t>(byte) << shift;
    }
    return result;
}

[[nodiscard]] glm::vec3 sun_direction(const float azimuth_degrees, const float elevation_degrees) {
    return rts_sun_direction(azimuth_degrees, elevation_degrees);
}

} // namespace

const char* environment_condition(const Season season, const Weather weather) noexcept {
    static constexpr const char* conditions[4][3] = {
        {"SpringClear", "SpringSnow", "SpringRain"},
        {"SummerClear", "SummerSnow", "SummerRain"},
        {"AutumnClear", "AutumnSnow", "AutumnRain"},
        {"WinterClear", "WinterSnow", "WinterRain"},
    };
    return conditions[static_cast<int>(season) % 4][static_cast<int>(weather) % 3];
}

glm::vec4 colour_from_argb(const std::uint32_t argb) {
    constexpr float scale = 1.0f / 255.0f;
    return {channel(argb, 16) * scale, channel(argb, 8) * scale, channel(argb, 0) * scale, channel(argb, 24) * scale};
}

SatelliteState evaluate_satellite(const formats::SkySatellite& satellite,
                                  const double time_of_day,
                                  const std::int32_t camera_tile_z,
                                  const Season season) {
    SatelliteState state;
    const bool northern = camera_tile_z >= northern_tile_z;
    const double rise_position = satellite.rise_position;

    // Twilight margin around rise and set.
    auto margin = std::abs(satellite.set_time - satellite.rise_time) / 10;
    margin = northern ? static_cast<std::int32_t>((180.0 - rise_position) / 90.0 * margin)
                      : static_cast<std::int32_t>(margin * ((rise_position - 180.0) / 90.0));

    auto now = time_of_day;
    if (satellite.rise_time < 0 && now - seconds_per_day > satellite.rise_time) now -= seconds_per_day;
    if (!(satellite.rise_time - margin <= now && margin + satellite.set_time >= now)) return state;
    state.visible = true;

    const double half = std::abs(satellite.set_time - satellite.rise_time) / 2;
    const auto culmination = static_cast<std::int32_t>(satellite.rise_time + half);
    const double height = 1.0 - std::abs(culmination - now) / half; // 1 at culmination, 0 at rise/set
    const double low = 1.0 - height;

    if (northern) {
        state.azimuth_degrees = static_cast<float>(now <= culmination ? (180.0 - rise_position) * height + rise_position
                                                                      : rise_position * low + 180.0);
    } else {
        state.azimuth_degrees = static_cast<float>(now <= culmination ? rise_position - rise_position * low
                                                                      : -height * rise_position);
    }

    // Maximum elevation follows the tile row (a crude latitude) and season.
    double culmination_elevation = (0x4000 - camera_tile_z) * 0.000125 * 180.0;
    if (culmination_elevation > 90.0) culmination_elevation = 180.0 - culmination_elevation;
    if (season == Season::summer) {
        culmination_elevation += 22.5;
    } else if (season == Season::winter) {
        culmination_elevation -= 22.5;
    }
    state.elevation_degrees = static_cast<float>(std::sin(height * pi / 2.0) * culmination_elevation);
    state.scale = static_cast<float>((satellite.low_scale * low + satellite.high_scale * height) * 0.01);

    // Colour weights for rise, high and set colours.
    double weights[3]{};
    if (margin == 0) {
        weights[1] = 1.0; // RTS would divide by zero here
    } else if (satellite.rise_time > now) {
        weights[0] = 1.0 - static_cast<double>(satellite.rise_time - now) / margin;
    } else if (satellite.set_time < now) {
        weights[2] = 1.0 - static_cast<double>(now - satellite.set_time) / margin;
    } else if (std::abs(now - culmination) >= 3 * margin) {
        if (now >= culmination) {
            weights[1] = static_cast<double>(satellite.set_time - now) / margin;
            weights[2] = 1.0 - weights[1];
        } else {
            weights[1] = static_cast<double>(now - satellite.rise_time) / margin;
            weights[0] = 1.0 - weights[1];
        }
    } else {
        weights[1] = 1.0;
    }
    state.ambient_colour = blend_colours(satellite.ambient_colours, weights);
    state.direct_colour = blend_colours(satellite.direct_colours, weights);
    return state;
}

SkyLayerState evaluate_layer_fade(const formats::SkyLayer& layer, const double time_of_day) {
    SkyLayerState state;
    auto now = time_of_day;
    if (layer.fade_in_start < 0 && layer.fade_in_start + seconds_per_day < now) now -= seconds_per_day;

    if (layer.fade_in_start != 0 && layer.fade_in_end > now) {
        const auto duration = static_cast<double>(layer.fade_in_end - layer.fade_in_start);
        auto value = duration != 0.0 ? (layer.fade_in_end - now) / duration : 0.0;
        value = value <= 1.0 ? 1.0 - value : 0.0;
        state.alpha = static_cast<float>(value);
        state.fading = true;
    }
    if (layer.fade_out_start != 0 && layer.fade_out_start < now) {
        const auto duration = static_cast<double>(layer.fade_out_end - layer.fade_out_start);
        auto value = duration != 0.0 ? (now - layer.fade_out_start) / duration : 0.0;
        value = value <= 1.0 ? 1.0 - value : 0.0;
        state.alpha = static_cast<float>(value);
        state.fading = true;
    }
    state.visible = state.alpha > 0.0f;
    return state;
}

SkySimulation::SkySimulation(const formats::EnvironmentDefinition& environment)
    : environment_(&environment), uv_offsets_(environment.sky_layers.size(), {0.0f, 0.0f}) {}

SkyState SkySimulation::update(const double time_of_day,
                               const float elapsed,
                               const std::int32_t camera_tile_z,
                               const Season season) {
    SkyState state;
    const auto& environment = *environment_;

    for (const auto& satellite : environment.satellites) {
        state.satellites.push_back(evaluate_satellite(satellite, time_of_day, camera_tile_z, season));
    }
    if (!environment.satellites.empty()) {
        const auto& sun_definition = environment.satellites.front();
        auto now = time_of_day;
        if (sun_definition.rise_time < 0 && now - seconds_per_day > sun_definition.rise_time) now -= seconds_per_day;
        state.night = sun_definition.rise_time > now || sun_definition.set_time < now;
    }

    // The first satellite is the sun and provides Light[0] and the sky colours.
    const auto sun = state.satellites.empty() ? SatelliteState{} : state.satellites.front();
    const auto direct = colour_from_argb(sun.direct_colour);
    auto ambient = colour_from_argb(sun.ambient_colour);
    state.world_lit = (ambient.r + ambient.g + ambient.b + direct.r + direct.g + direct.b) / 6.0f;
    const auto ambient_floor = colour_from_argb(environment.ambient_light.value_or(default_ambient_light));
    ambient = glm::vec4{glm::max(glm::vec3{ambient}, glm::vec3{ambient_floor}), ambient.w};

    state.lighting.sun_diffuse = direct;
    state.lighting.sun_ambient = ambient;
    state.lighting.sun_direction = sun_direction(sun.azimuth_degrees, sun.elevation_degrees);
    state.clear_colour = direct;

    const float fog_fade = sun.elevation_degrees >= -0.04f ? 1.0f : (std::max)(sun.elevation_degrees * 0.5f + 1.0f, 0.0f);
    state.fog_colour = direct * fog_fade;

    state.fog_end = 500.0f;
    for (std::size_t i = 0; i < environment.sky_layers.size(); ++i) {
        const auto& layer = environment.sky_layers[i];
        auto fade = evaluate_layer_fade(layer, time_of_day);

        if (!layer.shader.frames.empty() && layer.shader.frames.front().has_uv_scroll) {
            const auto& frame = layer.shader.frames.front();
            for (int axis = 0; axis < 2; ++axis) {
                auto& offset = uv_offsets_[i][axis];
                offset += frame.uv_scroll[axis] * elapsed;
                if (std::fabs(offset) > 2.0f) offset = std::fmod(offset, 2.0f);
            }
        }
        fade.uv_offset[0] = uv_offsets_[i][0];
        fade.uv_offset[1] = uv_offsets_[i][1];

        if (fade.visible && !layer.edge_steps.empty()) {
            state.fog_end = (std::max)(state.fog_end, static_cast<float>(layer.edge_steps.back().radius) - 100.0f);
        }
        state.layers.push_back(fade);
    }
    return state;
}

} // namespace openrts::renderer::rts
