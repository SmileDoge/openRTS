// openRTS's speed recorder's parts in place of Brakes.dll's
// (simulation/recorder.hpp): the УКБМ's periodic check (SL_SETUP 8) and the
// КПД-3's screen (SL_KPDSPD1).
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/simulation/recorder.hpp>

namespace {

using namespace openrts::simulation;

int failures = 0;

void check(const bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

constexpr float dt = 1.0f / 30.0f;

bool has(const std::vector<std::uint16_t>& sounds, const std::uint16_t code) {
    for (const auto s : sounds)
        if (s == code) return true;
    return false;
}

// A cab with the УКБМ powered, the ALSN on, running on a green (aspect 3).
struct Ukbm {
    RecorderSettings k;
    RecorderState s;
    std::vector<RecorderCabInputs> cabs = std::vector<RecorderCabInputs>(1);
    RecorderInputs in;
    RecorderOutput out;
    Ukbm() {
        k.setup = 8;
        k.needle = true;
        RecorderCab c;
        c.epk_key = 1;
        c.rb = 2;
        c.rbs = 3;
        c.ukbm_power = 4;
        k.cabs.push_back(c);
        cabs[0].epk_key = true;
        cabs[0].ukbm_power = true;
        in.dt = dt;
        in.speed = 15.0f;
        in.reverser = 1;
        in.aspect = 3;
        in.powered = true;
        in.ep_pressure = 1.0f;
        in.main_reservoir = 8.0f;
        s.flags |= recorder_flag::acts;
    }
    // `seconds` of steps; true when the whistle sounded.
    bool run(const float seconds) {
        bool whistled = false;
        for (float t = 0.0f; t < seconds; t += dt) {
            in.cabs = cabs;
            step_recorder(s, k, in, out);
            whistled = whistled || has(out.sounds, 56);
            recorder_switched(s, k, in, out);
            for (auto& c : cabs) c.rb_pressed = c.rbs_pressed = false;
            s.flags |= recorder_flag::acts;
        }
        return whistled;
    }
};

void ukbm() {
    {
        Ukbm u;
        u.run(31.0f);
        check((u.s.flags & ukbm_flag::window) != 0, "УКБМ: past 30 s its window");
        u.cabs[0].rb = u.cabs[0].rb_pressed = true;
        u.run(dt);
        check(u.s.periodic < 1.0f && !(u.s.flags & ukbm_flag::missed), "УКБМ: the РБ in the window passes the check");
    }
    {
        Ukbm u;
        check(u.run(36.0f), "УКБМ: missed, the whistle at 35 s");
        check((u.s.flags & ukbm_flag::missed) != 0, "УКБМ: missed, the lamp");
        u.run(8.0f);
        check((u.s.flags & recorder_flag::overdue) && u.out.vent, "УКБМ: past 42 s the ЭПК vents");
    }
    {
        // Missed, then the РБ then the РБС clear it; the next checks shorter.
        Ukbm u;
        u.run(36.0f);
        u.cabs[0].rb = u.cabs[0].rb_pressed = true;
        u.run(dt);
        check((u.s.flags & ukbm_flag::needs_rbs) != 0, "УКБМ: missed, the РБ asks for the РБС");
        check(u.run(27.0f), "УКБМ: missed, the next check shorter (the whistle at 25 s)");
        u.cabs[0].rbs = u.cabs[0].rbs_pressed = true;
        u.run(dt);
        check(!(u.s.flags & ukbm_flag::needs_rbs) && !(u.s.flags & ukbm_flag::missed), "УКБМ: the РБС while whistling clears it");
    }
    {
        // Powered off with the ALSN on: the whistle at once, the ЭПК in some 7 s.
        Ukbm u;
        u.cabs[0].ukbm_power = false;
        check(u.run(1.0f), "УКБМ off, ALSN on: the whistle at once");
        u.run(8.0f);
        check(u.out.vent, "УКБМ off, ALSN on: the ЭПК");
    }
}

void kpd() {
    RecorderSettings k;
    k.kpd = true;
    RecorderCab c;
    c.kpd_power = 5;
    c.kpd_screen = 6;
    c.kpd_speed = 7;
    k.cabs.push_back(c);
    RecorderState s;
    std::vector<RecorderCabInputs> cabs(1);
    cabs[0].kpd_power = true;
    RecorderInputs in;
    in.dt = dt;
    in.speed = 20.0f; // 72 km/h
    in.acceleration = -0.25f;
    in.day_time = 13 * 3600 + 7 * 60;
    in.cabs = cabs;
    RecorderOutput out;
    step_recorder(s, k, in, out);
    const auto text_of = [&](const std::uint32_t part) {
        std::u16string last;
        for (const auto& p : out.screen_parts)
            if (p.screen == 6 && p.part == part && p.text_set) last = p.text;
        return last;
    };
    const auto shown = [&](const std::uint32_t part) {
        int last = -1;
        for (const auto& p : out.screen_parts)
            if (p.screen == 6 && p.part == part && p.state_set) last = p.state;
        return last == 0;
    };
    check(text_of(0) == u"72" && text_of(1) == u"1307" && text_of(2) == u"025", "КПД-3: speed, clock, acceleration");
    check(shown(5), "КПД-3: the minus of a deceleration");
    in.speed = 2.0f; // 7.2 km/h: tenths
    out = {};
    step_recorder(s, k, in, out);
    check(text_of(0) == u"72" && shown(7), "КПД-3: under 10 km/h in tenths, the point");
    cabs[0].kpd_power = false;
    in.cabs = cabs;
    out = {};
    step_recorder(s, k, in, out);
    check(text_of(0).empty(), "КПД-3: off, blank");
}

void mu() {
    RecorderSettings k;
    k.mu = true;
    RecorderCab c;
    c.epk_key = 1;
    c.mu_disp1 = 10;
    c.mu_disp2 = 11;
    k.cabs.push_back(c);
    RecorderState s;
    s.flags |= recorder_flag::acts;
    s.mu_flags = 1; // its self-test done
    std::vector<RecorderCabInputs> cabs(1);
    cabs[0].epk_key = true;
    cabs[0].mu_power = true;
    RecorderInputs in;
    in.dt = dt;
    in.speed = 25.0f; // 90 km/h on a green
    in.reverser = 1;
    in.aspect = 3;
    in.powered = true;
    in.main_reservoir = 8.0f;
    in.cabs = cabs;
    RecorderOutput out;
    bool whistled = false, vented = false;
    std::u16string permitted, speed;
    for (float t = 0.0f; t < 45.0f; t += dt) {
        step_recorder(s, k, in, out);
        whistled = whistled || has(out.sounds, 56);
        vented = vented || out.vent;
        s.flags |= recorder_flag::acts;
        for (const auto& p : out.screen_parts) {
            if (p.screen == 10 && p.text_set) permitted = p.text;
            if (p.screen == 11 && p.text_set) speed = p.text;
        }
    }
    check(s.alsn_on, "МУ: the ALSN on by the ЭПК key");
    check(whistled && vented, "МУ: over 60 km/h on a green, the check: the whistle at 35 s, the ЭПК past 42");
    check(permitted == u"60" && speed == u"90", "МУ: the permitted 60 on a green, the speed 90");
    check(has(out.sounds, 3193) || (s.mu_flags & 4u), "МУ: over the permitted, the speed blinks (3193)");
}

} // namespace

int main() {
    ukbm();
    kpd();
    mu();
    if (failures == 0) std::cout << "recorder: all passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
