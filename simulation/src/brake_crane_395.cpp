#include <openrts/simulation/brake_crane_395.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {

BrakeCrane395::BrakeCrane395(BrakeCrane395Config config) : config_(config) {}

void BrakeCrane395::reset(const float brake_pipe, const float equalising) {
    brake_pipe_ = (std::max)(brake_pipe, 0.0f);
    boundary_pipe_ = brake_pipe_;
    equalising_ = (std::max)(equalising, 0.0f);
    initialized_ = true;
    position_ = ValvePosition::running;
}

void BrakeCrane395::set_state(const float brake_pipe, const float equalising) {
    brake_pipe_ = (std::max)(brake_pipe, 0.0f);
    boundary_pipe_ = brake_pipe_;
    equalising_ = (std::max)(equalising, 0.0f);
    initialized_ = true;
}

void BrakeCrane395::pre_step(const ValvePosition position) {
    // RRS krm395 preStep: in II, while the pipe is below the charging
    // pressure, the equalising piston puts it at 2 p0 - УР. This is a
    // boundary condition of the valve's own small chamber, not the train's
    // pipe; the caller's pipe node follows the resulting pressure.
    if (position == ValvePosition::running && brake_pipe_ < charge_pressure_) {
        const float ceiling = (std::max)(feed_line_, 0.0f);
        brake_pipe_ = std::clamp((std::max)(2.0f * charge_pressure_ - equalising_, 0.0f), 0.0f, ceiling);
    }
}

BrakeCrane395::Derivative BrakeCrane395::derivative(const ValvePosition position, const float pipe,
                                                    const float equalising) const {
    const bool i = position == ValvePosition::charge;
    const bool ii = position == ValvePosition::running;
    const bool va = position == ValvePosition::ep_service;
    const bool v = position == ValvePosition::service;
    const bool vi = position == ValvePosition::emergency;

    // The equalising reservoir: I charges it from the feed line, II only
    // while it is below the charging pressure; the stabiliser works in II;
    // VЭ, V and VI vent it.
    float q_equalising = 0.0f;
    if (i) q_equalising += config_.k1 * (feed_line_ - equalising);
    if (ii && equalising < charge_pressure_) q_equalising += config_.k1 * (feed_line_ - equalising);
    if (ii) q_equalising -= config_.k_stab * std::clamp(config_.k1_stab * equalising, 0.0f, 1.0f);
    if (va) q_equalising -= config_.k5 * equalising;
    if (v) q_equalising -= config_.k6 * equalising;
    if (vi) q_equalising -= config_.k9 * equalising;

    // The valve's own pipe chamber: the relay in II, the equalising piston's
    // vent, the emergency port, and I's wide channel.
    const float difference = config_.a1 * (pipe - equalising);
    const float k4 = config_.k4 * (1.0f + config_.k4_relay * (std::max)(difference, 0.0f));
    const float relay = std::clamp(config_.k2_relay * (std::max)(-difference, 0.0f), 0.0f, 1.0f) * (vi ? 0.0f : 1.0f);
    const float piston = std::clamp(config_.k3_relay * (std::max)(difference, 0.0f), 0.0f, 1.0f);
    float q_pipe = config_.k2 * (feed_line_ - pipe) * relay;
    q_pipe -= k4 * pipe * piston;
    if (vi) q_pipe -= config_.k7 * pipe;
    if (i) q_pipe += config_.k8 * (feed_line_ - pipe);

    return {q_pipe / config_.brake_pipe_volume, q_equalising / config_.eq_reservoir_volume};
}

void BrakeCrane395::step(const ValvePosition position, const float dt) {
    if (!initialized_) reset(charge_pressure_, equalising_);
    position_ = position;
    if (dt <= 0.0f) return;
    pre_step(position);
    boundary_pipe_ = brake_pipe_;

    const float step_limit = (std::max)(config_.max_substep, 1e-5f);
    const int steps = (std::max)(1, static_cast<int>(std::ceil(dt / step_limit)));
    const float h = dt / static_cast<float>(steps);
    for (int s = 0; s < steps; ++s) {
        const Derivative k1 = derivative(position, brake_pipe_, equalising_);
        const Derivative k2 = derivative(position, brake_pipe_ + 0.5f * h * k1.pipe,
                                         equalising_ + 0.5f * h * k1.equalising);
        const Derivative k3 = derivative(position, brake_pipe_ + 0.5f * h * k2.pipe,
                                         equalising_ + 0.5f * h * k2.equalising);
        const Derivative k4 = derivative(position, brake_pipe_ + h * k3.pipe,
                                         equalising_ + h * k3.equalising);
        brake_pipe_ += h * (k1.pipe + 2.0f * k2.pipe + 2.0f * k3.pipe + k4.pipe) / 6.0f;
        equalising_ += h * (k1.equalising + 2.0f * k2.equalising + 2.0f * k3.equalising + k4.equalising) / 6.0f;
        brake_pipe_ = (std::max)(brake_pipe_, 0.0f);
        equalising_ = (std::max)(equalising_, 0.0f);
    }
}

} // namespace openrts::simulation
