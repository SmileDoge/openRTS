// openRTS's КЛУБ-У (simulation/klub.hpp, the replacement of Brakes.dll's,
// docs/brakes-dll-replacement.md step 3): each rule as
// docs/reverse/brakes-dll.md §11 has it. The whole of it is set against
// Brakes.dll's golden runs by `openrts-train-check --klub shadow`
// (openrts.content.klub_*).
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/simulation/klub.hpp>
#include <openrts/simulation/klub_screen.hpp>

namespace {

using namespace openrts::simulation;
namespace f = klub_flag;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

bool near(const double a, const double b, const double within = 0.01) { return std::abs(a - b) <= within; }

constexpr float dt = 0.05f;
constexpr float kmh = 1.0f / 3.6f;

struct Rig {
    KlubSettings settings{.setup = 7, .alsn_delay = 5.0f, .green_speed = 120.0f, .yellow_speed = 60.0f, .block_length = 1000.0f,
                          .ept = true};
    KlubState state;
    KlubOutput out;
    std::vector<KlubSignal> signals{KlubSignal{.distance = 2000.0f, .aspect = {7, 0, 0, 0}, .flags = 1}};
    float speed = 0.0f; // m/s
    float limit = 120.0f * kmh;
    float next_limit = -1.0f, next_distance = -1.0f;
    bool epk_key = true, powered = true;
    KlubInputs::ElectronicMap ek; // openRTS's map's answer (not given: Brakes.dll's way)
    std::vector<std::uint16_t> sounds;
    // One step; the signals and the next post come closer as it runs.
    void step(const std::vector<KlubPress>& keys = {}, const bool rb = false) {
        KlubInputs in;
        in.dt = dt;
        in.speed = speed;
        in.signals = signals;
        in.cab_signal = signals.empty() ? KlubSignal{} : signals.front();
        in.limit = limit;
        in.next_limit = next_limit;
        in.next_limit_distance = next_distance;
        in.powered = powered;
        in.epk_key = epk_key;
        in.keys = keys;
        in.rb = rb;
        in.ek = ek;
        step_klub(state, settings, in, out);
        sounds.insert(sounds.end(), out.sounds.begin(), out.sounds.end());
        for (auto& g : signals) g.distance -= speed * dt;
        if (!signals.empty() && signals.front().distance <= 0.0f) signals.erase(signals.begin());
        if (next_distance >= 0.0f) {
            next_distance -= speed * dt;
            if (next_distance <= 0.0f) {
                limit = next_limit;
                next_limit = next_distance = -1.0f;
            }
        }
    }
    void run(const float seconds) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += dt) step();
    }
    void press(const KlubKey key, const std::uint8_t digit = 0) { step({KlubPress{key, digit}}); }
    void track(const std::uint8_t number) {
        press(KlubKey::track);
        press(KlubKey::digit, number);
        press(KlubKey::enter);
        press(KlubKey::digit, 1);
        press(KlubKey::enter);
    }
    bool heard(const std::uint16_t code) const {
        for (const auto c : sounds)
            if (c == code) return true;
        return false;
    }
};

void test_aspect_delay() {
    Rig rig;
    rig.run(1.0f);
    check(rig.state.signal.aspect[0] == 7, "green at the start");
    rig.signals.front().aspect[0] = 3;
    rig.run(4.5f);
    check(rig.state.signal.aspect[0] == 7, "a new aspect is held for ALSN_DELAY");
    rig.run(1.0f);
    check(rig.state.signal.aspect[0] == 3, "and taken after it");
}

void test_curve_with_the_ek() {
    Rig rig;
    rig.track(1);
    check((rig.state.track & 0x7F) == 1 && (rig.state.track & 0x80), "П 1 ВВОД 1 ВВОД: track 1, the right one");
    rig.signals = {KlubSignal{.distance = 500.0f, .aspect = {1, 0, 0, 0}, .flags = 1}};
    rig.run(6.0f);
    check(rig.state.signal.aspect[0] == 1, "КЖ");
    check(near(rig.state.permitted, 40.0), "КЖ with the ЭК: 0.08 km/h a metre to the signal (500 m: 40)");
    rig.signals.front().distance = 100.0f;
    rig.run(0.1f);
    check(near(rig.state.permitted, 10.0), "at least 10");
    // A speed post at 60 ahead under a green: the curve to it.
    Rig post;
    post.track(1);
    post.next_limit = 60.0f * kmh;
    post.next_distance = 500.0f;
    post.run(0.1f);
    check(near(post.state.permitted, 60.0 + 0.08 * 500.0, 0.1), "the curve to a post: its speed + 0.08 a metre");
}

// openRTS's map: matched, the target its distance (the entered track's
// signal) with the codes' aspect; nothing for the number, "без ЭК" as with
// no track entered, the number kept.
void test_electronic_map() {
    Rig rig;
    rig.track(3);
    rig.signals = {KlubSignal{.distance = 900.0f, .aspect = {1, 0, 0, 0}, .flags = 1}};
    rig.ek = {.given = true, .matched = true, .has_target = true, .distance = 500.0f};
    rig.run(6.0f);
    check(klub_ek(rig.state) && rig.state.signal.aspect[0] == 1, "the map's ЭК: КЖ as the codes give it");
    check(near(rig.state.signal_metres, 500.0) && near(rig.state.permitted, 40.0), "the curve to the map's target (500 m: 40), not the next signal's 900 m");
    Rig off;
    off.track(7);
    off.signals.front().aspect[0] = 3;
    off.ek = {.given = true, .matched = false};
    off.run(6.0f);
    check(!klub_ek(off.state) && (off.state.track & 0x7F) == 7, "the map has nothing for 7: без ЭК, 7 kept");
    check(near(off.state.target_speed, 60.0) && near(off.state.target_distance, 1000.0), "Ж without the ЭК: KLUB_YLSPD at KLUB_BLUCH");
    off.ek.matched = true;
    off.ek.has_target = true;
    off.ek.distance = 1500.0f;
    off.run(0.1f);
    check(klub_ek(off.state), "the map has it again: the ЭК");
}

// П N ВВОД waits for its признак правильности: the track (and the ЭК's
// number with it) changes at the second ВВОД (the testers' 2026-11-21);
// 0 at once.
void test_track_waits_for_its_sign() {
    Rig rig;
    rig.track(1);
    rig.press(KlubKey::track);
    rig.press(KlubKey::digit, 3);
    rig.press(KlubKey::enter);
    check((rig.state.track & 0xFF) == 0x81 && (rig.state.flags & klub_flag::right_entry),
          "П 3 ВВОД: ПРИЗН. ПРАВ. asked, track 1 (the right one) kept");
    check(rig.state.entry == 1, "the sign's field shows the last one (1, ПР)");
    rig.press(KlubKey::digit, 0);
    check(rig.state.entry == 0, "a digit replaces it");
    rig.press(KlubKey::enter);
    check(rig.state.track == 3 && !(rig.state.flags & klub_flag::right_entry), "its sign 0 ВВОД: track 3, not the right one");
    rig.press(KlubKey::track);
    check(rig.state.entry == 3, "П: the field shows the last track (3)");
    rig.press(KlubKey::digit, 5);
    check(rig.state.entry == 5, "a digit replaces it");
    rig.press(KlubKey::enter);
    check(rig.state.track == 3 && rig.state.entry == 0, "П 5 ВВОД: still 3, the field 0 (НП)");
    rig.press(KlubKey::digit, 1);
    rig.press(KlubKey::enter);
    check(rig.state.track == (5 | 0x80u), "1 ВВОД: track 5, the right one");
    rig.press(KlubKey::track);
    rig.press(KlubKey::digit, 2);
    rig.press(KlubKey::enter);
    rig.press(KlubKey::enter);
    check(rig.state.track == (2 | 0x80u), "П 2 ВВОД ВВОД: track 2 with the last sign (ПР)");
    rig.press(KlubKey::track);
    rig.press(KlubKey::digit, 0);
    rig.press(KlubKey::enter);
    check(rig.state.track == 0 && !(rig.state.flags & klub_flag::right_entry) && !klub_ek(rig.state), "П 0 ВВОД: no track at once");
}

// The next object line (KlubScreenInputs::objects_line): a crossing 300 m
// ahead takes the target signal's place, as Brakes.dll's; with the line off
// (openRTS's, the testers' 2026-11-21) the target signal stays.
void test_objects_line() {
    Rig rig;
    rig.track(1); // odd: the objects shown
    rig.ek = {.given = true, .matched = true, .has_target = true, .distance = 1500.0f};
    rig.run(25.0f); // the screen up (its first 20 s)
    const std::vector<KlubTrackItem> items{KlubTrackItem{.type = 5, .distance = 300.0f, .pos = 1}};
    KlubScreenInputs in;
    in.items = items;
    in.signal_name = u"Н1";
    KlubScreen with, without;
    auto state = rig.state;
    state.redraw = 0.0f; // a redraw's step: the lines are drawn then
    draw_klub(state, rig.settings, in, with);
    in.objects_line = false;
    draw_klub(state, rig.settings, in, without);
    check(with.bil[21].text == u"ПЕРЕЕЗД", "the objects' line: the crossing in the target's place");
    check(without.bil[21].text != u"ПЕРЕЕЗД" && without.bil[22].text != with.bil[22].text, "off: the target signal stays");
}

void test_without_the_ek() {
    Rig rig;
    rig.signals.front().aspect[0] = 3;
    rig.run(6.0f);
    check(near(rig.state.target_speed, 60.0) && near(rig.state.target_distance, 1000.0), "Ж without the ЭК: KLUB_YLSPD at KLUB_BLUCH");
    check(near(rig.state.permitted, 120.0), "far from it");
    rig.speed = 20.0f;
    rig.run(25.0f);
    check(near(rig.state.target_distance, 500.0, 2.0), "the distance run since the aspect counts down");
    check(near(rig.state.permitted, 60.0 + 0.08 * rig.state.target_distance, 0.5), "and the curve to it");
}

void test_vigilance() {
    Rig rig;
    rig.track(1);
    rig.speed = 40.0f * kmh;
    rig.limit = 30.0f * kmh;
    rig.run(1.0f);
    check(rig.state.flags & f::over_permitted, "over the permitted speed");
    check(rig.state.flags & f::vigilance, "a request at the redraw");
    rig.step({}, true);
    check(rig.state.flags & f::vigilance, "the РБ does not answer over the permitted speed");
    rig.run(1.0f);
    check(rig.heard(56), "the whistle at 5 s (the request's timer starts at 4.9)");
    check(!rig.out.vent, "not vented yet");
    rig.run(7.0f);
    check(rig.out.vent && rig.state.timer >= 45.0f, "the ЭПК at 12 s");
    // Under the permitted speed the РБ answers.
    Rig ok;
    ok.speed = 30.0f * kmh;
    ok.run(1.0f);
    ok.state.flags |= f::vigilance;
    ok.state.timer = 6.0f;
    ok.step({}, true);
    check(!(ok.state.flags & f::vigilance) && ok.state.timer == 0.0f, "the РБ answers under the permitted speed");
    check(ok.heard(57), "the whistle off");
}

// openRTS's САУТ brakes (saut_service_braking()): over its permitted speed a
// service application of 0.5 atm, deeper after 5 s, let go 3 km/h under it.
void test_saut_service_braking() {
    KlubSettings settings{.setup = 7 | 1, .alsn_delay = 5.0f, .green_speed = 120.0f, .yellow_speed = 60.0f, .ept = true};
    KlubState state;
    KlubOutput out;
    std::vector<KlubSignal> signals{KlubSignal{.distance = 5000.0f, .aspect = {7, 0, 0, 0}, .flags = 1}};
    const std::vector<SautInputs::Cab> cabs{SautInputs::Cab{.working = true, .on = true}};
    float speed = 0.0f;
    float limit = 120.0f * kmh;
    const auto run = [&](const float seconds) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += dt) {
            KlubInputs in;
            in.dt = dt;
            in.speed = speed;
            in.signals = signals;
            in.cab_signal = signals.front();
            in.limit = limit;
            in.saut_in.own = true;
            in.saut_in.powered = true;
            in.saut_in.cabs = cabs;
            in.saut_in.equalising = 5.0f;
            step_klub(state, settings, in, out);
        }
    };
    speed = 50.0f * kmh;
    run(3.0f);
    check(out.saut_service == 0.0f, "САУТ: no braking under its permitted speed");
    limit = 30.0f * kmh;
    run(3.0f);
    check(near(out.saut_service, 0.5f), "САУТ: 0.5 atm over its permitted speed (" + std::to_string(out.saut_service) + ")");
    run(6.0f);
    check(near(out.saut_service, 0.8f), "САУТ: 0.3 atm deeper 5 s on (" + std::to_string(out.saut_service) + ")");
    speed = 20.0f * kmh;
    run(3.0f);
    check(out.saut_service == 0.0f, "САУТ: let go well under it");
    saut_service_braking() = false;
    speed = 50.0f * kmh;
    run(3.0f);
    check(out.saut_service == 0.0f, "САУТ: none with the setting off (as Brakes.dll)");
    saut_service_braking() = true;
}

void test_own_limit_clears() {
    // 120 km/h's m/s as a float, times 3.6 in double, stored in a float: the
    // limit kept at a signal (under 5 m) is let go once the train is past.
    Rig rig;
    rig.track(1);
    rig.speed = 10.0f;
    rig.signals = {KlubSignal{.distance = 3.0f, .aspect = {7, 0, 0, 0}, .flags = 1},
                   KlubSignal{.distance = 1000.0f, .aspect = {7, 0, 0, 0}, .flags = 1}};
    rig.step();
    check(rig.state.own_limit > 0.0f, "the limit kept at the signal");
    rig.run(1.0f);
    check(rig.state.own_limit == 0.0f, "and let go past it");
}

void test_kzh_then_white_is_red() {
    Rig rig;
    rig.signals.front().aspect[0] = 1;
    rig.run(6.0f);
    rig.signals = {};
    rig.run(6.0f);
    check(rig.state.signal.aspect[0] == 0, "no codes after КЖ: К, not Б");
    check(rig.state.permitted == 0.0f, "К: 0 km/h");
    rig.press(KlubKey::vk);
    check(rig.state.signal.aspect[0] == 0 && !(rig.state.flags & f::permit_20), "the ВК on К gives nothing without the РБ");
    Rig kzh;
    kzh.signals = {KlubSignal{.distance = 150.0f, .aspect = {1, 0, 0, 0}, .flags = 1}};
    kzh.run(6.0f);
    kzh.press(KlubKey::vk);
    kzh.run(0.1f);
    check(kzh.state.flags & f::permit_20, "the ВК standing on КЖ: 20 km/h");
    check(near(kzh.state.permitted, 20.0), "permitted 20");
    check(kzh.state.flags & f::stood_at_kzh, "stood within 200 m of it");
}

void test_rising_limit_waits() {
    Rig rig;
    rig.track(1);
    rig.press(KlubKey::data);
    for (int i = 0; i < 3; ++i) rig.press(KlubKey::enter);
    rig.press(KlubKey::digit, 6);
    rig.press(KlubKey::enter);
    rig.press(KlubKey::enter);
    check(rig.state.wagons == 6, "6 wagons entered");
    rig.limit = 60.0f * kmh;
    rig.run(1.0f);
    rig.limit = 120.0f * kmh;
    rig.speed = 10.0f;
    rig.run(10.0f);
    check(near(rig.state.limit, 60.0), "the old limit holds while the train runs its length (6 x 25 m)");
    rig.run(6.0f);
    check(near(rig.state.limit, 120.0), "then the new one");
}

void test_kon() {
    Rig rig;
    rig.epk_key = false;
    rig.speed = 10.0f;
    rig.run(9.0f);
    check(!rig.out.vent, "КОН: not yet");
    rig.run(2.0f);
    check(rig.out.vent && (rig.state.device & klub_device::kon_vented), "moving with the ЭПК key off: КОН vents at 10 s");
}

void test_unpowered() {
    Rig rig;
    rig.powered = false;
    rig.run(6.0f);
    check(!rig.out.vent, "unpowered with the ЭПК key on: not yet");
    rig.run(2.0f);
    check(rig.out.vent, "the ЭПК vents 7 s on");
}

void test_commands() {
    Rig rig;
    rig.press(KlubKey::command);
    rig.press(KlubKey::digit, 7);
    rig.press(KlubKey::digit, 9);
    rig.press(KlubKey::digit, 9);
    rig.press(KlubKey::enter);
    rig.run(0.6f);
    check(!(rig.state.flags & f::white_799) && (rig.state.flags & f::white_entry) && rig.state.signal.aspect[0] != 2,
          "К 799: the speed asked first, not on white yet");
    rig.press(KlubKey::digit, 2);
    rig.press(KlubKey::digit, 5);
    rig.press(KlubKey::enter);
    rig.run(0.6f);
    check(rig.state.white_speed == 25 && rig.state.signal.aspect[0] == 2 && near(rig.state.permitted, 25.0),
          "К 799, 25: on white at 25");
    rig.press(KlubKey::mp);
    rig.run(0.6f);
    check(!(rig.state.flags & f::train_mode) && near(rig.state.permitted, 60.0), "the МП on Б: the shunting mode, 60");
}

} // namespace

int main() {
    test_aspect_delay();
    test_curve_with_the_ek();
    test_electronic_map();
    test_track_waits_for_its_sign();
    test_objects_line();
    test_without_the_ek();
    test_vigilance();
    test_saut_service_braking();
    test_own_limit_clears();
    test_kzh_then_white_is_red();
    test_rising_limit_waits();
    test_kon();
    test_unpowered();
    test_commands();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "klub: ok\n";
    return 0;
}
