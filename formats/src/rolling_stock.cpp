#include <openrts/formats/rolling_stock.hpp>

#include <algorithm>

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
    engine.max_power = static_cast<float>(values(text, "maxpower").quantity());
    engine.max_force = static_cast<float>(values(text, "maxforce").quantity());
    engine.max_continuous_force = static_cast<float>(values(text, "maxcontinuousforce").quantity());
    engine.max_velocity = static_cast<float>(values(text, "maxvelocity").quantity());
    engine.max_current = static_cast<float>(values(text, "maxcurrent").atof());
    engine.sanding_speed = static_cast<float>(values(text, "sanding").quantity());
    if (auto sand = values(text, "maxsandingtime"); sand.found()) engine.max_sanding_time = static_cast<float>(sand.atof());
    engine.main_reservoir_volume = static_cast<float>(values(text, "airbrakesmainresvolume").atof());
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

WagonDefinition load_wagon(const core::Vfs& vfs, const std::string_view folder, const std::string_view name,
                           const bool engine) {
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
    // parse_wagon expects file bytes; the merged text is already decoded.
    auto wagon = parse_wagon(*main);
    // The merged companion text comes first in the Wagon block, so its
    // first token is no longer the name; the file name is.
    wagon.name = std::string(name);
    return wagon;
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
