#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// openRTS's speed recorder (3СЛ-2М) and the ALSN vigilance of a cab without
// the КЛУБ, for the cabs with `SL_SPEED1` (docs/brakes-dll-replacement.md
// "The rest", 4): the
// needle, the clock, the odometer, the periodic vigilance by the aspect with
// its whistle (56 / 57, 106), its lamps (`SL_PSS`, `SL_HL`, `SL_PR`), the
// ЭПК venting the pipe when overdue or when the train runs with the ALSN
// off. Brakes.dll's then runs without it (`SL_SPEED1` −1).
namespace openrts::simulation {

// The recorder's parameters (SL_*, PB_*), each cab's ids.
struct RecorderCab {
    std::int32_t epk_key = -1, rb = -1, rbs = -1;  // PB_EPKKEY, PB_RB, PB_RBS
    std::int32_t als = -1, h = -1, dvij = -1;      // SL_ALS, SL_H, SL_DVIJ
    std::int32_t vk = -1, zavod = -1, kid = -1;    // SL_VK, SL_ZAVOD (the clock wound), BR_KID
    std::int32_t speed = -1, hour = -1, minute = -1; // SL_SPEED, SL_HOUR, SL_MIN
    std::array<std::int32_t, 6> odometer{-1, -1, -1, -1, -1, -1}; // SL_ODOM1 .. 6
    std::int32_t pss = -1, hl = -1, pr = -1;        // SL_PSS, SL_HL, SL_PR lamps
    // The КПД-3's (SL_KPD*<n>): its speed gauge, power and light switches,
    // its screens (one, SL_KPDMODE 0; two, mode 1), its lamps (power,
    // check, the light's two), the light's texture change, its keys 1 .. 7.
    std::int32_t kpd_speed = -1, kpd_power = -1, kpd_light = -1;
    std::int32_t kpd_screen = -1, kpd_upper = -1, kpd_lower = -1;
    std::int32_t kpd_lamp_power = -1, kpd_lamp_check = -1, kpd_lamp_dark = -1, kpd_lamp_lit = -1, kpd_texture = -1;
    std::array<std::int32_t, 7> kpd_keys{-1, -1, -1, -1, -1, -1, -1};
    std::int32_t ukbm_power = -1; // SL_UKBMPW<n>: the УКБМ's power switch (SL_SETUP 8)
    // The МУ blocks' (SL_MU*<n>): lamps lit while on (L1P, L2P), lamps
    // always off (L1P1, L2P1), the keys' lamps and keys 1 .. 7, power
    // switches (SW1P, SW2P), the cassette and its lamp, the vigilance lamps
    // (PSS, PSSP), the screens (1DISP the permitted speed, 2DISP the speed,
    // and their second copies), the switch for 115 on Ж (SL_MUMPID).
    std::int32_t mu_l1p = -1, mu_l2p = -1, mu_l1p1 = -1, mu_l2p1 = -1;
    std::array<std::int32_t, 7> mu_lamps{-1, -1, -1, -1, -1, -1, -1};
    std::array<std::int32_t, 7> mu_keys{-1, -1, -1, -1, -1, -1, -1};
    std::int32_t mu_power1 = -1, mu_power2 = -1, mu_cassette = -1, mu_cassette_lamp = -1;
    std::int32_t mu_pss = -1, mu_pss2 = -1, mu_disp1 = -1, mu_disp1b = -1, mu_disp2 = -1, mu_disp2b = -1, mu_fast = -1;
};

struct RecorderSettings {
    std::uint32_t setup{};       // SL_SETUP
    std::uint32_t klub_setup{};  // PB_SETUP (16: САУТ's power gates it)
    bool needle = false;         // SL_SPEED1 given: the needle, clock, odometer
    // The КПД-3 in its place (SL_KPDSPD1 given, no SL_SPEED1): its screens
    // instead, the same vigilance. SL_KPDMODE; PB_FLIP (the cab whose
    // acceleration's sign is turned, from 0; -1 none).
    bool kpd = false;
    std::uint32_t kpd_mode{};
    // The МУ blocks in their place (SL_SETUP 0x10, neither): they do it all.
    // SL_MUSPEED: the permitted speed on aspects 6 and 7.
    bool mu = false;
    float mu_speed = 100.0f;
    std::int32_t flip = -1;
    std::vector<RecorderCab> cabs;
};

// A cab's switches as Run saw them, and after it (Switched).
struct RecorderCabInputs {
    bool epk_key = false, als = false, rb = false, rbs = false, h = false, dvij = false, kid = false;
    // Moved after this step's Run: to a state not 0 (РБ, РБС, ВК), any (the
    // clock's key), the ЭПК key to off.
    bool rb_pressed = false, rbs_pressed = false, vk_pressed = false, zavod_moved = false, epk_key_off = false;
    bool epk_key_moved = false;
    bool epk_key_to = false;
    // The КПД-3: its power and light switches on; its keys moved to a state
    // not 0 this step (bit k for key k + 1).
    bool kpd_power = false, kpd_light = false;
    bool ukbm_power = false; // the УКБМ's power switch on
    // The МУ's: on (either power switch, one not given counts on), its keys
    // held, 115 on Ж, the cassette in.
    bool mu_power = false, mu_fast = false, mu_cassette = false;
    std::array<bool, 7> mu_held{};
    std::uint8_t kpd_pressed{};
};

struct RecorderInputs {
    float dt{};
    float speed{};          // Wagon +68 (velocity, m/s)
    float cylinder{};       // Wagon +96
    float independent{};    // Wagon +52
    float main_reservoir{}; // Locomotive +404
    std::int32_t reverser{};// Engine +128
    bool traction = false;  // Engine +136 not 0
    std::uint32_t aspect{}; // Cabin +112 after the ALSN delay
    bool powered = false;   // the ALSN on (the train's DLL's, the КЛУБ's)
    bool saut_powered = false;
    bool tskbm_on = false;  // ТСКБМ on
    bool bypass = false;    // bypassed
    float ep_pressure{};    // the ЭПТ's pressure
    double day_time{};      // RTSGetInteger 1, s
    float random{};         // the needle's error this step, m/s ((rand / 4096 − 6) × 0.2778)
    std::span<const RecorderCabInputs> cabs;
    float acceleration{};      // Wagon +72, m/s² (the КПД-3's)
    bool tape_recording = false; // RTSGetInteger 7 == 3: the КПД-3's check lamp steady
    // The МУ's: the coordinate (Engine +140, km), the computer's date (Brakes.dll's GetLocalTime).
    double milepost{};
    long long date_year{}, date_month{}, date_day{}, date_weekday{};
    // Switched's sounds too (Brakes.dll's Switched does not post them: not
    // loaded, or a check beside it).
    bool switched_sounds = true;
};

// Brakes.dll's variables the recorder keeps (their offsets).
struct RecorderState {
    float kon_timer{};      // + 16: running with the ALSN off and the brakes off
    float vented{};         // + 20: 1 once it vents
    float needle_timer{};   // + 36: the needle each second
    float periodic{};       // + 44: the vigilance timer
    float aspect_seen{};    // + 48: the aspect last seen
    float whistle{};        // + 52: 0, 1 whistling, 2 a request (the aspect fell)
    float blink{};          // + 56: SL_PSS's blink
    float distance{};       // + 224: m run
    std::uint32_t flags = 0x1; // + 4 (Init 0x1): 0x1 the driver acts, 0x2 moving, 0x4 stood after a request, 0x10 overdue, 0x20 ВК, 0x1000 the clock wound
    bool whistle_flag = false; // + 8 0x10000000
    bool epk_bit = false;      // + 32 bit 1
    bool alsn_on = false;      // Engine +106
    // openRTS: s after the ALSN came on during which an aspect is taken as
    // it is, not as a worse one (the first after the start: hot, the КЖ till
    // the ALSN's delay passed read as a fall from none, a request РБ could
    // not answer, the ЭПК vented 7 s on; testers 2026-11-26, the ВЛ85).
    // Only the first time after the train appeared: the key turned on later
    // (standing at a КЖ) asks as Brakes.dll's does.
    float start_grace{};
    bool ever_on = false;
    // The КПД-3's (mode 1): + 164 its page and flags (1 lit by its key, 2
    // the acceleration, 4 the clock, 8 the test "1000", 0x20 the menu, 0x40
    // / 0x80 the date, number 1, number 2), + 168 .. + 180 day, month and the
    // two numbers.
    std::uint32_t kpd_flags = 0;
    std::uint32_t kpd_day = 0, kpd_month = 0, kpd_number1 = 0, kpd_number2 = 0;
    // The МУ's (the same slots): + 164 1 self-test done, 2 the clock, 4
    // near the limit; + 168 the self-test's time left; + 172 m run on КЖ.
    std::uint32_t mu_flags = 0;
    float mu_test = 0.0f, mu_kzh_run = 0.0f;
};

namespace recorder_flag {
inline constexpr std::uint32_t acts = 0x1, moving = 0x2, stood = 0x4, overdue = 0x10, vk = 0x20, clock = 0x1000;
inline constexpr std::uint32_t mask = acts | moving | stood | overdue | vk | clock | 0x78000u;
}
// The УКБМ's (+ 4): a check missed (the lamp SL_PR), the РБС needed after
// the РБ, its window (30 .. 35 s) this step, the short cycle.
namespace ukbm_flag {
inline constexpr std::uint32_t missed = 0x8000, needs_rbs = 0x10000, window = 0x20000, short_cycle = 0x40000;
}

struct RecorderOutput {
    std::vector<std::uint16_t> sounds;
    bool vent = false;                  // TrainPipeRate −1.2
    std::int32_t cab_aspect = -1;       // Cabin +112 set (2: the ВК on К), else −1
    std::vector<std::pair<std::int32_t, float>> values;        // a cab object's Value
    std::vector<std::pair<std::int32_t, std::uint32_t>> states; // a cab object's State
    // The КПД-3's: a lamp's blink (its Fade's second value, s; 0 steady), a
    // texture change's state, a screen's sub-object (its text, its shown
    // state: 0 shown, -1 not; its blink).
    std::vector<std::pair<std::int32_t, float>> blinks;
    std::vector<std::pair<std::int32_t, std::uint16_t>> textures;
    struct ScreenPart {
        std::int32_t screen = -1;
        std::uint32_t part{};
        bool text_set = false, state_set = false, blink_set = false;
        std::u16string text;
        std::int32_t state{};
        float blink{};
    };
    std::vector<ScreenPart> screen_parts;
};

// Run's part, every step.
void step_recorder(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out);
// Switched's part, after Run: the switches moved.
void recorder_switched(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out);

} // namespace openrts::simulation
