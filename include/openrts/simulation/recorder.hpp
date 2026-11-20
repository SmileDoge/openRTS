#pragma once

#include <array>
#include <cstdint>
#include <span>
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
};

struct RecorderSettings {
    std::uint32_t setup{};       // SL_SETUP
    std::uint32_t klub_setup{};  // PB_SETUP (16: САУТ's power gates it)
    bool needle = false;         // SL_SPEED1 given: the needle, clock, odometer
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
};

namespace recorder_flag {
inline constexpr std::uint32_t acts = 0x1, moving = 0x2, stood = 0x4, overdue = 0x10, vk = 0x20, clock = 0x1000;
inline constexpr std::uint32_t mask = acts | moving | stood | overdue | vk | clock;
}

struct RecorderOutput {
    std::vector<std::uint16_t> sounds;
    bool vent = false;                  // TrainPipeRate −1.2
    std::int32_t cab_aspect = -1;       // Cabin +112 set (2: the ВК on К), else −1
    std::vector<std::pair<std::int32_t, float>> values;        // a cab object's Value
    std::vector<std::pair<std::int32_t, std::uint32_t>> states; // a cab object's State
};

// Run's part, every step.
void step_recorder(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out);
// Switched's part, after Run: the switches moved.
void recorder_switched(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out);

} // namespace openrts::simulation
