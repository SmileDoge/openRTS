#include <openrts/simulation/distributor.hpp>

#include <openrts/script/luau.hpp>

#include <algorithm>
#include <cmath>

#include <lua.h>

namespace openrts::simulation {
namespace {

// Distributor values (docs/train.md, the research table).
constexpr float s483 = 0.3f / 60.0f;       // stability, atm/s
// 292: a fall from 0.5 to 0.45 MPa in 75 s or slower takes the ЗК and the
// ЗР back into the pipe without braking (Крылов §31): 0.4 atm/min (0.5 by
// ЧИПС until 2026-11-17).
constexpr float s292 = 0.4f / 60.0f;
constexpr float apply_483 = 0.15f;         // РК − pipe that starts an application
constexpr float apply_292 = 0.10f;
// The 292's spool: it takes up a 0.015 gap against its springs and sticks in
// a 0.03 band, so a pipe movement under the band does not move it; with the
// 0.05 lap that makes the 0.095 the application threshold is drawn from.
constexpr float spool_band = 0.03f;
constexpr float quick_service = 0.12f;     // atm the 483's quick service takes locally
// The 292's КДР: a 1 l chamber the pipe empties into as the application
// starts, "дополнительная разрядка магистрали в эту камеру на величину
// 0,02-0,025 МПа" (Крылов §31): a car's pipe (19 l) drops by its share,
// 0.25 at 5 atm. 0.06 atm until 2026-11-17.
constexpr float kdr_292 = 0.001f;          // m³
// The 483 in its flat mode releases fully on a rise of the pipe of
// 0.015-0.03 MPa over where it stood braked, by the step (Крылов, table 9);
// until 2026-11-17 only on the pipe's coming back within 0.06 of the РК, so
// a long train's tail did not release in II for minutes.
constexpr float rise_483 = 0.25f;
constexpr float release_483 = 0.06f;       // flat mode: release once the pipe is back within this of РК
constexpr float release_292 = 0.12f;       // a rise this far above the ЗР releases fully
// s, ЗР charging time constants, from the pipe held at 5.0: a 483's 78 l
// to 4.8 in about 4.5 min (through 1.3 mm and its check valve, Асадченко,
// Крылов §32); a 292's to 4.8 in 130-180 s (Крылов §31). 45 and 35 until
// 2026-11-17 (145 s and 113 s).
constexpr float tau_zr483 = 84.0f;
constexpr float tau_zr292 = 48.0f;
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
    switch (kind) {
    case DistributorKind::vr483: return "483";
    case DistributorKind::evr305: return "305";
    case DistributorKind::evr305_001: return "305-001";
    default: return "292";
    }
}

const char* command_name(const EpCommand command) {
    switch (command) {
    case EpCommand::apply: return "apply";
    case EpCommand::hold: return "hold";
    case EpCommand::release: return "release";
    default: return "target";
    }
}

// ЭВР 305 (Крылов §39). The application valve fills its РК (1.5 l) from the
// ЗР through a 1.8 mm seat: 3 atm in 2.5-3.5 s, a step about 1 atm a second.
// The release valve lets it out through 1.3 mm (305-000: from 3.5-4 atm to
// 0.4 in 8-10 s) or 2.0 mm (305-001: 3.5-4.5 s), whatever the cylinder:
// taken as the gauge pressure falling with a time constant (3.5 -> 0.4 in
// 9 s and 4 s), at least 0.05 atm/s so it empties. Its relay keeps the ТЦ
// at the РК, fed from the ЗР.
constexpr float ep_relay = 4.0f;         // atm/s: the relay follows the РК
constexpr float ep_chamber_volume = 0.0015f; // m³
// s: the 305's release valve as an orifice (Крылов: 305-000 1.3 mm, 305-001
// 2.0 mm). Above 0.89 atm (1.89 absolute) the flow is choked and the
// absolute pressure falls as e^(-t/τ); below, the flow goes as the root of
// the overpressure, to the air in a finite time. τ keeps the book's 3.5-4 to
// 0.4 atm in 8-10 s / 3.5-4.5 s. Until 2026-11-17 the gauge pressure fell
// as e^(-t/τ) with τ 4.15 / 1.84 s, at least 0.05 atm/s: the 305-001 took
// some 9 s from 4 to 0 (testers: "7 s, should be 4"; the book's time is to 0.4).
float ep_release_time(const DistributorKind kind) { return kind == DistributorKind::evr305_001 ? 3.34f : 7.54f; }
constexpr float ep_choked = 0.89f; // atm, gauge: the orifice's critical ratio, 1.89 absolute
float ep_release_rate(const float chamber, const float tau) {
    if (chamber >= ep_choked) return (chamber + 1.0f) / tau;
    return (ep_choked + 1.0f) / tau * std::sqrt((std::max)(chamber, 0.0f) / ep_choked);
}

// The 305's РК for a sub-step: its valves by the line's command or target.
// The РК is held to the full service's cylinder: the book has it rise to the
// charging pressure with the handle left in VЭ long, which testers dispute
// (2026-11-16); kept at the full service until a source settles it.
void step_ep_chamber(DistributorState& s, const DistributorCar& car, const float ep, const EpCommand command, const float h) {
    bool apply = false, vent = false;
    float upper = car.full_cylinder, lower = 0.0f;
    if (!car.ep) {
        vent = true; // not wired: nothing holds its РК
    } else {
        switch (command) {
        case EpCommand::apply: apply = true; break;
        case EpCommand::hold: break;
        case EpCommand::release: vent = true; break;
        default:
            // A DLL's target; none (< 0): the wire is dead and its valves let go.
            if (ep < 0.0f) {
                vent = true;
                break;
            }
            upper = lower = (std::min)(ep, car.full_cylinder);
            apply = s.ep_chamber < upper;
            vent = s.ep_chamber > lower;
            break;
        }
    }
    if (apply && s.ep_chamber < upper && s.auxiliary > s.ep_chamber) {
        const float rise = (std::min)(upper - s.ep_chamber, car.ep_fill * h * (std::min)(1.0f, (s.auxiliary - s.ep_chamber) / 0.5f));
        s.ep_chamber += rise;
        s.auxiliary = (std::max)(s.auxiliary - rise * ep_chamber_volume / car.auxiliary_volume, 0.0f);
    }
    if (vent && s.ep_chamber > lower) {
        s.ep_chamber = (std::max)(s.ep_chamber - ep_release_rate(s.ep_chamber, ep_release_time(car.kind)) * h, lower);
    }
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
    case DistributorLoad::empty: return 1.9f;
    case DistributorLoad::medium: return 3.0f;
    default: return 4.1f;
    }
}

DistributorKind distributor_kind(const formats::WagonDefinition& definition) noexcept {
    switch (definition.brakes.distributor) {
    case formats::BrakeDistributor::vr483: return DistributorKind::vr483;
    case formats::BrakeDistributor::vr292: return DistributorKind::vr292;
    case formats::BrakeDistributor::evr305: return DistributorKind::evr305;
    case formats::BrakeDistributor::evr305_001: return DistributorKind::evr305_001;
    default: break;
    }
    return inferred_distributor_kind(definition);
}

DistributorKind default_distributor_kind(const formats::WagonDefinition& definition) noexcept {
    switch (definition.brakes.default_distributor) {
    case formats::BrakeDistributor::vr483: return DistributorKind::vr483;
    case formats::BrakeDistributor::vr292: return DistributorKind::vr292;
    case formats::BrakeDistributor::evr305: return DistributorKind::evr305;
    case formats::BrakeDistributor::evr305_001: return DistributorKind::evr305_001;
    default: return inferred_distributor_kind(definition);
    }
}

bool sensor418_fitted(const formats::WagonDefinition& definition) noexcept {
    return definition.kind == formats::WagonKind::engine && distributor_kind(definition) == DistributorKind::vr483 &&
           definition.brakes.sensor418.value_or(true);
}

DistributorKind inferred_distributor_kind(const formats::WagonDefinition& definition) noexcept {
    if (definition.brakes.equipment & 4 || definition.brakes.system & 2) return DistributorKind::evr305;
    if (definition.kind == formats::WagonKind::freight) return DistributorKind::vr483;
    return DistributorKind::vr292;
}

float brake_force_pressure(const formats::WagonDefinition& definition) noexcept {
    const float pressure = definition.brakes.cylinder_pressure_for_max_force;
    if (definition.brakes.realistic_brake_force > 0.0f || distributor_kind(definition) != DistributorKind::vr483) return pressure;
    // The 483 fills the ТЦ to its load mode's full pressure: the file's force
    // stands at whichever of that and the file's own pressure is lower (its
    // MaxBrakeForce was written for the mode its mass gives).
    const auto meant = effective_load(DistributorLoad::by_mass, definition.mass);
    return (std::min)(pressure, distributor_full_cylinder(DistributorKind::vr483, meant));
}

float brake_rigging_share(const float cylinder, const float reference) noexcept {
    if (reference <= brake_touch_pressure) return 0.0f;
    return std::clamp((cylinder - brake_touch_pressure) / (reference - brake_touch_pressure), 0.0f, 1.1f);
}

void step_distributor(DistributorState& s, const DistributorCar& car, const float rate, const float ep, const float h,
                      const EpCommand command) {
    (void)rate;
    const float p = s.pipe;
    const float zr_volume = car.auxiliary_volume;
    const float tc_max = car.full_cylinder;
    const auto take_for_cylinder = [&](const float rise) {
        s.cylinder += rise;
        s.auxiliary = (std::max)(s.auxiliary - rise * car.cylinder_volume / zr_volume, 0.0f);
    };
    auto rates = plug_rates(car.plug);
    rates.fill *= car.pace;
    rates.release *= car.pace;
    rates.emergency *= car.pace;
    if (s.emergency) {
        if (!s.braking) s.discharge = true;
        s.braking = true;
        const float fill = (car.kind == DistributorKind::vr483 ? 0.45f * car.pace : rates.emergency) * h;
        const float rise = (std::min)(tc_max - s.cylinder, fill);
        if (rise > 0.0f && s.auxiliary > s.cylinder) take_for_cylinder(rise);
        s.channel = (std::min)(tc_max, s.channel + fill);
        if (s.cylinder >= s.ep_chamber) s.ep_side = false;
        return;
    }
    // ЗР charging from the pipe (the air comes out of the pipe): a 483
    // through its check valve always, a 292 (a 305's too) only while
    // released (Крылов §31, §39: the ЗР of a 305 is fed while its 292 stays
    // released, so the ЭПТ is inexhaustible).
    if (p > s.auxiliary && !(s.braking && car.kind != DistributorKind::vr483)) {
        const float tau = car.kind == DistributorKind::vr483 ? tau_zr483 : tau_zr292;
        const float charge = (p - s.auxiliary) * h / tau;
        s.auxiliary += charge;
        s.pipe = (std::max)(s.pipe - charge * zr_volume / car.pipe_volume, 0.0f);
    }
    if (car.kind == DistributorKind::vr483) {
        // РК: charges from the pipe while released; follows a slow fall.
        if (!s.braking) {
            if (p > s.reference) s.reference += (p - s.reference) * h / (car.mountain ? tau_rk_mountain : tau_rk);
            else s.reference = (std::max)(p, s.reference - s483 * h);
        }
        const float drop = s.reference - p;
        if (!s.braking && drop >= apply_483 * car.sensitivity) {
            s.braking = true;
            s.discharge = true;
            s.pipe = (std::max)(s.pipe - quick_service, 0.0f); // quick service
            s.pipe_low = s.pipe;
        }
        if (s.braking && (car.mountain ? drop < step_483 : p >= s.reference - release_483)) s.braking = false;
        if (s.braking) {
            const float target = (std::min)(tc_max, tc_max / 1.5f * drop);
            // Its channel goes to the target at the cylinder's pace (down
            // only in mountain mode's graduated release).
            if (s.channel < target) s.channel = (std::min)(target, s.channel + tc_max * 0.9f / 18.0f * car.pace * h);
            else if (car.mountain) s.channel = (std::max)(target, s.channel - tc_max / 35.0f * car.pace * h);
            const bool filling = s.cylinder < target - 0.02f && s.auxiliary > s.cylinder;
            if (filling) take_for_cylinder((std::min)(target - s.cylinder, tc_max * 0.9f / 18.0f * car.pace * h));
            // The pipe's level in lap: while it fills, where the pipe is (a
            // rise then, the quick service's dip filling back, releases
            // nothing); in lap, the lowest since.
            s.pipe_low = filling ? p : (std::min)(s.pipe_low, p);
            if (car.mountain) {
                // Mountain mode: graduated release; the ТЦ comes down to
                // what the pipe's shortfall below РК gives, and is released
                // once the pipe is back at РК.
                if (s.cylinder > target + tc_max / 1.5f * step_483) {
                    s.cylinder = (std::max)(s.cylinder - tc_max / 35.0f * car.pace * h, target);
                }
                if (drop < step_483) s.braking = false;
            } else if (p >= s.reference - release_483 || (!filling && p >= s.pipe_low + rise_483)) {
                // Flat mode: no graduated release; a rise over the braked
                // pipe, or the pipe back by the РК, releases fully, and the
                // РК lets its air into the ЗК and the pipe (Крылов §32), so
                // it charges again from where the pipe stands.
                s.braking = false;
                s.reference = (std::min)(s.reference, p);
            }
        } else if (s.cylinder > 0.0f) {
            s.cylinder = (std::max)(s.cylinder - tc_max / 35.0f * car.pace * h, 0.0f);
        }
        if (!s.braking && s.channel > 0.0f) s.channel = (std::max)(s.channel - tc_max / 35.0f * car.pace * h, 0.0f);
        if (s.cylinder < 1.0e-4f) s.cylinder = 0.0f;
        return;
    }
    // ЭВР 305: its РК by the line (step_ep_chamber). The 292 behind it
    // answers the pipe all the same, and the switching valve keeps the ТЦ on
    // the higher side. Until 2026-11-16 the wire alone set it (a pipe
    // reduction with the ЭПТ on braked nothing: the ЭП2Д, testers), and the
    // ЭПТ released at 0.35 atm/s, or at the 292 plug's rate once its demand
    // was 0.
    const bool evr = is_evr305(car.kind);
    if (evr) step_ep_chamber(s, car, ep, command, h);
    // ВР 292: the ЗР is the reference; it flows back on a slow fall.
    if (!s.braking) {
        if (p < s.auxiliary) s.auxiliary -= (std::min)(s.auxiliary - p, s292 * h);
        // Any pipe level: until 2026-11-17 not above 4.9, so a first step
        // from a charging pressure of 5.2-5.4 could brake nothing.
        if (s.auxiliary - p >= apply_292 * car.sensitivity) {
            s.braking = true;
            s.discharge = true;
            s.reference = s.auxiliary;
            s.pipe = (std::max)(s.pipe - s.pipe * kdr_292 / (car.pipe_volume + kdr_292), 0.0f); // its КДР
        }
    }
    float pneumatic = 0.0f; // the ТЦ the 292 asks for
    if (s.braking) {
        const float ratio = car.ratio > 0.5f ? car.ratio : 3.0f;
        // The spool sticks in its friction band: a pipe rise shorter than the
        // band does not let the cylinder down, so the target only follows the
        // drop past it. Up to the gap and the band, it stays at lap.
        const float target = (std::min)(tc_max, ratio * (std::max)(s.reference - p - spool_band, 0.0f));
        pneumatic = target;
        if (s.channel < pneumatic) s.channel = (std::min)(pneumatic, s.channel + rates.fill * h);
        if (s.cylinder < pneumatic && s.auxiliary > s.cylinder) {
            take_for_cylinder((std::min)(pneumatic - s.cylinder, rates.fill * h));
            if (s.cylinder > s.ep_chamber) s.ep_side = false;
        }
        // A rise above the ЗР releases fully.
        if (p >= s.auxiliary + release_292) s.braking = false;
    }
    if (!s.braking && s.channel > 0.0f) s.channel = (std::max)(s.channel - rates.release * h, 0.0f);
    if (!evr) {
        if (!s.braking && s.cylinder > 0.0f) s.cylinder = (std::max)(s.cylinder - rates.release * h, 0.0f);
        return;
    }
    // The 305's side fills its channel to its РК.
    s.channel = (std::max)(s.channel, s.ep_chamber);
    if (s.ep_chamber > 0.0f && s.ep_chamber >= s.cylinder - 1e-4f) {
        // The 305's side: its relay fills the ТЦ from the ЗР up to its РК.
        if (s.ep_chamber > s.cylinder && s.auxiliary > s.cylinder) take_for_cylinder((std::min)(s.ep_chamber - s.cylinder, ep_relay * h));
        s.ep_side = true;
    } else if (s.ep_side) {
        // Its relay lets the ТЦ out after its РК (the 305's own release
        // time), down to what the 292 holds; there the 292's side has it.
        const float floor = (std::max)(s.ep_chamber, s.braking ? pneumatic : 0.0f);
        if (s.cylinder > floor) s.cylinder = (std::max)(s.cylinder - ep_relay * h, floor);
        if (s.braking && s.cylinder <= pneumatic + 1e-4f) s.ep_side = false;
        if (s.cylinder <= 0.0f) s.ep_side = false;
    } else if (!s.braking && s.cylinder > s.ep_chamber) {
        // The 292's side: let out at its plug's rate.
        s.cylinder = (std::max)(s.cylinder - rates.release * h, s.ep_chamber);
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
            if (report.charged < 0.0f && phase == 0 && state.auxiliary >= 4.8f) report.charged = time;
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
                             std::string& error, const EpCommand command) {
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
    lua_pushstring(t, command_name(command));
    lua_setfield(t, -2, "ep_command");
    number("tc_max", car.full_cylinder);
    number("ratio", car.ratio);
    number("v_pipe", car.pipe_volume);
    number("v_zr", car.auxiliary_volume);
    number("v_tc", car.cylinder_volume);
    number("mass", car.mass);
    number("pace", car.pace);
    number("sensitivity", car.sensitivity);
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
