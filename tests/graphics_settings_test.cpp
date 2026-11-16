// The graphics settings the start menu and the game share
// (viewer/src/graphics_settings): the game's defaults are the High preset,
// every preset is found again after it is set, only changed keys are
// written, and what is read is kept in range. Its own settings folder.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../viewer/src/graphics_settings.hpp"
#include "../viewer/src/viewer_state.hpp"

namespace {

namespace fs = std::filesystem;
using namespace openrts::viewer;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

[[nodiscard]] std::string state_text() {
    std::ifstream in(user_data_directory() / "viewer_state.ini", std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

int main() {
    // A settings folder of its own (viewer_state: %LOCALAPPDATA%\openRTS).
    const auto folder = fs::temp_directory_path() / "openrts_graphics_settings_test";
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder / "openRTS");
    // An empty one there: no older settings copied in from beside the program.
    std::ofstream(folder / "openRTS" / "viewer_state.ini").close();
#ifdef _WIN32
    _putenv_s("LOCALAPPDATA", folder.string().c_str());
#else
    setenv("XDG_DATA_HOME", folder.string().c_str(), 1);
#endif

    const auto defaults = load_graphics_settings();
    check(defaults == GraphicsSettings{}, "nothing kept: the defaults");
    check(matching_graphics_preset(defaults) == GraphicsPreset::high, "the defaults are the High preset");

    for (int i = 0; i < graphics_preset_count; ++i) {
        auto settings = defaults;
        settings.hdr = true; // the look is not a preset's
        apply_graphics_preset(settings, static_cast<GraphicsPreset>(i));
        check(matching_graphics_preset(settings) == static_cast<GraphicsPreset>(i),
              std::string("a preset is found again: ") + graphics_preset_name(static_cast<GraphicsPreset>(i)));
        check(settings.hdr, "a preset leaves HDR alone");
    }
    auto custom = defaults;
    custom.effects.shadow_cascades = 4;
    check(!matching_graphics_preset(custom), "a setting changed by hand: no preset (Custom)");
    auto low = defaults;
    apply_graphics_preset(low, GraphicsPreset::low);
    check(!low.effects.ambient_occlusion && low.effects.lamp_shadows == 0 && !low.cloud_shadows && low.effects.shadow_map_size == 1024,
          "Low: no ambient occlusion, lamp or cloud shadows, 1024 maps");

    // Only what changed is written.
    auto changed = defaults;
    changed.effects.lamp_shadows = 5;
    changed.cloud_cover = 70.0f;
    save_graphics_settings(changed, defaults);
    const auto written = state_text();
    check(written.find("lamp_shadows=5") != std::string::npos && written.find("cloud_cover=70") != std::string::npos, "the changes written");
    check(written.find("shadow_distance") == std::string::npos && written.find("effect_ao") == std::string::npos,
          "the untouched defaults are not written");
    const auto read = load_graphics_settings();
    check(read == changed, "read back as written");

    // Out of range or broken values: kept in range, or the default.
    remember("shadow_cascades", "9");
    remember("shadow_distance", "5");
    remember("shadow_quality", "3000");
    remember("cloud_shadow_sharpness", "rubbish");
    remember("effect_ao_quality", "low");
    remember("light_falloff", "physical");
    remember("pbr", "0");
    const auto clamped = load_graphics_settings();
    check(clamped.effects.shadow_cascades == 4, "cascades at most 4");
    check(clamped.effects.shadow_distance == 100.0f, "shadow distance at least 100 m");
    check(clamped.effects.shadow_map_size == 2048, "a size between is 2048");
    check(clamped.cloud_shadow_sharpness == GraphicsSettings{}.cloud_shadow_sharpness, "a broken number: the default");
    check(clamped.effects.occlusion_low && clamped.physical_light, "words read");
    check(defaults.pbr && !clamped.pbr, "PBR on by default, off as kept");

    // A preset of one's own: its code and back, in a message, a file.
    OwnGraphicsPreset mine;
    mine.name = "Вечер: 4K/тени?";
    mine.settings = defaults;
    apply_graphics_preset(mine.settings, GraphicsPreset::ultra);
    mine.settings.tone_curve = 5;
    mine.settings.exposure = 1.25f;
    mine.settings.msaa = 8;
    mine.settings.smooth_train_textures = true;
    mine.settings.native_water = false;
    const auto code = graphics_preset_code(mine);
    check(code.starts_with("openRTS-graphics:1:"), "the code's prefix: " + code.substr(0, 24));
    check(code.size() < 1500, "the code fits a Discord message: " + std::to_string(code.size()));
    std::cout << "a preset's code: " << code.size() << " characters\n";
    const auto back = read_graphics_preset(code, GraphicsSettings{});
    check(back && back->name == mine.name && back->settings == mine.settings, "the code read back");
    const auto quoted = read_graphics_preset("смотри мой пресет:\n" + code + "\nпопробуй", GraphicsSettings{});
    check(quoted && quoted->settings == mine.settings, "the code in a message copied whole");
    auto broken = code;
    broken[broken.size() / 2] = broken[broken.size() / 2] == 'A' ? 'B' : 'A';
    check(!read_graphics_preset(broken, GraphicsSettings{}) || read_graphics_preset(broken, GraphicsSettings{})->settings != mine.settings,
          "a damaged code does not read as the preset");
    check(!read_graphics_preset("openRTS-graphics:1:", GraphicsSettings{}), "an empty code");
    check(!read_graphics_preset("openRTS-graphics:7:AAAA", GraphicsSettings{}), "a later version's code");
    check(!read_graphics_preset("привет, как дела", GraphicsSettings{}), "a message without a preset");
    // An older code without some keys: they stay as the player has them.
    auto base = defaults;
    base.cloud_cover = 77.0f;
    const auto partial = read_graphics_preset("name=old\nlamp_shadows=2\n", base);
    check(partial && partial->settings.effects.lamp_shadows == 2 && partial->settings.cloud_cover == base.cloud_cover, "keys not there kept");
    // The file: written under a name a file may have, read back by its name.
    check(save_own_graphics_preset(mine), "the preset's file written");
    const auto listed = load_own_graphics_presets();
    check(listed.size() == 1 && listed.front().name == mine.name && listed.front().settings == mine.settings, "the preset read from its folder");
    check(!listed.empty() && listed.front().file.filename().u8string() == u8"Вечер_ 4K_тени_.ini", "its file name");
    check(!listed.empty() && delete_own_graphics_preset(listed.front()) && load_own_graphics_presets().empty(), "the preset deleted");

    fs::remove_all(folder, error);
    if (failures == 0) std::cout << "graphics settings: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
