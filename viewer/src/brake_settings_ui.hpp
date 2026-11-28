#pragma once

// The brakes' settings, one section for the start menu's Settings page and
// the game's Train window (2026-11-22, testers: the two had different
// settings and players looked for them in the wrong one). On top one
// choice, the preset, and a line of what it gives; under "Details" the
// settings one by one, each shown only where it works. All remembered
// (viewer_state.hpp) under the keys the game has used since 2026-11-13.

#include <string>

namespace openrts::viewer {

struct BrakeSettings {
    bool realistic = true;     // realistic_air: openRTS's air model
    int dll_air = 2;           // dll_air: trains with locomotive DLLs, 0 RTS's air, 1 the DLL's valve, 2 openRTS's valve
    bool gauges = true;        // gauges_openrts_air: the cab's air gauges from the model
    int rails = 1;             // rails: 0 as RTS, 1 by the weather, 2.. dry, wet, snow, ice, leaves
    bool limit = true;         // limit_brake_force: at most real stock's
    bool stability = true;     // distributor_stability (the RTS model)
    int condition = 6;         // brake_condition: 0 as the files, 1..10
    int pressing = 100;        // brake_pressing, %
    int own_brakes = 1;        // own_brakes: 1 openRTS's cab brake devices
    int brake_numbers = 0;     // brake_numbers: 1 the books'
    int own_klub = 1;          // own_klub: 1 openRTS's КЛУБ
    bool no_brakes_dll = true; // no_brakes_dll: not loaded where all of it is openRTS's (from the next train)
    bool klub_objects = false; // klub_objects
    bool saut_brakes = true;   // saut_brakes: openRTS's САУТ brakes (simulation::saut_service_braking())
    bool klub_map = true;      // klub_map
    bool operator==(const BrakeSettings&) const = default;
};

enum class BrakePreset { openrts, rts, own };

// The preset's settings (the КЛУБ's object line and map stay as they are).
[[nodiscard]] BrakeSettings brake_preset(BrakePreset preset, const BrakeSettings& now);
[[nodiscard]] BrakePreset brake_preset_of(const BrakeSettings& settings);

[[nodiscard]] BrakeSettings load_brake_settings();
void remember_brake_settings(const BrakeSettings& settings);

// What the game knows of the driven train, shown beside the settings
// (empty: nothing to show; the start menu gives none).
struct BrakeSettingsStatus {
    std::string train;     // its brakes, КЛУБ and Brakes.dll now
    std::string rails;     // the rails by the weather now
    std::string map;       // the ЭК's map
    std::string condition; // this train's wear
    std::string pressing;  // this train's shoes' force
};

// Draws the section (ImGui); remembers what was changed. True when
// `settings` changed. `label_width`: its labels' column, as the page's.
bool draw_brake_settings(BrakeSettings& settings, const BrakeSettingsStatus* status, float label_width = 200.0f);

} // namespace openrts::viewer
