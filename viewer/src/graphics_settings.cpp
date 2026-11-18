#include "graphics_settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "viewer_state.hpp"

namespace openrts::viewer {
namespace {

// Where a key's value comes from: viewer_state.ini, or a preset's text.
using Lookup = std::function<std::optional<std::string>(std::string_view)>;

[[nodiscard]] float number(const Lookup& value_of, const std::string_view key, const float fallback, const float low, const float high) {
    const auto value = value_of(key);
    if (!value) return fallback;
    char* end = nullptr;
    const float read = std::strtof(value->c_str(), &end);
    if (end == value->c_str()) return fallback;
    return std::clamp(read, low, high);
}

[[nodiscard]] int whole(const Lookup& value_of, const std::string_view key, const int fallback, const int low, const int high) {
    const auto value = value_of(key);
    if (!value) return fallback;
    return std::clamp(std::atoi(value->c_str()), low, high);
}

// "1" on, anything else off; `fallback` when not there.
[[nodiscard]] bool flag_one(const Lookup& value_of, const std::string_view key, const bool fallback) {
    const auto value = value_of(key);
    return value ? *value == "1" : fallback;
}

// "0" off, anything else on.
[[nodiscard]] bool flag_not_zero(const Lookup& value_of, const std::string_view key, const bool fallback) {
    const auto value = value_of(key);
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
        {"msaa_samples", text(s.msaa)},
        {"launcher_experimental", text(s.experimental)},
        {"launcher_hdr", text(s.hdr)},
        {"tone_mode", text(s.tone_curve)},
        {"exposure", text(s.exposure)},
        {"night_exposure", text(s.night_exposure)},
        {"auto_exposure", text(s.auto_exposure.on)},
        {"auto_exposure_strength", text(s.auto_exposure.strength)},
        {"auto_exposure_down", text(s.auto_exposure.down)},
        {"auto_exposure_up", text(s.auto_exposure.up)},
        {"auto_exposure_speed", text(s.auto_exposure.speed)},
        {"saturation", text(s.saturation)},
        {"effect_shadows", text(e.shadows)},
        {"effect_clusters", text(e.clustered_lights)},
        {"effect_ao", text(e.ambient_occlusion)},
        {"effect_ao_quality", e.occlusion_low ? "low" : "high"},
        {"effect_aerial", text(e.aerial_perspective)},
        {"effect_bloom", text(e.bloom)},
        {"bloom_strength", text(e.bloom_strength)},
        {"bloom_textures", text(e.bloom_textures)},
        {"effect_shafts", text(e.light_shafts)},
        {"shafts_strength", text(e.light_shafts_strength)},
        {"shadow_distance", text(e.shadow_distance)},
        {"shadow_quality", text(e.shadow_map_size)},
        {"shadow_cascades", text(e.shadow_cascades)},
        {"shadow_softness", text(e.shadow_softness)},
        {"shadow_cache", text(e.cache_far_cascades)},
        {"shadow_cab", text(e.cab_shadows)},
        {"lamp_shadows", text(e.lamp_shadows)},
        {"lamps_daylight", text(e.lamps_in_daylight)},
        {"ground_bounce", text(e.ground_bounce)},
        {"indoor_light", text(e.indoor_light)},
        {"sky_light_strength", text(e.sky_light_strength)},
        {"sky_reflection_strength", text(e.sky_reflection_strength)},
        {"sky_reflection_indoors", text(e.sky_reflection_indoors)},
        {"sky_light_tunnel", text(e.sky_light_tunnel)},
        {"parallax", text(e.parallax)},
        {"parallax_distance", text(e.parallax_distance)},
        {"light_falloff", s.physical_light ? "physical" : "rts"},
        {"pbr", text(s.pbr)},
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
        {"night_textures", text(s.night_textures)},
        {"screen_supersampling", text(s.screen_supersampling)},
        {"reshade_depth", text(s.reshade_depth)},
        {"smooth_train_textures", text(s.smooth_train_textures)},
        {"compress_textures", text(s.compress_textures)},
        {"texture_size", text(s.texture_size)},
    };
}

// The keys `value_of` has, over `s` (the others left as they are).
void read_settings(GraphicsSettings& s, const Lookup& value_of) {
    auto& e = s.effects;
    if (const auto samples = value_of("msaa_samples")) {
        s.msaa = valid_msaa_samples(std::atoi(samples->c_str()));
    } else if (const auto before = value_of("launcher_msaa")) {
        s.msaa = *before == "0" ? 1u : 4u; // the old switch: off or 4x
    }
    s.experimental = whole(value_of, "launcher_experimental", 0, 0, 1) != 0;
    s.hdr = whole(value_of, "launcher_hdr", 0, 0, 1) != 0;
    s.tone_curve = whole(value_of, "tone_mode", s.tone_curve, 0, 5);
    s.exposure = number(value_of, "exposure", s.exposure, 0.25f, 4.0f);
    s.night_exposure = number(value_of, "night_exposure", s.night_exposure, 0.5f, 6.0f);
    s.auto_exposure.on = flag_one(value_of, "auto_exposure", s.auto_exposure.on);
    s.auto_exposure.strength = number(value_of, "auto_exposure_strength", s.auto_exposure.strength, 0.0f, 1.0f);
    s.auto_exposure.down = number(value_of, "auto_exposure_down", s.auto_exposure.down, 0.0f, 3.0f);
    s.auto_exposure.up = number(value_of, "auto_exposure_up", s.auto_exposure.up, 0.0f, 3.0f);
    s.auto_exposure.speed = number(value_of, "auto_exposure_speed", s.auto_exposure.speed, 0.1f, 5.0f);
    s.saturation = number(value_of, "saturation", s.saturation, 0.5f, 1.8f);
    e.shadows = flag_one(value_of, "effect_shadows", e.shadows);
    e.clustered_lights = flag_one(value_of, "effect_clusters", e.clustered_lights);
    e.ambient_occlusion = flag_one(value_of, "effect_ao", e.ambient_occlusion);
    if (const auto quality = value_of("effect_ao_quality")) e.occlusion_low = *quality == "low";
    e.bloom = flag_not_zero(value_of, "effect_bloom", e.bloom);
    e.bloom_strength = number(value_of, "bloom_strength", e.bloom_strength, 0.0f, 3.0f);
    e.bloom_textures = flag_not_zero(value_of, "bloom_textures", e.bloom_textures);
    e.light_shafts = flag_one(value_of, "effect_shafts", e.light_shafts);
    e.light_shafts_strength = number(value_of, "shafts_strength", e.light_shafts_strength, 0.0f, 4.0f);
    e.aerial_perspective = flag_one(value_of, "effect_aerial", e.aerial_perspective);
    e.shadow_distance = number(value_of, "shadow_distance", e.shadow_distance, 100.0f, 1000.0f);
    if (const auto value = value_of("shadow_quality")) {
        const auto size = std::atoi(value->c_str());
        e.shadow_map_size = size <= 1024 ? 1024u : size >= 4096 ? 4096u : 2048u;
    }
    e.shadow_cascades = static_cast<std::uint32_t>(whole(value_of, "shadow_cascades", static_cast<int>(e.shadow_cascades), 2, 4));
    e.shadow_softness = number(value_of, "shadow_softness", e.shadow_softness, 0.5f, 4.0f);
    e.cache_far_cascades = flag_one(value_of, "shadow_cache", e.cache_far_cascades);
    e.cab_shadows = flag_one(value_of, "shadow_cab", e.cab_shadows);
    e.lamp_shadows = static_cast<std::uint32_t>(
        whole(value_of, "lamp_shadows", static_cast<int>(e.lamp_shadows), 0, static_cast<int>(renderer::SceneEffects::max_lamp_shadows)));
    e.lamps_in_daylight = number(value_of, "lamps_daylight", e.lamps_in_daylight, 0.0f, 1.0f);
    e.ground_bounce = number(value_of, "ground_bounce", e.ground_bounce, 0.0f, 0.5f);
    e.indoor_light = number(value_of, "indoor_light", e.indoor_light, 0.0f, 1.0f);
    e.sky_light_strength = number(value_of, "sky_light_strength", e.sky_light_strength, 0.0f, 2.0f);
    e.sky_reflection_strength = number(value_of, "sky_reflection_strength", e.sky_reflection_strength, 0.0f, 2.0f);
    e.sky_reflection_indoors = number(value_of, "sky_reflection_indoors", e.sky_reflection_indoors, 0.0f, 1.0f);
    e.sky_light_tunnel = number(value_of, "sky_light_tunnel", e.sky_light_tunnel, 0.0f, 0.5f);
    e.parallax = flag_one(value_of, "parallax", e.parallax);
    e.parallax_distance = number(value_of, "parallax_distance", e.parallax_distance, 5.0f, 100.0f);
    if (const auto falloff = value_of("light_falloff")) s.physical_light = *falloff == "physical";
    s.pbr = flag_not_zero(value_of, "pbr", s.pbr);
    s.light_range = number(value_of, "light_range", s.light_range, 0.0f, 3000.0f);
    s.headlight_reach = number(value_of, "headlight_reach", s.headlight_reach, 1.0f, 5.0f);
    s.night_ambient = number(value_of, "night_ambient", s.night_ambient, 0.5f, 8.0f);
    s.light_fullbright = flag_one(value_of, "light_fullbright", s.light_fullbright);
    s.lamp_own_model = flag_not_zero(value_of, "lamp_own_model", s.lamp_own_model);
    s.native_sky = flag_not_zero(value_of, "native_sky", s.native_sky);
    s.cloud_cover_weather = flag_not_zero(value_of, "cloud_cover_weather", s.cloud_cover_weather);
    s.cloud_cover = number(value_of, "cloud_cover", s.cloud_cover, 0.0f, 100.0f);
    s.cloud_shadows = flag_not_zero(value_of, "cloud_shadows", s.cloud_shadows);
    s.cloud_shadow_darkness = number(value_of, "cloud_shadow_darkness", s.cloud_shadow_darkness, 0.0f, 100.0f);
    s.cloud_shadow_sharpness = number(value_of, "cloud_shadow_sharpness", s.cloud_shadow_sharpness, 1.0f, 10.0f);
    s.native_water = flag_not_zero(value_of, "native_water", s.native_water);
    s.forest_clearance = flag_not_zero(value_of, "forest_clearance", s.forest_clearance);
    s.forest_clearance_distance = number(value_of, "forest_clearance_distance", s.forest_clearance_distance, 0.5f, 20.0f);
    s.signal_lights_real_size = flag_not_zero(value_of, "signal_lights_real_size", s.signal_lights_real_size);
    s.night_textures = flag_not_zero(value_of, "night_textures", s.night_textures);
    s.screen_supersampling = flag_not_zero(value_of, "screen_supersampling", s.screen_supersampling);
    s.reshade_depth = flag_one(value_of, "reshade_depth", s.reshade_depth);
    s.smooth_train_textures = flag_one(value_of, "smooth_train_textures", s.smooth_train_textures);
    s.compress_textures = whole(value_of, "compress_textures", s.compress_textures, 0, 2);
    s.texture_size = whole(value_of, "texture_size", s.texture_size, 0, 3);
}

// Characters a file name may not have, and control characters: "_".
[[nodiscard]] std::string file_name_of(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out += (u < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) ? '_' : c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("preset") : out;
}

[[nodiscard]] std::filesystem::path from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

[[nodiscard]] std::string utf8_of(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

constexpr std::string_view base64_letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] std::string base64url(const std::string_view bytes) {
    std::string out;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : bytes) {
        bits = (bits << 8) | static_cast<unsigned char>(c);
        count += 8;
        while (count >= 6) {
            count -= 6;
            out += base64_letters[(bits >> count) & 63u];
        }
    }
    if (count > 0) out += base64_letters[(bits << (6 - count)) & 63u];
    return out;
}

[[nodiscard]] std::optional<std::string> from_base64url(const std::string_view text) {
    std::string out;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        const auto at = base64_letters.find(c);
        if (at == std::string_view::npos) return std::nullopt;
        bits = (bits << 6) | static_cast<std::uint32_t>(at);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xFFu);
        }
    }
    return out;
}

[[nodiscard]] bool code_letter(const char c) { return base64_letters.find(c) != std::string_view::npos; }

} // namespace

GraphicsSettings load_graphics_settings() {
    GraphicsSettings s;
    read_settings(s, [](const std::string_view key) { return remembered(key); });
    return s;
}

std::filesystem::path graphics_presets_directory() {
    auto folder = user_data_directory() / "graphics_presets";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return folder;
}

std::string graphics_preset_text(const OwnGraphicsPreset& preset) {
    std::string out = "; openRTS graphics preset\nname=" + preset.name + "\n";
    for (const auto& [key, value] : keys_of(preset.settings)) out += std::string(key) + "=" + value + "\n";
    return out;
}

std::string graphics_preset_code(const OwnGraphicsPreset& preset) {
    const auto text = graphics_preset_text(preset);
    uLongf packed_size = compressBound(static_cast<uLong>(text.size()));
    std::string packed(packed_size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(packed.data()), &packed_size, reinterpret_cast<const Bytef*>(text.data()),
                  static_cast<uLong>(text.size()), 9)
        != Z_OK)
        return {};
    packed.resize(packed_size);
    // The text's length first (4 bytes, little end first), for inflating it.
    std::string body;
    const auto length = static_cast<std::uint32_t>(text.size());
    for (int i = 0; i < 4; ++i) body += static_cast<char>((length >> (8 * i)) & 0xFFu);
    body += packed;
    return std::string(graphics_preset_code_prefix) + "1:" + base64url(body);
}

std::optional<OwnGraphicsPreset> read_graphics_preset(const std::string_view text, const GraphicsSettings& base) {
    std::string plain;
    if (const auto at = text.find(graphics_preset_code_prefix); at != std::string_view::npos) {
        auto rest = text.substr(at + graphics_preset_code_prefix.size());
        if (!rest.starts_with("1:")) return std::nullopt; // a later version's
        rest.remove_prefix(2);
        std::size_t end = 0;
        while (end < rest.size() && code_letter(rest[end])) ++end;
        const auto body = from_base64url(rest.substr(0, end));
        if (!body || body->size() < 5) return std::nullopt;
        std::uint32_t length = 0;
        for (int i = 0; i < 4; ++i) length |= static_cast<std::uint32_t>(static_cast<unsigned char>((*body)[i])) << (8 * i);
        if (length == 0 || length > 1u << 16) return std::nullopt;
        plain.assign(length, '\0');
        uLongf size = length;
        if (uncompress(reinterpret_cast<Bytef*>(plain.data()), &size, reinterpret_cast<const Bytef*>(body->data() + 4),
                       static_cast<uLong>(body->size() - 4))
                != Z_OK
            || size != length)
            return std::nullopt;
    } else {
        plain.assign(text);
    }
    std::map<std::string, std::string, std::less<>> values;
    std::istringstream lines(plain);
    for (std::string line; std::getline(lines, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        line.erase(0, start);
        if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3); // a BOM
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos || equals == 0) continue;
        values[line.substr(0, equals)] = line.substr(equals + 1);
    }
    // At least one setting's key: else it is not a preset.
    bool any = false;
    for (const auto& [key, value] : keys_of(base)) any = any || values.contains(std::string_view(key));
    if (!any) return std::nullopt;
    OwnGraphicsPreset preset;
    if (const auto name = values.find("name"); name != values.end()) preset.name = name->second;
    preset.settings = base;
    read_settings(preset.settings, [&values](const std::string_view key) -> std::optional<std::string> {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    });
    return preset;
}

std::vector<OwnGraphicsPreset> load_own_graphics_presets() {
    std::vector<OwnGraphicsPreset> out;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(graphics_presets_directory(), error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".ini") continue;
        if (entry.file_size(error) > (1u << 16)) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        auto preset = read_graphics_preset(text.str(), GraphicsSettings{});
        if (!preset) continue;
        if (preset->name.empty()) preset->name = utf8_of(entry.path().stem());
        preset->file = entry.path();
        out.push_back(std::move(*preset));
    }
    std::sort(out.begin(), out.end(), [](const OwnGraphicsPreset& a, const OwnGraphicsPreset& b) { return a.name < b.name; });
    return out;
}

bool save_own_graphics_preset(const OwnGraphicsPreset& preset) {
    const auto path = graphics_presets_directory() / from_utf8(file_name_of(preset.name) + ".ini");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << graphics_preset_text(preset);
    return static_cast<bool>(out);
}

bool delete_own_graphics_preset(const OwnGraphicsPreset& preset) {
    std::error_code error;
    const auto path = preset.file.empty() ? graphics_presets_directory() / from_utf8(file_name_of(preset.name) + ".ini") : preset.file;
    return std::filesystem::remove(path, error);
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
    // Every preset: sun shadows, kept for far cascades every other frame.
    // MSAA stays as chosen (1, 2, 4 or 8: a choice of its own).
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
