#include <openrts/simulation/cab_brakes.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {

CabBrakeNumbers CabBrakeNumbers::brakes_dll() {
    // MaDaDev's Brakes.dll's (docs/reverse/brakes-dll.md §10).
    CabBrakeNumbers n;
    n.charge_ur_rate = 0.18f;
    n.charge_ur_gain = 2.0f;
    // II: at most 0.25 atm/s, slower near the charge (measured on the ЭП2Д
    // after a full service: 1.67 to 3.92 at 0.25 atm/s, the charge 5 s
    // later). 0.1 had been taken from a step's return, all of it near the
    // charge; after a full service the pipe came back at 0.1 and the
    // brakes took some 40 s to let off (testers).
    n.running_ur_rate = 0.25f;
    n.running_ur_gain = 1.0f;
    n.stabiliser = 0.002f;
    n.charging_drop = 0.1f;
    n.lap_ur_rate = 0.1f;
    n.slow_service = 0.03f;
    n.service = 0.22f;
    n.emergency_ur = 0.4f;
    n.independent_steps = {1.0f, 2.0f, 3.0f, 4.0f};
    // 0.1 a step; RTS steps its DLLs about 30 times a second.
    n.independent_release = 3.0f;
    n.independent_release_needs_buffer = true;
    n.vk4_apply = 0.3f;
    n.vk4_release = 0.25f;
    n.vk4_max = 4.0f;
    n.ept_volts_release = 50.0f;
    n.ept_volts_brake = 40.0f;
    n.ept_amps_lap = 4.0f;
    n.ept_amps_brake = 5.0f;
    // The ЭД9М's golden run: 0.25 atm in 0.25 s up, 0.16 down.
    n.ep_apply = 1.0f;
    n.ep_release = 0.65f;
    n.cylinder_needle_near = 0.2f;
    n.cylinder_needle_far = 1.0f;
    n.cylinder_needle_band = 0.5f;
    // 0.5 + 0.8 k / 100 with k = RTSGetInteger(2), 0 under openRTS's host.
    n.pipe_needle_fall = 0.5f;
    n.pipe_needle_per_wagon = 0.008f;
    n.pipe_needle_fall_running = 0.2f;
    n.pipe_needle_per_wagon_running = 0.012f;
    n.pipe_needle_fall_braking = 1.0f;
    n.sensor418_rate = 0.0125f;
    n.sensor418_time = 0.5f;
    n.sensor418_cylinder = 0.3f;
    // The ДТЦ opens at 0.4-0.7 kgf/cm² in the distributor's ТЦ channel, set
    // to 0.5 (0.5 -0.1 +0.2: МПС's ЦТ-533 on testing the brake equipment,
    // the ВЛ11's signaller; РЖД's: the lamp out over 0.07 MPa; testers
    // 2026-11-20); the ДДР closes at 1.1-1.3 kgf/cm² of the additional
    // discharge.
    n.sensor418_dtc = 0.5f;
    return n;
}

CabBrakeNumbers CabBrakeNumbers::openrts() {
    // The real devices' (docs/brakes-dll-replacement.md, "The numbers"):
    // Brakes.dll's where they fall within what the books give, else the
    // books'. Крылов В. И., Крылов В. В., «Автоматические тормоза
    // подвижного состава», 1983 (pages of the book).
    CabBrakeNumbers n = brakes_dll();
    // II: the reducer's charging as before (0.1 atm/s, closing in on the
    // charge 2 x the gap).
    n.running_ur_rate = 0.1f;
    n.running_ur_gain = 2.0f;
    // The reducer holds the УР at the pressure it is set to, within 0.005
    // MPa (§24, p. 100); Brakes.dll holds it 0.1 atm below the one named.
    n.charging_drop = 0.0f;
    // V: 0.5 to 0.4 MPa in 4-6 s (p. 102): 0.17-0.25 atm/s, Brakes.dll's
    // 0.22 within. VЭ (Va): 0.5 to 0.45 MPa in 15-20 s (p. 103), 0.03
    // within. The stabiliser: 6.0 to 5.8 in 80-120 s
    // (docs/reverse/air-brakes-real.md, 9), 0.002 within.
    // The 254: III 0.1-0.13 MPa, VI 0.38-0.4 MPa (§27, p. 115), 1 and 4
    // within. Its I lets the engine's own train brake off (p. 114), not
    // only with BR_BUFERKVT; its II lets the independent brake off at the
    // cylinder's own rate (full release within 13 s, p. 114).
    n.independent_release = 0.0f;
    n.independent_release_needs_buffer = false;
    // The gauges show the pressures as they are, as the cab gauges under
    // openRTS's air do (testers, 2026-11-17: Brakes.dll's needles lagged
    // behind the air).
    n.cylinder_needle_near = n.cylinder_needle_far = n.cylinder_needle_band = 0.0f;
    n.pipe_needle_fall = 0.0f;
    return n;
}

std::optional<ValvePosition> valve_position_of(const int state, const bool flipped) {
    const int d = direct_state(state, 6, flipped);
    if (state < 0 || d < 0 || d > 6) return std::nullopt;
    return static_cast<ValvePosition>(d);
}

std::optional<IndependentPosition> independent_position_of(const int state, const bool flipped) {
    const int d = direct_state(state, 5, flipped);
    if (state < 0 || d < 0 || d > 5) return std::nullopt;
    return static_cast<IndependentPosition>(d);
}

std::optional<Vk4Position> vk4_position_of(const int state, const bool flipped) {
    if (state < 0 || state > 2) return std::nullopt;
    return static_cast<Vk4Position>(flipped ? state : 2 - state);
}

namespace {

bool braking(const ValvePosition p) {
    return p == ValvePosition::ep_service || p == ValvePosition::service || p == ValvePosition::emergency;
}

bool lap(const ValvePosition p) { return p == ValvePosition::lap || p == ValvePosition::lap_feed; }

// A needle towards `target` at `near` atm/s within `band` of it, `far` beyond.
float needle(const float at, const float target, const float near, const float far, const float band, const float dt) {
    if (far <= 0.0f) return target;
    const float d = target - at;
    const float rate = std::abs(d) <= band ? near : far;
    return at + std::clamp(d, -rate * dt, rate * dt);
}

} // namespace

CabBrakeOutput step_cab_brakes(CabBrakeState& s, const CabBrakeControls& c, const CabBrakeAir& air, const CabBrakeNumbers& n,
                               const float dt) {
    CabBrakeOutput out;
    const float mr = air.main_reservoir;
    if (!s.started) {
        // The УР is the caller's (from where the valve it replaces stood).
        s.started = true;
        s.independent = air.independent;
        s.cylinder_needle = (std::max)(air.cylinder, air.independent);
        s.pipe_needle = air.pipe;
        s.last_pipe = air.pipe;
    }

    // The driver's valve: the УР (§10, "The driver's valve").
    auto& ur = s.valve.equalising;
    const float ur_before = ur;
    const float charging = (std::max)(c.charging - n.charging_drop, 0.0f);
    bool emergency = false;
    if (c.valve) {
        switch (*c.valve) {
        case ValvePosition::charge:
            ur += (std::min)((std::max)(n.charge_ur_gain * (mr - ur), 0.0f), n.charge_ur_rate) * dt;
            ur = (std::min)(ur, mr);
            break;
        case ValvePosition::running: {
            const float top = (std::min)(charging, mr);
            if (ur < top) ur = (std::min)(ur + (std::min)(n.running_ur_gain * (top - ur), n.running_ur_rate) * dt, top);
            ur = (std::max)(ur - n.stabiliser * dt, 0.0f);
            break;
        }
        case ValvePosition::lap:
            // The УР after the pipe through its 1.6 mm hole and the check
            // valve, down only (Крылов, p. 100); the pipe's leaks are not
            // made up.
            if (ur > air.pipe) ur = (std::max)(ur - n.lap_ur_rate * dt, air.pipe);
            out.feed = false;
            break;
        case ValvePosition::lap_feed:
            ur = (std::min)(ur, mr);
            break;
        case ValvePosition::ep_service:
            if (c.valve_resting) ur = (std::max)(ur - n.slow_service * dt, 0.0f);
            out.braking = true;
            break;
        case ValvePosition::service:
            if (c.valve_resting) ur = (std::max)(ur - n.service * dt, 0.0f);
            out.braking = true;
            break;
        case ValvePosition::emergency:
            ur = (std::max)(ur - n.emergency_ur * dt, 0.0f);
            emergency = true;
            break;
        }
    }
    if (c.keep_equalising) ur = ur_before;
    if (c.unfed) ur = (std::min)(ur, ur_before);
    const bool connected = c.valve.has_value() && !c.cut_off;
    out.direct = connected && *c.valve == ValvePosition::charge;
    out.equalising_kept = ur;
    out.equalising = connected ? ur : -1.0f;
    out.emergency = (connected && emergency) || c.emergency;

    // The ЭПТ (§10, "The ЭПТ"): the handle's commands while it works the
    // pipe, the buttons, the train's DLL's.
    if (c.ep_on) {
        bool hold = false, apply = false, handle_hold = false;
        if (connected) {
            if (lap(*c.valve)) handle_hold = true;
            if (braking(*c.valve)) handle_hold = apply = true;
        }
        hold = handle_hold;
        if (c.ep_apply_button) hold = apply = true;
        if (c.ep_hold_asked) hold = true;
        if (c.ep_apply_asked) apply = true;
        if (c.ep_release_button && !handle_hold) hold = false;
        // An emergency (VI, ЭТ, a stop valve, the ЭПК) takes the ЭПТ's
        // demand away (Brakes.dll's EPTValue -1; in VI the ЭД9М's golden run
        // builds no EP pressure): the ВР brake on the pipe.
        out.systems = 1 | 2;
        if (!out.emergency) {
            auto& level = s.valve.ep_target;
            // Its level, for cars on RTS's air (the realistic air's ЭВР take
            // the command and fill and let out at their own rates).
            if (apply) level = (std::min)(level + n.ep_apply * dt, 4.3f);
            else if (!hold) level = (std::max)(level - n.ep_release * dt, 0.0f);
            out.ep_value = level;
            out.ep_command = apply ? EpCommand::apply : hold ? EpCommand::hold : EpCommand::release;
        }
        out.lamp_ep_on = true;
        if (c.valve) {
            const auto p = *c.valve;
            out.lamp_ep_hold = lap(p) || (c.multiple_unit && braking(p));
            out.lamp_ep_brake = braking(p);
            const bool released = p == ValvePosition::charge || p == ValvePosition::running;
            out.ep_volts = released ? n.ept_volts_release : n.ept_volts_brake;
            out.ep_amps = released ? 0.0f : lap(p) ? n.ept_amps_lap : n.ept_amps_brake;
        }
    } else {
        s.valve.ep_target = 0.0f;
    }

    // The independent brake (§10, "The independent brake"): the higher of
    // the two valves; I lets it off at once, II gradually.
    const auto step_value = [&](const std::optional<IndependentPosition> p) -> float {
        if (!p || *p < IndependentPosition::step1) return -1.0f;
        const auto k = static_cast<std::size_t>(*p) - static_cast<std::size_t>(IndependentPosition::step1);
        return (std::min)(n.independent_steps[k], mr);
    };
    const float stepped = (std::max)(step_value(c.independent), step_value(c.independent2));
    const bool at_release = c.independent == IndependentPosition::release || c.independent2 == IndependentPosition::release;
    if (stepped >= 0.0f) {
        s.independent = stepped;
    } else if (at_release) {
        s.independent = 0.0f;
    } else if (c.independent || c.independent2) {
        s.independent = n.independent_release > 0.0f ? (std::max)(s.independent - n.independent_release * dt, 0.0f) : 0.0f;
    }
    if (c.vk4) {
        switch (*c.vk4) {
        case Vk4Position::apply:
            s.independent = (std::max)(s.independent, (std::min)(s.independent + n.vk4_apply * dt, (std::min)(n.vk4_max, mr)));
            break;
        case Vk4Position::release: s.independent = (std::max)(s.independent - n.vk4_release * dt, 0.0f); break;
        case Vk4Position::hold: break;
        }
    }
    out.independent_brake = s.independent;
    out.release_train_brake = c.release_train_brake || (at_release && (!n.independent_release_needs_buffer || c.buffer_kvt));

    // The gauges.
    s.cylinder_needle = needle(s.cylinder_needle, (std::max)(air.cylinder, air.independent), n.cylinder_needle_near,
                               n.cylinder_needle_far, n.cylinder_needle_band, dt);
    if (air.pipe >= s.pipe_needle || n.pipe_needle_fall <= 0.0f) {
        s.pipe_needle = air.pipe;
    } else {
        // Brakes.dll's fall by the handle: III .. VI at 1 atm/s, II and
        // the rest faster the more wagons (to 100).
        const float wagons = c.wagons <= 100 ? static_cast<float>(c.wagons) : 0.0f;
        float fall = n.pipe_needle_fall + n.pipe_needle_per_wagon * wagons;
        if (c.valve && *c.valve == ValvePosition::running) fall = n.pipe_needle_fall_running + n.pipe_needle_per_wagon_running * wagons;
        else if (c.valve && *c.valve >= ValvePosition::lap) fall = n.pipe_needle_fall_braking;
        s.pipe_needle = (std::max)(s.pipe_needle - fall * dt, air.pipe);
    }
    out.gauge_cylinder = s.cylinder_needle;
    out.gauge_pipe = s.pipe_needle;
    out.gauge_main_reservoir = mr;
    out.gauge_equalising = ur;

    // The 418: by the distributor's channels where the air has them (a
    // locomotive's 483 only), else the pipe falling without the ЭПТ until
    // the engine's brake answers.
    if (c.sensor418 && air.distributor_known) {
        const bool dtc_closed = air.distributor_channel < n.sensor418_dtc;
        if (air.sensor418_fitted && air.distributor_discharge && dtc_closed) s.sensor418 = true;
        if (!air.sensor418_fitted || !dtc_closed) s.sensor418 = false;
        s.sensor418_time = 0.0f;
    } else if (c.sensor418 && dt > 0.0f) {
        const float falling = (s.last_pipe - air.pipe) / dt;
        s.sensor418_time = !c.ep_on && falling > n.sensor418_rate ? s.sensor418_time + dt : 0.0f;
        if (s.sensor418_time > n.sensor418_time) s.sensor418 = true;
        if ((std::max)(air.cylinder, air.independent) > n.sensor418_cylinder) s.sensor418 = false;
    } else if (!c.sensor418) {
        s.sensor418 = false;
        s.sensor418_time = 0.0f;
    }
    if (dt > 0.0f && s.last_pipe >= 0.0f) s.pipe_rise += ((air.pipe - s.last_pipe) / dt - s.pipe_rise) * std::min(1.0f, dt);
    s.last_pipe = air.pipe;
    out.lamp_pipe = s.sensor418;

    // The air sounds (docs/reverse/brakes-dll.md §10, "Its air sounds"): a
    // sound starts and stops with its condition, each a bit of the state
    // as Brakes.dll keeps them (its 31xx codes with BR_130MODE /
    // BR_224MODE).
    const auto sound = [&](const std::uint32_t bit, const bool on, const std::uint16_t code_on, const bool mode_31) {
        if (on == ((s.sounds & bit) != 0)) return;
        s.sounds = on ? (s.sounds | bit) : (s.sounds & ~bit);
        const auto code = static_cast<std::uint16_t>(on ? code_on : code_on + 1);
        out.sounds.push_back(static_cast<std::uint16_t>(mode_31 ? code + 3000 : code));
    };
    const auto at = [&](const ValvePosition p) { return c.valve && *c.valve == p; };
    // The independent brake: filling (120) applying with the cylinder
    // under the value, letting out (122) releasing with it over; the 4ВК's
    // by its position, then (BR_254ID1 given) the 254's by 0.05 atm either
    // way; with a 4ВК the pipe gauge's sound also stops off II (a plain 137).
    if (c.vk4) {
        sound(0x2, *c.vk4 == Vk4Position::apply && air.independent < s.independent, 120, c.sounds_224);
        sound(0x4, *c.vk4 == Vk4Position::release && air.independent > s.independent, 122, c.sounds_224);
        if (!at(ValvePosition::running) && (s.sounds & 0x800u)) {
            s.sounds &= ~0x800u;
            out.sounds.push_back(137);
        }
    }
    if (c.independent_sounds) {
        const float gap = s.independent - air.independent;
        sound(0x2, gap > 0.05f, 120, c.sounds_224);
        sound(0x4, gap < -0.05f, 122, c.sounds_224);
    }
    // The pipe gauge's needle 0.2 atm over the pipe (falling behind it).
    sound(0x800, out.gauge_pipe > air.pipe + 0.2f, 136, c.sounds_130);
    // The valve's, while it works the pipe: I and II feeding it, V venting
    // it, II with the pipe up at the charge (0.4 under the pressure named:
    // Brakes.dll's (charge × 10 − 1 − 3) / 10).
    {
        // Off too when the КПТ takes the position over (a loop running stops).
        const bool own = connected && !c.by_controller;
        sound(0x8, own && at(ValvePosition::running) && air.pipe > c.charging - 0.4f, 132, c.sounds_130);
        sound(0x10, own && at(ValvePosition::charge) && air.valve_feed > 0.05f, 124, c.sounds_130);
        // II feeding: while the pipe is being charged (rising, or still
        // short of the charge), not while it only makes up its leaks at the
        // charge (the realistic air's train leaks through the engine's
        // relay all the time: the loud feed went on with the pipe steady,
        // testers on the ЧС7 and the ВЛ80т); there the quiet 132 alone.
        const float rising = (s.sounds & 0x20u) ? 0.003f : 0.01f; // on over 0.01 atm/s, off under 0.003
        const bool charging = air.pipe < c.charging - 0.4f || s.pipe_rise > rising;
        sound(0x20, own && at(ValvePosition::running) && air.valve_feed > 0.05f && charging, 126, c.sounds_130);
        sound(0x40, own && at(ValvePosition::service) && air.pipe > 0.1f, 128, c.sounds_130);
    }
    // The pipe vented at the engine (VI, ЭТ, a stop valve, the ЭПК; Brakes.dll's
    // TrainPipeRate −1.2) while there is air in it.
    sound(0x80, out.emergency && air.pipe > 0.1f, 130, c.sounds_130);
    // The ЭПТ: its release with air in it, its apply under 4.3 atm. In an
    // emergency openRTS's ЭПТ does nothing (Brakes.dll's applies in VI once
    // the pipe is under 0.1: its 134 then).
    if (c.ep_on) {
        const float level = s.valve.ep_target;
        const bool applying = out.ep_command == EpCommand::apply;
        const bool holding = applying || out.ep_command == EpCommand::hold;
        sound(0x400, !out.emergency && !holding && level > 0.05f, 122, c.sounds_130);
        sound(0x100, !out.emergency && applying && level < 4.3f, 134, c.sounds_130);
    }
    return out;
}

} // namespace openrts::simulation
