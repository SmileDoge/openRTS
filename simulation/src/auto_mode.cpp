#include <openrts/simulation/auto_mode.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {

namespace {

float clamp01(const float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

float reduction_for(const float damper, const float payload_min, const float payload_max,
                    const float reduction_min, const float reduction_max) noexcept {
    const float span = payload_max - payload_min;
    const float position = span > 0.0f ? clamp01((damper - payload_min) / span)
                                       : (damper >= payload_max ? 1.0f : 0.0f);
    return std::clamp(reduction_min + (reduction_max - reduction_min) * position, 0.0f, 1.0f);
}

} // namespace

void AutoMode265::reset(const float work_pressure, const float damper) noexcept {
    work_pressure_ = (std::max)(work_pressure, 0.0f);
    damper_ = clamp01(damper);
    initialized_ = true;
}

void AutoMode265::set_payload(const float payload) noexcept {
    payload_ = clamp01(payload);
}

float AutoMode265::target_brake_pressure() const noexcept {
    if (!initialized_) return 0.0f;
    const float work = (std::max)(work_pressure_, 0.0f);
    if (work <= lock_pressure) return work;
    const float reduction =
        reduction_for(damper_, payload_min, payload_max, reduction_min, reduction_max);
    return lock_pressure + reduction * (work - lock_pressure);
}

float AutoMode265::step(const float distributor_flow, const float brake_cylinder_pressure,
                        const float h) noexcept {
    if (!initialized_ || h <= 0.0f) return 0.0f;

    const float work = (std::max)(work_pressure_, 0.0f);
    const float pbc = (std::max)(brake_cylinder_pressure, 0.0f);
    const float flow_limit = (std::max)(flow_coefficient, 0.0f);
    const float passage_gain = (std::max)(gain, 0.0f);

    float to_cylinder = 0.0f;
    float from_cylinder = 0.0f;
    if (work > lock_pressure) {
        const float reduction =
            reduction_for(damper_, payload_min, payload_max, reduction_min, reduction_max);
        const float target = lock_pressure + reduction * (work - lock_pressure);
        const float difference = passage_gain * (target - pbc);
        to_cylinder = std::clamp(difference - 0.001f, 0.0f, flow_limit) * (work - pbc);
        from_cylinder = std::clamp(-difference, 0.0f, flow_limit) * pbc;
    } else {
        to_cylinder = flow_limit * (work - pbc);
    }

    const float to_brake_cylinder = to_cylinder - from_cylinder;
    const float volume = (std::max)(work_volume, 1.0e-9f);
    work_pressure_ = (std::max)(work + (distributor_flow - to_cylinder) / volume * h, 0.0f);

    if (work_pressure_ > lock_pressure) {
        // The damper is locked while the brake is applied.
    } else if (motion_time > 0.0f) {
        const float difference = payload_ - damper_;
        const float rate = std::abs(difference) > 0.05f
                               ? (difference > 0.0f ? 1.0f : -1.0f) / motion_time
                               : 20.0f * difference / motion_time;
        damper_ = clamp01(damper_ + rate * h);
    }

    return to_brake_cylinder;
}

} // namespace openrts::simulation
