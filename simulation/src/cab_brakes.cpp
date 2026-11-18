#include <openrts/simulation/cab_brakes.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {

CabBrakeNumbers CabBrakeNumbers::brakes_dll() {
    // MaDaDev's Brakes.dll's (docs/reverse/brakes-dll.md §10).
    CabBrakeNumbers n;
    n.charge_ur_rate = 0.18f;
    n.charge_ur_gain = 2.0f;
    n.running_ur_rate = 0.1f;
    n.running_ur_gain = 2.0f;
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
    n.sensor418_rate = 0.0125f;
    n.sensor418_time = 0.5f;
    n.sensor418_cylinder = 0.3f;
    return n;
}

CabBrakeNumbers CabBrakeNumbers::openrts() {
    // The real devices' (docs/brakes-dll-replacement.md, "The numbers"):
    // Brakes.dll's where they fall within what the books give, else the
    // books'. Крылов В. И., Крылов В. В., «Автоматические тормоза
    // подвижного состава», 1983 (pages of the book).
    CabBrakeNumbers n = brakes_dll();
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

std::optional<Vk4Position> vk4_position_of(const int state) {
    if (state < 0 || state > 2) return std::nullopt;
    return static_cast<Vk4Position>(state);
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
            break;
        case ValvePosition::service:
            if (c.valve_resting) ur = (std::max)(ur - n.service * dt, 0.0f);
            break;
        case ValvePosition::emergency:
            ur = (std::max)(ur - n.emergency_ur * dt, 0.0f);
            emergency = true;
            break;
        }
    }
    if (c.keep_equalising) ur = ur_before;
    const bool connected = c.valve.has_value() && !c.cut_off;
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
        s.pipe_needle = (std::max)(s.pipe_needle - n.pipe_needle_fall * dt, air.pipe);
    }
    out.gauge_cylinder = s.cylinder_needle;
    out.gauge_pipe = s.pipe_needle;
    out.gauge_main_reservoir = mr;
    out.gauge_equalising = ur;

    // The 418: the pipe falling without the ЭПТ until the engine's brake
    // answers.
    if (c.sensor418 && dt > 0.0f) {
        const float falling = (s.last_pipe - air.pipe) / dt;
        s.sensor418_time = !c.ep_on && falling > n.sensor418_rate ? s.sensor418_time + dt : 0.0f;
        if (s.sensor418_time > n.sensor418_time) s.sensor418 = true;
        if ((std::max)(air.cylinder, air.independent) > n.sensor418_cylinder) s.sensor418 = false;
    } else if (!c.sensor418) {
        s.sensor418 = false;
        s.sensor418_time = 0.0f;
    }
    s.last_pipe = air.pipe;
    out.lamp_pipe = s.sensor418;
    return out;
}

} // namespace openrts::simulation
