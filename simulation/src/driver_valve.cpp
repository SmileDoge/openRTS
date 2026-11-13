#include <openrts/simulation/driver_valve.hpp>

#include <algorithm>

namespace openrts::simulation {

ValveOutput step_valve_395(ValveState& state, const ValvePosition position, const ValveInputs& in, const float dt) {
    // Brake valve 395, as the ВЛ10к DLL (vl10k_main.cpp) works it. The pipe
    // follows the equalising reservoir; positive rates feed from the main
    // reservoir (a factor), negative ones vent, atm/s.
    constexpr float ur_charge_rate = 0.25f;    // BRAKE_UR_RATE_CHARGE
    constexpr float pipe_charge_div = 1.8f;    // BRAKE_PIPE_RATE_CHARGE
    constexpr float pipe_vent_max = 0.5f;      // BRAKE_PIPE_RATE
    constexpr float service_pipe_rate = 0.25f; // BRAKE_PIPE_APPL_RATE
    constexpr float emergency_rate = -1.2f;    // BRAKE_PIPE_EMERGENCY
    constexpr float lap_leak = -0.005f;        // PIPE_DISCHARGE_SLOW
    constexpr float slow_service_rate = 0.03f; // Va: 0.5 atm in 15-20 s
    auto& ur = state.equalising;
    const float p = in.pipe;
    const float mr = in.main_reservoir;
    float rate = 0.0f;
    switch (position) {
    case ValvePosition::charge:
        ur += ur_charge_rate * dt;
        ur = (std::min)(ur, mr);
        if (p < ur) rate = (ur - p) / pipe_charge_div;
        break;
    case ValvePosition::running:
        if (ur < in.charging_pressure) {
            ur += std::clamp((mr - ur) * 2.0f, 0.0f, ur_charge_rate) * dt;
        } else if (!in.realistic && in.cylinder > 0.0f && ur - p < 0.1f) {
            // The DLL's own overcharge while the locomotive is still braked:
            // it pushes the release through the RTS triple valve.
            ur += 0.15f * dt;
        }
        ur = (std::min)(ur, mr);
        // Stabiliser: overcharge removal (the DLL's 0.003 atm/s; the real
        // valve's 0.2 atm in 80-120 s with realistic air).
        if (ur > in.charging_pressure && (in.realistic || ur > p)) {
            ur = (std::max)(ur - (in.realistic ? 0.002f : 0.003f) * dt, in.charging_pressure);
        } else if (!in.realistic && ur > p) {
            ur -= 0.003f * dt;
        }
        if (p < ur - 0.01f) {
            rate = (ur - p) / pipe_charge_div;
        } else if (p > ur) {
            rate = (std::max)(ur - p, -pipe_vent_max);
        }
        break;
    case ValvePosition::lap:
        ur = (std::min)(ur, mr);
        if (p > ur) rate = ur - p;
        rate = rate < -pipe_vent_max ? -pipe_vent_max : (std::min)(rate, lap_leak);
        break;
    case ValvePosition::ep_service:
        ur = (std::min)(ur, mr);
        if (in.slow_service && !in.ep_on) {
            // Va: the УР goes down slowly and the pipe after it.
            ur = std::clamp(ur - slow_service_rate * dt, 0.0f, mr);
        }
        // ЭР9Т: the pipe only vents down to the УР (no leak, no feed).
        if (p > ur) rate = (std::max)(ur - p, -pipe_vent_max);
        break;
    case ValvePosition::lap_feed:
        ur = (std::min)(ur, mr);
        if (p > ur) {
            rate = ur - p;
        } else if (ur - p > 0.1f) {
            rate = 0.05f;
        }
        rate = (std::max)(rate, -pipe_vent_max);
        break;
    case ValvePosition::service:
        ur = std::clamp(ur - 0.2f * dt, 0.0f, mr);
        rate = -service_pipe_rate;
        break;
    case ValvePosition::emergency:
        ur = std::clamp(ur + emergency_rate * 1.2f * dt, 0.0f, mr);
        rate = emergency_rate;
        break;
    }
    ValveOutput out;
    out.pipe_rate = rate;
    out.equalising = ur;
    out.emergency = position == ValvePosition::emergency;
    // ЭПТ (ЭР9Т rates): EPT_APPLY_RATE 0.5, EPT_RELEASE_RATE 0.5 (II) and
    // EPT_RELEASE_RATE1 0.8 (I), at most 6 atm; none in VI (pneumatic).
    auto& target = state.ep_target;
    switch (position) {
    case ValvePosition::charge:
        target = (std::max)(target - 0.8f * dt, 0.0f);
        break;
    case ValvePosition::running:
        target = (std::max)(target - 0.5f * dt, 0.0f);
        break;
    case ValvePosition::ep_service:
    case ValvePosition::service:
        if (in.ep_on) target = (std::min)(target + 0.5f * dt, 6.0f);
        break;
    default:
        break;
    }
    if (in.ep_on && position != ValvePosition::emergency) {
        out.ep_value = target;
        out.systems = 1 | 2;
    }
    return out;
}

} // namespace openrts::simulation
