#include <openrts/simulation/distributor.hpp>

#include <openrts/script/luau.hpp>

#include <algorithm>
#include <cmath>

#include <lua.h>

namespace openrts::simulation {
namespace {

// Distributor values (docs/train.md, the research table).
constexpr float s483 = 0.3f / 60.0f;       // stability, atm/s
constexpr float s292 = 0.5f / 60.0f;
constexpr float apply_483 = 0.15f;         // РК − pipe that starts an application
constexpr float apply_292 = 0.10f;
constexpr float quick_service = 0.12f;     // atm the quick-service valve takes locally
constexpr float release_483 = 0.06f;       // flat mode: release once the pipe is back within this of РК
constexpr float release_292 = 0.12f;       // a rise this far above the ЗР releases fully
constexpr float tau_zr483 = 45.0f;         // s, ЗР charging time constant
constexpr float tau_zr292 = 35.0f;
constexpr float tau_rk = 60.0f;            // s, РК charging, flat mode (3 min to 95 %)
constexpr float tau_rk_mountain = 80.0f;   // s, mountain mode (4 min)
constexpr float step_483 = 0.03f;          // mountain: a pipe rise below this releases nothing

// The 292's rates by its plug, atm/s (the research table: release К 9–12 s,
// Д/УВ 19–24 s; emergency to 3.5 К 6–9 s, Д/УВ 12–16 s). Service fill is
// slowed as the emergency one (PROBABLE).
struct PlugRates {
    float fill, release, emergency;
};
PlugRates plug_rates(const DistributorPlug plug) {
    if (plug == DistributorPlug::short_train) return {0.5f, 0.35f, 0.5f};
    return {0.25f, 0.16f, 0.25f};
}

const char* kind_name(const DistributorKind kind) {
    return kind == DistributorKind::vr483 ? "483" : kind == DistributorKind::evr305 ? "305" : "292";
}

} // namespace

float distributor_auxiliary_volume(const DistributorKind kind) noexcept {
    return kind == DistributorKind::vr483 ? 0.078f : 0.055f;
}

DistributorLoad effective_load(const DistributorLoad load, const float mass) noexcept {
    if (load != DistributorLoad::by_mass) return load;
    return mass > 60000.0f ? DistributorLoad::loaded : mass > 35000.0f ? DistributorLoad::medium : DistributorLoad::empty;
}

float distributor_full_cylinder(const DistributorKind kind, const DistributorLoad load) noexcept {
    if (kind != DistributorKind::vr483) return 3.8f;
    switch (load) {
    case DistributorLoad::empty: return 1.5f;
    case DistributorLoad::medium: return 3.0f;
    default: return 3.8f;
    }
}

DistributorKind distributor_kind(const formats::WagonDefinition& definition) noexcept {
    if (definition.brakes.equipment & 4 || definition.brakes.system & 2) return DistributorKind::evr305;
    if (definition.kind == formats::WagonKind::freight) return DistributorKind::vr483;
    return DistributorKind::vr292;
}

float brake_force_pressure(const formats::WagonDefinition& definition) noexcept {
    const float pressure = definition.brakes.cylinder_pressure_for_max_force;
    if (definition.brakes.realistic_brake_force > 0.0f || distributor_kind(definition) != DistributorKind::vr483) return pressure;
    const auto meant = effective_load(DistributorLoad::by_mass, definition.mass);
    return pressure * distributor_full_cylinder(DistributorKind::vr483, meant) /
           distributor_full_cylinder(DistributorKind::vr483, DistributorLoad::loaded);
}

void step_distributor(DistributorState& s, const DistributorCar& car, const float rate, const float ep, const float h) {
    (void)rate;
    const float p = s.pipe;
    const float zr_volume = car.auxiliary_volume;
    const float tc_max = car.full_cylinder;
    const auto take_for_cylinder = [&](const float rise) {
        s.cylinder += rise;
        s.auxiliary = (std::max)(s.auxiliary - rise * car.cylinder_volume / zr_volume, 0.0f);
    };
    const auto rates = plug_rates(car.plug);
    if (s.emergency) {
        s.braking = true;
        const float fill = (car.kind == DistributorKind::vr483 ? 0.45f : rates.emergency) * h;
        const float rise = (std::min)(tc_max - s.cylinder, fill);
        if (rise > 0.0f && s.auxiliary > s.cylinder) take_for_cylinder(rise);
        return;
    }
    // ЗР charging from the pipe (the air comes out of the pipe).
    if (p > s.auxiliary && !(s.braking && car.kind == DistributorKind::vr292)) {
        const float tau = car.kind == DistributorKind::vr483 ? tau_zr483 : tau_zr292;
        const float charge = (p - s.auxiliary) * h / tau;
        s.auxiliary += charge;
        s.pipe = (std::max)(s.pipe - charge * zr_volume / car.pipe_volume, 0.0f);
    }
    // ЭВР 305 with the EP brake working: the cylinder follows the wire.
    if (car.kind == DistributorKind::evr305 && car.ep && ep >= 0.0f) {
        const float target = (std::min)(ep, tc_max);
        const float change = std::clamp(target - s.cylinder, -0.35f * h, 1.0f * h);
        s.cylinder = (std::max)(s.cylinder + change, 0.0f);
        s.braking = s.cylinder > 0.05f;
        s.reference = s.auxiliary;
        return;
    }
    if (car.kind == DistributorKind::vr483) {
        // РК: charges from the pipe while released; follows a slow fall.
        if (!s.braking) {
            if (p > s.reference) s.reference += (p - s.reference) * h / (car.mountain ? tau_rk_mountain : tau_rk);
            else s.reference = (std::max)(p, s.reference - s483 * h);
        }
        const float drop = s.reference - p;
        if (!s.braking && drop >= apply_483) {
            s.braking = true;
            s.pipe = (std::max)(s.pipe - quick_service, 0.0f); // quick service
        }
        if (s.braking) {
            const float target = (std::min)(tc_max, tc_max / 1.5f * drop);
            if (s.cylinder < target && s.auxiliary > s.cylinder) {
                take_for_cylinder((std::min)(target - s.cylinder, tc_max * 0.9f / 18.0f * h));
            }
            if (car.mountain) {
                // Mountain mode: graduated release; the ТЦ comes down to
                // what the pipe's shortfall below РК gives, and is released
                // once the pipe is back at РК.
                if (s.cylinder > target + tc_max / 1.5f * step_483) {
                    s.cylinder = (std::max)(s.cylinder - tc_max / 35.0f * h, target);
                }
                if (drop < step_483) s.braking = false;
            } else if (p >= s.reference - release_483) {
                // Flat mode: no graduated release; back within this of РК releases fully.
                s.braking = false;
            }
        } else if (s.cylinder > 0.0f) {
            s.cylinder = (std::max)(s.cylinder - tc_max / 35.0f * h, 0.0f);
        }
        return;
    }
    // ВР 292: the ЗР is the reference; it flows back on a slow fall.
    if (!s.braking) {
        if (p < s.auxiliary) s.auxiliary -= (std::min)(s.auxiliary - p, s292 * h);
        if (s.auxiliary - p >= apply_292 && p < 4.9f) {
            s.braking = true;
            s.reference = s.auxiliary;
            s.pipe = (std::max)(s.pipe - quick_service * 0.5f, 0.0f);
        }
    }
    if (s.braking) {
        const float ratio = car.ratio > 0.5f ? car.ratio : 3.0f;
        const float target = (std::min)(tc_max, ratio * (s.reference - p));
        if (s.cylinder < target && s.auxiliary > s.cylinder) take_for_cylinder((std::min)(target - s.cylinder, rates.fill * h));
        // A rise above the ЗР releases fully.
        if (p >= s.auxiliary + release_292) s.braking = false;
    } else if (s.cylinder > 0.0f) {
        s.cylinder = (std::max)(s.cylinder - rates.release * h, 0.0f);
    }
}

StandReport run_stand(const DistributorCar& car, ScriptDistributor* script) {
    // The program: (until, target pipe, rate atm/s, emergency).
    struct Phase {
        float until, target, rate;
        bool emergency;
    };
    constexpr float stabiliser = 0.15f / 60.0f;
    const Phase program[] = {
        {300.0f, 5.0f, 0.5f, false},  {360.0f, 4.5f, 0.25f, false},     {450.0f, 5.0f, 0.25f, false},
        {510.0f, 3.5f, 0.25f, false}, {570.0f, 5.6f, 0.5f, false},      {840.0f, 5.0f, stabiliser, false},
        {900.0f, 0.0f, 2.5f, true},
    };
    StandReport report;
    DistributorState state;
    constexpr float h = 1.0f / 300.0f;
    float time = 0.0f;
    float next_sample = 0.0f;
    float last_pipe = 0.0f;
    float lowest = 1e9f; // ТЦ, the least during the overcharge's removal
    std::size_t phase = 0;
    for (; phase < std::size(program); ++phase) {
        const auto& p = program[phase];
        while (time < p.until) {
            // The stand holds the pipe to its program.
            const float step = p.rate * h;
            state.pipe = state.pipe < p.target ? (std::min)(state.pipe + step, p.target) : (std::max)(state.pipe - step, p.target);
            const float rate = (state.pipe - last_pipe) / h;
            last_pipe = state.pipe;
            state.emergency = p.emergency;
            if (script) {
                if (!script->step(state, car, rate, -1.0f, h, report.error)) return report;
            } else {
                step_distributor(state, car, rate, -1.0f, h);
            }
            time += h;
            if (time >= next_sample) {
                next_sample += 1.0f;
                report.samples.push_back({time, state.pipe, state.auxiliary, state.cylinder, state.reference, state.braking});
            }
            // The figures.
            if (report.charged < 0.0f && phase == 0 && state.auxiliary >= 4.9f) report.charged = time;
            if (phase == 1) report.step_cylinder = state.cylinder;
            if (phase == 3) report.full_cylinder = state.cylinder;
            if (phase == 5) {
                lowest = (std::min)(lowest, state.cylinder);
                report.creep = (std::max)(report.creep, state.cylinder - lowest);
            }
            if (phase == 6) report.emergency_cylinder = state.cylinder;
            if (phase == 6 && report.emergency_fill < 0.0f && state.cylinder >= 0.95f * car.full_cylinder) {
                report.emergency_fill = time - program[5].until;
            }
            if (phase == 2 && report.release < 0.0f && state.cylinder < 0.4f) report.release = time - program[1].until;
        }
    }
    // The step's fill time, from the samples: 95 % of its final ТЦ.
    for (const auto& s : report.samples) {
        if (s.time > program[0].until && s.time <= program[1].until && s.cylinder >= 0.95f * report.step_cylinder) {
            report.step_fill = s.time - program[0].until;
            break;
        }
    }
    return report;
}

struct ScriptDistributor::Impl {
    script::Vm vm;
    int car_ref = 0;
};

ScriptDistributor::ScriptDistributor() : impl_(std::make_unique<Impl>()) {}
ScriptDistributor::~ScriptDistributor() = default;

std::unique_ptr<ScriptDistributor> ScriptDistributor::load(const std::string& source, const std::string& name, std::string& error) {
    const auto chunk = script::compile(source, name, error);
    if (!chunk) return nullptr;
    std::unique_ptr<ScriptDistributor> result(new ScriptDistributor());
    auto& vm = result->impl_->vm;
    if (!vm.run(*chunk, error)) return nullptr;
    lua_State* t = vm.thread();
    lua_getglobal(t, "step");
    const bool has_step = lua_isfunction(t, -1);
    lua_pop(t, 1);
    if (!has_step) {
        error = name + ": no function step(c, dt)";
        return nullptr;
    }
    // The car's table, kept between steps; `state` is the script's own.
    lua_createtable(t, 0, 20);
    lua_createtable(t, 0, 0);
    lua_setfield(t, -2, "state");
    result->impl_->car_ref = lua_ref(t, -1);
    lua_pop(t, 1);
    return result;
}

bool ScriptDistributor::step(DistributorState& s, const DistributorCar& car, const float rate, const float ep, const float h,
                             std::string& error) {
    auto& vm = impl_->vm;
    lua_State* t = vm.thread();
    lua_getglobal(t, "step");
    lua_getref(t, impl_->car_ref);
    const auto number = [t](const char* key, const double value) {
        lua_pushnumber(t, value);
        lua_setfield(t, -2, key);
    };
    const auto flag = [t](const char* key, const bool value) {
        lua_pushboolean(t, value ? 1 : 0);
        lua_setfield(t, -2, key);
    };
    number("pipe", s.pipe);
    number("rate", rate);
    number("zr", s.auxiliary);
    number("tc", s.cylinder);
    number("ref", s.reference);
    flag("braking", s.braking);
    flag("emergency", s.emergency);
    number("ep", ep);
    number("tc_max", car.full_cylinder);
    number("ratio", car.ratio);
    number("v_pipe", car.pipe_volume);
    number("v_zr", car.auxiliary_volume);
    number("v_tc", car.cylinder_volume);
    number("mass", car.mass);
    flag("ep_wired", car.ep);
    lua_pushstring(t, kind_name(car.kind));
    lua_setfield(t, -2, "kind");
    lua_pushstring(t, car.load == DistributorLoad::empty ? "empty" : car.load == DistributorLoad::medium ? "medium" : "loaded");
    lua_setfield(t, -2, "load");
    flag("mountain", car.mountain);
    lua_pushstring(t, car.plug == DistributorPlug::short_train ? "short" : car.plug == DistributorPlug::long_train ? "long" : "no_accelerator");
    lua_setfield(t, -2, "plug");
    lua_pushnumber(t, h);
    if (!vm.call(2, 0, error)) return false;
    // Back: what the script set (numbers only, not below 0).
    lua_getref(t, impl_->car_ref);
    const auto read = [t](const char* key, float& out) {
        lua_getfield(t, -1, key);
        if (lua_isnumber(t, -1)) {
            const double value = lua_tonumber(t, -1);
            if (std::isfinite(value)) out = static_cast<float>((std::max)(value, 0.0));
        }
        lua_pop(t, 1);
    };
    const auto read_flag = [t](const char* key, bool& out) {
        lua_getfield(t, -1, key);
        if (lua_isboolean(t, -1)) out = lua_toboolean(t, -1) != 0;
        lua_pop(t, 1);
    };
    read("pipe", s.pipe);
    read("zr", s.auxiliary);
    read("tc", s.cylinder);
    read("ref", s.reference);
    read_flag("braking", s.braking);
    read_flag("emergency", s.emergency);
    lua_pop(t, 1);
    return true;
}

} // namespace openrts::simulation
