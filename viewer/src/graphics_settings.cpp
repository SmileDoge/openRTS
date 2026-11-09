#include "graphics_settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "viewer_state.hpp"

namespace openrts::viewer {
namespace {

[[nodiscard]] float number(const std::string_view key, const float fallback, const float low, const float high) {
    const auto value = remembered(key);
    if (!value) return fallback;
    char* end = nullptr;
    const float read = std::strtof(value->c_str(), &end);
    if (end == value->c_str()) return fallback;
    return std::clamp(read, low, high);
}

[[nodiscard]] int whole(const std::string_view key, const int fallback, const int low, const int high) {
    const auto value = remembered(key);
    if (!value) return fallback;
    return std::clamp(std::atoi(value->c_str()), low, high);
}

// "1" on, anything else off; `fallback` when not there.
[[nodiscard]] bool flag_one(const std::string_view key, const bool fallback) {
    const auto value = remembered(key);
    return value ? *value == "1" : fallback;
}

// "0" off, anything else on.
[[nodiscard]] bool flag_not_zero(const std::string_view key, const bool fallback) {
    const auto value = remembered(key);
    return value ? *value != "0" : fallback;
}

[[nodiscard]] std::string text(const bool on) { return on ? "1" : "0"; }
[[nodiscard]] std::string text(const float value) { return std::to_string(value); }
[[nodiscard]] std::string text(const int value) { return std::to_string(value); }
[[nodiscard]] std::string text(const std::uint32_t value) { return std::to_string(value); }

// Every key with its value as written.
[[nodiscard]] std::vector<std::pair<const char*, std::string>> keys_of(const GraphicsSettings& s) {
    const auto& e = s.effects;
    return {
        {"launcher_msaa", text(s.msaa)},
        {"launcher_experimental", text(s.experimental)},
        {"launcher_hdr", text(s.hdr)},
        {"tone_mode", text(s.tone_curve)},
        {"exposure", text(s.exposure)},
        {"night_exposure", text(s.night_exposure)},
        {"saturation", text(s.saturation)},
        {"effect_shadows", text(e.shadows)},
        {"effect_clusters", text(e.clustered_lights)},
        {"effect_ao", text(e.ambient_occlusion)},
        {"effect_ao_quality", e.occlusion_low ? "low" : "high"},
        {"effect_aerial", text(e.aerial_perspective)},
        {"shadow_distance", text(e.shadow_distance)},
        {"shadow_quality", text(e.shadow_map_size)},
        {"shadow_cascades", text(e.shadow_cascades)},
        {"shadow_softness", text(e.shadow_softness)},
        {"shadow_cache", text(e.cache_far_cascades)},
        {"shadow_cab", text(e.cab_shadows)},
        {"lamp_shadows", text(e.lamp_shadows)},
        {"lamps_daylight", text(e.lamps_in_daylight)},
        {"ground_bounce", text(e.ground_bounce)},
        {"light_falloff", s.physical_light ? "physical" : "rts"},
        {"light_range", text(s.light_range)},
        {"headlight_reach", text(s.headlight_reach)},
        {"night_ambient", text(s.night_ambient)},
        {"light_fullbright", text(s.light_fullbright)},
        {"lamp_own_model", text(s.lamp_own_model)},
        {"native_sky", text(s.native_sky)},
        {"cloud_cover_weather", text(s.cloud_cover_weather)},
        {"cloud_cover", text(s.cloud_cover)},
        {"cloud_shadows", text(s.cloud_shadows)},
        {"cloud_shadow_darkness", text(s.cloud_shadow_darkness)},
        {"cloud_shadow_sharpness", text(s.cloud_shadow_sharpness)},
        {"native_water", text(s.native_water)},
        {"forest_clearance", text(s.forest_clearance)},
        {"forest_clearance_distance", text(s.forest_clearance_distance)},
        {"signal_lights_real_size", text(s.signal_lights_real_size)},
        {"reshade_depth", text(s.reshade_depth)},
    };
}

} // namespace

GraphicsSettings load_graphics_settings() {
    GraphicsSettings s;
    auto& e = s.effects;
    s.msaa = whole("launcher_msaa", 1, 0, 1) != 0;
    s.experimental = whole("launcher_experimental", 0, 0, 1) != 0;
    s.hdr = whole("launcher_hdr", 0, 0, 1) != 0;
    s.tone_curve = whole("tone_mode", s.tone_curve, 0, 4);
    s.exposure = number("exposure", s.exposure, 0.25f, 4.0f);
    s.night_exposure = number("night_exposure", s.night_exposure, 0.5f, 6.0f);
    s.saturation = number("saturation", s.saturation, 0.5f, 1.8f);
    e.shadows = flag_one("effect_shadows", e.shadows);
    e.clustered_lights = flag_one("effect_clusters", e.clustered_lights);
    e.ambient_occlusion = flag_one("effect_ao", e.ambient_occlusion);
    if (const auto quality = remembered("effect_ao_quality")) e.occlusion_low = *quality == "low";
    e.aerial_perspective = flag_one("effect_aerial", e.aerial_perspective);
    e.shadow_distance = number("shadow_distance", e.shadow_distance, 100.0f, 1000.0f);
    if (const auto value = remembered("shadow_quality")) {
        const auto size = std::atoi(value->c_str());
        e.shadow_map_size = size <= 1024 ? 1024u : size >= 4096 ? 4096u : 2048u;
    }
    e.shadow_cascades = static_cast<std::uint32_t>(whole("shadow_cascades", static_cast<int>(e.shadow_cascades), 2, 4));
    e.shadow_softness = number("shadow_softness", e.shadow_softness, 0.5f, 4.0f);
    e.cache_far_cascades = flag_one("shadow_cache", e.cache_far_cascades);
    e.cab_shadows = flag_one("shadow_cab", e.cab_shadows);
    e.lamp_shadows = static_cast<std::uint32_t>(
        whole("lamp_shadows", static_cast<int>(e.lamp_shadows), 0, static_cast<int>(renderer::SceneEffects::max_lamp_shadows)));
    e.lamps_in_daylight = number("lamps_daylight", e.lamps_in_daylight, 0.0f, 1.0f);
    e.ground_bounce = number("ground_bounce", e.ground_bounce, 0.0f, 0.5f);
    if (const auto falloff = remembered("light_falloff")) s.physical_light = *falloff == "physical";
    s.light_range = number("light_range", s.light_range, 0.0f, 3000.0f);
    s.headlight_reach = number("headlight_reach", s.headlight_reach, 1.0f, 5.0f);
    s.night_ambient = number("night_ambient", s.night_ambient, 0.5f, 8.0f);
    s.light_fullbright = flag_one("light_fullbright", s.light_fullbright);
    s.lamp_own_model = flag_not_zero("lamp_own_model", s.lamp_own_model);
    s.native_sky = flag_not_zero("native_sky", s.native_sky);
    s.cloud_cover_weather = flag_not_zero("cloud_cover_weather", s.cloud_cover_weather);
    s.cloud_cover = number("cloud_cover", s.cloud_cover, 0.0f, 100.0f);
    s.cloud_shadows = flag_not_zero("cloud_shadows", s.cloud_shadows);
    s.cloud_shadow_darkness = number("cloud_shadow_darkness", s.cloud_shadow_darkness, 0.0f, 100.0f);
    s.cloud_shadow_sharpness = number("cloud_shadow_sharpness", s.cloud_shadow_sharpness, 1.0f, 10.0f);
    s.native_water = flag_not_zero("native_water", s.native_water);
    s.forest_clearance = flag_not_zero("forest_clearance", s.forest_clearance);
    s.forest_clearance_distance = number("forest_clearance_distance", s.forest_clearance_distance, 0.5f, 20.0f);
    s.signal_lights_real_size = flag_not_zero("signal_lights_real_size", s.signal_lights_real_size);
    s.reshade_depth = flag_one("reshade_depth", s.reshade_depth);
    return s;
}

void save_graphics_settings(const GraphicsSettings& now, const GraphicsSettings& before) {
    const auto wanted = keys_of(now);
    const auto had = keys_of(before);
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (wanted[i].second != had[i].second) remember(wanted[i].first, wanted[i].second);
    }
}

const char* graphics_preset_name(const GraphicsPreset preset) noexcept {
    switch (preset) {
    case GraphicsPreset::low: return "Low";
    case GraphicsPreset::medium: return "Medium";
    case GraphicsPreset::high: return "High";
    case GraphicsPreset::ultra: return "Ultra";
    }
    return "";
}

const char* graphics_preset_note(const GraphicsPreset preset) noexcept {
    switch (preset) {
    case GraphicsPreset::low:
        return "For weak video cards (GTX 1050 / 1650 class): near shadows only, no ambient occlusion, no lamp or cloud shadows.";
    case GraphicsPreset::medium:
        return "Shadows to 250 m, ambient occlusion at half resolution, one lamp shadow, cloud shadows.";
    case GraphicsPreset::high:
        return "As openRTS starts: full ambient occlusion, three lamp shadows.";
    case GraphicsPreset::ultra:
        return "For strong video cards: shadows to 500 m in 4096 maps, four cascades, eight lamp shadows.";
    }
    return "";
}

void apply_graphics_preset(GraphicsSettings& s, const GraphicsPreset preset) {
    auto& e = s.effects;
    // Every preset: MSAA (RTS's content counts on its alpha to coverage),
    // sun shadows kept for far cascades every other frame.
    s.msaa = true;
    e.shadows = true;
    switch (preset) {
    case GraphicsPreset::low:
        e.shadow_distance = 150.0f;
        e.shadow_map_size = 1024;
        e.shadow_cascades = 2;
        e.cab_shadows = false;
        e.ambient_occlusion = false;
        e.lamp_shadows = 0;
        s.cloud_shadows = false;
        break;
    case GraphicsPreset::medium:
        e.shadow_distance = 250.0f;
        e.shadow_map_size = 2048;
        e.shadow_cascades = 3;
        e.cab_shadows = true;
        e.ambient_occlusion = true;
        e.occlusion_low = true;
        e.lamp_shadows = 1;
        s.cloud_shadows = true;
        break;
    case GraphicsPreset::high:
        e.shadow_distance = 250.0f;
        e.shadow_map_size = 2048;
        e.shadow_cascades = 3;
        e.cab_shadows = true;
        e.ambient_occlusion = true;
        e.occlusion_low = false;
        e.lamp_shadows = 3;
        s.cloud_shadows = true;
        break;
    case GraphicsPreset::ultra:
        e.shadow_distance = 500.0f;
        e.shadow_map_size = 4096;
        e.shadow_cascades = 4;
        e.cab_shadows = true;
        e.ambient_occlusion = true;
        e.occlusion_low = false;
        e.lamp_shadows = 8;
        s.cloud_shadows = true;
        break;
    }
}

std::optional<GraphicsPreset> matching_graphics_preset(const GraphicsSettings& settings) {
    for (int i = 0; i < graphics_preset_count; ++i) {
        auto copy = settings;
        apply_graphics_preset(copy, static_cast<GraphicsPreset>(i));
        if (copy == settings) return static_cast<GraphicsPreset>(i);
    }
    return std::nullopt;
}

} // namespace openrts::viewer
