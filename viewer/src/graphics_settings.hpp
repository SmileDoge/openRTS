#pragma once

// The graphics settings the start menu and the game's panel both show, as
// viewer_state.ini keeps them (the keys the game reads at its start), with
// the game's defaults and ranges; and the presets that set the costly ones
// together.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/renderer/auto_exposure.hpp>
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
    // HDR output to an HDR display (launcher_hdr_output; DirectX 12, Windows's
    // HDR on): the paper white and the peak, nits (hdr_paper_white, hdr_peak).
    bool hdr_output = false;
    float hdr_paper_white = 200.0f;
    float hdr_peak = 1000.0f;
    // HDR (the new renderer): tone_mode (0 none, 1 Khronos, 2 AgX, 3 ACES,
    // 4 Khronos by day and AgX by night, 5 ACES by day and AgX by night),
    // exposure, night_exposure, saturation.
    int tone_curve = 4;
    float exposure = 1.0f;
    float night_exposure = 1.5f;
    float saturation = 1.0f;
    // Auto exposure (PBR stage 3): auto_exposure, auto_exposure_strength,
    // auto_exposure_down, auto_exposure_up, auto_exposure_speed.
    renderer::AutoExposureSettings auto_exposure;
    // The new renderer's effects: effect_shadows, effect_clusters, effect_ao,
    // effect_ao_quality, effect_aerial, shadow_distance, shadow_quality,
    // shadow_cascades, shadow_softness, shadow_cache, shadow_cab,
    // lamp_shadows, lamps_daylight, ground_bounce, indoor_light,
    // sky_light_strength, sky_reflection_strength, sky_reflection_indoors,
    // sky_light_tunnel.
    renderer::SceneEffectSettings effects;
    // Lamps and headlights (the new renderer).
    bool physical_light = false;    // light_falloff: physical / rts
    bool pbr = true;                // pbr: physically based light (renderer::pbr_shading, with HDR)
    float light_range = 0.0f;       // m; 0: RTS's MaxLightDistance
    float headlight_reach = 1.0f;   // times the .eng's
    float night_ambient = 1.0f;     // the sky's light at night, a factor
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
    bool grass = true;           // openRTS's grass (docs/grass.md)
    float grass_density = 1.0f;  // 0.25..2
    float grass_reach = 45.0f;   // m, 20..120
    bool signal_lights_real_size = true;
    // The models' night textures (TEXTURES\NIGHT, ESD_Alternative_Texture)
    // at night, as RTS; off: the day's, lit by the lamps (night_textures).
    bool night_textures = true;
    // The cab screens each drawn into an image of their own, blended in
    // linear light (renderer::cab_screens_supersampled); off: as RTS.
    bool screen_supersampling = true;
    bool reshade_depth = false;
    // The trains' DXT1 textures smoothed and kept as BC7 (renderer/bc7.hpp,
    // smooth_train_textures; openRTS's own, not as in RTS).
    bool smooth_train_textures = false;
    // RTS's uncompressed textures and the materials' maps in BC7 / BC5
    // (renderer/bc7.hpp: a quarter of the video memory; openRTS's own):
    // 0 not (as RTS), 1 encoded each load, 2 kept on disk too.
    int compress_textures = 2;
    // The models' textures at most so many texels a side
    // (renderer::texture_size_limit; openRTS's own): 0 as the files give
    // them, 1 1024, 2 512, 3 256.
    int texture_size = 0;

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

// A preset of one's own, to pass to other players: a name and every setting
// above. Kept in graphics_presets/ in user_data_directory() as <name>.ini
// (viewer_state.ini's keys, one key=value a line, "name=" first), and passed
// as a line of text, its code: "openRTS-graphics:1:" and the file's text
// deflated, in base64url (about 600 characters: a Discord message holds it).
struct OwnGraphicsPreset {
    std::string name;
    GraphicsSettings settings;
    std::filesystem::path file; // where it was read from (empty: not from a file)
};
inline constexpr std::string_view graphics_preset_code_prefix = "openRTS-graphics:";
// The folder, made when missing.
[[nodiscard]] std::filesystem::path graphics_presets_directory();
// Every *.ini there that reads as a preset, by name (a file without
// "name=": its file name).
[[nodiscard]] std::vector<OwnGraphicsPreset> load_own_graphics_presets();
// Written as <name>.ini (characters a file name may not have: "_"),
// replacing one of that name. False when it could not be written.
bool save_own_graphics_preset(const OwnGraphicsPreset& preset);
bool delete_own_graphics_preset(const OwnGraphicsPreset& preset);
// The file's text and the code.
[[nodiscard]] std::string graphics_preset_text(const OwnGraphicsPreset& preset);
[[nodiscard]] std::string graphics_preset_code(const OwnGraphicsPreset& preset);
// A code (found anywhere in `text`: a message copied whole) or a preset
// file's text. Keys not there keep `base`'s values (a code of an older
// version). None when it reads as neither.
[[nodiscard]] std::optional<OwnGraphicsPreset> read_graphics_preset(std::string_view text, const GraphicsSettings& base);

} // namespace openrts::viewer
