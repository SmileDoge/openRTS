#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// openRTS's КЛУБ-У in place of MaDaDev's Brakes.dll's
// (docs/brakes-dll-replacement.md, step 3; what Brakes.dll does:
// docs/reverse/brakes-dll.md §11). It does what Brakes.dll's does, step
// for step: the signal from RTS's ALSN arrays, the limits, the permitted
// speed and its curve, the vigilance requests, the whistle and the ЭПК,
// the keys and the commands. The screen and the lamps are drawn from its
// state by the caller. Its state keeps Brakes.dll's variables by their
// meaning (the offsets in the comments), so a run can be set against a
// golden run of Brakes.dll (openrts-train-check --klub shadow).
namespace openrts::simulation {

// A signal of the ALSN array of the running direction (RTS's SignalsInfo).
struct KlubSignal {
    float distance{};             // m
    std::uint32_t aspect[4]{};    // NORMAL, DISTANCE, SHUNTING, INFO heads' states
    std::uint32_t flags{};        // 1 NORMAL, 2 DISTANCE, 0x10 beyond an obstruction; 0x20: Б forced
    float speed_limit = -1.0f;    // m/s, -1 none
};

// A track item as RTSGetTrackItems gives it (the screen's lines, САУТ's
// announcements).
struct KlubTrackItem {
    std::uint8_t type{};        // 2 speed post, 3 platform, 4 siding, 5 level crossing
    float distance{};           // m from the front ahead (under the train: < 0), from the rear behind
    std::uint16_t pos{};        // 1 ahead, 2 behind, 4 under the train
    std::u16string name;        // a platform's station, a siding's name
    std::uint32_t flags{};      // a speed post's
    float value{};              // a speed post's limit, m/s
};

// САУТ's keys (SAUT_*ID<n>).
enum class SautKey : std::uint8_t { kom, gr, yk, k20, pod, otp };

// What САУТ (PB_SETUP 1, docs/brakes-dll-replacement.md "The rest", 3) reads besides
// the КЛУБ's: openRTS's (`own`) runs in the КЛУБ's step in place of
// Brakes.dll's.
struct SautInputs {
    bool own = false;
    bool powered = false;        // every SAUT_PIT on
    bool high = false;           // SAUT_MODE 1, or SAUT_MODET's switch at SAUT_MODEON: 162 km/h at most (else 102)
    bool clock = false;          // PB_SETUP 16: the clock on its screen
    float acceleration{};        // Wagon +72, m/s²
    float cylinder{};            // Wagon +96, atm
    bool ept = false;            // Engine +104 bit 2
    std::uint32_t brake_bits{};  // the valve in a braking position
    std::int32_t reverser{};     // Engine +128
    bool traction = false;       // Engine +136 not 0
    float equalising{};          // Engine +84
    float milepost{};            // Engine +140, km (CurrentMilepost)
    std::u16string_view signal_name; // Cabin +136's (RTSSignalData +16)
    std::span<const KlubTrackItem> items; // RTSGetTrackItems'
    bool flipped = false;        // the car's Wagon::Flags bit 0 (RTSGetTrackItems swaps ahead and behind)
    std::span<const SautKey> keys;        // the working cab's pressed (CanSwitch)
    // Each cab (BR_NUMCABS): working (the only one, or its 367 in
    // BR_367OPPOL), SAUT_SWID on, SAUT_OSID on, KLUB_KASS on; after this
    // step's Run (Switched): SAUT_SWID turned on, PB_RB moved.
    struct Cab {
        bool working = false, on = false, os = false, cassette = false;
        bool turned_on = false, rb_moved = false;
    };
    std::span<const Cab> cabs;
    std::int32_t flip = -1; // PB_FLIP: the cab (from 0) whose forward is the car's back
};

// A САУТ display's objects as one step left them (SAUT_DBID's 0 .. 5,
// SAUT_DFID's 0 .. 4); what it did not set is the cab's.
struct SautObject {
    std::int32_t state{};
    float blink{};
    std::u16string text;
    bool state_set = false, blink_set = false, text_set = false;
};
struct SautScreen {
    std::array<SautObject, 6> db{};
    std::array<SautObject, 5> df{};
};

// The КЛУБ's keys (KLUB_B*ID).
enum class KlubKey : std::uint8_t {
    digit,     // B0 .. B9 (KlubPress::digit)
    plus,      // BPL
    minus,     // BMN
    reset,     // BSB: the entry 0
    command,   // BK (К)
    enter,     // BVV (ВВОД)
    track,     // BP (П)
    data,      // BL: the train's data
    vk,        // BVK (ВК)
    mp,        // BMP (МП)
    backlight, // BPS
    info,      // BI
};
struct KlubPress {
    KlubKey key{};
    std::uint8_t digit{};
};

// The КЛУБ's parameters (the BRAKES library's AuxLibraryParameters).
struct KlubSettings {
    std::uint32_t setup = 7;     // PB_SETUP: 1 САУТ, 2 the КЛУБ, 4 ТСКБМ and the periodic vigilance, 8 the БЛОК
    float alsn_delay = 0.0f;     // ALSN_DELAY, s
    float green_speed = -1.0f;   // KLUB_GRSPD, km/h (-1 none)
    float yellow_speed = -1.0f;  // KLUB_YLSPD, km/h
    float block_length = 1000.0f; // KLUB_BLUCH, m
    bool ept = false;            // BR_EPT: a wagon 25 m (else 14) for the rising limits
    bool has_screen = true;      // KLUB_DBIL given (the target's beep)
    bool cassette_given = false; // KLUB_KASS given: ВВОД's confirmation needs the cassette in
    bool block_mode = false;     // BLOCK_MODE (the БЛОК's whistle codes)
    std::uint32_t screen_type = 0;  // KLUB_BILTYPE (2: the ЭП2К's screen)
    bool block_scale_150 = false;   // BLOCK_MODE 1: the БЛОК's bars to 150 km/h (else 300)
};

// What the КЛУБ gets this step.
struct KlubInputs {
    float dt{};
    float speed{}; // m/s, the engine's (Wagon::Velocity as the DLL has it)
    // RTS's ALSN call (the cab has scripted signal lights): the running
    // direction's array (empty: none), Cabin::Signal as RTS set it before
    // (the leading cab: the next signal, else aspect 0; another cab: 8),
    // cars ahead of the engine (no codes), the train's lowest limit, the
    // next speed post's limit and distance.
    bool alsn = true;
    std::span<const KlubSignal> signals;
    KlubSignal cab_signal{.aspect = {0, 0, 0, 0}};
    bool cars_ahead = false;
    float limit = -1.0f, next_limit = -1.0f, next_limit_distance = -1.0f; // m/s, m/s, m
    // The cab.
    bool powered = true;   // every KLUB_PIT on
    bool epk_key = true;   // the ЭПК key (Engine::ALSNOn)
    bool rb_held = false;  // the РБ down (ВК, МП)
    bool tskbm = false;    // ТСКБМ switched on (TSKBM_SW of any cab): no periodic vigilance
    bool saut = false;     // САУТ switched on (SAUT_SWID; PB_SETUP 1)
    float saut_permitted{}; // its permitted speed, km/h
    bool wheelslip = false; // Engine::WheelSlip
    float cylinder{};      // the higher of the engine's cylinders, atm (КОН)
    double day_time{};     // s since midnight (command 4)
    bool cassette = false; // KLUB_KASS's switch on (the ЭК's cassette in)
    // The keys pressed before this step (CanSwitch: they act at once).
    std::span<const KlubPress> keys;
    // Switched after this step's Run (RTS moves a cab switch after the
    // DLLs' Run): the РБ / РБС moved either way, the ЭПК key turned to
    // `epk_key_to` (Engine::ALSNOn from then: `epk_key` the next step).
    bool rb = false, rbs = false;
    bool epk_key_turned = false, epk_key_to = false;
    // A switch of the car moved after this step's Run (Brakes.dll's
    // Switched): ТСКБМ's timer starts again.
    bool switched = false;
    // Engine::ALSNOn as this step's Run found it (the train's DLL sets it
    // too, as the ЭП20 started "on"): ТСКБМ's; -1 as `epk_key`.
    int alsn_on = -1;
    // САУТ (PB_SETUP 1): openRTS's with `saut_in.own` (then `saut` and
    // `saut_permitted` above are its own), else Brakes.dll's.
    SautInputs saut_in;
    // openRTS's electronic map (electronic_map.hpp) for the entered track,
    // the leading cab's: the ЭК mode or "без ЭК", and the distance to the
    // map's target (the entered track's signal, whatever way the switches
    // lie). Not given: Brakes.dll's way (a number entered is the ЭК, the
    // target the next signal as the switches stand).
    struct ElectronicMap {
        bool given = false;
        bool matched = false;
        bool has_target = false;
        float distance{}; // m
    } ek;
};

// var + 8 (KlubState::flags).
namespace klub_flag {
inline constexpr std::uint32_t command_mode = 0x1, kgf = 0x2, lamp_test = 0x4, vigilance = 0x8, tskbm = 0x10,
                               line14_cylinder = 0x20, white_799 = 0x40, white_809 = 0x80, second_set = 0x100, dps = 0x200,
                               train_mode = 0x400, double_traction = 0x800, permit_20 = 0x1000, white_entry = 0x2000,
                               saut_diagnostics = 0x4000, track_entry = 0x8000, right_entry = 0x10000, backlight = 0x20000,
                               stood_at_kzh = 0x80000, traction_262 = 0x100000, epk_test = 0x200000, overspeed = 0x400000,
                               warning = 0x800000, epk_vented = 0x1000000, white_permit = 0x2000000, fresh_entry = 0x4000000,
                               developer = 0x8000000, over_permitted = 0x10000000;
}
// var + 4's bits the КЛУБ keeps (KlubState::device).
namespace klub_device {
inline constexpr std::uint32_t tskbm_on = 0x8, saut_on = 0x40, kon_vented = 0x200, kon = 0x400, reversed = 0x800,
                               tskbm_whistle = 0x2000, after_kzh = 0x100000, distance_white = 0x200000, tskbm_warning = 0x400000;
}

struct KlubState {
    // Cabin::Signal as the КЛУБ leaves it: the aspect shown (0 К, 1 КЖ,
    // 2 Б, 3 .. 5 Ж, 6 .. 7 З, 8 none), its distance, limit, flags.
    KlubSignal signal{.aspect = {7, 0, 0, 0}};
    std::uint32_t device{};               // var + 4 (klub_device)
    std::uint32_t flags = klub_flag::train_mode; // var + 8 (klub_flag)
    std::uint32_t marks{};                // var + 128: 1 the target's beep, 2 the curve's, 4 КЖ's request, 8 the slip's, 0x20 Б's
    std::uint32_t limit_marks{};          // var + 88: 0x4000 the signal's limit 0, 0x40000 a rising limit held
    float timer{};                        // + 24: vigilance (5 the whistle, 12 the ЭПК, 45 vented)
    float limit{};                        // + 80, km/h
    float signal_from_50{};               // + 84
    float target_speed{}, target_distance{}; // + 116, + 120
    float permitted{};                    // + 140, km/h
    float signal_metres{};                // + 144
    std::uint32_t entry{};                // + 132
    float message_time{};                 // + 136, s (a command's message: 2 s)
    std::uint32_t white_speed = 40;       // + 148, km/h
    float periodic{};                     // + 152, s
    float tone{};                         // + 156, s
    std::uint32_t track{};                // + 160: the track number (0x7F), 0x80 the right track
    std::uint32_t driver{}, train_number{}, axles{}, wagons{}, mass{}; // + 164 .. + 180
    std::uint32_t field{};                // + 184: the train's data field (1 .. 5)
    float rise_run{}, accepted_limit{};   // + 188, + 192
    float white_start = -1.0f;            // + 196
    float run{};                          // + 200, m (powered)
    float time_shown = -1.0f;             // + 208 (command 4)
    float refresh{}, slip_time{}, shown_speed{}; // + 212, + 216, + 220
    std::uint32_t alsen_table{}, alsen_count{}; // + 240, + 244
    float redraw{}, blink{}, powered_time{}; // + 284, + 300, + 304
    float yellow_run{};                   // + 332, m
    float own_limit{};                    // + 336, km/h
    // _Run's: the ALSN delay, the aspect before, the ЭПК part's last aspect
    // and АЛС-ЕН count, КОН.
    std::uint32_t accepted_aspect = 7, accepted_count{}; // + 72, + 248
    float delay_timer{};                  // + 68 (ALSN_DELAY at the start)
    std::uint32_t previous_aspect{};      // + 64
    std::uint32_t epk_aspect = 7;         // + 48
    std::uint32_t count_seen{};           // + 228
    float kon_timer{};                    // + 16, s
    bool kon_vented = false;              // + 20
    bool epk_bit = false;                 // var + 32 bit 1
    bool saut_rules = false;              // САУТ's speed in place of the КЛУБ's (the screen's "С", "САУТ" on the БЛОК)
    // Command 799 waits for its speed: on white only once it is entered (the
    // real КЛУБ-У's way; Brakes.dll's is on white at once, at the speed
    // before). openRTS's own, not Brakes.dll's.
    bool white_799_entry = false;
    std::uint32_t drawn_flags = klub_flag::train_mode; // `flags` where the redraw shows the request (before its overspeed check)
    bool started = false;
    // The map gives nothing for the entered track ("без ЭК" with a number
    // entered: none of the map's, 99, off its way; KlubInputs::ek).
    bool ek_off = false;
    // ТСКБМ (PB_SETUP 4; docs/brakes-dll-replacement.md "The rest", 2):
    // its timer (s; 90 at the start and on any switch, 60 in the
    // developer's mode) and the timer as this step's screens show it
    // (before its fall).
    float tskbm_timer = 90.0f;
    float tskbm_shown = 90.0f;
    // САУТ (PB_SETUP 1), openRTS's: its flags (beside the КЛУБ's
    // limit_marks), its timers and values.
    std::uint32_t saut_flags{};           // + 88 (0x1 .. 0x20000 but the КЛУБ's)
    float saut_display_timer{};           // + 92
    float saut_reminder{};                // + 96
    float saut_target{};                  // + 100, km/h
    float saut_mode_distance{};           // + 104, m
    float saut_mode_start{};              // + 108 (the coordinate, km × 1000... as Brakes.dll keeps it)
    float saut_mode_time{};               // + 112, s
    std::uint32_t saut_aspect{};          // + 124: the aspect last announced
    float saut_coefficient_timer{};       // + 252
    float saut_shown{};                   // + 256, km/h
    float saut_stop_timer{};              // + 260
    float saut_clock{};                   // + 288 (0.3 s)
    float saut_caught{};                  // + 292: the УР where it caught up
    float saut_distance{};                // + 308, m
    std::uint32_t saut_coefficient = 33;  // Engine +120 (Flags): Init's 33, 67 in the 162 km/h mode
    bool saut_started = false;
    bool saut_braking = false;            // its braking (kept between its redraws)
};

// The ЭК mode: a track entered and the map (when there is one) has it.
[[nodiscard]] inline bool klub_ek(const KlubState& s) noexcept { return (s.track & 0x7F) != 0 && !s.ek_off; }

struct KlubOutput {
    bool vent = false;            // the ЭПК or КОН vents the pipe (TrainPipeRate -1.2, EPTValue -1)
    // The КЛУБ wrote var + 32 bit 1 this step (KlubState::epk_bit): its
    // trip sets it, an answer with the timer running and the ЭПК key off
    // clear it. Brakes.dll's other parts set it too, so it is written back
    // only then.
    bool epk_bit_written = false;
    bool safety_powered = false;  // var + 32 bit 4
    bool saut_braking = false;    // САУТ's braking (the train's DLLs cut traction)
    // САУТ's displays this step, a cab's each (openRTS's САУТ).
    std::vector<SautScreen> saut_screens;
    std::vector<std::uint16_t> sounds; // its SMS triggers (56 / 57, 3192 .. 3199)
};

// One step of the КЛУБ in RTS's and Brakes.dll's order: the keys
// (CanSwitch), ALSN, the ALSN delay, updateKLUB, the ЭПК part of Run, then
// the switches' Switched (the РБ, the ЭПК key).
void step_klub(KlubState& state, const KlubSettings& settings, const KlubInputs& inputs, KlubOutput& out);

} // namespace openrts::simulation
