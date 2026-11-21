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
// at every redraw (0.5 s; 0.33 s on the БЛОК), cleared every step while
// unpowered, kept between. The БЛОК's screen (PB_SETUP 8: objects 0 .. 9
// and 35 .. 42 its own way) and KLUB_BILTYPE 2's (the ЭП2К's: the speed and
// the distance as bars, 35 .. 127) as it draws them too.
namespace openrts::simulation {

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
    // Brakes.dll's own parts, which go on running beside openRTS's КЛУБ:
    float tskbm_timer = 100.0f;     // ТСКБМ's (var + 12; under 7 the vigilance sign, the БЛОК's 35 / 36)
    float block_value{};            // var + 296 (the ЭП20's DLL writes it): the БЛОК's object 37
    float saut_distance{};          // САУТ's distance (var + 308): the БЛОК's object 5
    bool saut_caught = false;       // САУТ caught up with its speed: "СРЫВ ЭПК САУТ"
    std::u16string signal_name; // the target signal's (RTSSignalData)
    // RTSGetTrackItems' entries of the player's train (the leading cab's
    // ahead first), and mdd_Libraries/Brakes/ChangeStanc.bto's names: a
    // station starting with the first (upper case) shown as the second,
    // "SKIP..." not at all.
    std::span<const KlubTrackItem> items;
    bool tskbm_switch = false; // this cab's TSKBM_SW on
    // The next object line (21 / 22, its distance 20): a platform, a
    // crossing or a limit within 500 m in place of the target signal, as
    // Brakes.dll's; off, the target signal only (openRTS's setting, off in
    // the game: the testers, 2026-11-21, could not see which signal the ЭК
    // took).
    bool objects_line = true;
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
    std::vector<KlubScreenObject> bil = std::vector<KlubScreenObject>(128); // KLUB_DBIL's objects 0 .. 127
    std::array<KlubScreenObject, 8> dals{}, dpom{};                         // KLUB_DALS, KLUB_DPOM
    std::array<std::int8_t, 8> lamps{};   // KLUB_B1POM .. B8POM: 0 off, 1 lit
    std::int8_t lamp = 0;                 // KLUB_PLAMP
    std::int8_t backlight = 0;            // KLUB_TCHNUM's texture
    // ТСКБМ's (PB_SETUP 4): TSKBM_DISP's objects 0 .. 2, TSKBM_PIT, L1 ..
    // L3 (0 off, 1 lit) and L2's blink (its Fade's second value, s).
    std::array<KlubScreenObject, 3> tskbm{};
    std::int8_t tskbm_power = 0;
    std::array<std::int8_t, 3> tskbm_lamps{};
    float tskbm_l2_blink{};
    bool drawn = false;                   // a step has been drawn
};

// This step's drawing, after step_klub of the same step.
void draw_klub(const KlubState& state, const KlubSettings& settings, const KlubScreenInputs& inputs, KlubScreen& screen);

} // namespace openrts::simulation
