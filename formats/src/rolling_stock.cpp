#include <openrts/formats/rolling_stock.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#include "rts_text.hpp"

namespace openrts::formats {
namespace {

using detail::rts_atof;
using detail::rts_atoi;
using detail::rts_find;
using detail::rts_next;
using detail::rts_product;
using detail::rts_quantity;
using detail::rts_token;
using detail::TextRange;

// psi to atmospheres as RTS converts brake values (1 / 14.69).
constexpr double psi_to_atm = 0.06807351940095303158;

// A pressure as MSTS writes it, in atm: psi unless the token names its unit
// (bar, kPa, inHg; one AirBrakesCompressorRestartPressure of the installation
// says "7.5bar"). 0 for none or nonsense.
[[nodiscard]] float pressure_atm(const std::string_view token) {
    std::size_t length = 0;
    while (length < token.size() && (std::isdigit(static_cast<unsigned char>(token[length])) || token[length] == '.' || token[length] == '-'))
        ++length;
    if (length == 0) return 0.0f;
    const double number = std::atof(std::string(token.substr(0, length)).c_str());
    std::string unit(token.substr(length));
    for (auto& c : unit) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    double atm = number * psi_to_atm;
    if (unit.starts_with("bar")) atm = number * 0.98692327;
    else if (unit.starts_with("kpa")) atm = number / 101.325;
    else if (unit.starts_with("inhg")) atm = number * 0.0334211;
    return atm > 0.0 ? static_cast<float>(atm) : 0.0f;
}

// Reads consecutive values of the block `keyword` found in `within`.
class Values {
public:
    Values(std::string_view text, std::optional<TextRange> block) : text_(text), block_(block) {
        if (block_) at_ = block_->begin;
    }
    [[nodiscard]] bool found() const noexcept { return block_.has_value(); }
    // RTS reads at the position where the search left off; for a missing
    // block that is the start of the file, which parses as 0.
    [[nodiscard]] std::string_view here() const {
        return block_ ? text_.substr(at_, block_->end - at_) : std::string_view{};
    }
    double atof() { return take(rts_atof(here())); }
    // The next whole number (negative read as 0), as read_uint, moving on.
    std::uint32_t uint() {
        const auto value = static_cast<std::uint32_t>((std::max)(0, rts_atoi(here())));
        advance();
        return value;
    }
    double quantity() { return take(rts_quantity(here())); }
    double product() { return take(rts_product(here())); }
    std::string token() {
        auto value = block_ ? rts_token(text_, at_, block_->end) : std::string{};
        advance();
        return value;
    }

private:
    double take(double value) {
        advance();
        return value;
    }
    void advance() {
        if (block_) at_ = rts_next(text_, at_, block_->end);
    }
    std::string_view text_;
    std::optional<TextRange> block_;
    std::size_t at_{};
};

[[nodiscard]] Values values(std::string_view text, std::string_view keyword, TextRange within) {
    return Values(text, rts_find(text, keyword, within));
}

[[nodiscard]] Values values(std::string_view text, std::string_view keyword) {
    return values(text, keyword, {0, text.size()});
}

[[nodiscard]] bool contains(std::string_view text, std::optional<TextRange> range, std::string_view word) {
    if (!range) return false;
    return text.substr(range->begin, range->end - range->begin).find(word) != std::string_view::npos;
}

void read_coupling(std::string_view text, TextRange block, float mass, const Coupling* defaults, Coupling& out) {
    const auto type = values(text, "type", block).token();
    out.type = type == "automatic" ? 0 : type == "bar" ? 1 : type == "chain" ? 2 : 3;
    auto stiffness = values(text, "stiffness", block);
    out.stiffness_tension = static_cast<float>(stiffness.atof());
    out.stiffness_compression = static_cast<float>(stiffness.atof());
    auto damping = values(text, "damping", block);
    out.damping_tension = static_cast<float>(damping.atof());
    out.damping_compression = static_cast<float>(damping.atof());
    // Zero values take the front coupling's, or mass-based defaults.
    if (out.stiffness_tension == 0.0f) out.stiffness_tension = defaults ? defaults->stiffness_tension : mass * 100.0f;
    if (out.stiffness_compression == 0.0f) {
        out.stiffness_compression = defaults ? defaults->stiffness_compression : mass * 120.0f;
    }
    if (out.damping_tension == 0.0f) out.damping_tension = defaults ? defaults->damping_tension : mass * 200.0f;
    if (out.damping_compression == 0.0f) {
        out.damping_compression = defaults ? defaults->damping_compression : mass * 250.0f;
    }
    auto breaking = values(text, "break", block);
    out.break_tension = static_cast<float>(breaking.atof());
    out.break_compression = static_cast<float>(breaking.atof());
    if (out.break_tension <= 0.0f) out.break_tension = mass * 100.0f;
    if (out.break_compression <= 0.0f) out.break_compression = mass * 200.0f;
    auto r0 = values(text, "r0", block);
    out.r0_tension = static_cast<float>(r0.quantity());
    out.r0_compression = static_cast<float>(r0.quantity());
    out.rigid = rts_find(text, "couplinghasrigidconnection", block).has_value();
    out.velocity = static_cast<float>(values(text, "velocity", block).atof());
}

void read_brakes(std::string_view text, BrakeEquipment& brakes, std::string& error) {
    if (const auto equipment = rts_find(text, "brakeequipmenttype")) {
        if (contains(text, equipment, "auxilary_reservoir") || contains(text, equipment, "auxiliary_reservoir")) {
            brakes.equipment |= 2;
        }
        if (contains(text, equipment, "triple_valve")) brakes.equipment |= 1;
        if (contains(text, equipment, "ep_brake") || contains(text, equipment, "electro-pneumatic")) brakes.equipment |= 4;
        if (contains(text, equipment, "handbrake")) brakes.equipment |= 8;
        if (contains(text, equipment, "vacuum_brake")) brakes.equipment |= 0x10;
    }
    if (const auto system = rts_find(text, "brakesystemtype")) {
        if (contains(text, system, "air_single_pipe")) brakes.system |= 1;
        if (contains(text, system, "electro-pneumatic") || contains(text, system, "ep")) brakes.system |= 2;
        if (contains(text, system, "air_twin_pipe") || contains(text, system, "air_charge_pipe")) brakes.system |= 4;
        if (contains(text, system, "vacuum_single_pipe")) brakes.system |= 8;
    }
    if (brakes.equipment & 8) brakes.max_handbrake_force = static_cast<float>(values(text, "maxhandbrakeforce").quantity());
    brakes.max_brake_force = static_cast<float>(values(text, "maxbrakeforce").quantity());
    brakes.triple_valve_ratio = static_cast<float>(values(text, "triplevalveratio").atof());
    if (brakes.triple_valve_ratio <= 0.0f) error = "Triple valve ratio non-positive"; // RTS refuses the wagon
    brakes.max_release_rate = static_cast<float>(values(text, "maxreleaserate").atof() * psi_to_atm);
    if (auto rts = values(text, "rtsmaxreleaserate"); rts.found()) {
        if (const auto value = rts.atof(); value > 0.0) brakes.max_release_rate = static_cast<float>(value * psi_to_atm);
    }
    brakes.max_application_rate = static_cast<float>(values(text, "maxapplicationrate").atof() * psi_to_atm);
    if (auto rts = values(text, "rtsmaxapplicationrate"); rts.found()) {
        brakes.max_application_rate = static_cast<float>(rts.atof() * psi_to_atm);
    }
    brakes.max_auxiliary_charging_rate = static_cast<float>(values(text, "maxauxilarychargingrate").atof() * psi_to_atm);
    brakes.cylinder_pressure_for_max_force =
        static_cast<float>(values(text, "brakecylinderpressureformaxbrakebrakeforce").atof() * psi_to_atm);
    if (auto leak = values(text, "trainpipeleakrate"); leak.found()) {
        brakes.train_pipe_leak_rate = static_cast<float>(leak.atof() * psi_to_atm);
    }
    brakes.distributor_script = values(text, "ortsbrakedistributor").token();
    if (const auto builtin = brake_distributor_from(brakes.distributor_script)) {
        brakes.distributor = *builtin;
        brakes.distributor_script.clear();
    }
    if (auto sensor = values(text, "ortsbrakesensor418"); sensor.found()) brakes.sensor418 = sensor.atof() != 0.0;
    brakes.shoe = brake_shoe_from(values(text, "ortsbrakeshoetype").token());
    if (auto force = values(text, "ortsbrakeforce"); force.found()) brakes.realistic_brake_force = (std::max)(static_cast<float>(force.quantity()), 0.0f);
    if (auto add = values(text, "addbrakeforce"); add.found()) brakes.max_brake_force += static_cast<float>(add.quantity());
    if (auto scale = values(text, "scalebrakeforce"); scale.found()) {
        brakes.max_brake_force *= static_cast<float>(scale.atof());
    }
}

std::uint32_t read_uint(Values v) {
    return static_cast<std::uint32_t>((std::max)(0, rts_atoi(v.here())));
}

// "count name ( value ) name ( "text" ) ..." (EngineParameters, AuxLibraryParameters).
std::vector<EngineParameter> read_parameters(std::string_view text, TextRange block) {
    std::vector<EngineParameter> out;
    std::size_t at = rts_next(text, block.begin, block.end); // past the count
    while (at < block.end) {
        const auto name = rts_token(text, at, block.end);
        if (name.empty()) break;
        auto open = rts_next(text, at, block.end);
        if (open >= block.end || text[open] != '(') break;
        ++open;
        std::size_t close = open;
        bool quoted = false;
        while (close < block.end && (quoted || text[close] != ')')) {
            if (text[close] == '"') quoted = !quoted;
            ++close;
        }
        EngineParameter p;
        p.name = name;
        std::size_t v = open;
        while (v < close && detail::rts_space(text[v])) ++v;
        if (v < close && text[v] == '"') {
            p.text = rts_token(text, v, close);
            p.value = static_cast<float>(rts_atof(p.text));
        } else {
            p.value = static_cast<float>(rts_atof(text.substr(v, close - v)));
        }
        out.push_back(std::move(p));
        at = close + 1;
        while (at < block.end && detail::rts_space(text[at])) ++at;
    }
    return out;
}

// Every block `keyword` inside `within`, in order.
std::vector<TextRange> blocks(std::string_view text, std::string_view keyword, TextRange within) {
    std::vector<TextRange> out;
    std::size_t from = within.begin;
    while (const auto b = rts_find(text, keyword, {from, within.end})) {
        out.push_back(*b);
        from = b->end + 1;
        if (from >= within.end) break;
    }
    return out;
}

// RTS 0x50036C: one Light block.
LightDefinition read_light(std::string_view text, TextRange light) {
    LightDefinition d;
    if (auto type = values(text, "type", light); type.found()) d.type = static_cast<std::uint8_t>(read_uint(type));
    if (const auto conditions = rts_find(text, "conditions", light)) {
        auto byte = [&](std::string_view key) {
            auto v = values(text, key, *conditions);
            return v.found() ? static_cast<std::uint8_t>(read_uint(v)) : std::uint8_t{0};
        };
        d.headlight = byte("headlight");
        d.control = byte("control");
        d.service = byte("service");
        d.unit = byte("unit");
        d.time_of_day = byte("timeofday");
        d.weather = byte("weather");
    }
    d.fade_in = static_cast<float>(values(text, "fadein", light).atof());
    d.fade_out = static_cast<float>(values(text, "fadeout", light).atof());
    d.cycle = static_cast<float>(values(text, "cycle", light).atof());
    d.texture = values(text, "lighttexture", light).token();
    if (const auto states = rts_find(text, "states", light)) {
        for (const auto state : blocks(text, "state", *states)) {
            LightState s;
            if (auto colour = values(text, "lightcolour", state); colour.found())
                s.colour = static_cast<std::uint32_t>(std::strtoul(colour.token().c_str(), nullptr, 16));
            s.duration = static_cast<float>(values(text, "duration", state).atof());
            if (auto p = values(text, "position", state); p.found())
                for (auto& v : s.position) v = static_cast<float>(p.atof());
            if (auto a = values(text, "azimuth", state); a.found())
                for (auto& v : s.azimuth) v = static_cast<float>(a.atof());
            if (auto e = values(text, "elevation", state); e.found())
                for (auto& v : s.elevation) v = static_cast<float>(e.atof());
            s.transition = static_cast<float>(values(text, "transition", state).atof());
            s.radius = static_cast<float>(values(text, "radius", state).atof());
            s.angle = static_cast<float>(values(text, "angle", state).atof());
            s.source_radius = static_cast<float>(values(text, "sourceradius", state).atof());
            d.states.push_back(s);
        }
    }
    if (auto a = values(text, "attenuation", light); a.found()) {
        const float start = static_cast<float>(a.atof());
        const float length = static_cast<float>(a.atof());
        const float kind = static_cast<float>(a.atof());
        if (start >= 0.0f && length > 0.0f) d.attenuation = {kind, start, length};
    }
    if (auto e = values(text, "spotexponent", light); e.found()) d.spot_exponent = static_cast<float>(e.atof()) * 128.0f;
    d.candela = static_cast<float>(values(text, "ortscandela", light).atof());
    d.lumens = static_cast<float>(values(text, "ortslumens", light).atof());
    d.kelvin = static_cast<float>(values(text, "ortskelvin", light).atof());
    if (auto n = values(text, "ortsname", light); n.found()) d.name = n.token();
    return d;
}

// Lights ( n ... ) then RTSLights ( m ... ): n + m entries, a LightID puts a
// block at its index (RTS 0x44247C).
void read_lights(std::string_view text, TextRange wagon, std::vector<LightDefinition>& lights) {
    auto add = [&](std::string_view list, std::string_view item) {
        const auto block = rts_find(text, list, wagon);
        if (!block) return;
        const auto declared = static_cast<std::size_t>((std::min)(read_uint(values(text, list, wagon)), 8192u));
        const std::size_t base = lights.size();
        lights.resize(base + declared);
        std::size_t next = base;
        for (const auto light : blocks(text, item, *block)) {
            std::size_t index = next;
            if (auto id = values(text, "lightid", light); id.found()) {
                const auto wanted = static_cast<std::size_t>(read_uint(id));
                if (wanted < lights.size()) index = wanted;
            }
            if (index >= lights.size()) break;
            lights[index] = read_light(text, light);
            next = index + 1;
        }
    };
    add("lights", "light");
    add("rtslights", "rtslight");
}

void read_extended(std::string_view text, TextRange extended, EngineDefinition& engine) {
    auto library = values(text, "librarymodule", extended);
    engine.library_module = library.token();
    engine.library_param = static_cast<std::uint32_t>((std::max)(0, rts_atoi(library.here())));
    auto cabin = values(text, "cabin", extended);
    engine.cabin = cabin.token();
    engine.cabin_param = static_cast<std::uint32_t>((std::max)(0, rts_atoi(cabin.here())));
    if (auto keep = values(text, "donothideextincabin", extended); keep.found())
        engine.keep_exterior_in_cab = static_cast<std::uint8_t>(read_uint(keep) & 3u);
    if (auto stack = values(text, "variablestack", extended); stack.found()) engine.variable_stack = read_uint(stack);
    engine.series = values(text, "locomotiveseries", extended).token();
    if (auto n = values(text, "numengines", extended); n.found()) engine.num_engines = read_uint(n);
    if (const auto motors = rts_find(text, "numengines", extended)) {
        engine.engine_axles.clear();
        for (const auto& block : blocks(text, "engineaxles", *motors)) {
            Values v(text, block);
            // EngineAxles ( count axle ... ). read_uint took a copy and read
            // the count each time: until 2026-11-18 every motor of the ЭД9М
            // ( 1 0 ) .. ( 1 3 ) turned axle 1, which slipped under the whole
            // car's force with the axles' adhesion (testers: "0 284* 0 0").
            const auto count = (std::min)(v.uint(), 32u);
            std::vector<std::uint8_t> axles;
            for (std::uint32_t k = 0; k < count; ++k) axles.push_back(static_cast<std::uint8_t>((std::min)(v.uint(), 31u)));
            engine.engine_axles.push_back(std::move(axles));
        }
    }
    if (auto n = values(text, "numaxles", extended); n.found()) engine.num_axles = read_uint(n);
    if (auto n = values(text, "maxnumslaveloco", extended); n.found()) engine.max_slaves = read_uint(n);
    if (auto c = values(text, "smecable", extended); c.found()) {
        engine.sme_cable[0] = static_cast<std::uint8_t>(c.atof() != 0.0);
        engine.sme_cable[1] = static_cast<std::uint8_t>(c.atof() != 0.0);
    }
    if (auto c = values(text, "hvbus", extended); c.found()) {
        engine.hv_bus[0] = static_cast<std::uint8_t>(c.atof() != 0.0);
        engine.hv_bus[1] = static_cast<std::uint8_t>(c.atof() != 0.0);
    }
    if (const auto params = rts_find(text, "engineparameters", extended)) engine.parameters = read_parameters(text, *params);
    if (const auto voltages = rts_find(text, "worksonlinevoltages", extended)) {
        for (const auto range : blocks(text, "linevoltage", *voltages)) {
            float v[4]{};
            auto at = range.begin;
            for (auto& value : v) {
                if (at >= range.end) break;
                value = static_cast<float>(rts_atof(rts_token(text, at, range.end)));
                at = rts_next(text, at, range.end);
            }
            engine.line_voltages.push_back({v[0], v[1], v[2], v[3]});
        }
    }
    if (auto current = values(text, "maxlinecurrent", extended); current.found()) {
        engine.max_line_current = (std::max)(static_cast<float>(current.atof()), 0.0f);
    }
    if (auto cooling = values(text, "maxlinecurrentvelocityincrease", extended); cooling.found()) {
        engine.line_current_cooling[0] = static_cast<float>(cooling.atof());
        engine.line_current_cooling[1] = static_cast<float>(cooling.atof());
        // RTS 0x44247C: a negative value drops both.
        if (engine.line_current_cooling[0] < 0.0f || engine.line_current_cooling[1] < 0.0f) engine.line_current_cooling = {};
    }
    if (auto burn = values(text, "maxlinecurrentcatenaryburntime", extended); burn.found()) {
        engine.max_line_current_burn_time = (std::max)(static_cast<float>(burn.atof()), 0.0f);
    }
    // Pantographs (0x44247C): ExcludePantograph may repeat, 1..8.
    for (const auto excluded : blocks(text, "excludepantograph", extended)) {
        const int number = rts_atoi(text.substr(excluded.begin, excluded.end - excluded.begin));
        if (number >= 1 && number <= 8) engine.exclude_pantographs |= static_cast<std::uint8_t>(1u << (number - 1));
    }
    if (auto speed = values(text, "raisesecondpantowhenspeedbelow", extended); speed.found()) {
        engine.raise_second_panto_below = static_cast<float>(speed.quantity());
    }
    if (auto mask = values(text, "alwaysusepanto", extended); mask.found()) {
        engine.always_use_panto = static_cast<std::uint8_t>(read_uint(mask));
    }
    if (auto mask = values(text, "lowerpantoonstop", extended); mask.found()) {
        engine.lower_panto_on_stop = static_cast<std::uint8_t>(read_uint(mask));
    }
    engine.train_plugin = values(text, "ortstrainplugin", extended).token();
    if (const auto aux = rts_find(text, "auxlibraries", extended)) {
        for (const auto lib : blocks(text, "auxlibrary", *aux)) {
            AuxLibraryDefinition a;
            a.name = values(text, "auxlibraryname", lib).token();
            a.module = values(text, "auxlibrarymodule", lib).token();
            if (auto stack = values(text, "auxvariablestack", lib); stack.found()) a.variable_stack = read_uint(stack);
            if (const auto params = rts_find(text, "auxlibraryparameters", lib)) a.parameters = read_parameters(text, *params);
            for (const auto call : blocks(text, "auxcall", lib)) {
                const auto fn = rts_token(text, call.begin, call.end);
                const auto next = rts_next(text, call.begin, call.end);
                const auto order = rts_atoi(text.substr(next, call.end - next));
                a.calls.emplace_back(fn, order);
            }
            a.sound = values(text, "auxlibrarysound", lib).token();
            const auto sound_type = values(text, "auxlibrarysoundtype", lib).token();
            a.sound_type = sound_type == "ambient"         ? AuxSoundType::ambient
                : sound_type == "routeforwagon"            ? AuxSoundType::route_for_wagon
                                                           : AuxSoundType::wagon;
            if (auto position = values(text, "auxlibrarysoundposition", lib); position.found()) {
                for (auto& v : a.sound_position) v = static_cast<float>(position.atof());
            }
            const auto flag = [&](std::string_view key) {
                auto v = values(text, key, lib);
                return v.found() && read_uint(v) != 0;
            };
            a.sound_external = flag("auxlibrarysoundexternal");
            a.sound_own_stack = flag("auxlibrarysoundownstack");
            a.sound_single_wagon = flag("auxlibrarysoundsinglewagon");
            // MaDaDev's Brakes.dll: where its valve and gauge are.
            const auto module = core::fold_case(a.module);
            if (module == "brakes.dll" || (module.size() > 11 && (module.ends_with("/brakes.dll") || module.ends_with("\\brakes.dll")))) {
                auto& brakes_dll = engine.brakes_library;
                brakes_dll.present = true;
                std::int32_t separating_id = -1;
                bool separating_fitted = false;
                for (const auto& p : a.parameters) {
                    const auto name = core::fold_case(p.name);
                    // Every cab's switches: BR_395ID<k> and the like, k from 1.
                    for (const auto& [prefix, field] : std::initializer_list<std::pair<std::string_view, std::int32_t EngineDefinition::BrakesLibrary::CabIds::*>>{
                             {"br_395id", &EngineDefinition::BrakesLibrary::CabIds::valve},
                             {"br_kptid", &EngineDefinition::BrakesLibrary::CabIds::controller},
                             {"br_manu", &EngineDefinition::BrakesLibrary::CabIds::equalising_gauge},
                             {"br_redid", &EngineDefinition::BrakesLibrary::CabIds::reducer},
                             {"br_kkid", &EngineDefinition::BrakesLibrary::CabIds::combined},
                             {"br_367id", &EngineDefinition::BrakesLibrary::CabIds::block},
                             {"br_razid", &EngineDefinition::BrakesLibrary::CabIds::separating}}) {
                        if (!name.starts_with(prefix) || name.size() != prefix.size() + 1) continue;
                        const char digit = name.back();
                        if (digit < '1' || digit > '8') continue;
                        const auto k = static_cast<std::size_t>(digit - '1');
                        if (brakes_dll.cabs.size() <= k) brakes_dll.cabs.resize(k + 1);
                        brakes_dll.cabs[k].*field = static_cast<std::int32_t>(p.value);
                    }
                    if (name == "br_395id1") brakes_dll.valve = static_cast<std::int32_t>(p.value);
                    else if (name == "br_kptid1") brakes_dll.controller = static_cast<std::int32_t>(p.value);
                    else if (name == "br_manu1") brakes_dll.equalising_gauge = static_cast<std::int32_t>(p.value);
                    else if (name == "br_zarpress") brakes_dll.charging = p.value;
                    else if (name == "br_redid1") brakes_dll.reducer = static_cast<std::int32_t>(p.value);
                    else if (name == "br_kkid1") brakes_dll.combined = static_cast<std::int32_t>(p.value);
                    else if (name == "br_367id1") brakes_dll.block = static_cast<std::int32_t>(p.value);
                    else if (name == "br_367oppol") brakes_dll.block_operating = static_cast<std::int32_t>(p.value);
                    else if (name == "br_razid1") separating_id = static_cast<std::int32_t>(p.value);
                    else if (name == "br_razobshk") separating_fitted = p.value != 0.0f;
                }
                if (separating_fitted) brakes_dll.separating = separating_id;
                if (!separating_fitted)
                    for (auto& c : brakes_dll.cabs) c.separating = -1;
            }
            engine.aux_libraries.push_back(std::move(a));
        }
    }
}

// rts1.exe 0x44247C: the smoke defaults of the engine, then each exhaust of
// DieselSpecialEffects with RTS's own keywords and limits.
void read_exhausts(std::string_view text, TextRange block, EngineDefinition& engine) {
    engine.smoke_initial_magnitude = static_cast<float>(values(text, "dieselsmokeeffectinitialmagnitude", block).atof());
    engine.smoke_max_magnitude = static_cast<float>(values(text, "dieselsmokeeffectmaxmagnitude", block).atof());
    if (auto rate = values(text, "dieselsmokeeffectinitialsmokerate", block); rate.found()) {
        engine.smoke_initial_rate = (std::min)(static_cast<float>(rate.atof()), 1.0f);
    }
    if (auto rate = values(text, "dieselsmokeeffectmaxsmokerate", block); rate.found()) {
        engine.smoke_max_rate = static_cast<float>(rate.atof()) * 0.01f;
    }
    engine.smoke_initial_rate = (std::min)(engine.smoke_initial_rate, 3.0f);
    engine.smoke_max_rate = (std::min)(engine.smoke_max_rate, 3.0f);
    const auto effects = rts_find(text, "dieselspecialeffects", block);
    if (!effects) return;
    for (int k = 0; k <= 16; ++k) {
        const std::string name = k == 0 ? std::string("exhaust") : "exhaust" + std::to_string(k);
        const auto exhaust = rts_find(text, name, *effects);
        if (!exhaust) continue;
        DieselExhaust e;
        Values v(text, exhaust);
        for (auto& p : e.position) p = static_cast<float>(v.atof());
        for (auto& d : e.direction) d = static_cast<float>(v.atof());
        e.size = static_cast<float>(v.atof());
        if (auto colour = values(text, "colour", *exhaust); colour.found()) {
            e.colour = static_cast<std::uint32_t>(std::strtoul(colour.token().c_str(), nullptr, 16));
        }
        if (auto density = values(text, "density", *exhaust); density.found()) e.density = static_cast<float>(density.atof());
        if (auto mass = values(text, "mass", *exhaust); mass.found()) e.mass = static_cast<float>(mass.atof());
        if (auto magnitude = values(text, "magnitude", *exhaust); magnitude.found()) {
            e.magnitude = {static_cast<float>(magnitude.atof()), static_cast<float>(magnitude.atof())};
        } else {
            e.magnitude = {engine.smoke_initial_magnitude * 0.65f, engine.smoke_max_magnitude};
        }
        if (auto rate = values(text, "rate", *exhaust); rate.found()) {
            e.rate = {static_cast<float>(rate.atof()), static_cast<float>(rate.atof())};
        } else {
            e.rate = {engine.smoke_initial_rate, engine.smoke_max_rate};
        }
        e.dispersion = 0.5f;
        if (auto dispersion = values(text, "dispersion", *exhaust); dispersion.found()) e.dispersion = static_cast<float>(dispersion.atof());
        if (auto texture = values(text, "texture", *exhaust); texture.found()) e.texture = texture.token();
        // The limits RTS puts on them.
        e.density = (std::max)(e.density, 0.0f);
        e.dispersion = (std::max)(e.dispersion, 1.0f);
        if (e.magnitude[0] <= 0.5f) e.magnitude[0] = 0.5f;
        e.magnitude[1] = (std::max)(e.magnitude[1], 1.0f);
        e.mass = (std::max)(e.mass, 0.5f);
        e.rate[0] = (std::max)(e.rate[0], 0.0f);
        if (e.rate[1] < 0.0f) e.rate[1] = 1.0f;
        engine.exhausts.push_back(std::move(e));
    }
}

[[nodiscard]] EngineDefinition read_engine(std::string_view text, TextRange block) {
    EngineDefinition engine;
    const auto type = values(text, "type", block).token();
    engine.kind = type == "electric" ? EngineKind::electric
        : type == "diesel"           ? EngineKind::diesel
        : type == "steam"            ? EngineKind::steam
                                     : EngineKind::unknown;
    engine.cab_view = values(text, "cabview").found();
    engine.max_power = static_cast<float>(values(text, "maxpower").quantity());
    engine.max_force = static_cast<float>(values(text, "maxforce").quantity());
    engine.max_continuous_force = static_cast<float>(values(text, "maxcontinuousforce").quantity());
    engine.max_velocity = static_cast<float>(values(text, "maxvelocity").quantity());
    engine.max_current = static_cast<float>(values(text, "maxcurrent").atof());
    engine.sanding_speed = static_cast<float>(values(text, "sanding").quantity());
    if (auto sand = values(text, "maxsandingtime"); sand.found()) engine.max_sanding_time = static_cast<float>(sand.atof());
    if (auto fuel = values(text, "maxdiesellevel"); fuel.found()) engine.max_diesel_level = static_cast<float>(fuel.atof());
    engine.main_reservoir_volume = static_cast<float>(values(text, "airbrakesmainresvolume").atof());
    engine.main_reservoir_max = pressure_atm(values(text, "airbrakesmainmaxairpressure").token());
    engine.compressor_restart = pressure_atm(values(text, "airbrakescompressorrestartpressure").token());
    engine.compressor_capacity = (std::max)(static_cast<float>(values(text, "ortscompressorcapacity").atof()), 0.0f);
    engine.dynamic_brake_max_force = static_cast<float>(values(text, "dynamicbrakesmaximumforce").quantity());
    if (const auto controllers = rts_find(text, "enginecontrollers")) {
        if (const auto throttle = rts_find(text, "throttle", *controllers)) {
            engine.throttle_notches = static_cast<std::uint32_t>((std::max)(0, rts_atoi(values(text, "numnotches", *throttle).here())));
        }
    }
    if (const auto extended = rts_find(text, "engextended")) read_extended(text, *extended, engine);
    read_exhausts(text, block, engine);
    return engine;
}

[[nodiscard]] std::string rts_companion_name(std::string_view path) {
    // name.eng -> name.rtseng (RTS inserts "rts" before the extension).
    const auto dot = path.find_last_of('.');
    const auto slash = path.find_last_of('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return std::string(path.substr(0, dot + 1)) + "rts" + std::string(path.substr(dot + 1));
}

} // namespace

double rts_quantity_value(std::string_view token) {
    return rts_quantity(token);
}

double rts_product_value(std::string_view token) {
    return rts_product(token);
}

WagonDefinition parse_wagon(const std::string_view bytes) {
    WagonDefinition wagon;
    std::string error;
    auto text = detail::rts_text(bytes, error);
    if (!text) return {.error = error};
    detail::rts_blank_comments(*text);
    const std::string_view t = *text;

    const auto block = rts_find(t, "wagon");
    if (!block) return {.error = "no Wagon block"};
    wagon.name = rts_token(t, block->begin, block->end);
    const auto type = values(t, "type", *block).token();
    wagon.kind = type == "freight" ? WagonKind::freight
        : type == "carriage"       ? WagonKind::passenger
        : type == "engine"         ? WagonKind::engine
        : type == "tender"         ? WagonKind::tender
                                   : WagonKind::unknown;
    wagon.shape = values(t, "rtswagonshape").token();
    if (wagon.shape.empty()) wagon.shape = values(t, "wagonshape").token();
    wagon.freight_shape = values(t, "freightanim").token();

    auto size = values(t, "size");
    wagon.width = static_cast<float>(size.atof());
    wagon.height = static_cast<float>(size.atof());
    wagon.length = static_cast<float>(size.atof());
    if (wagon.width <= 0.0f || wagon.height <= 0.0f || wagon.length <= 0.0f) wagon.error = "bad Size";

    // Mass is read as tonnes whatever the unit.
    wagon.mass = static_cast<float>(rts_atof(values(t, "mass").token()) * 1000.0);
    wagon.wheel_radius = static_cast<float>(values(t, "wheelradius").atof());
    wagon.wheels = static_cast<std::uint8_t>(rts_atoi(values(t, "numwheels").here()));
    if (auto raise = values(t, "pantographraisetime", *block); raise.found()) {
        wagon.pantograph_raise_time = static_cast<float>(raise.atof());
        if (wagon.pantograph_raise_time <= 0.0f) wagon.pantograph_raise_time = 3.0f;
    }
    wagon.passenger_capacity = wagon.kind == WagonKind::passenger ? 200.0f : 0.0f;
    if (auto capacity = values(t, "passengercapacity"); capacity.found())
        wagon.passenger_capacity = (std::max)(static_cast<float>(capacity.atof()), 0.0f);

    if (const auto front = rts_find(t, "coupling")) {
        read_coupling(t, *front, wagon.mass, nullptr, wagon.couplings[0]);
        if (const auto rear = rts_find(t, "coupling", {front->end, t.size()})) {
            read_coupling(t, *rear, wagon.mass, &wagon.couplings[0], wagon.couplings[1]);
        } else {
            wagon.couplings[1] = wagon.couplings[0];
        }
    } else {
        // No Coupling block: RTS reads the values from the start of the file
        // (zeros), so the mass-based defaults apply.
        read_coupling(t, {0, 0}, wagon.mass, nullptr, wagon.couplings[0]);
        wagon.couplings[1] = wagon.couplings[0];
    }

    auto friction = values(t, "friction");
    wagon.friction.c1 = static_cast<float>(friction.quantity());
    wagon.friction.e1 = static_cast<float>(friction.quantity());
    wagon.friction.transition = static_cast<float>(friction.atof()); // RTS: no unit conversion here
    wagon.friction.c2 = static_cast<float>(friction.quantity());
    wagon.friction.e2 = static_cast<float>(friction.quantity());
    if (wagon.friction.e1 == 0.0f && wagon.friction.transition == 0.0f) wagon.friction.e1 = 1.0f;

    auto adhesion = values(t, "adheasion");
    for (auto& value : wagon.adhesion) value = static_cast<float>(adhesion.atof());
    wagon.derail_rail_height = static_cast<float>(values(t, "derailrailheight").quantity());
    wagon.derail_rail_force = static_cast<float>(values(t, "derailrailforce").product());
    if (wagon.derail_rail_force <= 0.0f) wagon.derail_rail_force = 240000.0f;
    wagon.derail_buffer_force = static_cast<float>(values(t, "derailbufferforce").quantity());
    {
        auto stiffness = values(t, "framespringstiffness");
        const auto read = stiffness.atof();
        wagon.frame_spring_stiffness = rts_find(t, "framespringstiffness") ? read : wagon.mass * 30.0;
        auto damping = values(t, "framespringdamping");
        const auto damped = damping.atof();
        wagon.frame_spring_damping = rts_find(t, "framespringdamping") ? damped : wagon.mass * 1.25;
        if (rts_find(t, "frameosccorrection")) {
            auto correction = values(t, "frameosccorrection");
            for (auto& value : wagon.frame_osc_correction) value = (std::max)(static_cast<float>(correction.atof()), 0.0f);
        }
    }

    std::string brake_error;
    read_brakes(t, wagon.brakes, brake_error);
    if (wagon.error.empty() && !brake_error.empty()) wagon.error = brake_error;

    // RTS 0x44247C: in the Wagon block RTSExteriorSound, else Sound; in the
    // Engine block RTSCabinSound, else Sound (MSTS kept the cab script there:
    // the MaDaDev ED9M names mdd_ed4m-cabHead.sms so).
    read_lights(t, *block, wagon.lights);
    wagon.sound = values(t, "rtsexteriorsound", *block).token();
    if (wagon.sound.empty()) wagon.sound = values(t, "sound", *block).token();
    if (const auto interior = rts_find(t, "interior", *block)) {
        if (auto file = values(t, "interiorfile", *interior); file.found()) {
            wagon.interior = file.token();
            wagon.interior_flipped = static_cast<std::uint8_t>(rts_atoi(file.here()) & 3);
            wagon.interior_sound = values(t, "soundint", *interior).token();
            if (auto keep = values(t, "donothideextinsalon", *interior); keep.found())
                wagon.keep_exterior_in_salon = static_cast<std::uint8_t>(read_uint(keep) & 3u);
        }
    }

    if (const auto engine = rts_find(t, "engine")) {
        wagon.engine = read_engine(t, *engine);
        wagon.cab_sound = values(t, "rtscabinsound", *engine).token();
        if (wagon.cab_sound.empty()) wagon.cab_sound = values(t, "sound", *engine).token();
    }
    return wagon;
}

core::VirtualPath wagon_path(const std::string_view folder, const std::string_view name, const bool engine) {
    const auto text = "TRAINS/TRAINSET/" + std::string(folder) + "/" + std::string(name) + (engine ? ".eng" : ".wag");
    return core::VirtualPath::parse(text).value_or(core::VirtualPath{});
}

namespace {

// Name ( "..." ) of the Engine block, else the Wagon block, in the file's
// own case: found in the lower-cased text (the same length, byte for byte)
// and read from the decoded one.
[[nodiscard]] std::string written_name(const std::string_view bytes) {
    std::string error;
    const auto original = detail::decode_text(bytes, error);
    if (!original) return {};
    std::string lower = *original;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    detail::rts_blank_comments(lower);
    for (const std::string_view block_name : {"engine", "wagon"}) {
        const auto block = rts_find(lower, block_name);
        if (!block) continue;
        if (const auto name = rts_find(lower, "name", *block)) {
            auto value = rts_token(*original, name->begin, name->end);
            if (!value.empty()) return value;
        }
    }
    return {};
}

} // namespace

WagonDefinition load_wagon(const core::Vfs& vfs, const std::string_view folder, const std::string_view name,
                           const bool engine, const WagonRules& rules) {
    const auto path = wagon_path(folder, name, engine);
    std::string error;
    const auto bytes = vfs.read(path, &error);
    if (!bytes) return {.name = std::string(name), .error = error};
    auto main = detail::rts_text(*bytes, error);
    if (!main) return {.name = std::string(name), .error = error};
    detail::rts_blank_comments(*main);

    if (const auto companion = core::VirtualPath::parse(rts_companion_name(path.str())); companion && vfs.exists(*companion)) {
        if (const auto companion_bytes = vfs.read(*companion, &error)) {
            if (auto companion_text = detail::rts_text(*companion_bytes, error)) {
                detail::rts_blank_comments(*companion_text);
                detail::rts_merge(*main, *companion_text, {"wagon", "engine", "engextended"});
            }
        }
    }
    // The distributor the file itself names (before openRTS's companion).
    const auto file_distributor = brake_distributor_from(values(*main, "ortsbrakedistributor").token());
    // openRTS's own companion (a mod's), over RTS's: its values come first.
    bool own_distributor = false, own_compressor = false, own_capacity = false;
    if (const auto own = wagon_override_path(folder, name, engine); vfs.exists(own)) {
        if (const auto own_bytes = vfs.read(own, &error)) {
            if (auto own_text = detail::rts_text(*own_bytes, error)) {
                detail::rts_blank_comments(*own_text);
                own_distributor = own_text->find("ortsbrakedistributor") != std::string::npos;
                own_compressor = own_text->find("airbrakesmainmaxairpressure") != std::string::npos ||
                                 own_text->find("airbrakescompressorrestartpressure") != std::string::npos;
                own_capacity = own_text->find("ortscompressorcapacity") != std::string::npos;
                detail::rts_merge(*main, *own_text, {"wagon", "engine", "engextended"});
            }
        }
    }
    // parse_wagon expects file bytes; the merged text is already decoded.
    auto wagon = parse_wagon(*main);
    // The merged companion text comes first in the Wagon block, so its
    // first token is no longer the name; the file name is.
    wagon.name = std::string(name);
    wagon.file = path.str();
    wagon.display_name = written_name(*bytes);
    // openRTS's stock rules (Luau): the file < the rules < the companion.
    auto& brakes_now = wagon.brakes;
    brakes_now.distributor_in_companion = own_distributor;
    brakes_now.default_distributor = file_distributor.value_or(BrakeDistributor::inferred);
    if (rules) {
        const auto file = path.filename();
        const auto given = rules(wagon, folder, file, engine);
        if (given.distributor && !file_distributor && brakes_now.distributor_script.empty() && !own_distributor) {
            brakes_now.distributor = *given.distributor;
        }
        if (given.distributor && !file_distributor) brakes_now.default_distributor = *given.distributor;
        if (wagon.engine && !own_compressor) {
            if (given.compressor_off) wagon.engine->main_reservoir_max = *given.compressor_off;
            if (given.compressor_on) wagon.engine->compressor_restart = *given.compressor_on;
        }
        if (wagon.engine && !own_capacity && given.compressor_capacity) wagon.engine->compressor_capacity = *given.compressor_capacity;
    }
    // openRTS: a distributor script, next to the file.
    if (auto& brakes = wagon.brakes; !brakes.distributor_script.empty()) {
        const auto script = core::VirtualPath::parse(path.parent().str() + "/" + brakes.distributor_script);
        std::string script_error;
        const auto source = script ? vfs.read(*script, &script_error) : std::nullopt;
        if (source) {
            brakes.distributor_source = *source;
        } else {
            brakes.distributor_error = brakes.distributor_script + ": " + (script_error.empty() ? "not found" : script_error);
        }
    }
    return wagon;
}

BrakeShoe brake_shoe_from(const std::string_view name) {
    const auto s = core::fold_case(std::string(name));
    if (s.find("cast") != std::string::npos) return BrakeShoe::cast_iron;
    if (s.find("composite") != std::string::npos) return BrakeShoe::composite;
    if (s.find("disc") != std::string::npos || s.find("disk") != std::string::npos) return BrakeShoe::disc;
    if (s.find("wood") != std::string::npos) return BrakeShoe::wood;
    return BrakeShoe::unknown;
}

const char* brake_shoe_name(const BrakeShoe shoe) noexcept {
    switch (shoe) {
    case BrakeShoe::cast_iron: return "cast_iron";
    case BrakeShoe::composite: return "composite";
    case BrakeShoe::disc: return "disc";
    case BrakeShoe::wood: return "wood";
    default: return "";
    }
}

std::optional<BrakeDistributor> brake_distributor_from(const std::string_view name) {
    const auto s = core::fold_case(std::string(name));
    if (s == "483" || s == "vr483") return BrakeDistributor::vr483;
    if (s == "292" || s == "vr292") return BrakeDistributor::vr292;
    if (s == "305" || s == "305-000" || s == "evr305" || s == "evr305-000") return BrakeDistributor::evr305;
    if (s == "305-001" || s == "evr305-001") return BrakeDistributor::evr305_001;
    return std::nullopt;
}

const char* brake_distributor_name(const BrakeDistributor distributor) noexcept {
    switch (distributor) {
    case BrakeDistributor::vr483: return "483";
    case BrakeDistributor::vr292: return "292";
    case BrakeDistributor::evr305: return "305-000";
    case BrakeDistributor::evr305_001: return "305-001";
    default: return "";
    }
}

core::VirtualPath wagon_override_path(const std::string_view folder, const std::string_view name, const bool engine) {
    return core::VirtualPath::parse(wagon_path(folder, name, engine).str() + ".openrts").value_or(core::VirtualPath{});
}

namespace {

// `text` (a companion's) with its Wagon block's `key` line (folded, no
// blanks) replaced by `entry`, or taken out for an empty one; the rest kept.
// Empty when nothing is left in it.
std::string set_override_line(const std::string_view text, const std::string_view key, const std::string& entry) {
    // Line by line: the companion is openRTS's own small file, a block a line.
    std::vector<std::string> lines;
    {
        std::size_t from = 0;
        while (from <= text.size()) {
            auto to = text.find('\n', from);
            if (to == std::string_view::npos) to = text.size();
            std::string line(text.substr(from, to - from));
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            from = to + 1;
        }
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
    }
    // The line folded and without blanks, to compare.
    const auto bare = [](const std::string& line) {
        std::string out;
        for (const char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };
    std::erase_if(lines, [&](const std::string& line) { return bare(line).find(key) != std::string::npos; });
    if (!entry.empty()) {
        const auto opener = std::find_if(lines.begin(), lines.end(), [&](const std::string& line) { return bare(line) == "wagon("; });
        if (opener != lines.end()) {
            lines.insert(opener + 1, entry);
        } else {
            lines.emplace_back("Wagon (");
            lines.push_back(entry);
            lines.emplace_back(")");
        }
    } else {
        // An emptied Wagon block goes too.
        for (std::size_t i = 0; i + 1 < lines.size();) {
            std::size_t next = i + 1;
            while (next < lines.size() && bare(lines[next]).empty()) ++next;
            if (bare(lines[i]) == "wagon(" && next < lines.size() && bare(lines[next]) == ")") {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i), lines.begin() + static_cast<std::ptrdiff_t>(next) + 1);
            } else {
                ++i;
            }
        }
    }
    // Nothing but blanks and comments left: no file.
    const bool empty = std::all_of(lines.begin(), lines.end(), [&](const std::string& line) {
        const auto b = bare(line);
        return b.empty() || b.starts_with("comment(");
    });
    if (empty) return {};
    std::string out;
    for (const auto& line : lines) out += line + "\r\n";
    return out;
}

// `text` without its Lights ( ... ) and RTSLights ( ... ) blocks, however
// many lines each takes.
std::string without_light_blocks(const std::string_view text) {
    std::string out;
    int depth = 0;
    bool inside = false;
    std::size_t from = 0;
    while (from < text.size()) {
        auto to = text.find('\n', from);
        if (to == std::string_view::npos) to = text.size();
        const auto line = text.substr(from, to - from + (to < text.size() ? 1 : 0));
        from = to + 1;
        std::string bare;
        for (const char c : line)
            if (!std::isspace(static_cast<unsigned char>(c))) bare.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (!inside && (bare.starts_with("lights(") || bare.starts_with("rtslights("))) {
            inside = true;
            depth = 0;
        }
        if (!inside) {
            out += line;
            continue;
        }
        bool quoted = false;
        for (const char c : line) {
            if (c == '"') quoted = !quoted;
            if (quoted) continue;
            if (c == '(') ++depth;
            if (c == ')') --depth;
        }
        if (depth <= 0) inside = false;
    }
    return out;
}

} // namespace

std::string write_lights(const std::vector<LightDefinition>& lights) {
    char buffer[160];
    const auto numbers = [&](const char* name, std::initializer_list<float> values) {
        std::string out = std::string(" ") + name + " (";
        for (const float v : values) {
            std::snprintf(buffer, sizeof buffer, " %.6g", static_cast<double>(v));
            out += buffer;
        }
        return out + " )";
    };
    std::string out = "\tLights ( " + std::to_string(lights.size()) + "\r\n";
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const auto& l = lights[i];
        if (l.states.empty()) continue;
        std::snprintf(buffer, sizeof buffer, "\t\tLight ( LightID ( %zu ) Type ( %u )", i, static_cast<unsigned>(l.type));
        out += buffer;
        std::snprintf(buffer, sizeof buffer,
                      " Conditions ( Headlight ( %u ) Control ( %u ) Service ( %u ) Unit ( %u ) TimeOfDay ( %u ) Weather ( %u ) )",
                      static_cast<unsigned>(l.headlight), static_cast<unsigned>(l.control), static_cast<unsigned>(l.service),
                      static_cast<unsigned>(l.unit), static_cast<unsigned>(l.time_of_day), static_cast<unsigned>(l.weather));
        out += buffer;
        out += numbers("FadeIn", {l.fade_in}) + numbers("FadeOut", {l.fade_out}) + numbers("Cycle", {l.cycle});
        if (!l.texture.empty()) out += " LightTexture ( \"" + l.texture + "\" )";
        out += " States ( " + std::to_string(l.states.size());
        for (const auto& s : l.states) {
            std::snprintf(buffer, sizeof buffer, " State ( LightColour ( %08x )", s.colour);
            out += buffer;
            out += numbers("Duration", {s.duration}) + numbers("Position", {s.position[0], s.position[1], s.position[2]})
                 + numbers("Azimuth", {s.azimuth[0], s.azimuth[1], s.azimuth[2]})
                 + numbers("Elevation", {s.elevation[0], s.elevation[1], s.elevation[2]}) + numbers("Transition", {s.transition})
                 + numbers("Radius", {s.radius}) + numbers("Angle", {s.angle}) + numbers("SourceRadius", {s.source_radius}) + " )";
        }
        out += " )";
        out += numbers("Attenuation", {l.attenuation[1], l.attenuation[2], l.attenuation[0]});
        out += numbers("SpotExponent", {l.spot_exponent / 128.0f});
        if (l.candela > 0.0f) out += numbers("ORTSCandela", {l.candela});
        if (l.lumens > 0.0f) out += numbers("ORTSLumens", {l.lumens});
        if (l.kelvin > 0.0f) out += numbers("ORTSKelvin", {l.kelvin});
        if (!l.name.empty()) out += " ORTSName ( \"" + l.name + "\" )";
        out += " )\r\n";
    }
    // Every light is in Lights: the file's own RTSLights would add theirs again.
    return out + "\t)\r\n\tRTSLights ( 0 )";
}

std::string set_override_lights(const std::string_view text, const std::optional<std::vector<LightDefinition>>& lights) {
    const auto rest = without_light_blocks(text);
    return set_override_line(rest, "\x01", lights ? write_lights(*lights) : std::string{});
}

std::string set_override_distributor(const std::string_view text, const BrakeDistributor distributor) {
    if (distributor == BrakeDistributor::inferred) return set_override_line(text, "ortsbrakedistributor", {});
    return set_override_line(text, "ortsbrakedistributor",
                             std::string("\tORTSBrakeDistributor ( \"") + brake_distributor_name(distributor) + "\" )");
}

std::string set_override_sensor418(const std::string_view text, const std::optional<bool> fitted) {
    if (!fitted) return set_override_line(text, "ortsbrakesensor418", {});
    return set_override_line(text, "ortsbrakesensor418", *fitted ? "\tORTSBrakeSensor418 ( 1 )" : "\tORTSBrakeSensor418 ( 0 )");
}

ConsistDefinition parse_consist(const std::string_view bytes) {
    ConsistDefinition consist;
    std::string error;
    auto text = detail::rts_text(bytes, error);
    if (!text) return {.error = error};
    detail::rts_blank_comments(*text);
    const std::string_view t = *text;

    const auto config = rts_find(t, "traincfg");
    if (!config) return {.error = "no TrainCfg block"};
    consist.name = rts_token(t, config->begin, config->end);
    consist.max_velocity = static_cast<float>(values(t, "maxvelocity").atof());
    if (auto durability = values(t, "durability"); durability.found()) {
        consist.durability = static_cast<float>(durability.atof());
    }

    // Engine / Wagon blocks in order inside TrainCfg.
    std::size_t at = config->begin;
    while (at < config->end) {
        const auto engine = rts_find(t, "engine", {at, config->end});
        const auto wagon = rts_find(t, "wagon", {at, config->end});
        if (!engine && !wagon) break;
        const bool is_engine = engine && (!wagon || engine->begin < wagon->begin);
        const auto block = is_engine ? *engine : *wagon;
        ConsistEntry entry{.engine = is_engine};
        if (auto uid = values(t, "uid", block); uid.found()) entry.uid = static_cast<std::uint32_t>(rts_atoi(uid.here()));
        auto data = values(t, is_engine ? "enginedata" : "wagondata", block);
        entry.name = data.token();
        entry.folder = data.token();
        entry.flipped = rts_find(t, "flip", block).has_value();
        entry.cold = is_engine && rts_find(t, "cold", block).has_value();
        if (auto handbrake = values(t, "handbrakepercent", block); handbrake.found()) {
            entry.handbrake = static_cast<float>(handbrake.atof());
        }
        if (auto drag = values(t, "placedrag", block); drag.found()) entry.place_drag = static_cast<std::uint8_t>(rts_atoi(drag.here()) & 3);
        if (auto shoes = values(t, "ortsbrakeshoes", block); shoes.found()) {
            entry.shoes[0] = brake_shoe_from(shoes.token());
            const auto rear = brake_shoe_from(shoes.token());
            entry.shoes[1] = rear != BrakeShoe::unknown ? rear : entry.shoes[0];
        }
        for (const auto anim : blocks(t, "freightanim", block)) {
            ConsistFreight f;
            auto shape = values(t, "freightanimshape", anim);
            f.shape = shape.token();
            f.shape_location = static_cast<std::uint32_t>((std::max)(0, rts_atoi(shape.here())));
            if (auto position = values(t, "freightanimposition", anim); position.found()) {
                for (auto& v : f.position) v = static_cast<float>(position.atof());
            }
            if (auto mass = values(t, "freightanimmass", anim); mass.found()) f.mass = static_cast<float>(mass.quantity());
            if (auto volume = values(t, "freightanimvolume", anim); volume.found()) f.volume = static_cast<float>(volume.atof());
            f.cargo_type = values(t, "freightanimcargotype", anim).token();
            if (auto id = values(t, "freightanimcargoid", anim); id.found()) f.cargo_id = rts_atoi(id.here());
            entry.freight.push_back(std::move(f));
        }
        if (!entry.name.empty()) consist.entries.push_back(std::move(entry));
        at = block.end + 1;
    }
    if (consist.entries.empty()) consist.error = "consist has no engines or wagons";
    return consist;
}

std::optional<core::VirtualPath> find_consist(const core::Vfs& vfs, const std::string_view name) {
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) {
        std::string path(name);
        std::replace(path.begin(), path.end(), '\\', '/');
        if (auto parsed = core::VirtualPath::parse(path); parsed && vfs.exists(*parsed)) return parsed;
        return std::nullopt;
    }
    auto stem = std::string(name);
    if (stem.size() > 4 && core::fold_case(stem.substr(stem.size() - 4)) == ".con") stem.resize(stem.size() - 4);
    for (const char* folder : {"TRAINS/CONSISTS_RTS/", "TRAINS/CONSISTS/"}) {
        if (auto path = core::VirtualPath::parse(std::string(folder) + stem + ".con"); path && vfs.exists(*path)) return path;
    }
    // In a folder of its own under them (the MDD build's "Составы РЖД/
    // Сплотки/..."), as the start menu lists them by the name alone.
    const auto wanted = core::fold_case(stem + ".con");
    for (const char* folder : {"TRAINS/CONSISTS_RTS", "TRAINS/CONSISTS"}) {
        const auto root = core::VirtualPath::parse(folder);
        if (!root) continue;
        std::vector<core::VirtualPath> stack;
        for (const auto& entry : vfs.list(*root))
            if (entry.directory) stack.push_back(entry.path);
        while (!stack.empty()) {
            const auto here = stack.back();
            stack.pop_back();
            for (const auto& entry : vfs.list(here)) {
                if (entry.directory) {
                    stack.push_back(entry.path);
                } else if (core::fold_case(entry.path.filename()) == wanted) {
                    return entry.path;
                }
            }
        }
    }
    return std::nullopt;
}

ConsistDefinition load_consist(const core::Vfs& vfs, const std::string_view name) {
    const auto path = find_consist(vfs, name);
    if (!path) return {.name = std::string(name), .error = "consist not found"};
    std::string error;
    const auto bytes = vfs.read(*path, &error);
    if (!bytes) return {.name = std::string(name), .error = error};
    auto consist = parse_consist(*bytes);
    if (consist.name.empty()) consist.name = std::string(name);
    return consist;
}

} // namespace openrts::formats
