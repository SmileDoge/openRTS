#pragma once

// The graphics settings the start menu and the game's panel both show, as
// viewer_state.ini keeps them (the keys the game reads at its start), with
// the game's defaults and ranges; and the presets that set the costly ones
// together.

#include <cstdint>
#include <optional>

#include <openrts/renderer/scene_effects.hpp>

namespace openrts::viewer {

// MSAA samples as the menu offers them: 1 (off), 2, 4, 8; anything else to
// the nearest below (at least 1).
[[nodiscard]] constexpr std::uint32_t valid_msaa_samples(const int wanted) noexcept {
    return wanted >= 8 ? 8u : wanted >= 4 ? 4u : wanted >= 2 ? 2u : 1u;
}

struct GraphicsSettings {
    // The start: the menu passes them on the game's command line.
    // msaa_samples (until 2026-11-14 launcher_msaa: 0 off, 1 4x): 4 by default,
    // as RTS's "4x MSAA +Alpha" (its content counts on the alpha to coverage).
    std::uint32_t msaa = 4;
    bool experimental = false;  // launcher_experimental: the new renderer
    bool hdr = false;           // launcher_hdr
    // HDR (the new renderer): tone_mode (0 none, 1 Khronos, 2 AgX, 3 ACES,
    // 4 Khronos by day and AgX by night), exposure, night_exposure, saturation.
    int tone_curve = 4;
    float exposure = 1.0f;
    float night_exposure = 2.0f;
    float saturation = 1.0f;
    // The new renderer's effects: effect_shadows, effect_clusters, effect_ao,
    // effect_ao_quality, effect_aerial, shadow_distance, shadow_quality,
    // shadow_cascades, shadow_softness, shadow_cache, shadow_cab,
    // lamp_shadows, lamps_daylight, ground_bounce.
    renderer::SceneEffectSettings effects;
    // Lamps and headlights (the new renderer).
    bool physical_light = false;    // light_falloff: physical / rts
    float light_range = 0.0f;       // m; 0: RTS's MaxLightDistance
    float headlight_reach = 1.0f;   // times the .eng's
    float night_ambient = 1.0f;     // the sky's light at night, a factor
    bool light_fullbright = false;  // FullBright lit as ordinary surfaces
    bool lamp_own_model = true;     // a lamp throws no shadow of its own model
    // Sky and water (openRTS's own, not as in RTS).
    bool native_sky = true;
    bool cloud_cover_weather = true;
    float cloud_cover = 42.0f;            // %
    bool cloud_shadows = true;            // the new renderer
    float cloud_shadow_darkness = 85.0f;  // %
    float cloud_shadow_sharpness = 3.0f;  // 1..10
    bool native_water = true;
    // The world.
    bool forest_clearance = true;
    float forest_clearance_distance = 3.0f; // m
    bool signal_lights_real_size = true;
    // The cab screens each drawn into an image of their own, blended in
    // linear light (renderer::cab_screens_supersampled); off: as RTS.
    bool screen_supersampling = true;
    bool reshade_depth = false;

    bool operator==(const GraphicsSettings&) const = default;
};

// As remembered (the defaults above where a key is not there).
[[nodiscard]] GraphicsSettings load_graphics_settings();
// The keys whose values `now` has other than `before` (so a default not
// touched stays a default, and changes with a later version).
void save_graphics_settings(const GraphicsSettings& now, const GraphicsSettings& before);

// Presets: the costly settings together (shadows, ambient occlusion, lamp
// shadows, cloud shadows); MSAA (a choice of its own since there are four)
// and the look (HDR, exposure, lamps' strength, sky and water on or off)
// are left as they are.
enum class GraphicsPreset { low, medium, high, ultra };
inline constexpr int graphics_preset_count = 4;
// English keys for tr(): the name and what it is for.
[[nodiscard]] const char* graphics_preset_name(GraphicsPreset preset) noexcept;
[[nodiscard]] const char* graphics_preset_note(GraphicsPreset preset) noexcept;
void apply_graphics_preset(GraphicsSettings& settings, GraphicsPreset preset);
// The preset the settings are, none when changed by hand ("Custom").
[[nodiscard]] std::optional<GraphicsPreset> matching_graphics_preset(const GraphicsSettings& settings);

} // namespace openrts::viewer
