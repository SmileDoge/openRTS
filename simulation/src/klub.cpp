#include <openrts/simulation/klub.hpp>

#include "saut_part.hpp"

#include <cmath>
#include <cstdint>

// What each part does is docs/reverse/brakes-dll.md §11, in its order.
// The numbers and the order of the rules are Brakes.dll's, so a run can be
// set against its golden runs; float arithmetic follows it where a rounding
// shows (the whole km/h and metres it compares).
namespace openrts::simulation {

namespace {

namespace f = klub_flag;
namespace d = klub_device;

constexpr float white_limit = 11.12f;   // m/s, Б's 40 km/h
constexpr float yellow_limit = 16.667f; // m/s, Ж's 60 km/h without a limit of its own

// The whole number a float makes as Brakes.dll takes it ((unsigned)(int64)x).
std::uint32_t whole(const double x) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(x)); }

// km/h of m/s as Brakes.dll has it: the product in double (its 3.6 is a
// double), kept in a float where it stores one. The own limit's clearing
// turns on it: 120 km/h's float of m/s makes 119.9999954, stored 119.9999924.
double kmh_of(const float ms) { return std::abs(static_cast<double>(ms) * 3.6); }

struct Sounds {
    const KlubSettings& k;
    KlubOutput& out;
    void add(const std::uint16_t code) const { out.sounds.push_back(code); }
    // The КЛУБ's beeps: 3199 / 3198 (the БЛОК's 3195 / 3196).
    void accept() const { add((k.setup & 8) ? 3195 : 3199); }
    void notice() const { add((k.setup & 8) ? 3196 : 3198); }
    void whistle(const bool on) const {
        if ((k.setup & 8) && !k.block_mode) add(on ? 3956 : 3957);
        else add(on ? 56 : 57);
    }
};

bool no_codes(const std::uint32_t aspect) { return aspect == 0 || aspect == 8; }

void ask(KlubState& s, const float at_least = 4.9f) {
    s.flags |= f::vigilance;
    if (s.timer < at_least) s.timer = at_least;
}

// _ALSN: the signal, the limits, the next limit and the target.
void alsn(KlubState& s, const KlubInputs& in) {
    s.signal = in.cab_signal;
    s.alsen_count = 0;
    const double train = kmh_of(in.limit);
    s.limit = static_cast<float>(train);
    if (s.own_limit > 0.0f) s.limit = s.own_limit;
    s.device &= ~d::distance_white;
    bool found = false, distance_white = false;
    if (!in.signals.empty()) {
        for (const auto& g : in.signals) {
            if ((g.flags & 2) && !found && g.aspect[1] == 2) {
                distance_white = true;
                s.device |= d::distance_white;
            }
            if (!(g.flags & 1)) continue;
            if (!found) {
                found = true;
                const bool none = (g.flags & 0x10) || in.cars_ahead;
                s.signal.aspect[0] = none ? 8 : g.aspect[0];
                s.signal.distance = g.distance;
                s.signal.speed_limit = none ? 0.0f : g.speed_limit;
                s.signal.flags = g.flags;
                if (no_codes(s.signal.aspect[0])) {
                    if (!(s.device & d::after_kzh)) {
                        s.signal.aspect[0] = 2;
                        s.signal.speed_limit = white_limit;
                        s.signal.flags |= 0x20;
                    }
                } else if (s.signal.aspect[0] == 1) {
                    s.device |= d::after_kzh;
                } else {
                    s.device &= ~d::after_kzh;
                }
            }
            if (!s.alsen_table || s.alsen_table >= 5) break;
            if (g.aspect[0] != 6 && g.aspect[0] != 7) break;
            ++s.alsen_count;
            if (s.alsen_count > 4) s.alsen_count = 4;
        }
        if (distance_white) {
            s.signal.aspect[0] = 2;
            s.signal.speed_limit = white_limit;
            s.signal.flags |= 0x20;
        }
    } else if (!(s.device & d::after_kzh)) {
        s.signal.aspect[0] = 2;
        s.signal.speed_limit = white_limit;
    }
    // openRTS's map: the target where the map has it, the aspect as the
    // codes give it. САУТ keeps the signal actually ahead.
    s.saut_signal_distance = s.signal.distance;
    if (in.ek.given && in.ek.matched && in.ek.has_target) s.signal.distance = in.ek.distance;
    auto& aspect = s.signal.aspect[0];
    if (aspect == 1) {
        s.signal.speed_limit = 0.0f;
        if (s.limit > 63.0f) s.limit = 63.0f;
    }
    if ((aspect == 3 || aspect == 4 || aspect == 5) && s.signal.speed_limit < 0.0f) s.signal.speed_limit = yellow_limit;
    if (train > s.own_limit) s.own_limit = 0.0f;
    if (s.signal.distance < 5.0f && train > s.own_limit) s.own_limit = static_cast<float>(train);
    if (in.next_limit_distance < 5.0f && in.next_limit > 0.0f && s.own_limit > 0.0f)
        s.own_limit = static_cast<float>(kmh_of(in.next_limit));
    s.target_speed = static_cast<float>(kmh_of(in.next_limit));
    s.target_distance = in.next_limit_distance;
    if (in.next_limit_distance == -1.0f) s.target_distance = 100000.0f;
    if (in.next_limit < 0.0f) s.target_speed = s.limit;
    if (s.signal.speed_limit >= 0.0f &&
        (s.signal.distance < s.target_distance ||
         static_cast<double>(s.signal.distance - s.target_distance) * 0.08 <
             static_cast<double>(s.target_speed) - static_cast<double>(s.signal.speed_limit) * 3.6)) {
        s.target_speed = static_cast<float>(kmh_of(s.signal.speed_limit));
        s.target_distance = s.signal.distance;
    }
    if (s.target_speed > 255.0f) s.target_speed = 255.0f;
    if (s.signal.speed_limit == 0.0f) s.limit_marks |= 0x4000;
    else s.limit_marks &= ~0x4000u;
    s.signal_from_50 = static_cast<float>(whole(s.signal.distance) - 50u);
    if (s.signal_from_50 > 9999.0f) s.signal_from_50 = 0.0f;
    s.saut_from_50 = static_cast<float>(whole(s.saut_signal_distance) - 50u);
    if (s.saut_from_50 > 9999.0f) s.saut_from_50 = 0.0f;
    s.signal_metres = static_cast<float>(whole(s.signal.distance));
}

// _Run before the КЛУБ: the ALSN delay; Б after КЖ is К.
void delay(KlubState& s, const KlubSettings& k, const KlubInputs& in) {
    auto& aspect = s.signal.aspect[0];
    if (aspect == s.accepted_aspect && s.alsen_count == s.accepted_count) {
        s.delay_timer = 0.0f;
    } else if (k.alsn_delay >= s.delay_timer) {
        s.delay_timer += in.dt;
        aspect = s.accepted_aspect;
        s.alsen_count = s.accepted_count;
    } else {
        s.accepted_aspect = aspect;
        s.accepted_count = s.alsen_count;
        s.delay_timer = 0.0f;
        s.yellow_run = 0.0f;
    }
    if (aspect == 2 && s.previous_aspect == 1) {
        aspect = 0;
        if (s.limit == 0.0f) {
            s.limit = s.target_speed;
            if (s.limit == 0.0f && aspect == 1) s.limit = 63.0f;
        }
    } else if (s.previous_aspect == 1 && s.limit == 0.0f) {
        s.limit = s.target_speed;
        if (s.limit == 0.0f && aspect == 1) s.limit = 63.0f;
    } else {
        s.previous_aspect = aspect;
    }
}

// updateKLUB.
void update(KlubState& s, const KlubSettings& k, const KlubInputs& in, const Sounds& sound, KlubOutput& out) {
    const float dt = in.dt;
    const float v = std::abs(in.speed);
    auto& aspect = s.signal.aspect[0];
    s.flags &= ~f::overspeed;
    if (in.powered) {
        if (!(s.flags & f::train_mode)) s.flags &= ~f::permit_20;
        out.safety_powered = true;
        if (v > 0.0f) s.run += v * dt;
        s.refresh = s.refresh <= 0.5f ? s.refresh + dt : 0.0f;
        s.blink = s.blink <= 2.0f ? s.blink + dt : 0.0f;
        if (s.powered_time < 21.0f) s.powered_time += dt;
        if (in.wheelslip) {
            if (s.slip_time > 0.3f && !(s.marks & 8)) {
                sound.notice();
                s.marks |= 8;
            }
            s.slip_time += dt;
        } else {
            s.slip_time = 0.0f;
            s.marks &= ~8u;
        }
    } else {
        s.powered_time = 0.0f;
        s.flags &= ~f::permit_20;
    }
    // A rising limit waits until the train has run its length.
    if (s.limit > s.accepted_limit && s.wagons && in.powered) {
        if (!(s.limit_marks & 0x40000)) {
            s.limit_marks |= 0x40000;
            s.rise_run = 0.0f;
        }
        const float wagon = k.ept ? 25.0f : 14.0f;
        if (wagon * static_cast<float>(s.wagons) <= s.rise_run) {
            if (s.limit_marks & 0x40000) sound.notice();
            s.limit_marks &= ~0x40000u;
            s.accepted_limit = s.limit;
        }
        s.limit = s.accepted_limit;
        s.rise_run += v * dt;
    } else {
        s.limit_marks &= ~0x40000u;
        s.accepted_limit = s.limit;
    }
    // Without the ЭК: KLUB_GRSPD, and KLUB_YLSPD / 20 after a yellow / КЖ.
    if (k.green_speed > -1.0f && k.green_speed < s.limit) s.limit = k.green_speed;
    if (!klub_ek(s)) {
        if (k.block_length < s.yellow_run) s.yellow_run = k.block_length;
        if (aspect == 3 || aspect == 4 || aspect == 5) {
            s.limit = k.green_speed;
            s.yellow_run += v * dt;
            s.target_speed = k.yellow_speed;
            s.target_distance = k.block_length - s.yellow_run;
        } else if (aspect == 1) {
            s.limit = k.green_speed;
            s.yellow_run += v * dt;
            s.target_speed = 20.0f;
            s.target_distance = k.block_length - s.yellow_run;
        } else {
            s.limit = k.green_speed;
            s.target_speed = k.green_speed;
            s.target_distance = 10000.0f;
            s.yellow_run = 0.0f;
        }
    }
    // The curve: 0.08 km/h a metre.
    if (aspect <= 1 && in.powered && klub_ek(s)) {
        if (static_cast<float>(whole(s.limit)) * 12.5f <= s.signal_metres) {
            s.marks &= ~2u;
            s.permitted = s.limit;
        } else {
            s.permitted = static_cast<float>(static_cast<double>(s.signal_metres) * 0.08);
            if (!(s.marks & 2)) {
                s.marks |= 2;
                sound.notice();
            }
        }
    } else if (in.powered) {
        const bool forced = (s.signal.flags & 0x20) != 0;
        const float from = forced ? 40.0f : s.limit;
        if (from <= s.target_speed || static_cast<float>(whole(from - s.target_speed)) * 12.5f <= s.target_distance ||
            (forced && s.target_speed <= 0.0f)) {
            s.marks &= ~2u;
            s.permitted = s.limit;
        } else {
            s.permitted = static_cast<float>(static_cast<double>(s.target_distance) * 0.08 + s.target_speed);
            if (!(s.marks & 2)) {
                s.marks |= 2;
                sound.notice();
            }
        }
    }
    if (aspect == 1) {
        s.signal.speed_limit = 0.0f;
        if (s.permitted > 60.0f) s.permitted = 60.0f;
    }
    if (s.permitted < 10.0f) s.permitted = 10.0f;
    if (aspect >= 2 && aspect != 8) s.flags &= ~f::permit_20;
    if (aspect != 0 && aspect != 8) s.flags &= ~f::white_permit;
    if (s.flags & f::permit_20) {
        if (no_codes(aspect) || aspect == 1) s.permitted = 20.0f;
    } else if (no_codes(aspect)) {
        s.permitted = 0.0f;
    }
    if ((s.flags & f::white_permit) && no_codes(aspect)) {
        aspect = 2;
        s.permitted = 40.0f;
    }
    const double kmh = kmh_of(in.speed);
    if (kmh <= s.permitted) s.flags &= ~f::over_permitted;
    else s.flags |= f::over_permitted;
    // The screen's clock.
    const float period = (k.setup & 8) ? 0.33f : 0.5f;
    if (period <= s.redraw) s.redraw = 0.0f;
    else s.redraw += dt;
    if (!in.powered) return;
    if (s.message_time <= 2.0f) s.message_time += dt;
    if (s.flags & f::white_799) {
        aspect = 2;
        s.permitted = static_cast<float>(s.white_speed);
    }
    if ((s.flags & f::white_809) && aspect == 2) s.permitted = static_cast<float>(s.white_speed);
    if (!(s.flags & f::train_mode)) {
        aspect = 2;
        s.permitted = 60.0f;
    }
    // Periodic vigilance.
    if (!(k.setup & 4) || (s.device & d::tskbm_on) || (in.speed <= 0.1f && in.speed >= -0.1f) || (s.flags & f::traction_262)) {
        s.periodic = 0.0f;
    } else {
        s.periodic += dt;
        const float after = (aspect == 2 || aspect == 6 || aspect == 7) ? 90.0f : 40.0f;
        if (s.periodic > after) {
            if (!(s.flags & f::vigilance)) sound.notice();
            s.flags |= f::vigilance;
        }
    }
    // К, КЖ and Б at the start.
    if (aspect != 2 || (s.flags & (f::double_traction | f::white_799 | f::white_809)) || !(s.flags & f::train_mode)) {
        s.white_start = -1.0f;
        if (aspect != 1 || (s.flags & f::double_traction)) {
            if (aspect && (aspect != 8 || kmh <= 1.0f || (s.flags & f::double_traction))) {
                s.marks &= ~4u;
                s.flags &= ~f::stood_at_kzh;
            } else {
                if (!(s.flags & f::stood_at_kzh) || kmh > 20.0f) {
                    if (kmh > 0.5f) {
                        ask(s);
                        s.device |= d::kon;
                    }
                }
                s.marks &= ~4u;
            }
        } else {
            if (s.signal.distance < 200.0f && kmh < 0.5f) s.flags |= f::stood_at_kzh;
            if (kmh <= 1.5f) {
                s.marks &= ~4u;
            } else if (!(s.marks & 4)) {
                s.marks |= 4;
                ask(s);
            }
        }
    } else {
        if (s.white_start == -1.0f) {
            s.white_start = static_cast<float>(whole(kmh));
            s.run = 0.0f;
        } else {
            s.permitted = static_cast<float>(static_cast<double>(s.white_start) + 5.0 - 0.02 * s.run);
            if (s.permitted < 43.0f) s.permitted = 43.0f;
        }
        if (kmh <= 1.5f) {
            s.marks &= ~0x20u;
        } else if (!(s.marks & 0x20)) {
            s.marks |= 0x20;
            if (!(k.setup & 8) || !(s.device & d::saut_on)) ask(s);
        }
    }
    // The speed as the screen has it.
    std::uint32_t shown = whole(kmh);
    if (in.wheelslip) shown = static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(shown) + 20.0f * s.slip_time));
    if (s.refresh == 0.0f) s.shown_speed = static_cast<float>(shown);
    if (s.flags & f::warning) {
        s.tone += dt;
        if (s.tone > 0.5f) {
            s.tone = 0.0f;
            if (whole(s.permitted) >= shown) sound.accept();
        }
    }
    if (s.redraw != 0.0f) return;
    // The redraw (the working cab).
    s.flags &= ~f::warning;
    s.saut_rules = false;
    // What the screen shows: under САУТ its speed (+5 km/h without the
    // УКБМ). With the УКБМ (PB_SETUP 8) Brakes.dll does it in its drawing
    // alone: var + 140 keeps the КЛУБ's (the ЭП2Д's МПСУ dial reads it; set
    // here it fell for a step at each redraw, every 0.33 s, and the dial
    // jerked, testers 2026-11-28).
    if (s.permitted < 0.0f) s.permitted = 0.0f;
    s.drawn_permitted = s.permitted;
    if ((s.device & d::saut_on) && (k.setup & 1) && !(s.flags & f::white_799) && (!(s.flags & f::white_809) || aspect != 2) &&
        (s.flags & f::train_mode)) {
        // updateKLUB's drawing does it ("САУТ" / "С" on the screen; the sum
        // in the x87's precision). Brakes.dll's ЭП1 run shows it on both
        // cabs' screens, though its code reads as doing it in the first
        // cab drawn only (PROBABLE: all of them).
        // САУТ's speed shown: openRTS's own, else Brakes.dll's.
        const float saut_permitted = in.saut_in.own ? s.saut_shown : in.saut_permitted;
        if (k.setup & 8) {
            if (s.drawn_permitted > saut_permitted) {
                s.drawn_permitted = saut_permitted;
                s.saut_rules = true;
            }
        } else if (static_cast<double>(saut_permitted) + 5.0 < static_cast<double>(s.drawn_permitted)) {
            // Without the УКБМ Brakes.dll keeps it (the ЭП1's run:
            // klub_ep1): the permitted speed itself.
            s.permitted = s.drawn_permitted = static_cast<float>(static_cast<double>(saut_permitted) + 5.0);
            s.saut_rules = true;
        }
    }
    if (s.drawn_permitted < 0.0f) s.drawn_permitted = 0.0f;
    if (!in.epk_key) return;
    if (s.target_speed >= s.drawn_permitted || s.target_distance >= 2000.0f || !k.has_screen) {
        s.marks &= ~1u;
    } else if (!(s.marks & 1)) {
        s.marks |= 1;
        sound.notice();
    }
    s.drawn_flags = s.flags;
    if (s.drawn_permitted - static_cast<float>(shown) <= 3.0f && shown) {
        if (s.drawn_permitted - static_cast<float>(shown) <= 2.0f && shown > 1) {
            s.flags |= f::warning;
            if (s.drawn_permitted - static_cast<float>(shown) < 0.0f && (s.slip_time == 0.0f || s.slip_time > 10.0f) &&
                !(s.flags & f::double_traction)) {
                ask(s);
                s.flags |= f::overspeed;
            }
        }
    }
}

// _Run's ЭПК part: the timer, the whistle, the trip; the aspect changes; КОН.
void epk(KlubState& s, const KlubSettings& k, const KlubInputs& in, const Sounds& sound, KlubOutput& out) {
    const float dt = in.dt;
    if (in.epk_key) {
        const std::uint32_t seen = in.powered ? s.signal.aspect[0] : 0u;
        if (!in.powered && s.timer < 5.0f) s.timer = 5.0f;
        if ((!(s.flags & f::vigilance) && in.powered && !(s.flags & f::epk_vented)) || (s.flags & f::developer)) {
            if (s.timer > 0.0f && s.timer < 45.0f) {
                sound.whistle(false);
                s.timer = 0.0f;
                s.epk_bit = false;
                out.epk_bit_written = true;
            }
        } else {
            s.timer += dt;
            if (s.timer >= 5.0f && s.timer - dt < 5.0f) sound.whistle(true);
            if (s.timer > 12.0f || (s.flags & f::epk_vented)) {
                s.timer = 45.0f;
                s.device |= d::kon;
                s.flags |= f::epk_vented;
                s.epk_bit = true;
                out.epk_bit_written = true;
            }
        }
        if (s.epk_aspect != seen && in.powered) {
            s.periodic = 0.0f;
            if ((k.setup & 8) && seen == 2 && (s.device & d::saut_on)) {
                sound.add(3192);
            } else {
                sound.notice();
                if ((seen == 1 || seen == 2 || no_codes(seen)) && std::abs(in.speed) > 1.0f) {
                    s.flags |= f::vigilance;
                    s.timer = 4.95f - dt;
                }
            }
        }
        if (s.alsen_count != s.count_seen && in.powered) sound.notice();
        s.count_seen = s.alsen_count;
        s.epk_aspect = seen;
    } else {
        if (s.timer > 5.0f) sound.whistle(false);
        s.timer = 0.0f;
        s.epk_bit = false;
        out.epk_bit_written = true;
        s.flags &= ~f::epk_vented;
    }
    if (in.powered) {
        if (s.kon_timer < 10.0f) {
            if ((in.speed > 0.1f || in.speed < -0.1f) && !(s.flags & f::double_traction) && !in.epk_key &&
                (in.cylinder < 0.7f || (s.device & d::kon_vented))) {
                s.kon_timer += dt;
            }
        } else {
            s.kon_vented = true;
            s.device |= d::kon_vented;
            s.timer = 45.0f;
        }
    } else if (!in.epk_key) {
        s.kon_timer = 0.0f;
        s.device &= ~d::kon_vented;
        s.kon_vented = false;
        s.timer = 0.0f;
    }
    out.vent = s.timer >= 12.0f || s.kon_vented;
}

// ТСКБМ (after the КЛУБ's permitted speed): while its switch is on, on the
// move with the ЭПК key in, its timer runs down; under 15 s the lamp L2
// blinks, under 7 the whistle and the request, at 0
// the ЭПК vents (the vigilance timer held at 45, TrainPipeRate -1.2). At a
// stand it waits at 6.9. With the БЛОК (PB_SETUP 8): it runs at a stand
// too, its request from 15 s, its beep 3196 at 15 and 7 s.
void tskbm(KlubState& s, const KlubSettings& k, const KlubInputs& in, KlubOutput& out) {
    const auto whistle = [&](const bool on) {
        const bool block = (k.setup & 8) && !k.block_mode;
        out.sounds.push_back(on ? (block ? 3956 : 56) : (block ? 3957 : 57));
    };
    auto& timer = s.tskbm_timer;
    if (s.flags & f::developer) timer = 60.0f;
    s.tskbm_shown = timer;
    if (!in.tskbm) return;
    const bool moving = std::abs(in.speed) > 0.1f;
    const bool may = !(k.setup & 2) || in.powered;
    if (moving && may) {
        if (timer >= 7.0f) {
            if (s.device & d::tskbm_whistle) {
                s.device &= ~d::tskbm_whistle;
                whistle(false);
            }
        } else {
            s.flags |= f::tskbm | f::vigilance;
            if (!(s.device & d::tskbm_whistle) && timer > 0.0f) {
                whistle(true);
                s.device |= d::tskbm_whistle;
            }
        }
        if (timer <= 0.0f) {
            if (s.device & d::tskbm_whistle) {
                s.device &= ~d::tskbm_whistle;
                whistle(false);
            }
            s.timer = 45.0f;
            out.vent = true;
        }
    }
    bool warn = false;
    const bool alsn = in.alsn_on >= 0 ? in.alsn_on == 1 : in.epk_key;
    if ((moving || (k.setup & 8)) && alsn && may) {
        timer -= in.dt;
        if (!moving && timer < 7.0f) timer = 6.9f;
        if ((k.setup & 8) && timer < 15.0f) s.flags |= f::tskbm;
        warn = timer < 7.0f || (timer < 15.0f && timer > 14.0f);
    }
    if (warn) {
        if (!(s.device & d::tskbm_warning)) {
            s.device |= d::tskbm_warning;
            if (k.setup & 8) out.sounds.push_back(3196);
        }
    } else {
        s.device &= ~d::tskbm_warning;
    }
}

// The РБ, the РБС and the ЭПК key (Switched).
void answers(KlubState& s, const KlubSettings& k, const KlubInputs& in, const Sounds& sound) {
    const bool klub = (k.setup & 2) != 0;
    if (in.rb && !(s.flags & f::tskbm) && !(s.flags & f::over_permitted)) {
        if (s.timer < 12.0f && (!klub || in.powered)) {
            if (std::abs(in.speed) < 0.1f || !(s.device & d::kon)) {
                if (s.timer > 5.0f) sound.whistle(false);
                s.flags &= ~f::vigilance;
                s.periodic = 0.0f;
                s.timer = 0.0f;
                s.device &= ~d::kon;
            }
            if (klub && in.powered) sound.accept();
        }
    }
    if (in.rbs && !(s.flags & f::over_permitted) && (!klub || in.powered)) {
        if (klub && in.powered) sound.accept();
        if (s.timer < 12.0f) {
            if (s.timer > 5.0f) sound.whistle(false);
            s.flags &= ~f::vigilance;
            s.periodic = 0.0f;
            s.timer = 0.0f;
            s.device &= ~d::kon;
        }
        if (s.flags & f::tskbm) {
            sound.add(57);
            s.flags &= ~f::tskbm;
        }
    }
    if (in.epk_key_turned) {
        if (in.epk_key_to) {
            if (klub && in.powered) sound.notice();
        } else {
            if (s.timer > 5.0f) sound.whistle(false);
            s.timer = 0.0f;
            s.periodic = 0.0f;
            s.flags &= ~f::vigilance;
            s.device &= ~d::kon;
        }
    }
}

// SwitchKLUB: one key (CanSwitch, the КЛУБ powered, the working cab).
void key(KlubState& s, const KlubSettings& k, const KlubInputs& in, const KlubPress& press, const Sounds& sound) {
    const bool standing = whole(kmh_of(in.speed)) == 0;
    const auto done = [&] { sound.accept(); };
    const auto digit = [&](const std::uint32_t d) {
        if (s.flags & f::fresh_entry) {
            s.entry = 0;
            s.flags &= ~f::fresh_entry;
        }
        s.entry = s.entry * 10 + d;
        sound.add(3197);
    };
    // ВВОД taken: without the cassette (KLUB_KASS given, its switch off) the
    // refusing beep, the value taken all the same.
    const auto confirm = [&] {
        if (k.cassette_given && !in.cassette) sound.notice();
        else sound.accept();
    };
    switch (press.key) {
    case KlubKey::vk:
        sound.add(3197);
        if (s.flags & f::permit_20) s.flags &= ~f::permit_20;
        if (standing && s.signal.aspect[0] == 1) s.flags |= f::permit_20;
        if (standing && in.rb_held && no_codes(s.signal.aspect[0])) s.flags |= f::white_permit;
        done();
        return;
    case KlubKey::mp:
        sound.add(3197);
        if (s.flags & f::double_traction) s.flags &= ~(f::double_traction | f::traction_262);
        if (standing && in.rb_held && (s.flags & f::train_mode)) {
            if (s.flags & f::double_traction) s.flags &= ~(f::double_traction | f::traction_262);
            else s.flags |= f::double_traction;
            sound.accept();
            done();
            return;
        }
        if (s.signal.aspect[0] == 2 && standing && !(s.flags & f::double_traction)) {
            s.flags ^= f::train_mode;
            sound.accept();
        }
        done();
        return;
    case KlubKey::command:
        sound.add(3197);
        s.flags ^= f::command_mode;
        s.entry = 0;
        s.time_shown = -1.0f;
        done();
        return;
    case KlubKey::data:
        sound.add(3197);
        if (kmh_of(in.speed) < 0.5) {
            s.flags &= ~f::command_mode;
            s.entry = s.driver;
            s.field = 1;
            s.time_shown = -1.0f;
            s.flags |= f::fresh_entry;
        }
        done();
        return;
    case KlubKey::track:
        sound.add(3197);
        s.flags |= f::fresh_entry;
        if (!(s.flags & f::track_entry)) {
            // The field shows the last track until a digit replaces it (the
            // real КЛУБ-У's, the testers' 2026-11-21; Brakes.dll's 0).
            s.flags |= f::track_entry;
            s.entry = s.track & 0x7Fu;
        } else {
            s.flags &= ~f::track_entry;
        }
        s.time_shown = -1.0f;
        done();
        return;
    case KlubKey::info:
        sound.add(3197);
        done();
        return;
    case KlubKey::backlight:
        sound.add(3197);
        s.flags ^= f::backlight;
        done();
        return;
    case KlubKey::reset:
        s.entry = 0;
        sound.add(3197);
        done();
        return;
    case KlubKey::plus:
        ++s.entry;
        sound.add(3197);
        done();
        return;
    case KlubKey::minus:
        if (--s.entry == 0) s.entry = 0;
        sound.add(3197);
        done();
        return;
    case KlubKey::digit:
        digit(press.digit);
        done();
        return;
    case KlubKey::enter:
        break;
    }
    // ВВОД.
    sound.add(3197);
    s.time_shown = -1.0f;
    if (!(s.flags & f::command_mode)) {
        if (s.flags & f::white_entry) {
            s.flags &= ~f::white_entry;
            s.white_speed = s.entry;
            s.entry = 0;
            if (s.white_799_entry) s.flags |= f::white_799;
            s.white_799_entry = false;
            confirm();
        } else if (!(s.flags & f::track_entry)) {
            if (s.flags & f::right_entry) {
                // The track entered takes effect now, with its sign (1: the
                // right track, "ПР").
                s.flags &= ~f::right_entry;
                const std::uint32_t number = s.pending_track ? s.pending_track : (s.track & 0x7Fu);
                s.track = number | (s.entry ? 0x80u : 0u);
                s.pending_track = 0;
                ++s.track_entries;
                s.entry = 0;
                confirm();
            } else {
                switch (s.field) {
                case 1:
                    s.driver = s.entry;
                    s.field = 2;
                    s.entry = s.train_number;
                    s.flags |= f::fresh_entry;
                    confirm();
                    break;
                case 2:
                    s.train_number = s.entry;
                    s.field = 3;
                    s.entry = s.axles;
                    s.flags |= f::fresh_entry;
                    confirm();
                    break;
                case 3:
                    s.axles = s.entry;
                    s.field = 4;
                    s.flags |= f::fresh_entry;
                    confirm();
                    s.entry = s.wagons;
                    break;
                case 4:
                    s.wagons = s.entry;
                    s.field = 5;
                    s.flags |= f::fresh_entry;
                    confirm();
                    s.entry = s.mass;
                    break;
                case 5:
                    s.mass = s.entry;
                    s.field = 0;
                    s.entry = 0;
                    confirm();
                    break;
                default:
                    if (s.alsen_table != 5) return;
                    if (s.entry > 4) s.entry = 0;
                    s.alsen_table = s.entry;
                    s.entry = 0;
                    sound.accept();
                    break;
                }
            }
        } else {
            // The number waits for its sign (KlubState::pending_track), the
            // sign's field showing the last one (1 ПР, 0 НП) until a digit
            // replaces it (the real КЛУБ-У's, the testers' 2026-11-21;
            // Brakes.dll's 0); 0 (no track) at once.
            s.flags &= ~f::track_entry;
            s.flags |= f::right_entry;
            s.pending_track = s.entry > 0x7F ? 0x7F : s.entry;
            s.entry = 0;
            if (!s.pending_track) {
                s.track = 0;
                s.flags &= ~f::right_entry;
                ++s.track_entries;
            } else {
                s.entry = (s.track & 0x80u) ? 1 : 0;
                s.flags |= f::fresh_entry;
            }
            confirm();
        }
        return;
    }
    // A command (К number ВВОД).
    s.flags &= ~f::command_mode;
    sound.notice();
    const std::uint32_t c = s.entry;
    switch (c) {
    case 4: s.time_shown = static_cast<float>(static_cast<std::int64_t>(in.day_time)); s.message_time = 0.0f; break;
    case 7:
        if (kmh_of(in.speed) < 0.5 && (k.setup & 8)) {
            s.entry = s.driver;
            s.field = 1;
            s.time_shown = -1.0f;
            s.flags |= f::fresh_entry;
        }
        break;
    case 10: case 45: case 46: case 47: case 1000: case 1001: case 1037: case 1038: s.message_time = 0.0f; break;
    case 70: s.flags &= ~f::lamp_test; break;
    case 71: s.flags |= f::lamp_test; break;
    case 80: s.flags &= ~f::line14_cylinder; s.message_time = 0.0f; break;
    case 81: s.flags |= f::line14_cylinder; s.message_time = 0.0f; break;
    case 91: s.flags &= ~f::second_set; break;
    case 92: s.flags |= f::second_set; break;
    case 122: s.flags &= ~f::kgf; s.message_time = 0.0f; break;
    case 123: s.flags |= f::kgf; s.message_time = 0.0f; break;
    case 137:
        s.entry = s.alsen_table;
        s.alsen_table = 5;
        s.flags |= f::fresh_entry;
        break;
    case 257:
        s.flags |= f::epk_test | f::vigilance;
        s.timer = 4.7f;
        break;
    case 258:
        if (s.flags & f::epk_test) s.flags &= ~(f::epk_test | f::vigilance);
        break;
    case 259: s.flags |= f::saut_diagnostics; s.message_time = 0.0f; break;
    case 260: s.flags &= ~f::saut_diagnostics; break;
    case 262:
        if (s.flags & f::double_traction) s.flags |= f::traction_262;
        break;
    case 799:
        // The speed first; on white once it is entered (ВВОД above).
        s.flags |= f::white_entry | f::fresh_entry;
        s.white_799_entry = true;
        s.entry = s.white_speed;
        break;
    case 800:
        s.flags &= ~(f::white_799 | f::white_809);
        s.white_799_entry = false;
        break;
    case 809:
        s.flags |= f::white_809 | f::white_entry | f::fresh_entry;
        s.entry = s.white_speed;
        break;
    case 1036: s.flags ^= f::dps; s.message_time = 0.0f; break;
    default: break;
    }
}

} // namespace

void step_klub(KlubState& s, const KlubSettings& k, const KlubInputs& in, KlubOutput& out) {
    out.vent = false;
    out.safety_powered = false;
    out.epk_bit_written = false;
    out.sounds.clear();
    const Sounds sound{k, out};
    if (!s.started) {
        s.started = true;
        s.delay_timer = k.alsn_delay;
    }
    // The map's answer for the track entered (the last step's number).
    s.ek_off = in.ek.given && !in.ek.matched;
    if (in.tskbm) s.device |= d::tskbm_on;
    else s.device &= ~d::tskbm_on;
    // САУТ's "on": openRTS's САУТ sets it, else Brakes.dll's.
    if (!in.saut_in.own) {
        if (in.saut) s.device |= d::saut_on;
        else s.device &= ~d::saut_on;
    }
    out.saut_screens.clear();
    // The keys before the step (CanSwitch).
    if ((k.setup & 2) && in.powered)
        for (const auto& press : in.keys) key(s, k, in, press, sound);
    const bool saut_here = in.saut_in.own && (k.setup & 1);
    if (saut_here) saut::keys(s, k, in, out);
    if (in.alsn) alsn(s, in);
    delay(s, k, in);
    // САУТ first, before the permitted speed (it reads this step's ALSN).
    if (saut_here) saut::step(s, k, in, out);
    if (k.setup & 2) update(s, k, in, sound, out);
    if (k.setup & 4) tskbm(s, k, in, out);
    if (k.setup & 2) epk(s, k, in, sound, out);
    // Switched after Run: the РБ, the РБС, the ЭПК key; any switch starts
    // ТСКБМ's timer again unless it asks.
    answers(s, k, in, sound);
    if (in.switched && !(s.flags & f::tskbm)) s.tskbm_timer = 90.0f;
    if (in.saut_in.own && (k.setup & 1)) saut::switched(s, in, out);
}

} // namespace openrts::simulation
