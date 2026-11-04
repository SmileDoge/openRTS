// The ВЛ10к's locomotive logic as a native openRTS plugin: an example of the
// API (include/openrts/plugin/train_plugin.hpp), ported from the RTS DLL's
// source (TRAINS/TRAINSET/tsrLoco_VL10k-1461/cab/vl10k_main.cpp) for one
// section driven from its cab. What it keeps:
// - the main circuit: pantographs (SW_TPO / SW_TP), the main breaker (SW_BV
//   off, B_BVV on), DC line 2..4.5 kV only (the breaker trips on anything
//   else), the compressor (SW_COMPR) between 6 and 9.1 atm;
// - the controller: 45 positions in four connections (С 1-16, СП 17-27,
//   П1 28-36, П2 37-45) set by the buttons B_0, B_S, B_SP, B_P1, B_P2 and
//   stepped by B_+ / B_-, one position a second while the motor current is
//   below the setting R_CURM; field weakening ОП1..ОП4 (B_SH) on a running
//   position; the DLL's tractive effort curves, smoothed by 30 kN/s;
// - the reverser keys K_STR / K_BACK, the 395 driver's valve (C_394) and the
//   254 independent valve (C_254) as the DLL works them, the sander, horn
//   and whistle;
// - the gauges D_UR, D_TM, D_GR, D_TC and the lamps DD_S .. DD_P2, DD_SH.
// Left out: multiple units, regenerative braking, the КЛУБ and САУТ, the
// auxiliary machines and the second section (it is not driven).

#include <openrts/plugin/train_plugin.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

constexpr float current_q = 430.0f;        // CURRENT_Q: N of force per A
constexpr float force_shift = 30000.0f;    // FORCE_SHIFT: N/s
constexpr float switch_time = 1.0f;        // TIME_THROTTLE_SWITCH1, s per position
constexpr float ur_charge_rate = 0.25f;    // BRAKE_UR_RATE_CHARGE
constexpr float pipe_charge_rate = 1.8f;   // BRAKE_PIPE_RATE_CHARGE
constexpr float pipe_rate = 0.5f;          // BRAKE_PIPE_RATE
constexpr float pipe_apply_rate = 0.25f;   // BRAKE_PIPE_APPL_RATE
constexpr float pipe_emergency = -1.2f;    // BRAKE_PIPE_EMERGENCY
constexpr float pipe_slow = -0.005f;       // PIPE_DISCHARGE_SLOW

class Vl10kNative final : public orts::Locomotive {
public:
    Vl10kNative(orts::Host host, const OrtsTrainSetup&) : orts::Locomotive(host) {
        const auto& h = this->host();
        valve_ = h.control("C_394");
        independent_ = h.control("C_254");
        buttons_[0] = h.control("B_0");
        buttons_[1] = h.control("B_S");
        buttons_[2] = h.control("B_SP");
        buttons_[3] = h.control("B_P1");
        buttons_[4] = h.control("B_P2");
        minus_ = h.control("B_-");
        plus_ = h.control("B_+");
        shunt_ = h.control("B_SH");
        sand_ = h.control("B_SAND");
        horn_ = h.control("B_TIF");
        whistle_ = h.control("B_SVIST");
        breaker_off_ = h.control("SW_BV");
        breaker_on_ = h.control("B_BVV");
        panto_[0] = h.control("SW_TPO");
        panto_[1] = h.control("SW_TP");
        compressor_ = h.control("SW_COMPR");
        current_setting_ = h.control("R_CURM");
        forward_ = h.control("K_STR");
        back_ = h.control("K_BACK");
        gauge_ur_ = h.indicator("D_UR");
        gauge_pipe_ = h.indicator("D_TM");
        gauge_main_ = h.indicator("D_GR");
        gauge_cylinder_ = h.indicator("D_TC");
        lamp_group_[0] = h.indicator("DD_S");
        lamp_group_[1] = h.indicator("DD_SP");
        lamp_group_[2] = h.indicator("DD_P1");
        lamp_group_[3] = h.indicator("DD_P2");
        lamp_shunt_ = h.indicator("DD_SH");
        lamp_plus_ = h.indicator("DD_+");
        lamp_minus_ = h.indicator("DD_-");
        // The DLL's Init for a running start: the breaker on, the rear
        // pantograph up, the valve in II, the equalising reservoir charged.
        h.set(breaker_off_, 0.0f);
        h.set(panto_[0], 0.0f);
        h.set(valve_, 1.0f);
        h.set(compressor_, 1.0f);
        breaker_ = true;
        pantographs_ = 1;
        ur_ = 5.0f;
    }

    void step(double dt_in, const OrtsTrainInputs& in, OrtsTrainOutputs& out) override {
        const auto dt = static_cast<float>(dt_in);
        const auto& h = host();
        // The main circuit: a DC line of 2..4.5 kV through a raised
        // pantograph and the closed breaker (the DLL trips it otherwise).
        bool current_on = false;
        if (in.line_voltage > 0.0f && breaker_) {
            if (in.line_voltage > 2000.0f && in.line_voltage < 4500.0f && in.line_frequency == 0.0f) {
                current_on = true;
            } else {
                trip("БВ: напряжение не то");
            }
        }
        // The compressor (SW_COMPR on): starts below 6, stops above 9.1 atm.
        out.main_reservoir_rate = 0.0f;
        if (current_on && h.state(compressor_) > 0.5f) {
            if (!compressing_ && in.main_reservoir < 6.0f) {
                compressing_ = true;
                h.sound(101, true);
                h.sound(101, false);
            } else if (compressing_ && in.main_reservoir > 9.1f) {
                compressing_ = false;
                h.sound(102, true);
                h.sound(102, false);
            }
        } else if (compressing_) {
            compressing_ = false;
            h.sound(102, true);
            h.sound(102, false);
        }
        if (compressing_) out.main_reservoir_rate = (12.1f - in.main_reservoir) * 0.015f;

        // The controller steps towards the position the buttons asked for,
        // one a second while the current is under the setting.
        const float setting = current_limit();
        clock_ += dt;
        if (target_ != position_ && clock_ >= switch_time) {
            clock_ = 0.0f;
            if (target_ < position_) {
                --position_;
            } else if (current_ < setting) {
                ++position_;
            }
            h.sound(16, true);
        }
        if (position_ == 0) shunts_ = 0;

        // Tractive effort (the DLL's curves by connection).
        float force = 0.0f;
        float motor_voltage = 0.0f;
        if (current_on && reverse_ != 0 && position_ > 0) {
            const int p = position_;
            const int connection = p <= 16 ? 1 : p <= 27 ? 2 : p <= 36 ? 3 : 4;
            float top = 0.0f;
            switch (connection) {
            case 1: top = -3.125f + p * 0.245f; break;
            case 2: top = -2.625f + p * 0.225f; break;
            default: top = -15.525f + p * 0.636f; break;
            }
            const float line_q = in.line_voltage / 3000.0f;
            const float v = in.speed * static_cast<float>(reverse_) - top;
            if (v > 0.0f) {
                switch (connection) {
                case 1:
                    force = (150000.0f + 9000.0f * p) / std::pow(v, 2.18f + (16 - p) * 0.06f);
                    motor_voltage = 375.0f * line_q;
                    break;
                case 2:
                    force = (150000.0f + 10000.0f * p) / std::pow(v, 2.49f - p * 0.02f);
                    motor_voltage = 750.0f * line_q;
                    break;
                default:
                    force = (250000.0f + 22000.0f * p) / std::pow(v, 2.16f - p * 0.01f);
                    motor_voltage = 1500.0f * line_q;
                    break;
                }
                force *= 5.0f;
            } else {
                force = 800000.0f;
                motor_voltage = 375.0f * line_q;
            }
            static constexpr float shunt_gain[5] = {1.0f, 1.25f, 1.8f, 2.4f, 2.8f};
            force *= shunt_gain[std::clamp(shunts_, 0, 4)] * line_q;
        }
        // Smoothed by FORCE_SHIFT.
        if (force > smoothed_) {
            smoothed_ = (std::min)(smoothed_ + force_shift * dt, force);
        } else {
            smoothed_ = (std::max)(smoothed_ - force_shift * dt, force);
        }
        current_ = smoothed_ / current_q * (shunts_ > 0 ? 1.2f : 1.0f);
        out.traction_force = smoothed_ * static_cast<float>(reverse_);
        (void)motor_voltage;

        // Sanding while the button is held.
        out.sanding = h.state(sand_) > 0.5f ? 1.0f : 0.0f;
        if ((out.sanding > 0.0f) != sanding_) {
            sanding_ = out.sanding > 0.0f;
            h.sound(sanding_ ? 4u : 5u, false);
        }
        // The horn and the whistle sound a second per press.
        for (int k = 0; k < 2; ++k) {
            if (blast_[k] > 0.0f) {
                blast_[k] -= dt;
                if (blast_[k] <= 0.0f) {
                    h.sound(k == 0 ? 9u : 11u, true);
                    h.sound(k == 0 ? 9u : 11u, false);
                    h.set(k == 0 ? horn_ : whistle_, 0.0f);
                }
            }
        }

        // The 254: independent brake by its position.
        const int indep = static_cast<int>(std::lround(h.state(independent_)));
        switch (indep) {
        case 0: independent_target_ = 0.0f; break;
        case 1: independent_target_ = in.independent; break;
        default: independent_target_ = (std::max)(independent_target_, static_cast<float>(indep - 1)); break;
        }
        out.independent_brake = (std::min)(independent_target_, in.main_reservoir);

        // The 395 as the DLL works it (vl10k_main.cpp, "Train brake").
        float rate = 0.0f;
        const int valve = static_cast<int>(std::lround(h.state(valve_)));
        switch (valve) {
        case 0: // I: charge
            ur_ = (std::min)(ur_ + ur_charge_rate * dt, in.main_reservoir);
            if (in.brake_pipe < ur_) rate = (ur_ - in.brake_pipe) / pipe_charge_rate;
            break;
        case 1: // II: running
            if (ur_ < 5.0f) {
                ur_ += std::clamp((in.main_reservoir - ur_) * 2.0f, 0.0f, ur_charge_rate) * dt;
            } else if (in.brake_cylinder > 0.0f && ur_ - in.brake_pipe < 0.1f) {
                ur_ += 0.15f * dt;
            }
            ur_ = (std::min)(ur_, in.main_reservoir);
            if (ur_ > in.brake_pipe) ur_ -= 0.003f * dt;
            if (in.brake_pipe < ur_ - 0.01f) {
                rate = (ur_ - in.brake_pipe) / pipe_charge_rate;
            } else if (in.brake_pipe > ur_) {
                rate = (std::max)(ur_ - in.brake_pipe, -pipe_rate);
            }
            break;
        case 2: // III: lap without feed
            ur_ = (std::min)(ur_, in.main_reservoir);
            if (in.brake_pipe > ur_) rate = ur_ - in.brake_pipe;
            rate = std::clamp(rate, -pipe_rate, pipe_slow);
            break;
        case 3: // IV: lap with feed
            ur_ = (std::min)(ur_, in.main_reservoir);
            if (in.brake_pipe > ur_) {
                rate = ur_ - in.brake_pipe;
            } else if (ur_ - in.brake_pipe > 0.1f) {
                rate = 0.05f;
            }
            rate = (std::max)(rate, -pipe_rate);
            break;
        case 4: // V: service
            ur_ = std::clamp(ur_ - 0.2f * dt, 0.0f, in.main_reservoir);
            rate = -pipe_apply_rate;
            break;
        default: // VI: emergency
            ur_ = std::clamp(ur_ + pipe_emergency * 1.2f * dt, 0.0f, in.main_reservoir);
            rate = pipe_emergency;
            break;
        }
        out.brake_pipe_rate = rate;
        out.equalising = ur_;
        out.emergency = valve >= 5 ? 1u : 0u;
        out.brake_systems = 1;
        out.ep_value = -1.0f;
        out.brake_force = 0.0f;

        out.pantographs = pantographs_;
        out.main_switch = breaker_ ? 1u : 0u;

        // Gauges and lamps.
        h.show(gauge_ur_, ur_);
        h.show(gauge_pipe_, in.brake_pipe);
        h.show(gauge_main_, in.main_reservoir);
        h.show(gauge_cylinder_, (std::max)(in.brake_cylinder, in.independent));
        const int connection = position_ == 0 ? 0 : position_ <= 16 ? 1 : position_ <= 27 ? 2 : position_ <= 36 ? 3 : 4;
        for (int k = 0; k < 4; ++k) h.show(lamp_group_[k], 0.0f, connection == k + 1 ? 1u : 0u);
        h.show(lamp_shunt_, 0.0f, shunts_ > 0 ? 1u : 0u);
        h.show(lamp_plus_, 0.0f, target_ > position_ ? 1u : 0u);
        h.show(lamp_minus_, 0.0f, target_ < position_ ? 1u : 0u);
    }

    bool request(OrtsHandle control, float, float to) override {
        // The reverser only with the controller at 0 (the DLL's CanSwitch).
        if ((control == forward_ || control == back_) && position_ != 0) {
            host().message(2, "Реверсор: сначала контроллер на 0");
            return false;
        }
        // The breaker closes only with the controller at 0.
        if (control == breaker_on_ && to > 0.5f && position_ != 0) {
            host().message(2, "БВ: сначала контроллер на 0");
            return false;
        }
        return true;
    }

    void changed(OrtsHandle control, float) override {
        const auto& h = host();
        const float state = h.state(control);
        const bool pressed = state > 0.5f;
        for (int k = 0; k < 5; ++k) {
            if (control != buttons_[k] || !pressed) continue;
            static constexpr int ends[5] = {0, 16, 27, 36, 45};
            target_ = ends[k];
            h.set(control, 0.0f); // the button springs back
        }
        if (control == plus_ && pressed) {
            target_ = (std::min)((std::max)(target_, position_) + 1, 45);
            h.set(control, 0.0f);
        }
        if (control == minus_ && pressed) {
            if (shunts_ > 0) {
                --shunts_;
            } else {
                target_ = (std::max)((std::min)(target_, position_) - 1, 0);
            }
            h.set(control, 0.0f);
        }
        if (control == shunt_ && pressed) {
            // Field weakening on a running position only (16, 27, 36, 45).
            if (position_ == 16 || position_ == 27 || position_ == 36 || position_ == 45) shunts_ = (std::min)(shunts_ + 1, 4);
            h.set(control, 0.0f);
        }
        if (control == forward_ || control == back_) {
            // KEY switches: 0 key out, 1 in, 2 turned.
            const bool fwd = h.state(forward_) >= 2.0f, bwd = h.state(back_) >= 2.0f;
            reverse_ = fwd && !bwd ? 1 : bwd && !fwd ? -1 : 0;
        }
        if (control == breaker_off_ && pressed) trip("БВ отключён");
        if (control == breaker_on_ && pressed) {
            if (h.state(breaker_off_) < 0.5f && pantographs_) {
                breaker_ = true;
                h.sound(23, true);
            }
            h.set(control, 0.0f);
        }
        for (int k = 0; k < 2; ++k) {
            // State 0 raises the pantograph, 1 lowers it (the DLL's AsyncFlags).
            if (control != panto_[k]) continue;
            const std::uint32_t bit = 1u << k;
            pantographs_ = state < 0.5f ? (pantographs_ | bit) : (pantographs_ & ~bit);
            if (!pantographs_) trip("");
        }
        if (control == horn_ && pressed) {
            blast_[0] = 1.0f;
            h.sound(8, true);
            h.sound(8, false);
        }
        if (control == whistle_ && pressed) {
            blast_[1] = 1.0f;
            h.sound(10, true);
            h.sound(10, false);
        }
    }

private:
    // R_CURM: the current setting from its frame (the DLL's var[5]).
    [[nodiscard]] float current_limit() const {
        const float f = host().state(current_setting_);
        if (f <= 1.0f) return f * 200.0f;
        if (f >= 9.0f) return 600.0f + (f - 9.0f) * 100.0f;
        return 200.0f + (f - 1.0f) * 50.0f;
    }
    void trip(const char* why) {
        if (!breaker_) return;
        breaker_ = false;
        target_ = position_ = 0;
        shunts_ = 0;
        host().sound(24, true);
        if (why && *why) host().message(2, why);
    }

    OrtsHandle valve_ = -1, independent_ = -1, minus_ = -1, plus_ = -1, shunt_ = -1, sand_ = -1, horn_ = -1, whistle_ = -1;
    OrtsHandle breaker_off_ = -1, breaker_on_ = -1, compressor_ = -1, current_setting_ = -1, forward_ = -1, back_ = -1;
    OrtsHandle buttons_[5]{-1, -1, -1, -1, -1};
    OrtsHandle panto_[2]{-1, -1};
    OrtsHandle gauge_ur_ = -1, gauge_pipe_ = -1, gauge_main_ = -1, gauge_cylinder_ = -1;
    OrtsHandle lamp_group_[4]{-1, -1, -1, -1};
    OrtsHandle lamp_shunt_ = -1, lamp_plus_ = -1, lamp_minus_ = -1;
    int position_ = 0, target_ = 0, shunts_ = 0, reverse_ = 0;
    float clock_ = 0.0f, smoothed_ = 0.0f, current_ = 0.0f, ur_ = 5.0f, independent_target_ = 0.0f;
    float blast_[2]{};
    std::uint32_t pantographs_ = 0;
    bool breaker_ = false, compressing_ = false, sanding_ = false;
};

} // namespace

ORTS_TRAIN_PLUGIN(Vl10kNative, "ВЛ10к (openRTS native sample)")
