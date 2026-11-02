// Route electrification (docs/electrification.md): the .electrotype /
// .electrodb parsers, the line under a car as RTS finds it (0x5B53A8), the
// train's supply (0x503CE4), WorksOnLineVoltages and the keyboard driver
// without a line it works on. With an RTS installation as the argument, also
// the installed routes (Pavlovsk: a docking station; Kuban: AC; Chernyshevsk: DC).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/electrification.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/electrification.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

namespace {

using namespace openrts;
using simulation::Electrification;
using simulation::LineState;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

bool near(const double a, const double b, const double tolerance) { return std::abs(a - b) <= tolerance; }

// Substation 1: 3300 V DC, 20 V/km; substation 2: 27500 V 50 Hz, no drop.
const char* types_text = R"(SIMRTS@@@@@@@@@@JINX0D0e______
ElectricSupplyTypes (
    ElectricSupplyType ( ID ( 1 ) Name ( "DC" ) Colour ( ffffaa00 ) Voltage ( 3300 ) Frequency ( 0 ) VoltageDrop ( 20 ) )
    ElectricSupplyType ( ID ( 2 ) Name ( "AC" ) Colour ( ff1e90ff ) Voltage ( 27500 ) Frequency ( 50 ) VoltageDrop ( 0 ) )
)
CatenaryTypes ( CatenaryType ( ID ( 1 ) Name ( "wire" ) Colour ( ff4169e1 ) Position ( 0 5.7 ) Type ( 1 ) NumWires ( 2 ) BurnQ ( 1 ) ) )
ElectricSubstations (
    ElectricSubstation ( ID ( 1 ) Name ( "DC sub" ) ElectroType ( 1 ) Position ( 0 0 0 0 0 ) PhaseNum ( 1 ) Power ( 40 ) Voltage ( 3300 ) PowerDecline ( 0.9 0.2 ) )
    ElectricSubstation ( ID ( 2 ) Name ( "AC sub" ) ElectroType ( 2 ) Position ( 0 0 0 0 3000 ) PhaseNum ( 3 ) Power ( 40 ) Voltage ( 27500 ) PowerDecline ( 0.9 0.2 ) )
)
)";

// Node 1 (2000 m along +z): section 1 (DC, feeder at the start) to 800 m,
// dead to 1000 m, then the docking section 2 (AC chosen, DC also there).
// Node 1 of a second database: a wire only from 64 m "to -1", which RTS
// does not take as a wire.
const char* database_text = R"(SIMRTS@@@@@@@@@@JINX0D1e______
ElectroSections ( 2
    ElectroSection ( ID ( 1 ) Name ( "line" ) BasicCatenaryType ( 1 )
        PowerSources ( 1 PowerSource ( Substation ( 1 ) Phase ( 1 ) Active ( 3 ) FeederPos ( 0 0 0 0 0 ) FeederQ ( 100 ) ) ) )
    ElectroSection ( ID ( 2 ) Name ( "station" ) CurrentSource ( 0 ) BasicCatenaryType ( 1 )
        PowerSources ( 2
            PowerSource ( Substation ( 2 ) Phase ( 1 ) UseSubstationPos ( 1 ) FeederQ ( 100 ) )
            PowerSource ( Substation ( 1 ) Phase ( 1 ) UseSubstationPos ( 1 ) FeederQ ( 100 ) ) ) )
)
SectionElectrification ( 1
    Section ( 1
        Electricity ( 3 1 0 00000000 0 800 00000001 2 1000 00000001 )
        Catenary ( 1 1 0 -1 -1 00000000 )
    )
)
)";

const char* engine_common = R"(
    Size ( 3m 4m 20m )
    WheelRadius ( 0.6m )
    NumWheels ( 4 )
    Coupling ( Type ( Automatic ) Spring ( Stiffness ( 1e6 1e6 ) Damping ( 1e6 1e6 ) Break ( 1e7 1e7 ) r0 ( 0.1m 0.1m ) ) )
    Friction ( 1000 1 0 0 1 )
    Adheasion ( 0.2 0.3 2 0 )
    BrakeEquipmentType ( "Triple_valve, Auxilary_reservoir" )
    BrakeSystemType ( "Air_single_pipe" )
    MaxBrakeForce ( 100kN )
)";

std::string engine_text(const std::string& voltages) {
    return std::string("Wagon ( loco\n Type ( Engine )\n Mass ( 100t )\n") + engine_common + R"()
Engine ( loco
    Type ( Electric )
    MaxPower ( 3000kW )
    MaxForce ( 300kN )
    MaxVelocity ( 100km/h )
    AirBrakesMainResVolume ( 30 )
    EngineControllers ( Throttle ( 0 1 0.1 0 NumNotches ( 10 ) ) )
)
EngExtended ( )" + voltages + R"( MaxLineCurrent ( 700 ) )
)";
}

formats::RouteElectrification route(const bool database, const bool types, const bool electrified) {
    formats::RouteElectrification r;
    r.electrified = electrified;
    if (types) {
        r.types = formats::parse_electric_types(types_text);
        r.has_types = r.types.ok();
    }
    if (database) {
        r.database = formats::parse_electro_database(database_text);
        r.has_database = r.database.ok();
    }
    return r;
}

simulation::TrackPoint point_at(const simulation::TrackNetwork& network, const double offset) {
    return network.point(1, offset).value_or(simulation::TrackPoint{});
}

void test_parse() {
    const auto types = formats::parse_electric_types(types_text);
    check(types.ok() && types.supply_types.size() == 2 && types.substations.size() == 2 && types.catenary_types.size() == 1,
          "electrotype: two supplies, two substations, a catenary type");
    check(types.supply_types.size() == 2 && types.supply_types[1].frequency == 50.0f && types.supply_types[0].voltage_drop == 20.0f,
          "supply frequency and drop");
    check(types.supply_types.size() == 2 && types.supply_types[0].colour == 0xffffaa00u, "colour as AARRGGBB");
    const auto db = formats::parse_electro_database(database_text);
    check(db.ok() && db.sections.size() == 2 && db.nodes.size() == 1, "electrodb: two sections, one node");
    if (db.sections.size() == 2) {
        const auto& station = db.sections[1];
        check(station.current_source == 0 && station.sources.size() == 2, "CurrentSource");
        check(station.sources.size() == 2 && station.sources[0].active == 3u && station.sources[1].active == 2u,
              "CurrentSource switches the chosen source in and enables all (0x491DC8)");
        check(db.sections[0].sources[0].feeder.has_value() && db.sections[0].sources[0].feeder_q == 100.0f, "feeder");
    }
    if (db.nodes.size() == 1) {
        const auto& n = db.nodes[0];
        check(n.node == 1 && n.electricity.size() == 3 && n.catenary.size() == 1, "node pieces");
        check(n.electricity.size() == 3 && n.electricity[1].section == 0 && n.electricity[1].start == 800.0f &&
                  n.electricity[1].flags == 1u, "a dead piece");
        check(n.catenary.size() == 1 && n.catenary[0].length == -1.0f, "catenary to the node end");
    }
    const auto engine = formats::parse_wagon(engine_text(
        "WorksOnLineVoltages ( 2 LineVoltage ( 19000 30000 50 50 ) LineVoltage ( 2400 4000 0 0 ) )"));
    check(engine.engine && engine.engine->line_voltages.size() == 2 && engine.engine->line_voltages[1].max_voltage == 4000.0f &&
              engine.engine->max_line_current == 700.0f, "WorksOnLineVoltages and MaxLineCurrent");
}

void test_line() {
    const auto network = simulation::straight_test_track(2000.0);
    check(network != nullptr, "test track");
    if (!network) return;
    const auto dc = formats::parse_wagon(engine_text("WorksOnLineVoltages ( 1 LineVoltage ( 2500 4000 0 0 ) )"));
    const auto dual = formats::parse_wagon(engine_text(
        "WorksOnLineVoltages ( 2 LineVoltage ( 19000 30000 50 50 ) LineVoltage ( 2400 4000 0 0 ) )"));
    const auto bare = formats::parse_wagon(engine_text(""));

    Electrification line(route(true, true, true), *network);
    check(line.mode() == Electrification::Mode::sections, "sections mode with an .electrodb");
    const auto a = line.at(1, 100.0, point_at(*network, 100.0), &*dc.engine);
    check(a.state == LineState::live && a.supply.frequency == 0.0f && near(a.supply.voltage, 3300.0 - 20.0 * 0.1, 0.5),
          "section 1: DC less 20 V/km from the feeder");
    const auto d = line.at(1, 900.0, point_at(*network, 900.0), &*dc.engine);
    check(d.state == LineState::dead && d.supply.voltage == 0.0f, "a dead stretch");
    const auto s = line.at(1, 1500.0, point_at(*network, 1500.0), &*dc.engine);
    check(s.state == LineState::live && s.supply.voltage == 27500.0f && s.supply.frequency == 50.0f, "the docking section on AC");
    line.set_current_source(1, 1);
    const auto s2 = line.at(1, 1500.0, point_at(*network, 1500.0), &*dc.engine);
    check(s2.state == LineState::live && s2.supply.frequency == 0.0f && near(s2.supply.voltage, 3300.0 - 20.0 * 1.5, 0.5),
          "switched to the DC source, the drop from its substation");
    line.set_current_source(1, 0);
    line.automatic_sources = true;
    const auto s3 = line.at(1, 1500.0, point_at(*network, 1500.0), &*dc.engine);
    check(s3.supply.frequency == 0.0f && s3.source == 1, "automatic: a DC engine gets the DC source");
    const auto s4 = line.at(1, 1500.0, point_at(*network, 1500.0), &*dual.engine);
    check(s4.supply.frequency == 50.0f && s4.source == 0, "automatic: a dual-system engine its first range, AC");
    line.automatic_sources = false;
    line.own_voltage = true;
    const auto own = line.at(1, 1500.0, point_at(*network, 1500.0), &*dc.engine);
    check(own.supply.voltage == 3250.0f && own.supply.frequency == 0.0f, "own voltage: a DC engine on the AC section gets DC");
    line.own_voltage = false;

    const auto stretches = line.stretches(1);
    check(stretches.size() == 3 && stretches[1].state == LineState::dead && near(stretches[1].from, 800.0, 0.01) &&
              near(stretches[1].to, 1000.0, 0.01), "map stretches: live, dead, live");

    // The fallbacks of 0x503CE4.
    Electrification types_only(route(false, true, true), *network);
    const auto t = types_only.at(1, 100.0, point_at(*network, 100.0), &*dc.engine);
    check(types_only.mode() == Electrification::Mode::single_type && t.supply.voltage == 3300.0f,
          "only an .electrotype: its first supply type everywhere");
    Electrification by_engine(route(false, false, true), *network);
    check(by_engine.at(1, 0.0, {}, &*dc.engine).supply.voltage == 3250.0f, "only Electrified: the middle of the first range");
    check(by_engine.at(1, 0.0, {}, &*dual.engine).supply.frequency == 50.0f, "a dual-system engine: its first range (AC)");
    check(by_engine.at(1, 0.0, {}, &*bare.engine).supply.voltage == 3000.0f, "no WorksOnLineVoltages: 3 kV DC (openRTS)");
    Electrification none(route(false, false, false), *network);
    check(none.at(1, 0.0, {}, &*dc.engine).supply.voltage == 0.0f, "not electrified: nothing");

    check(Electrification::works_on(*dual.engine, {27500.0f, 50.0f}) && Electrification::works_on(*dual.engine, {3300.0f, 0.0f}),
          "a dual-system engine works on both");
    check(!Electrification::works_on(*dc.engine, {27500.0f, 50.0f}), "a DC engine does not work on AC");
    check(Electrification::works_on(*dc.engine, {3800.0f, 0.0f}), "a DC engine works on DC above its range (the DLL judges the level)");
    check(Electrification::works_on(*bare.engine, {27500.0f, 50.0f}) && !Electrification::works_on(*bare.engine, {0.0f, 0.0f}),
          "no ranges: any voltage, but some");

    // A train: the line goes to its engines, and the keyboard driver has no
    // traction on a line its engine does not work on.
    const auto run = [&](const formats::WagonDefinition& stock, const double front, const bool raised) {
        simulation::Train train({{.definition = &stock}});
        std::string error;
        check(train.place(*network, 1, front, true, &error), "placed: " + error);
        const auto driver = simulation::attach_keyboard_driver(train);
        driver->reverser = 1;
        driver->throttle = 5;
        const auto supply = line.apply(train, {raised});
        train.step();
        return std::pair{supply, train.cars().front().force};
    };
    const auto [on_ac, force_ac] = run(dc, 1500.0, true);
    check(on_ac.voltage == 27500.0f && force_ac == 0.0f, "a DC engine on the AC section: no traction");
    const auto [on_dc, force_dc] = run(dc, 300.0, true);
    check(on_dc.frequency == 0.0f && on_dc.voltage > 3000.0f && force_dc > 0.0f, "on DC it pulls");
    const auto [down, force_down] = run(dc, 300.0, false);
    check(down.voltage == 0.0f && force_down == 0.0f, "pantographs down: no voltage, no traction");
    const auto [dual_ac, force_dual] = run(dual, 1500.0, true);
    check(dual_ac.frequency == 50.0f && force_dual > 0.0f, "the dual-system engine pulls on AC");

    // A wire written as "from 64 m, length -1" on a node with it alone: RTS
    // (0x5B53A8) matches it neither as the whole node nor as a stretch.
    std::string odd = database_text;
    odd.replace(odd.find("Catenary ( 1 1 0 -1 -1"), std::string("Catenary ( 1 1 0 -1 -1").size(), "Catenary ( 1 1 64 -1 -1");
    formats::RouteElectrification r = route(false, true, true);
    r.database = formats::parse_electro_database(odd);
    r.has_database = r.database.ok();
    Electrification gap(std::move(r), *network);
    check(gap.at(1, 100.0, point_at(*network, 100.0), &*dc.engine).state == LineState::no_wire, "start > 0 with length -1: no wire");
}

// The installed routes.
void test_content(const std::filesystem::path& root) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    const auto vl82 = formats::load_wagon(vfs, "ev_VL82m-032", "vl82m-032a", true);
    check(vl82.engine && vl82.engine->line_voltages.size() == 2, "VL82m: two supplies");
    const auto vl10 = formats::load_wagon(vfs, "tsrLoco_VL10k-1461", "vl10k-1461a", true);
    check(vl10.engine && vl10.engine->line_voltages.size() == 1 && vl10.engine->line_voltages[0].max_frequency == 0.0f,
          "VL10k: DC only");

    struct Expect {
        const char* route;
        bool ac, dc;
    };
    for (const auto& expect : {Expect{"Pavlovsk_RTS", true, true}, Expect{"Kuban_RTS", true, false},
                               Expect{"Chernyshevsk_RTS", false, true}}) {
        const auto path = core::VirtualPath::parse(std::string("ROUTES/") + expect.route);
        if (!path) continue;
        std::string error;
        auto network = simulation::TrackNetwork::load(vfs, *path, &error);
        check(network != nullptr, std::string(expect.route) + ": network " + error);
        if (!network) continue;
        Electrification line(formats::load_route_electrification(vfs, *path), *network);
        const auto& data = line.data();
        check(line.mode() == Electrification::Mode::sections, std::string(expect.route) + ": sections");
        std::size_t not_vector = 0, live_ac = 0, live_dc = 0, dead = 0;
        float low[2]{1e9f, 1e9f}, high[2]{0.0f, 0.0f}; // AC, DC
        for (const auto& n : data.database.nodes) {
            const auto* node = network->node(n.node);
            if (!node || node->kind != formats::TrackNodeKind::vector) {
                ++not_vector;
                continue;
            }
            for (const double share : {0.25, 0.5, 0.75}) {
                const double offset = node->length * share;
                const auto p = line.at(n.node, offset, network->point(n.node, offset).value_or(simulation::TrackPoint{}),
                                       vl82.engine ? &*vl82.engine : nullptr);
                if (p.state == LineState::live) {
                    const int kind = p.supply.frequency > 0.0f ? 0 : 1;
                    (kind == 0 ? live_ac : live_dc) += 1;
                    low[kind] = (std::min)(low[kind], p.supply.voltage);
                    high[kind] = (std::max)(high[kind], p.supply.voltage);
                }
                if (p.state == LineState::dead) ++dead;
            }
        }
        std::cout << "  " << expect.route << ": " << data.database.sections.size() << " sections, " << data.database.nodes.size()
                  << " nodes (" << not_vector << " not vector); samples AC " << live_ac << ", DC " << live_dc << ", dead " << dead
                  << '\n';
        if (live_ac) std::cout << "    AC " << low[0] << ".." << high[0] << " V\n";
        if (live_dc) std::cout << "    DC " << low[1] << ".." << high[1] << " V\n";
        check(not_vector == 0, std::string(expect.route) + ": every electrified node is a vector node of the TDB");
        check((live_ac > 0) == expect.ac && (live_dc > 0) == expect.dc, std::string(expect.route) + ": the kinds of current");
    }
}

} // namespace

int main(const int argc, char** argv) {
    test_parse();
    test_line();
    if (argc > 1) test_content(argv[1]);
    std::cout << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
