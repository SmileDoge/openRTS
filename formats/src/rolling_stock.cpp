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
    engine.main_reservoir_volume = static_cast<float>(values(text, "airbrakesmainresvolume").atof());
    engine.dynamic_brake_max_force = static_cast<float>(values(text, "dynamicbrakesmaximumforce").quantity());
    if (const auto controllers = rts_find(text, "enginecontrollers")) {
        if (const auto throttle = rts_find(text, "throttle", *controllers)) {
            engine.throttle_notches = static_cast<std::uint32_t>((std::max)(0, rts_atoi(values(text, "numnotches", *throttle).here())));
        }
    }
    if (const auto extended = rts_find(text, "engextended")) {
        engine.library_module = values(text, "librarymodule", *extended).token();
        engine.cabin = values(text, "cabin", *extended).token();
        if (auto stack = values(text, "variablestack", *extended); stack.found()) {
            engine.variable_stack = static_cast<std::uint32_t>((std::max)(0, rts_atoi(stack.here())));
        }
    }
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

    wagon.sound = values(t, "rtsexteriorsound").token();
    if (wagon.sound.empty()) wagon.sound = values(t, "sound").token();
    wagon.cab_sound = values(t, "rtscabinsound").token();

    if (const auto engine = rts_find(t, "engine")) wagon.engine = read_engine(t, *engine);
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
