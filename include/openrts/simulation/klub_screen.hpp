#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <openrts/simulation/klub.hpp>

// The КЛУБ-У's screen (its БИЛ, KLUB_DBIL), its ALSN lights (KLUB_DALS,
// KLUB_DPOM, the KLUB_B1POM .. B8POM lamps) and its lamp and backlight as
// Brakes.dll's updateKLUB draws them from its state
// (docs/reverse/brakes-dll.md §11, "Its screen"): cleared and drawn again
// at every redraw (0.5 s), cleared every step while unpowered, kept
// between. The БЛОК's screen (PB_SETUP 8) and KLUB_BILTYPE 2 are not drawn.
namespace openrts::simulation {

// A track item as RTSGetTrackItems gives it, for the screen's lines.
struct KlubTrackItem {
    std::uint8_t type{};        // 2 speed post, 3 platform, 4 siding, 5 level crossing
    float distance{};           // m from the front ahead (under the train: < 0), from the rear behind
    std::uint16_t pos{};        // 1 ahead, 2 behind, 4 under the train
    std::u16string name;        // a platform's station, a siding's name
    std::uint32_t flags{};      // a speed post's
    float value{};              // a speed post's limit, m/s
};

// What the screen shows besides the КЛУБ's state.
struct KlubScreenInputs {
    bool powered = true;
    bool epk_key = true;        // Engine::ALSNOn
    bool working_cab = true;    // its 367 working, or one cab
    float speed{};              // m/s, the engine's
    std::int32_t direction = 1; // PB_FLIP's: the cab's forward as + (-1 the other way)
    // updateKLUB runs before the brakes in Brakes.dll's Run: the УР and the
    // ТМ gauge it shows are the last step's.
    float pipe{};               // atm: the ТМ gauge's (Brakes.dll's var + 28), else the pipe
    float cylinder{};           // atm: the higher of the engine's cylinders
    float equalising{};         // atm: Engine::UR
    float acceleration{};       // m/s², Wagon::Acceleration
    float milepost{};           // km, Engine::CurrentMilepost
    float line_frequency{};     // Hz at the pantographs: 0 DC (the ALSN's 50 Hz), else 25 Hz
    bool wheelslip = false;     // Engine::WheelSlip
    double day_time{};          // s since midnight
    bool cassette = false;      // KLUB_KASS's switch on
    bool cassette_given = false;
    float saut_coefficient = -1.0f; // САУТ on (PB_SETUP 1): its braking coefficient, else < 0
    std::u16string signal_name; // the target signal's (RTSSignalData)
    // RTSGetTrackItems' entries of the player's train (the leading cab's
    // ahead first), and mdd_Libraries/Brakes/ChangeStanc.bto's names: a
    // station starting with the first (upper case) shown as the second,
    // "SKIP..." not at all.
    std::span<const KlubTrackItem> items;
    std::span<const std::pair<std::u16string, std::u16string>> station_names;
};

// An object of a screen display as the КЛУБ left it; what it never set is
// the cab's own (`*_set` false).
struct KlubScreenObject {
    std::int32_t state{};
    float value{};
    float blink{};             // its fade[2]: the blink's period, 0 none
    std::u16string text;
    bool state_set = false, value_set = false, blink_set = false, text_set = false;
};

struct KlubScreen {
    std::vector<KlubScreenObject> bil = std::vector<KlubScreenObject>(45); // KLUB_DBIL's objects 0 .. 44
    std::array<KlubScreenObject, 8> dals{}, dpom{};                         // KLUB_DALS, KLUB_DPOM
    std::array<std::int8_t, 8> lamps{};   // KLUB_B1POM .. B8POM: 0 off, 1 lit
    std::int8_t lamp = 0;                 // KLUB_PLAMP
    std::int8_t backlight = 0;            // KLUB_TCHNUM's texture
    bool drawn = false;                   // a step has been drawn
};

// This step's drawing, after step_klub of the same step.
void draw_klub(const KlubState& state, const KlubSettings& settings, const KlubScreenInputs& inputs, KlubScreen& screen);

} // namespace openrts::simulation
