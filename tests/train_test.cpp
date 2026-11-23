// Trains: RTS value units, wagon parsing, brakes with and without distributor
// stability, and the train ends staying a train length apart at a buffer stop.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <array>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/brake_shoes.hpp>
#include <openrts/simulation/distributor.hpp>
#include <openrts/simulation/stock_rules.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

namespace {

using namespace openrts;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

bool near(const double a, const double b, const double tolerance) { return std::abs(a - b) < tolerance; }

const std::string common = R"(
    Size ( 3m 4m 20m )
    WheelRadius ( 0.6m )
    NumWheels ( 4 )
    Coupling ( Type ( Automatic ) Spring ( Stiffness ( 1e6 1e6 ) Damping ( 1e6 1e6 ) Break ( 1e7 1e7 ) r0 ( 0.1m 0.1m ) ) )
    Friction ( 1000 1 0 0 1 )
    Adheasion ( 0.2 0.3 2 0 )
    BrakeEquipmentType ( "Triple_valve, Auxilary_reservoir" )
    BrakeSystemType ( "Air_single_pipe" )
    MaxBrakeForce ( 100kN )
    TripleValveRatio ( 2.5 )
    MaxReleaseRate ( 20 )
    MaxApplicationRate ( 30 )
    MaxAuxilaryChargingRate ( 2 )
    BrakeCylinderPressureForMaxBrakeBrakeForce ( 55 )
    DerailBufferForce ( 732kN )
)";

const std::string engine_text = "Wagon ( loco\n Type ( Engine )\n Mass ( 100t )\n" + common + R"()
Engine ( loco
    Type ( Electric )
    MaxPower ( 3000kW )
    MaxForce ( 300kN )
    MaxVelocity ( 100km/h )
    AirBrakesMainResVolume ( 30 )
    EngineControllers ( Throttle ( 0 1 0.1 0 NumNotches ( 10 ) ) )
))";

const std::string wagon_text = "Wagon ( car\n Type ( Freight )\n Mass ( 80t )\n" + common + ")";

void test_units() {
    check(near(formats::rts_quantity_value("100kn"), 100000.0, 1e-6), "kN");
    check(near(formats::rts_quantity_value("60mph"), 60.0 * 1609.0 / 3600.0, 1e-6), "mph");
    check(near(formats::rts_quantity_value("72km/h"), 20.0, 1e-6), "km/h");
    check(near(formats::rts_quantity_value("2mw"), 2e6, 1e-3), "MW");
}

// The same stock with electro-pneumatic brakes.
std::string with_ep(std::string text) {
    const std::string from = R"(BrakeSystemType ( "Air_single_pipe" ))";
    text.replace(text.find(from), from.size(), R"(BrakeSystemType ( "Air_single_pipe, Electro-pneumatic" ))");
    return text;
}

struct Stock {
    explicit Stock(const bool ep = false)
        : engine(formats::parse_wagon(ep ? with_ep(engine_text) : engine_text)),
          wagon(formats::parse_wagon(ep ? with_ep(wagon_text) : wagon_text)) {}

    formats::WagonDefinition engine;
    formats::WagonDefinition wagon;

    [[nodiscard]] std::vector<simulation::CarSetup> cars(std::size_t wagons) const {
        std::vector<simulation::CarSetup> result{{.definition = &engine}};
        for (std::size_t i = 0; i < wagons; ++i) result.push_back({.definition = &wagon});
        return result;
    }
};

void test_parse(const Stock& stock) {
    check(stock.engine.ok() && stock.wagon.ok(), "stock parses");
    check(stock.engine.engine.has_value() && !stock.wagon.engine.has_value(), "engine block");
    check(near(stock.engine.mass, 100000.0, 1e-3), "mass in tonnes");
    check(near(stock.engine.length, 20.0, 1e-6), "size");
    check(stock.engine.brakes.system & 1, "single pipe");
    check(near(stock.engine.brakes.triple_valve_ratio, 2.5, 1e-6), "triple valve ratio");
    check(near(stock.engine.brakes.cylinder_pressure_for_max_force, 55.0 / 14.69, 1e-3), "psi to atm");
    check(stock.engine.engine && near(stock.engine.engine->max_force, 300000.0, 1e-3), "max force");
}

struct Run {
    std::unique_ptr<simulation::TrackNetwork> network = simulation::straight_test_track(3000.0);
    simulation::Train train;
    std::shared_ptr<simulation::DriverHandles> driver;
    float max_cylinder = 0.0f;

    Run(const Stock& stock, const simulation::TrainSettings settings, const double front, const std::size_t wagons = 10)
        : train(stock.cars(wagons), settings) {
        std::string error;
        check(train.place(*network, 1, front, true, &error), "placed: " + error);
        driver = simulation::attach_keyboard_driver(train);
        check(driver != nullptr, "keyboard driver");
    }

    void run(const double seconds) {
        for (int i = 0; i < static_cast<int>(seconds / simulation::train_step); ++i) {
            train.step();
            for (const auto& car : train.cars()) max_cylinder = (std::max)(max_cylinder, car.cylinder);
        }
    }
};

// The keyboard driver's 395 valve (the ВЛ10к DLL's) on the RTS triple valve.
void test_brakes(const Stock& stock) {
    using Handle = simulation::DriverHandles::BrakeHandle;
    const auto report = [](const char* what, const Run& r) {
        std::cerr << "  " << what << ": UR " << r.driver->equalising << ", head pipe " << r.train.cars().front().pipe
                  << ", last pipe " << r.train.cars().back().pipe << ", last cylinder " << r.train.cars().back().cylinder
                  << ", max cylinder so far " << r.max_cylinder << '\n';
    };
    // Charged and in II: nothing applies.
    {
        Run r(stock, {}, 1000.0);
        r.run(300.0);
        report("II 300 s", r);
        check(r.max_cylinder < 0.05f, "II keeps the train released");
        check(near(r.train.cars().back().pipe, r.driver->charging_pressure, 0.15), "II holds the charging pressure");
        // V for 3 s (the УР −0.6), then III: every car applies.
        r.driver->brake = Handle::service;
        r.run(3.0);
        r.driver->brake = Handle::lap;
        r.run(20.0);
        report("V 3 s, III 20 s", r);
        float least = 99.0f;
        for (const auto& car : r.train.cars()) least = (std::min)(least, car.cylinder);
        check(least > 0.3f, "a service step applies every car");
        // Back to II: the valve releases the train.
        r.driver->brake = Handle::running;
        r.run(120.0);
        report("II 120 s", r);
        float most = 0.0f;
        for (const auto& car : r.train.cars()) most = (std::max)(most, car.cylinder);
        check(most < 0.05f, "II releases after a service step");
    }
    // Overcharge: I held 10 s, then II until the stabiliser has removed it.
    {
        Run r(stock, {}, 1000.0);
        r.run(60.0);
        r.driver->brake = Handle::charge;
        r.run(10.0);
        report("I 10 s", r);
        check(r.driver->equalising > r.driver->charging_pressure + 1.0f, "I overcharges the equalising reservoir");
        r.driver->brake = Handle::running;
        r.max_cylinder = 0.0f;
        r.run(900.0);
        report("II 900 s", r);
        // As in RTS with this DLL: the stabiliser only works while the pipe
        // is below the УР (air drawn by the reservoirs; RTS has no pipe leak),
        // so the overcharge stays once they are charged, and nothing applies.
        check(r.max_cylinder < 0.05f, "an overcharge does not apply the brakes");
        // A service step from the overcharge applies, II releases it.
        r.driver->brake = Handle::service;
        r.run(4.0);
        r.driver->brake = Handle::lap;
        r.run(20.0);
        report("V 4 s, III 20 s", r);
        check(r.train.cars().back().cylinder > 0.3f, "a service step from an overcharge applies");
        r.driver->brake = Handle::running;
        r.run(180.0);
        report("II 180 s", r);
        float most = 0.0f;
        for (const auto& car : r.train.cars()) most = (std::max)(most, car.cylinder);
        check(most < 0.05f, "II releases after a step from an overcharge");
    }
    // VI: emergency.
    {
        Run r(stock, {}, 1000.0);
        r.run(60.0);
        r.driver->brake = Handle::emergency;
        r.run(30.0);
        report("VI 30 s", r);
        check(r.train.cars().back().cylinder > 2.0f, "VI applies fully");
    }
}

// Driven into the end of the track, the leading end stops and the trailing
// end must not keep moving.
void test_buffer_stop(const Stock& stock) {
    Run r(stock, {}, 2900.0);
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    r.run(60.0);
    auto walker = *r.train.rear_end();
    walker.move(r.train.length());
    const auto front = *r.train.front_end();
    check(walker.node() == front.node() && near(walker.offset(), front.offset(), 0.05),
          "train ends stay a train length apart at the end of the track");
}

// Changing ends: the old rear becomes the front, facing the other way, the
// cars keep their places, and driving ahead moves towards the old rear.
// The body on its springs: driving over a straight track, the rail joints
// knock it up and down, hunting sways it sideways, acceleration nods it;
// all stays within centimetres, as in RTS.
// 1000 m of straight track along +z, then a 60 m radius 90 degree curve.
std::unique_ptr<simulation::TrackNetwork> curved_test_track() {
    const std::string sections = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackSections ( 2\n"
                                 "    TrackSection ( 1 SectionSize ( 1.5 500 ) )\n"
                                 "    TrackSection ( 2 SectionSize ( 1.5 0 ) SectionCurve ( 60 90 ) )\n)\nTrackShapes ( 0 )\n";
    const std::string database = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackDB ( Serial ( 1 ) TrackNodes ( 3\n"
                                 "TrackNode ( 1 TrVectorNode ( TrVectorSections ( 3 "
                                 "1 0 0 0 0 1 1 00 0 0 0 0 0 0 0 0 "
                                 "1 0 0 0 0 1 1 00 0 0 0 0 500 0 0 0 "
                                 "2 0 0 0 0 1 1 00 0 0 0 0 1000 0 0 0 "
                                 ") ) TrPins ( 1 1 TrPin ( 2 1 ) TrPin ( 3 1 ) ) )\n"
                                 "TrackNode ( 2 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 TrPin ( 1 1 ) ) )\n"
                                 "TrackNode ( 3 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 TrPin ( 1 0 ) ) )\n"
                                 ") TrItemTable ( 0\n) )\n";
    auto library = formats::parse_track_sections(sections);
    auto db = formats::parse_track_database(database);
    if (!library.ok() || !db.ok()) return nullptr;
    return std::make_unique<simulation::TrackNetwork>(std::move(db), std::move(library));
}

// Too fast into a tight curve: a car overturns outwards (RTS 0x580B30), its
// roll grows to 0.1 + 0.04 |v|, and past 0.8 rad it lies down with its hoses
// torn off, so the train loses its air.
void test_overturn(const Stock& stock) {
    auto network = curved_test_track();
    check(network != nullptr, "curved test track builds");
    if (!network) return;
    simulation::Train train(stock.cars(3));
    std::string error;
    check(train.place(*network, 1, 960.0, true, &error), "placed before the curve: " + error);
    train.add_velocity(25.0f);
    bool derailed = false;
    bool broke = false;
    float most = 0.0f;
    bool fallen = false;
    for (int i = 0; i < static_cast<int>(8.0 / simulation::train_step); ++i) {
        train.step();
        for (const auto& event : train.take_events()) {
            derailed = derailed || event.kind == simulation::TrainEventKind::derailment;
            broke = broke || event.kind == simulation::TrainEventKind::coupler_break;
        }
        for (const auto& car : train.cars()) {
            most = (std::max)(most, std::abs(car.tilt));
            fallen = fallen || car.fallen;
        }
    }
    std::cerr << "  overturn: most roll " << most << " rad, speed " << train.speed() << '\n';
    check(derailed, "overspeed in a tight curve derails");
    // RTS 0x581694: past 0.9 rad a coupler is damaged, the train stays one
    // (a derailed VL80 stayed one train in the live capture).
    check(!broke, "a derailment cuts no coupler: the train stays one");
    check(most > 0.8f && fallen, "the car rolls past 0.8 rad and lies down");
    bool torn = false;
    for (const auto& car : train.cars()) torn = torn || (car.fallen && (car.pipe_ends & simulation::pipe_hose_front) != 0);
    check(torn, "a fallen car's hoses are torn off");
}

// Off the end of the track the leading car derails and slides on along
// its heading until its friction stops it (RTS 0x5059A8, sliding branch).
void test_slide_off(const Stock& stock) {
    Run r(stock, {}, 2950.0);
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    bool derailed = false;
    for (int i = 0; i < static_cast<int>(40.0 / simulation::train_step) && !derailed; ++i) {
        r.train.step();
        for (const auto& event : r.train.take_events()) derailed = derailed || event.kind == simulation::TrainEventKind::derailment;
    }
    check(derailed, "running off the end of the track derails");
    const auto& lead = r.train.cars().front();
    check(lead.derail == simulation::DerailKind::sliding, "the leading car slides off");
    r.driver->throttle = 0;
    for (int i = 0; i < static_cast<int>(10.0 / simulation::train_step); ++i) r.train.step();
    check(r.train.cars().front().slide > 0.2f && r.train.cars().front().slide_speed == 0.0f,
          "the slid car comes to rest a little way on");
    check(r.train.cars().front().slide_origin_set, "the place it left the track is kept");
    std::size_t folded = 0;
    for (const auto& car : r.train.cars()) folded += car.derail == simulation::DerailKind::sliding && car.yaw != 0.0f;
    std::cerr << "  slide off: lead slid " << r.train.cars().front().slide << " m, " << folded << " cars folded\n";
    check(folded > 0, "the cars behind run into it and fold");
}

// A hit on a car on the rails: below DerailBufferForce the body takes the
// shove, above it the car comes off (RTS 0x589A14).
void test_hit(const Stock& stock) {
    Run r(stock, {}, 1500.0);
    const float along[3] = {0.0f, 0.0f, 1.0f};
    r.train.hit(2, 100000.0f, along, 0.1f);
    check(r.train.cars()[2].derail == simulation::DerailKind::none && r.train.cars()[2].body[2] > 0.0f,
          "a light hit shoves the body");
    r.train.hit(2, 2000000.0f, along, 0.1f);
    check(r.train.cars()[2].derail == simulation::DerailKind::sliding, "a hard hit derails the car");
}

void test_sway(const Stock& stock) {
    Run r(stock, {}, 1500.0);
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    float lateral = 0.0f, vertical = 0.0f, longitudinal = 0.0f;
    std::size_t joints = 0;
    for (int i = 0; i < static_cast<int>(60.0 / simulation::train_step); ++i) {
        r.train.step();
        for (const auto& event : r.train.take_events()) joints += event.kind == simulation::TrainEventKind::joint;
        const auto& car = r.train.cars()[3];
        lateral = (std::max)(lateral, std::abs(car.body[0]));
        vertical = (std::max)(vertical, std::abs(car.body[1]));
        longitudinal = (std::max)(longitudinal, std::abs(car.body[2]));
    }
    std::cerr << "  sway: lateral " << lateral << " vertical " << vertical << " longitudinal " << longitudinal
              << " m, " << joints << " joint events, speed " << r.train.speed() << '\n';
    check(joints > 10, "rail joints every 50 m give joint events");
    check(vertical > 0.001f && vertical < 0.2f, "rail joints knock the body within centimetres");
    check(lateral > 0.0f && lateral < 0.2f, "hunting sways the body sideways");
    check(longitudinal > 0.0f && longitudinal < 0.2f, "acceleration nods the body");
}

// The way a train travels (leading_end: the signals, speed posts and ALSN
// ahead) after a stop: its couplers ease and the front creeps back a little.
// Each such step made the rear the lead: the signals ahead ran as the other
// end's, closed under a path, the ALSN read К, until the train moved on
// (testers 2026-11-18). A real move back turns it.
// A saved game (Train::save_state / restore_state): a train saved while it
// runs and brakes, put back into a new one placed elsewhere, goes on exactly
// as the first: the same place, speed, pipe and cylinders, step by step.
void test_save_restore(const Stock& stock, const simulation::BrakeModel model) {
    simulation::TrainSettings settings;
    Run a(stock, settings, 1500.0);
    a.train.set_brake_model(model);
    a.driver->reverser = 1;
    a.driver->throttle = a.driver->notches;
    a.run(20.0);
    a.driver->throttle = 0;
    a.driver->brake = simulation::DriverHandles::BrakeHandle::service;
    a.run(3.0);
    a.driver->brake = simulation::DriverHandles::BrakeHandle::lap;
    a.run(2.0);
    simulation::StateWriter saved;
    a.train.save_state(saved);

    Run b(stock, settings, 900.0);
    b.train.set_brake_model(model);
    b.train.restore_state(simulation::StateReader(saved.text()));
    check(b.driver->brake == simulation::DriverHandles::BrakeHandle::lap && b.driver->reverser == 1,
          "restored: the driver's handles");
    check(std::abs(b.train.speed() - a.train.speed()) < 1e-6, "restored: the speed");
    check(b.train.front_end() && a.train.front_end() && b.train.front_end()->node() == a.train.front_end()->node()
              && std::abs(b.train.front_end()->offset() - a.train.front_end()->offset()) < 1e-9,
          "restored: the front where it was");
    double worst = 0.0;
    float worst_air = 0.0f;
    for (int i = 0; i < static_cast<int>(30.0 / simulation::train_step); ++i) {
        if (i == static_cast<int>(10.0 / simulation::train_step)) {
            a.driver->brake = b.driver->brake = simulation::DriverHandles::BrakeHandle::running;
        }
        a.train.step();
        b.train.step();
        worst = (std::max)(worst, std::abs(a.train.front_end()->offset() - b.train.front_end()->offset()));
        for (std::size_t k = 0; k < a.train.cars().size(); ++k) {
            worst_air = (std::max)({worst_air, std::abs(a.train.cars()[k].pipe - b.train.cars()[k].pipe),
                                    std::abs(a.train.cars()[k].cylinder - b.train.cars()[k].cylinder)});
        }
    }
    const char* name = model == simulation::BrakeModel::realistic ? "realistic air" : "RTS air";
    std::cerr << "  save and restore (" << name << "): front apart at most " << worst * 1000.0 << " mm, air " << worst_air << " atm"
              << std::endl;
    check(worst < 1e-3, std::string("restored (") + name + "): it runs on as the saved one, within a millimetre");
    check(worst_air < 1e-3f, std::string("restored (") + name + "): its air the same");
}

// The brakes' condition (docs/train.md "Brake condition"): drawn per car
// from the seed, worse with a lower condition, kept by a save; the pipe
// left in III loses more the worse; the pressing follows the rigging and
// the factor.
void test_brake_condition(const Stock& stock) {
    using Handle = simulation::DriverHandles::BrakeHandle;
    simulation::TrainSettings settings;
    settings.brake_model = simulation::BrakeModel::realistic;
    const auto mean = [](const simulation::Train& train, float simulation::BrakeWear::*field) {
        double sum = 0.0;
        for (const auto& car : train.cars()) sum += car.wear.*field;
        return sum / static_cast<double>(train.cars().size());
    };
    const auto spread = [](const simulation::Train& train, float simulation::BrakeWear::*field) {
        float least = 9.0f, most = 0.0f;
        for (const auto& car : train.cars()) {
            least = (std::min)(least, car.wear.*field);
            most = (std::max)(most, car.wear.*field);
        }
        return most - least;
    };
    simulation::Train fresh(stock.cars(10), settings), worn(stock.cars(10), settings), again(stock.cars(10), settings);
    for (const auto& car : fresh.cars()) check(car.wear == simulation::BrakeWear{}, "condition: none until set");
    fresh.set_brake_condition(10, 7u);
    worn.set_brake_condition(1, 7u);
    again.set_brake_condition(1, 7u);
    bool same = true;
    for (std::size_t i = 0; i < worn.cars().size(); ++i) same = same && worn.cars()[i].wear == again.cars()[i].wear;
    check(same, "condition: the same seed, the same cars");
    again.set_brake_condition(1, 8u);
    check(!(worn.cars()[1].wear == again.cars()[1].wear), "condition: another seed, other cars");
    std::cerr << "  condition 10 / 1: pipe leak " << mean(fresh, &simulation::BrakeWear::pipe_leak) << " / "
              << mean(worn, &simulation::BrakeWear::pipe_leak) << " atm/min, pace spread " << spread(fresh, &simulation::BrakeWear::pace)
              << " / " << spread(worn, &simulation::BrakeWear::pace) << ", rigging " << mean(fresh, &simulation::BrakeWear::rigging)
              << " / " << mean(worn, &simulation::BrakeWear::rigging) << ", pressing " << fresh.brake_pressing() << " / "
              << worn.brake_pressing() << std::endl;
    check(mean(worn, &simulation::BrakeWear::pipe_leak) > 10.0 * mean(fresh, &simulation::BrakeWear::pipe_leak), "condition 1 leaks far more");
    check(spread(fresh, &simulation::BrakeWear::pace) > 0.0f, "condition 10: the cars still differ a little");
    check(spread(worn, &simulation::BrakeWear::pace) > 3.0f * spread(fresh, &simulation::BrakeWear::pace), "condition 1: far less alike");
    check(worn.brake_pressing() < 0.9 * fresh.brake_pressing(), "condition 1: the rigging presses less");
    const double pressing = fresh.brake_pressing();
    fresh.set_brake_force_factor(1.5f);
    check(near(fresh.brake_pressing(), 1.5 * pressing, 1e-6 * pressing), "the pressing factor scales the pressing");
    fresh.set_brake_condition(0);
    for (const auto& car : fresh.cars()) check(car.wear == simulation::BrakeWear{}, "condition 0: none again");

    // Charged, then III for a minute: the pipe's fall.
    const auto lap_fall = [&](const int condition) {
        Run r(stock, settings, 1000.0);
        r.train.set_brake_condition(condition, 11u);
        r.run(400.0);
        const float before = r.train.cars().back().pipe;
        r.driver->brake = Handle::lap;
        r.run(60.0);
        return before - r.train.cars().back().pipe;
    };
    const float f10 = lap_fall(10), f6 = lap_fall(6), f1 = lap_fall(1);
    std::cerr << "  III for 60 s, the tail's pipe falls: condition 10 " << f10 << ", 6 " << f6 << ", 1 " << f1 << " atm" << std::endl;
    check(f10 < 0.05f, "condition 10: the pipe holds in III");
    check(f6 > f10 && f6 < 0.25f, "condition 6: within the rules' 0.2 atm a minute, about");
    check(f1 > 0.3f, "condition 1: the pipe falls fast in III");

    // A save keeps each car's wear and the train's condition.
    Run a(stock, settings, 1000.0);
    a.train.set_brake_condition(3, 99u);
    simulation::StateWriter saved;
    a.train.save_state(saved);
    Run b(stock, settings, 1000.0);
    b.train.set_brake_condition(8, 5u);
    b.train.restore_state(simulation::StateReader(saved.text()));
    same = b.train.brake_condition() == 3 && b.train.brake_seed() == 99u;
    for (std::size_t i = 0; i < a.train.cars().size(); ++i) same = same && near(a.train.cars()[i].wear.pace, b.train.cars()[i].wear.pace, 1e-6);
    check(same, "condition: a save keeps it, car by car");
}

void test_travel_after_stop(const Stock& stock) {
    Run r(stock, {}, 1500.0);
    const auto leads_front = [&] {
        const auto lead = r.train.leading_end();
        const auto front = r.train.front_end();
        return lead && front && lead->node() == front->node() && lead->forward() == front->forward()
               && std::abs(lead->offset() - front->offset()) < 1e-6;
    };
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    for (int i = 0; i < static_cast<int>(25.0 / simulation::train_step); ++i) r.train.step();
    r.driver->throttle = 0;
    r.driver->brake = simulation::DriverHandles::BrakeHandle::emergency;
    bool stood = false, kept = true;
    for (int i = 0; i < static_cast<int>(60.0 / simulation::train_step); ++i) {
        r.train.step();
        if (r.train.speed() <= 0.0) stood = true;
        if (stood) kept = kept && leads_front();
    }
    check(stood, "the train stops");
    check(kept, "coming to rest the front stays the lead");
    // The brakes released: the couplers squeezed by the stop ease and the
    // front rolls back some 17 cm at up to 6 cm/s, then a little forward.
    const auto start = *r.train.front_end();
    double back = 0.0;
    r.driver->brake = simulation::DriverHandles::BrakeHandle::charge;
    for (int i = 0; i < static_cast<int>(20.0 / simulation::train_step); ++i) {
        r.train.step();
        back = (std::max)(back, start.offset() - r.train.front_end()->offset());
        kept = kept && leads_front();
    }
    std::cerr << "  released after the stop: the front rolled back up to " << back * 1000.0 << " mm" << std::endl;
    check(back > 0.02 && back < 0.5, "released, it rolls back some centimetres");
    check(kept, "and the front stays the lead all along");
    // Backing off for real turns the lead to the rear.
    r.driver->brake = simulation::DriverHandles::BrakeHandle::running;
    r.driver->reverser = -1;
    r.driver->throttle = r.driver->notches;
    bool turned = false;
    for (int i = 0; i < static_cast<int>(40.0 / simulation::train_step) && !turned; ++i) {
        r.train.step();
        turned = !leads_front();
    }
    check(turned, "backing off turns the lead to the rear");
}

// openRTS's body motion after a stop: the body swings back on its springs
// and comes to rest smoothly. It was settled (put at rest at once) as soon
// as its rates were small, which a swing's far end is too: the body and the
// cab camera on it jumped back (testers 2026-11-18).
void test_body_settles(const Stock& stock) {
    Run r(stock, {}, 1500.0);
    r.train.set_body_motion(simulation::BodyMotionModel::openrts);
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    for (int i = 0; i < static_cast<int>(25.0 / simulation::train_step); ++i) r.train.step();
    r.driver->throttle = 0;
    r.driver->brake = simulation::DriverHandles::BrakeHandle::emergency;
    bool stood = false;
    float jump = 0.0f, swing = 0.0f;
    std::array<float, 3> last{};
    for (int i = 0; i < static_cast<int>(60.0 / simulation::train_step); ++i) {
        r.train.step();
        const auto& car = r.train.cars()[1];
        if (stood) {
            for (int a = 0; a < 3; ++a) {
                jump = (std::max)(jump, std::abs(car.body[a] - last[static_cast<std::size_t>(a)]));
                swing = (std::max)(swing, std::abs(car.body[a]));
            }
        }
        if (r.train.speed() == 0.0) stood = true;
        for (int a = 0; a < 3; ++a) last[static_cast<std::size_t>(a)] = car.body[a];
    }
    std::cerr << "  body after the stop: swing " << swing * 1000.0f << " mm, largest step " << jump * 1000.0f << " mm" << '\n';
    check(stood, "the train stops");
    check(jump < 0.004f, "after the stop the body swings back with no jump");
    check(std::abs(r.train.cars()[1].body_rate[2]) < 1e-3f, "and comes to rest");
}

// openRTS's rail joints on the 3000 m (welded) test node: a bogie's joint
// events come in a span's bursts, 12.5 m apart, with a string between.
void test_rail_joints(const Stock& stock) {
    Run r(stock, {}, 1500.0);
    r.network->set_rail_joint_layout(simulation::RailJointLayout::openrts);
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    std::vector<double> at;
    for (int i = 0; i < static_cast<int>(90.0 / simulation::train_step); ++i) {
        r.train.step();
        for (const auto& event : r.train.take_events()) {
            if (event.kind == simulation::TrainEventKind::joint && event.car == 3 && event.bogie == 0) at.push_back(r.train.cars()[3].travelled);
        }
    }
    bool spaced = !at.empty();
    for (std::size_t k = 1; k < at.size(); ++k) {
        const double gap = at[k] - at[k - 1];
        spaced = spaced && (std::abs(gap - 12.5) < 1.5 || gap > 500.0);
    }
    std::cerr << "  openRTS joints: " << at.size() << " events of car 4's front bogie over " << r.train.cars()[3].travelled << " m\n";
    check(spaced, "openRTS joints: a span's joints 12.5 m apart, strings between");
}

void test_turn_around(const Stock& stock) {
    Run r(stock, {}, 1500.0);
    const auto old_front = *r.train.front_end();
    const auto old_rear = *r.train.rear_end();
    const bool first_flipped = r.train.cars().front().flipped;
    check(r.train.turn_around(), "a standing train changes ends");
    const auto front = *r.train.front_end();
    const auto rear = *r.train.rear_end();
    check(front.node() == old_rear.node() && near(front.offset(), old_rear.offset(), 0.05) && front.forward() != old_rear.forward(),
          "the new front is the old rear, facing the other way");
    check(rear.node() == old_front.node() && near(rear.offset(), old_front.offset(), 0.05), "the new rear is the old front");
    check(r.train.cars().back().flipped != first_flipped, "the old first car is last and turned");
    // The master is the new front engine, as the viewer re-attaches it.
    r.driver = simulation::attach_keyboard_driver(r.train);
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    r.run(20.0);
    const auto moved = *r.train.front_end();
    check(moved.offset() < front.offset() - 1.0, "driving ahead after changing ends moves towards the old rear");
    check(!r.train.turn_around(), "a moving train does not change ends");
}

// Uncoupling: closed cocks keep both parts charged; open cocks (a coupler
// break) vent the pipe and both parts brake. The cut stands where it was.
// The joints' force on the move (Train::joint_forces): Newton on the cars
// behind; RTS's CF stops at the springs' full stretch.
void test_joint_forces(const Stock& stock) {
    Run r(stock, {}, 1500.0);
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    r.run(40.0);
    const auto& cars = r.train.cars();
    const auto forces = r.train.joint_forces();
    check(forces.size() + 1 == cars.size(), "a force for every joint");
    check(r.train.speed() > 0.5, "the train is moving after 40 s of full throttle");
    if (forces.size() + 1 != cars.size() || r.train.speed() < 0.5) return;
    // Behind the engine: its traction less what it needs itself.
    const auto& engine = cars.front();
    const double own = engine.force - engine.grade - engine.resistance - engine.brake_force;
    const double need = engine.definition->mass * engine.acceleration;
    check(near(forces.front(), own - need, 2000.0), "the first joint passes the engine's traction less its own needs");
    // It falls along the train and the last one pulls just the last car.
    bool falling = true;
    for (std::size_t j = 1; j < forces.size(); ++j) falling = falling && forces[j] <= forces[j - 1] + 500.0f;
    check(falling, "the force falls along the train");
    const auto& last = cars.back();
    check(near(forces.back(), last.definition->mass * last.acceleration + last.resistance + last.brake_force + last.grade, 2000.0),
          "the last joint pulls the last car");
}

// Brake shoes (brake_shoes.hpp): the Правила тяговых расчётов friction, and
// the realistic air's brake force by the speed.
void test_brake_shoes(const Stock& stock) {
    using formats::BrakeShoe;
    // Cast iron at 30 kN: 0.261 standing, 0.087 at 100 km/h; composite at 15 kN: 0.364 standing.
    check(near(simulation::shoe_friction(BrakeShoe::cast_iron, 0.0f, 30.0f), 0.2612, 0.001), "cast iron standing");
    check(near(simulation::shoe_friction(BrakeShoe::cast_iron, 100.0f, 30.0f), 0.0871, 0.001), "cast iron at 100 km/h");
    check(near(simulation::shoe_friction(BrakeShoe::composite, 0.0f, 15.0f), 0.3638, 0.001), "composite standing");
    check(near(simulation::shoe_factor(BrakeShoe::cast_iron, 40.0f, 1.0f), 1.0, 1e-5), "the content's force at 40 km/h");
    check(simulation::shoe_factor(BrakeShoe::cast_iron, 0.0f, 1.0f) > 1.9f, "cast iron grips twice as hard at a stop");
    check(simulation::shoe_factor(BrakeShoe::composite, 0.0f, 1.0f) < 1.3f, "composite only a fifth more");
    check(simulation::shoe_factor(BrakeShoe::cast_iron, 40.0f, 0.4f) > 1.0f, "a lightly pressed cast-iron shoe grips relatively harder");
    // The defaults: the engine cast iron, the freight car composite.
    check(simulation::shoe_of(stock.engine) == BrakeShoe::cast_iron && simulation::shoe_of(stock.wagon) == BrakeShoe::composite,
          "default shoes by the kind");
    // A train braking: the realistic air's force grows as it slows (cast
    // iron on the engine), RTS's stays.
    for (const bool realistic : {true, false}) {
        Run r(stock, {}, 1500.0);
        if (realistic) r.train.set_brake_model(simulation::BrakeModel::realistic);
        r.driver->reverser = 1;
        r.driver->throttle = r.driver->notches;
        r.run(60.0);
        r.driver->throttle = 0;
        r.driver->independent = 3.0f;
        // The force per atm of the cylinder (the cylinder still fills).
        double fast = 0.0, slow = 0.0;
        for (int i = 0; i < 4000 && r.train.speed() > 0.3; ++i) {
            r.train.step();
            const auto& engine = r.train.cars().front();
            const double per_atm = engine.brake_force / (std::max)(engine.independent, engine.cylinder);
            if (engine.independent > 2.9f && fast == 0.0) fast = per_atm;
            if (engine.independent > 2.9f) slow = per_atm;
        }
        if (realistic) {
            check(fast > 0.0 && slow > fast * 1.3, "realistic air: the engine's cast-iron shoes brake harder as it slows");
        } else {
            check(fast > 0.0 && near(slow, fast, fast * 0.01), "RTS model: the brake force does not change with the speed");
        }
    }
}

// The rails' adhesion, wheel slide and the shoes' warmth (adhesion.hpp).
void test_adhesion(const Stock& stock) {
    using simulation::RailCondition;
    check(near(simulation::wheel_rail_adhesion(0.0f), 0.3315, 0.001) && near(simulation::wheel_rail_adhesion(100.0f), 0.2131, 0.001),
          "dry adhesion: 0.33 standing, 0.21 at 100 km/h");
    check(simulation::rails_for_weather(2) == RailCondition::wet && simulation::rails_for_weather(1) == RailCondition::snow &&
              simulation::rails_for_weather(0) == RailCondition::dry,
          "the rails by the weather");
    check(simulation::wet_shoe_factor(formats::BrakeShoe::composite, 1.0f) < simulation::wet_shoe_factor(formats::BrakeShoe::cast_iron, 1.0f),
          "wet composite shoes lose more than cast iron");
    // An emergency on ice: wheels slide; they roll again by the stop.
    {
        // Away on dry rails (on ice the full throttle slips its axles and
        // the train hardly gets going), the ice for the braking.
        Run r(stock, {.rails = RailCondition::dry}, 1500.0);
        r.train.set_brake_model(simulation::BrakeModel::realistic);
        r.driver->reverser = 1;
        r.driver->throttle = r.driver->notches;
        r.run(60.0);
        r.train.set_rails(RailCondition::ice);
        r.driver->throttle = 0;
        r.driver->brake = simulation::DriverHandles::BrakeHandle::emergency;
        bool slid = false;
        // A sliding bogie's wheels stand (they are drawn by CarState::rolled).
        bool stood = true, rolled_on = false;
        for (int i = 0; i < 6000 && r.train.speed() > 0.05; ++i) {
            std::vector<std::array<double, 2>> before;
            for (const auto& car : r.train.cars()) before.push_back(car.rolled);
            r.train.step();
            for (std::size_t c = 0; c < r.train.cars().size(); ++c) {
                const auto& car = r.train.cars()[c];
                slid = slid || car.sliding();
                for (std::size_t b = 0; b < 2; ++b) {
                    const bool moved = std::abs(car.rolled[b] - before[c][b]) > 1e-9;
                    if (car.shoe_state[b].sliding && moved) stood = false;
                    if (!car.shoe_state[b].sliding && moved) rolled_on = true;
                }
            }
        }
        check(stood && rolled_on, "a sliding bogie's wheels stand, the rolling ones turn");
        r.run(2.0);
        bool still = false;
        float distance = 0.0f;
        for (const auto& car : r.train.cars()) {
            still = still || car.sliding();
            distance = (std::max)(distance, car.slid);
        }
        check(slid && distance > 1.0f, "an emergency on ice slides the wheels");
        check(!still, "standing, no wheel slides");
    }
    // Wheel slip with openRTS's limit (rails other than "as RTS"): full
    // throttle on ice slips the axles, the first of each bogie the most
    // loaded against its grip; the force falls; let go, they grip again.
    {
        Run r(stock, {.rails = RailCondition::ice}, 1500.0);
        r.driver->reverser = 1;
        r.driver->throttle = r.driver->notches;
        r.run(3.0);
        const auto& engine = r.train.cars().front();
        check(engine.axles >= 2 && engine.wheelslip != 0, "full throttle on ice slips the axles (" + std::to_string(engine.axles) + " axles, bits "
                                                          + std::to_string(engine.wheelslip) + ")");
        if (engine.axles >= 4) {
            const bool flipped = engine.flipped;
            const float lead = std::abs(engine.slip_ratio[flipped ? 1 : 0]), trail = std::abs(engine.slip_ratio[flipped ? 0 : 1]);
            check(lead > trail, "the bogie's first axle the nearest its grip (" + std::to_string(lead) + " against " + std::to_string(trail) + ")");
        }
        Run dry(stock, {.rails = RailCondition::dry}, 1500.0);
        dry.driver->reverser = 1;
        dry.driver->throttle = dry.driver->notches;
        dry.run(3.0);
        check(std::abs(dry.train.cars().front().force) > std::abs(r.train.cars().front().force) * 1.5f, "on ice the slipping engine pulls far less");
        r.driver->throttle = 0;
        r.run(1.0);
        check(r.train.cars().front().wheelslip == 0, "the throttle off, the wheels grip again");
        // As RTS: its own limit, the axles' shares not given.
        Run rts(stock, {.rails = RailCondition::rts}, 1500.0);
        rts.driver->reverser = 1;
        rts.driver->throttle = rts.driver->notches;
        rts.run(3.0);
        check(rts.train.cars().front().axles == 0, "with the rails as RTS, RTS's limit");
    }
    // Dry rails, the same emergency: no slide.
    {
        Run r(stock, {.rails = RailCondition::dry}, 1500.0);
        r.train.set_brake_model(simulation::BrakeModel::realistic);
        r.driver->reverser = 1;
        r.driver->throttle = r.driver->notches;
        r.run(60.0);
        r.driver->throttle = 0;
        r.driver->brake = simulation::DriverHandles::BrakeHandle::emergency;
        bool slid = false;
        for (int i = 0; i < 6000 && r.train.speed() > 0.05; ++i) {
            r.train.step();
            for (const auto& car : r.train.cars()) slid = slid || car.sliding();
        }
        check(!slid, "an emergency on dry rails slides nothing");
    }
    // Rain wets the shoes; braking warms and dries them.
    {
        // Standing in the rain, then away for a minute (the track is 3 km).
        Run r(stock, {.rails = RailCondition::wet}, 1200.0);
        r.train.set_brake_model(simulation::BrakeModel::realistic);
        r.run(240.0);
        r.driver->reverser = 1;
        r.driver->throttle = r.driver->notches;
        // Away on dry rails (wet ones slip the full throttle's axles); the
        // shoes dry over minutes, a minute leaves them soaked.
        r.train.set_rails(RailCondition::dry);
        r.run(60.0);
        r.train.set_rails(RailCondition::wet);
        const float soaked = r.train.cars()[3].shoe_state[0].wet;
        check(soaked > 0.8f, "rain soaks the shoes while they do not brake");
        r.driver->throttle = 0;
        r.driver->brake = simulation::DriverHandles::BrakeHandle::service;
        r.run(4.0);
        r.driver->brake = simulation::DriverHandles::BrakeHandle::lap_feed;
        float hottest = 0.0f;
        for (int i = 0; i < 400 && r.train.speed() > 1.0; ++i) {
            r.train.step();
            hottest = (std::max)(hottest, r.train.cars()[3].shoe_state[0].heat);
        }
        std::cerr << "  shoes: soaked " << soaked << ", hottest +" << hottest << " K, then wet " << r.train.cars()[3].shoe_state[0].wet
                  << ", speed " << r.train.speed() * 3.6 << " km/h, cylinder " << r.train.cars()[3].cylinder << '\n';
        // Composite shoes: the wheel takes nearly all the heat (a few K);
        // their faces dry all the same.
        check(hottest > 2.0f && hottest < 30.0f, "braking warms the composite shoes a little");
        check(r.train.cars()[3].shoe_state[0].wet < soaked - 0.2f, "warm shoes dry");
    }
    // The brake force limited to real stock's: less at the engine, none at a wagon below it.
    {
        Run plain(stock, {.rails = RailCondition::dry}, 1500.0);
        Run limited(stock, {.rails = RailCondition::dry, .limit_brake_force = true}, 1500.0);
        for (auto* r : {&plain, &limited}) {
            r->train.set_brake_model(simulation::BrakeModel::realistic);
            r->driver->independent = 3.0f;
            r->run(10.0);
        }
        const float engine_plain = plain.train.cars().front().brake_force;
        const float engine_limited = limited.train.cars().front().brake_force;
        const float limit = simulation::shoe_force_limit(formats::BrakeShoe::cast_iron) * 100000.0f * 10.0f *
                            simulation::shoe_factor(formats::BrakeShoe::cast_iron, 0.0f, 3.0f / 3.79f) * 3.0f / 3.79f;
        check(engine_limited < engine_plain && near(engine_limited, limit, limit * 0.05),
              "the limit brings the engine's 100 kN down to real stock's");
        check(simulation::shoe_force_limit(formats::BrakeShoe::cast_iron) < simulation::shoe_force_limit(formats::BrakeShoe::composite),
              "cast iron's limit is the lower (its grip rises more at a stop)");
    }
    // ORTSBrakeForce: the realistic air takes it (the limit leaves it be),
    // RTS's model keeps MaxBrakeForce.
    {
        Stock own = stock;
        own.engine.brakes.realistic_brake_force = 50000.0f;
        const float share = 3.0f / own.engine.brakes.cylinder_pressure_for_max_force;
        for (const bool limit : {false, true}) {
            Run r(own, {.rails = RailCondition::dry, .limit_brake_force = limit}, 1500.0);
            r.train.set_brake_model(simulation::BrakeModel::realistic);
            r.driver->independent = 3.0f;
            r.run(10.0);
            const float expected = share * 50000.0f * simulation::shoe_factor(formats::BrakeShoe::cast_iron, 0.0f, share);
            check(near(r.train.cars().front().brake_force, expected, expected * 0.03),
                  limit ? "ORTSBrakeForce stays under the limit" : "the realistic air brakes with ORTSBrakeForce");
        }
        Run rts(own, {}, 1500.0);
        rts.driver->independent = 3.0f;
        rts.run(10.0);
        check(near(rts.train.cars().front().brake_force, share * 100000.0f, 1500.0), "RTS's model keeps MaxBrakeForce");
    }
    // RTS's load modes are in its files: an empty car's MaxBrakeForce is its
    // empty mode's full application (brake_force_pressure), not cut again
    // by the 483's 1.5 atm (a MaDaDev empty gondola: 24 kN, braked with 9).
    {
        Stock empty = stock;
        empty.wagon.mass = 23000.0f;
        empty.wagon.brakes.max_brake_force = 24000.0f;
        Run r(empty, {.rails = RailCondition::dry}, 1500.0);
        r.train.set_brake_model(simulation::BrakeModel::realistic);
        r.run(240.0);
        r.driver->brake = simulation::DriverHandles::BrakeHandle::service;
        r.run(12.0);
        r.driver->brake = simulation::DriverHandles::BrakeHandle::lap_feed;
        r.run(20.0);
        const auto& car = r.train.cars()[3];
        const float share = car.cylinder / simulation::brake_force_pressure(empty.wagon);
        const float expected = share * 24000.0f * simulation::shoe_factor(formats::BrakeShoe::composite, 0.0f, share);
        std::cerr << "  empty car: cylinder " << car.cylinder << " atm, share " << share << ", force " << car.brake_force / 1000.0f
                  << " kN (expected " << expected / 1000.0f << ")\n";
        check(car.cylinder < 1.7f && share > 0.85f, "an empty car's full service fills its empty mode's cylinder");
        check(near(car.brake_force, expected, expected * 0.03) && car.brake_force > 0.8f * 24000.0f,
              "an empty car brakes with its own file's force");
    }
    // Wooden shoes: a bogie dragged at 60 km/h with 20 kN smoulders, catches
    // fire and burns away; other shoes never burn; new ones are new.
    {
        check(formats::brake_shoe_from("Wood") == formats::BrakeShoe::wood && std::string(formats::brake_shoe_name(formats::BrakeShoe::wood)) == "wood",
              "wood by its name");
        const auto drag = [](const formats::BrakeShoe shoe, simulation::ShoeState& state, const float force, const float seconds) {
            const simulation::BogieBrake bogie{.shoe = shoe,
                                               .share = 0.5f,
                                               .force = force,
                                               .standing_force = force * 2.0f,
                                               .weight = 400000.0f,
                                               .rails = 1.0f,
                                               .wetness = 0.0f,
                                               .speed_kmh = 60.0f};
            bool smouldered = false, alight = false;
            for (int i = 0; i < static_cast<int>(seconds / 0.05f); ++i) {
                (void)simulation::step_bogie_brake(state, bogie, 0.05f);
                smouldered = smouldered || state.heat >= simulation::wood_smoulders;
                alight = alight || state.burning;
            }
            return std::pair{smouldered, alight};
        };
        simulation::ShoeState wood;
        const auto [smouldered, alight] = drag(formats::BrakeShoe::wood, wood, 20000.0f, 60.0f);
        std::cerr << "  wood: smouldered " << smouldered << ", alight " << alight << ", worn " << wood.wear << ", +" << wood.heat << " K\n";
        check(smouldered && alight && wood.wear > 0.0f, "wooden shoes dragged smoulder and catch fire");
        (void)drag(formats::BrakeShoe::wood, wood, 0.0f, 150.0f);
        check(wood.wear >= 1.0f && !wood.burning, "alight, they burn away");
        simulation::ShoeState iron;
        const auto [hot, burnt] = drag(formats::BrakeShoe::cast_iron, iron, 20000.0f, 60.0f);
        check(!burnt && iron.wear == 0.0f, "cast iron never burns");
        // A share of the heat (the wheel takes the rest) into some 100 kg of
        // iron: a minute dragged at 20 kN, 60 km/h warms them by about 100 K
        // (until 2026-11-15 all of it into 25 kJ/K: some 1200 K, held at 800).
        std::cerr << "  cast iron dragged a minute: +" << iron.heat << " K\n";
        check(iron.heat > 50.0f && iron.heat < 200.0f, "cast iron dragged a minute warms by about 100 K");
        Run r(stock, {}, 1500.0);
        r.train.set_brake_shoes(3, {formats::BrakeShoe::wood, formats::BrakeShoe::wood});
        r.train.replace_brake_shoes(3, 0);
        check(r.train.cars()[3].shoe_state[0].wear == 0.0f && !r.train.cars()[3].shoe_state[0].burning, "new shoes are new");
    }
    // RTS's rails: the traction limit stays the .eng's.
    {
        Run dry(stock, {.rails = RailCondition::rts}, 1500.0);
        Run wet(stock, {.rails = RailCondition::wet}, 1500.0);
        for (auto* r : {&dry, &wet}) {
            r->driver->reverser = 1;
            r->driver->throttle = r->driver->notches;
            r->run(10.0);
        }
        check(wet.train.speed() <= dry.train.speed(), "wet rails pull no faster than RTS's");
    }
}

void test_uncouple(const Stock& stock) {
    {
        Run r(stock, {}, 1500.0);
        const auto old_rear = *r.train.rear_end();
        const auto cars = r.train.cars().size();
        auto cut = r.train.uncouple(4, true, true);
        check(cut != nullptr && cut->cars().size() == cars - 4 && r.train.cars().size() == 4, "uncoupling splits the cars");
        check(cut && cut->rear_end()->node() == old_rear.node() && near(cut->rear_end()->offset(), old_rear.offset(), 0.01),
              "the cut keeps the old rear end");
        if (cut) {
            auto walker = *cut->front_end();
            const auto gap_to_front = *r.train.rear_end();
            check(near(walker.offset(), gap_to_front.offset(), 1.0), "the cut's front is at the train's new rear");
        }
        for (int i = 0; i < 100; ++i) {
            r.train.step();
            if (cut) cut->step();
        }
        check(r.train.cars().back().pipe > 4.5f && cut && cut->cars().front().pipe > 4.5f,
              "closed end cocks keep both parts charged");
    }
    {
        Run r(stock, {}, 1500.0);
        auto cut = r.train.uncouple(4, false, false);
        for (int i = 0; i < 100; ++i) {
            r.train.step();
            if (cut) cut->step();
        }
        // At the cut both open ends empty fast; farther cars follow slower.
        check(r.train.cars().back().pipe < 1.0f && cut && cut->cars().front().pipe < 1.0f, "open pipe ends vent both parts");
        check(cut && cut->cars().front().cylinder > 2.0f && cut->cars().back().pipe < 3.0f, "the vented cut brakes");
    }
}

// Two trains meeting: gently they couple, hard they collide, and couplers
// left open after uncoupling only push.
void test_contacts(const Stock& stock) {
    // The trains start `gap` metres apart and the contacts are resolved
    // before each step, so the impact (RTS: the overlap the step would
    // make) is known: closing speed * dt - gap.
    const auto meet = [&](const float speed, const double gap, const bool keep_open) {
        auto network = simulation::straight_test_track(3000.0);
        simulation::Train ahead(stock.cars(3));
        simulation::Train behind(stock.cars(3));
        std::string error;
        check(ahead.place(*network, 1, 1500.0, true, &error), "ahead placed: " + error);
        // 5 m behind the first train's rear end.
        const double rear = ahead.rear_end()->offset();
        check(behind.place(*network, 1, rear - gap, true, &error), "behind placed: " + error);
        if (keep_open) {
            auto cut = behind.uncouple(3, true, true); // open coupler only at the cut, not at the front
            (void)cut;
        }
        behind.add_velocity(speed);
        std::vector<simulation::ContactResult> results;
        std::array<simulation::Train*, 2> trains{&ahead, &behind};
        for (int i = 0; i < 200 && results.empty(); ++i) {
            results = simulation::resolve_contacts(trains);
            ahead.step();
            behind.step();
        }
        return std::tuple{results, ahead.cars().size(), behind.cars().size(), ahead.speed(), behind.speed()};
    };
    {
        const auto [results, ahead_cars, behind_cars, ahead_speed, behind_speed] = meet(1.0f, 5.0, false);
        check(!results.empty() && results.front().outcome == simulation::ContactOutcome::coupled, "a gentle meeting couples");
        check(ahead_cars == 8 && behind_cars == 0, "the coupled train has every car");
    }
    {
        const auto [results, ahead_cars, behind_cars, ahead_speed, behind_speed] = meet(8.0f, 0.05, false);
        check(!results.empty() && results.front().outcome == simulation::ContactOutcome::collision, "a hard meeting collides");
        check(ahead_cars == 4 && behind_cars == 4 && ahead_speed > 1.0 && near(ahead_speed, behind_speed, 0.5),
              "a collision passes the momentum on");
    }
    {
        // The front train's rear coupler was left open by an uncoupling: a
        // gentle meeting only pushes.
        auto network = simulation::straight_test_track(3000.0);
        simulation::Train ahead(stock.cars(4));
        check(ahead.place(*network, 1, 1500.0, true), "ahead placed");
        auto cut = ahead.uncouple(4, true, true);
        cut.reset();
        simulation::Train behind(stock.cars(3));
        check(behind.place(*network, 1, ahead.rear_end()->offset() - 0.02, true), "behind placed");
        behind.add_velocity(1.0f);
        std::array<simulation::Train*, 2> trains{&ahead, &behind};
        const auto results = simulation::resolve_contacts(trains);
        check(!results.empty() && results.front().outcome == simulation::ContactOutcome::buffers,
              "an open coupler does not couple");
        check(ahead.cars().size() == 4 && behind.cars().size() == 4 && ahead.speed() > 0.3, "the trains push each other");
    }
}

// Cars as entities, step 1 (docs/car-architecture.md): a train refers to its
// cars by an ordered list of CarIds that moves with the cars: a cut in the
// middle, the coupling back, a coupling with a train standing tail first and
// a turn keep every car's id with its car, in order, its direction with it.
void test_car_ids(const Stock& stock) {
    using simulation::CarId;
    simulation::CarRegistry registry;
    // Cars with ids; the third flipped (its direction is the car's own).
    // `count` cars: the engine and its wagons.
    const auto made = [&](const std::size_t count, const std::size_t flipped = SIZE_MAX) {
        auto setups = stock.cars(count - 1);
        for (std::size_t i = 0; i < setups.size(); ++i) {
            setups[i].id = registry.create();
            setups[i].flipped = i == flipped;
        }
        return setups;
    };
    const auto flips = [](const simulation::Train& train) {
        std::vector<bool> out;
        for (const auto& car : train.cars()) out.push_back(car.flipped);
        return out;
    };
    const auto ids_of = [](const std::vector<simulation::CarSetup>& setups) {
        std::vector<CarId> out;
        for (const auto& setup : setups) out.push_back(setup.id);
        return out;
    };
    {
        const auto setups = made(5, 2);
        simulation::Train train(setups);
        const auto ids = ids_of(setups);
        check(train.car_ids() == ids, "ids: the train's cars by id, in the setup's order");
        check(train.index_of(ids[3]) == std::optional<std::size_t>{3}, "ids: a car found by its id");
        check(!train.index_of(CarId{}) && !train.index_of(registry.create()), "ids: no id, another car's: not in the train");
        check(flips(train) == std::vector<bool>{false, false, true, false, false}, "ids: the flipped car");
    }
    // Uncoupling in the middle and coupling back.
    {
        auto network = simulation::straight_test_track(3000.0);
        const auto setups = made(5, 2);
        const auto ids = ids_of(setups);
        simulation::Train train(setups);
        check(train.place(*network, 1, 1500.0, true), "ids: placed");
        const auto before = flips(train);
        auto cut = train.uncouple(3, true, false);
        check(cut != nullptr, "ids: cut before car 3");
        if (cut) {
            check(train.car_ids() == std::vector<CarId>(ids.begin(), ids.begin() + 3) &&
                      cut->car_ids() == std::vector<CarId>(ids.begin() + 3, ids.end()),
                  "ids: a cut in the middle: each part its cars' ids, in order");
            check(flips(train) == std::vector<bool>(before.begin(), before.begin() + 3), "ids: the cut: the flipped car still flipped");
            const double gap = train.rear_end()->offset() - cut->front_end()->offset();
            check(train.couple(*cut, false, true, (std::max)(gap, 0.0)), "ids: coupled back");
            check(train.car_ids() == ids && cut->car_ids().empty(), "ids: coupled back: the same ids in the same order");
            check(flips(train) == before, "ids: coupled back: each car's direction as before");
        }
    }
    // A train standing tail first: it is coupled reversed, its cars turned.
    {
        auto network = simulation::straight_test_track(3000.0);
        const auto a_setups = made(2);
        const auto b_setups = made(3, 1);
        const auto a_ids = ids_of(a_setups);
        auto b_ids = ids_of(b_setups);
        simulation::Train a(a_setups);
        simulation::Train b(b_setups);
        check(a.place(*network, 1, 1500.0, true), "ids: ahead placed");
        check(b.place(*network, 1, a.rear_end()->offset() - 5.0, true), "ids: behind placed");
        check(b.turn_around(), "ids: behind turned: tail first");
        std::reverse(b_ids.begin(), b_ids.end());
        check(b.car_ids() == b_ids, "ids: a turn reverses the order, the ids with their cars");
        check(flips(b) == std::vector<bool>{true, false, true}, "ids: a turn turns every car");
        // Its rear (its old front) faces the ahead train's rear.
        const double gap = a.rear_end()->offset() - b.rear_end()->offset();
        check(a.couple(b, false, false, (std::max)(std::abs(gap), 0.0)), "ids: coupled with the train tail first");
        auto expected = a_ids;
        expected.insert(expected.end(), b_ids.rbegin(), b_ids.rend());
        check(a.car_ids() == expected, "ids: coupled tail first: its cars reversed behind, each its id");
        // Behind ahead's cars, its cars turned back as they were placed.
        check(flips(a) == std::vector<bool>{false, false, false, true, false}, "ids: coupled tail first: each car's direction its own");
    }
}

} // namespace

// ЭПТ: every car fills at once, the pipe stays charged, I/II release.
void test_ept() {
    using Handle = simulation::DriverHandles::BrakeHandle;
    const Stock stock(true);
    check((stock.wagon.brakes.system & 2) != 0, "EP brake system parses");
    Run r(stock, {}, 1000.0);
    r.run(60.0);
    r.driver->ep_brake = true;
    r.driver->brake = Handle::ep_service;
    r.run(3.0);
    r.driver->brake = Handle::lap_feed;
    r.run(10.0);
    float least = 99.0f;
    float most = 0.0f;
    for (const auto& car : r.train.cars()) {
        least = (std::min)(least, car.cylinder);
        most = (std::max)(most, car.cylinder);
    }
    check(least > 1.0f && most - least < 0.05f, "EP application fills every car to the same pressure");
    // The reservoirs refill from the pipe and IV feeds it only weakly (the
    // ВЛ10к valve), so it sags a little; a pneumatic application to the same
    // cylinder pressure would need about 0.8 atm.
    check(r.train.cars().back().pipe > r.driver->charging_pressure - 0.4f, "the pipe stays near charged under EP braking");
    r.driver->brake = Handle::running;
    r.run(30.0);
    most = 0.0f;
    for (const auto& car : r.train.cars()) most = (std::max)(most, car.cylinder);
    check(most < 0.05f, "II releases the EP brake");
}

// openRTS's stability on the RTS model: an overcharge removed at the
// stabiliser's pace creeps the cylinders on without it (as RTS), not with it;
// a service step still brakes with it.
void test_distributor_stability(const Stock& stock) {
    using Handle = simulation::DriverHandles::BrakeHandle;
    const auto creep = [&](const bool stable) {
        simulation::TrainSettings settings;
        settings.distributor_stability = stable;
        Run r(stock, settings, 1000.0);
        r.run(240.0);
        // An overcharge of 0.6 in the УР, held until the train has it ...
        r.driver->brake = Handle::lap_feed;
        r.driver->equalising += 0.6f;
        r.run(120.0);
        // ... then taken off at 0.15 atm/min, as a stabiliser does.
        r.max_cylinder = 0.0f;
        for (int i = 0; i < static_cast<int>(240.0 / simulation::train_step); ++i) {
            r.driver->equalising -= 0.15f / 60.0f * static_cast<float>(simulation::train_step);
            r.run(simulation::train_step);
        }
        const float crept = r.max_cylinder;
        // A service step brakes all the same.
        r.driver->brake = Handle::service;
        r.run(4.0);
        r.driver->brake = Handle::lap;
        r.run(10.0);
        return std::pair{crept, r.train.cars().back().cylinder};
    };
    const auto [rts_creep, rts_step] = creep(false);
    const auto [stable_creep, stable_step] = creep(true);
    check(rts_creep > 0.2f, "RTS model: an overcharge removed slowly creeps the cylinders (" + std::to_string(rts_creep) + ")");
    check(stable_creep < 0.05f, "stability: no creep (" + std::to_string(stable_creep) + ")");
    check(stable_step > 0.3f && rts_step > 0.3f, "stability: a service step still brakes (" + std::to_string(stable_step) + ")");
}

// openRTS realistic air (docs/train.md): stability, service, release, emergency;
// a controller tuned to RTS's triple valve keeps the RTS model.
void test_realistic_air(const Stock& stock) {
    using Handle = simulation::DriverHandles::BrakeHandle;
    simulation::TrainSettings settings;
    settings.brake_model = simulation::BrakeModel::realistic;
    {
        Run r(stock, settings, 1000.0);
        check(r.train.brake_model() == simulation::BrakeModel::realistic, "the realistic model runs");
        r.run(120.0);
        check(r.max_cylinder < 0.05f, "realistic: II keeps the train released");
        check(near(r.train.cars().back().pipe, r.driver->charging_pressure, 0.2), "realistic: II holds the charging pressure");
        // Stability: the УР let down at 0.2 atm/min (below the 483's 0.3) applies nothing.
        r.driver->brake = Handle::lap;
        for (int i = 0; i < static_cast<int>(60.0 / simulation::train_step); ++i) {
            r.driver->equalising -= 0.2f / 60.0f * static_cast<float>(simulation::train_step);
            r.run(simulation::train_step);
        }
        check(r.max_cylinder < 0.05f, "realistic: a slow fall below the stability applies nothing");
        r.driver->brake = Handle::running;
        r.run(120.0);
        // Service: V 4 s, III 20 s.
        r.max_cylinder = 0.0f;
        r.driver->brake = Handle::service;
        r.run(4.0);
        r.driver->brake = Handle::lap;
        r.run(20.0);
        float least = 99.0f;
        for (const auto& car : r.train.cars()) least = (std::min)(least, car.cylinder);
        check(least > 0.3f, "realistic: a service step applies every car");
        // II: back within reach of the control chambers, the 483s release.
        r.driver->brake = Handle::running;
        r.run(240.0);
        float most = 0.0f;
        for (const auto& car : r.train.cars()) most = (std::max)(most, car.cylinder);
        check(most < 0.05f, "realistic: II releases after a service step");
    }
    {
        Run r(stock, settings, 1000.0);
        r.run(60.0);
        r.driver->brake = Handle::emergency;
        r.run(10.0);
        check(r.train.cars().back().cylinder > 2.0f, "realistic: VI applies the whole train within 10 s");
    }
    {
        // I: the main reservoir straight into the pipe through the 395's
        // wide channel, past the relay: the pipe at the engine runs ahead of
        // the УР (testers 2026-11-19: it rose with the УР).
        Run r(stock, settings, 1000.0);
        r.run(120.0);
        r.driver->brake = Handle::service;
        r.run(4.0);
        r.driver->brake = Handle::lap;
        r.run(10.0);
        r.driver->brake = Handle::charge;
        r.run(1.0);
        check(r.train.cars().front().pipe > r.driver->equalising + 0.2f,
              "realistic: I feeds the pipe at the engine ahead of the УР (" + std::to_string(r.train.cars().front().pipe) + " / " +
                  std::to_string(r.driver->equalising) + ")");
    }
    {
        // Passenger cars (305 with a 292), charged to 5.2: a first step of
        // about 0.3 brakes them (the 292 braked nothing above a pipe of 4.9
        // until 2026-11-17); I then II releases and nothing brakes again
        // while the overcharge goes (what that limit was there for).
        const Stock ep(true);
        Run r(ep, settings, 1000.0);
        r.driver->charging_pressure = 5.2f;
        r.run(400.0);
        r.driver->brake = Handle::service;
        r.run(1.2);
        r.driver->brake = Handle::lap;
        r.run(15.0);
        float least = 99.0f;
        for (const auto& car : r.train.cars())
            if (!car.definition->engine) least = (std::min)(least, car.cylinder);
        check(least > 0.3f, "realistic, 5.2: a 0.3 step brakes every car (" + std::to_string(least) + ")");
        r.driver->brake = Handle::charge;
        r.run(6.0);
        r.driver->brake = Handle::running;
        r.run(60.0);
        r.max_cylinder = 0.0f;
        r.run(240.0);
        float most = 0.0f;
        for (const auto& car : r.train.cars())
            if (!car.definition->engine) most = (std::max)(most, car.cylinder);
        check(most < 0.05f, "realistic, 5.2: I then II releases, nothing brakes again (" + std::to_string(most) + ")");
    }
    {
        // The pipe passes a change no faster than sound (2026-11-17: the
        // flow alone had no front, a 16-car train braked all at once,
        // 1366 m/s): the last car brakes later than the first by at least
        // the distance at about 600 m/s (sound, and a closed end's
        // reflection); and II releases a 60-car train's last car within a
        // minute (its tail did not release in minutes before).
        const auto network = simulation::straight_test_track(3000.0);
        const auto run_for = [](simulation::Train& train, const double seconds, const std::function<void()>& each) {
            for (int i = 0; i < static_cast<int>(seconds / simulation::train_step); ++i) {
                train.step();
                each();
            }
        };
        std::string error;
        {
            simulation::Train t(stock.cars(16), settings);
            check(t.place(*network, 1, 2000.0, true, &error), "16 cars placed: " + error);
            const auto driver = simulation::attach_keyboard_driver(t);
            run_for(t, 600.0, [] {});
            double first = -1.0, last = -1.0, time = 0.0;
            driver->brake = Handle::service;
            run_for(t, 20.0, [&] {
                time += simulation::train_step;
                if (driver->brake == Handle::service && time >= 3.0) driver->brake = Handle::lap;
                if (first < 0.0 && t.cars()[1].cylinder > 0.1f) first = time;
                if (last < 0.0 && t.cars().back().cylinder > 0.1f) last = time;
            });
            const double span = std::abs(t.cars()[1].offset - t.cars().back().offset);
            check(first > 0.0 && last > first + span / 600.0,
                  "the brake wave no faster than sound (" + std::to_string(first) + " / " + std::to_string(last) + " s over " +
                      std::to_string(span) + " m)");
        }
        simulation::Train t(stock.cars(60), settings);
        check(t.place(*network, 1, 2000.0, true, &error), "60 cars placed: " + error);
        const auto driver = simulation::attach_keyboard_driver(t);
        const auto steps = [&](const double seconds, const std::function<void()>& each) { run_for(t, seconds, each); };
        steps(900.0, [] {});
        double time = 0.0;
        driver->brake = Handle::service;
        steps(30.0, [&] {
            time += simulation::train_step;
            if (driver->brake == Handle::service && time >= 3.0) driver->brake = Handle::lap;
        });
        driver->brake = Handle::running;
        double released = -1.0;
        time = 0.0;
        steps(90.0, [&] {
            time += simulation::train_step;
            if (released < 0.0 && t.cars().back().cylinder < 0.4f) released = time;
        });
        check(released > 0.0 && released < 60.0, "II releases a 60-car train's last car within a minute (" + std::to_string(released) + " s)");
    }
    {
        // A train's own ЭПТ pace: 0.75-1.25 atm/s, drawn per train.
        const Stock ep(true);
        simulation::Train a(ep.cars(4));
        a.scatter_ep_fill(1);
        simulation::Train b(ep.cars(4));
        b.scatter_ep_fill(2);
        bool within = true;
        for (const auto* t : {&a, &b})
            for (const auto& car : t->cars()) within = within && car.ep_fill >= 0.75f && car.ep_fill <= 1.25f;
        check(within && a.cars()[1].ep_fill != b.cars()[1].ep_fill, "each train its ЭПТ pace, 0.75-1.25 atm/s");
        simulation::Train c(ep.cars(4));
        check(c.cars()[1].ep_fill == 1.0f, "1 atm/s until drawn");
    }
    {
        struct RtsValve final : simulation::LocomotiveController {
            void step(double, const simulation::EngineInputs&, simulation::EngineDemands&) override {}
            [[nodiscard]] bool rts_brakes() const override { return true; }
        };
        Run r(stock, settings, 1000.0);
        r.train.set_controller(0, std::make_unique<RtsValve>());
        check(r.train.brake_model() == simulation::BrakeModel::rts, "a legacy valve keeps the RTS model");
    }
    // The wagons' distributor in Luau (ORTSBrakeDistributor,
    // sdk/luau/vr292.luau): the train charges, holds, brakes on a service
    // step, releases and stops in emergency with it.
    {
#ifndef OPENRTS_SOURCE_DIR
#define OPENRTS_SOURCE_DIR "."
#endif
        Stock scripted = stock;
        std::ifstream in(std::string(OPENRTS_SOURCE_DIR) + "/sdk/luau/vr292.luau", std::ios::binary);
        std::stringstream source;
        source << in.rdbuf();
        scripted.wagon.brakes.distributor_script = "vr292.luau";
        scripted.wagon.brakes.distributor_source = source.str();
        check(!scripted.wagon.brakes.distributor_source.empty(), "the Luau 292 is there");
        Run r(scripted, settings, 1000.0);
        r.run(120.0);
        check(r.max_cylinder < 0.05f, "Luau ВР: II keeps the train released");
        r.driver->brake = Handle::service;
        r.run(4.0);
        r.driver->brake = Handle::lap;
        r.run(20.0);
        float least = 99.0f;
        for (std::size_t k = 1; k < r.train.cars().size(); ++k) least = (std::min)(least, r.train.cars()[k].cylinder);
        check(least > 0.3f, "Luau ВР: a service step applies every wagon (" + std::to_string(least) + ")");
        r.driver->brake = Handle::running;
        r.run(240.0);
        float most = 0.0f;
        for (const auto& car : r.train.cars()) most = (std::max)(most, car.cylinder);
        check(most < 0.05f, "Luau ВР: II releases after a service step");
        r.driver->brake = Handle::emergency;
        r.run(10.0);
        check(r.train.cars().back().cylinder > 2.0f, "Luau ВР: VI applies the last wagon within 10 s");
    }
    // The 292's plug (passenger wagons): at УВ the accelerators do not vent
    // the pipe and the cylinders fill as at Д, so in VI the tail brakes
    // later than at К, but it does.
    {
        Stock passenger = stock;
        passenger.wagon.kind = formats::WagonKind::passenger;
        const auto tail_time = [&](const simulation::DistributorPlug plug) {
            Run r(passenger, settings, 1000.0);
            for (std::size_t k = 0; k < r.train.cars().size(); ++k) {
                simulation::DistributorModes modes;
                modes.plug = plug;
                r.train.set_distributor_modes(k, modes);
            }
            r.run(120.0);
            r.driver->brake = Handle::emergency;
            for (int k = 1; k <= 600; ++k) {
                r.run(0.05);
                if (r.train.cars().back().cylinder > 1.0f) return k * 0.05f;
            }
            return 99.0f;
        };
        const float k_time = tail_time(simulation::DistributorPlug::short_train);
        const float uv_time = tail_time(simulation::DistributorPlug::no_accelerator);
        check(k_time < 10.0f && uv_time < 30.0f && uv_time > k_time,
              "УВ: VI reaches the tail later than at К (" + std::to_string(k_time) + " / " + std::to_string(uv_time) + " s)");
    }
    // A script that fails: the built-in distributor takes over.
    {
        Stock broken = stock;
        broken.wagon.brakes.distributor_script = "broken.luau";
        broken.wagon.brakes.distributor_source = "function step(c, dt)\n  error('broken')\nend\n";
        Run r(broken, settings, 1000.0);
        r.run(120.0);
        r.driver->brake = Handle::service;
        r.run(4.0);
        r.driver->brake = Handle::lap;
        r.run(20.0);
        check(r.train.cars().back().cylinder > 0.3f, "a failing script: the built-in distributor brakes the wagon");
    }
}

// What a hand at the track does (RTS's car menu, 0x463F84): brake shoes,
// the release valve, the emergency valve, parted hoses, the EP cable, and
// the train standing at a free end.
void test_car_menu(const Stock& stock) {
    using Flags = simulation::Train::CarFlags;
    using Handle = simulation::DriverHandles::BrakeHandle;
    {
        Run r(stock, {}, 1000.0);
        r.run(1.0);
        const float before = r.train.cars()[3].brake_force;
        r.train.set_car_flags(3, Flags::chocks, simulation::chock_front | simulation::chock_rear, true);
        r.run(0.1);
        check(near(r.train.cars()[3].brake_force - before, 2.0 * simulation::chock_force, 1.0), "two brake shoes add 20 kN");
        r.train.set_car_flags(3, Flags::chocks, simulation::chock_front, false);
        r.run(0.1);
        check(near(r.train.cars()[3].brake_force - before, simulation::chock_force, 1.0), "a removed shoe takes its 10 kN away");
    }
    {
        Run r(stock, {}, 1000.0);
        r.run(60.0);
        r.driver->brake = Handle::service;
        r.run(3.0);
        r.driver->brake = Handle::lap;
        r.run(10.0);
        const float applied = r.train.cars()[5].cylinder;
        r.train.set_car_flags(5, Flags::valves, simulation::valve_release, true);
        r.run(4.0);
        check(applied > 0.5f && r.train.cars()[5].cylinder < 0.05f, "the release valve empties the cylinder");
        check(r.train.cars()[6].cylinder > applied - 0.2f, "the next car keeps its brake");
    }
    {
        Run r(stock, {}, 1000.0);
        r.run(60.0);
        r.train.set_car_flags(5, Flags::valves, simulation::valve_emergency, true);
        r.run(10.0);
        // The valve in II still feeds the pipe; the open valve holds it low.
        check(r.train.cars()[5].pipe < 1.0f && r.train.cars().back().cylinder > 2.0f, "the emergency valve vents and brakes the train");
    }
    {
        Run r(stock, {}, 1000.0);
        r.run(60.0);
        r.train.set_car_flags(2, Flags::pipe_ends, simulation::pipe_hose_rear, true);
        r.train.set_car_flags(3, Flags::pipe_ends, simulation::pipe_hose_front, true);
        r.run(10.0);
        check(r.train.cars()[3].pipe < 1.0f && r.train.cars()[3].cylinder > 2.0f, "parted hoses with open cocks vent and brake");
    }
    {
        Run r(stock, {}, 1000.0);
        r.run(60.0);
        r.train.set_car_flags(2, Flags::pipe_ends, simulation::pipe_cock_rear | simulation::pipe_hose_rear, true);
        r.train.set_car_flags(3, Flags::pipe_ends, simulation::pipe_cock_front | simulation::pipe_hose_front, true);
        r.run(10.0);
        check(r.train.cars()[3].pipe > 4.5f && r.train.cars()[3].cylinder < 0.1f, "closed cocks first: the hoses part quietly");
    }
    {
        const Stock ep(true);
        Run r(ep, {}, 1000.0);
        r.run(60.0);
        r.train.set_car_flags(3, Flags::pipe_ends, simulation::ep_cable_rear, true);
        r.train.set_car_flags(4, Flags::pipe_ends, simulation::ep_cable_front, true);
        r.driver->ep_brake = true;
        r.driver->brake = Handle::ep_service;
        r.run(3.0);
        r.driver->brake = Handle::lap_feed;
        r.run(5.0);
        check(r.train.cars()[2].cylinder > 1.0f && r.train.cars()[7].cylinder < 0.3f, "the EP demand stops at a parted EP cable");
    }
    {
        Run r(stock, {}, 1500.0);
        auto cut = r.train.uncouple(4, true, true);
        check(cut != nullptr, "uncoupled for the contact");
        if (cut) {
            std::vector<simulation::Train*> trains{&r.train, cut.get()};
            const auto contact = simulation::end_contact(r.train, false, trains, 5.0);
            check(contact && contact->other == cut.get() && contact->other_front && contact->gap < 0.6,
                  "the cut stands at the train's rear end");
            check(!simulation::end_contact(r.train, true, trains, 5.0), "nothing at the train's front");
        }
    }
}

// A collision breaks the end couplers; broken ends only push (RTS +14 0x10).
// Sanding raises adhesion by Adheasion's third value and runs out.
void test_broken_and_sand(const Stock& stock) {
    const auto network = simulation::straight_test_track(3000.0);
    simulation::Train a(stock.cars(0));
    simulation::Train b(stock.cars(0));
    std::string error;
    check(a.place(*network, 1, 1000.0, true, &error) && b.place(*network, 1, 970.0, true, &error), "two engines placed: " + error);
    a.set_car_flags(0, simulation::Train::CarFlags::coupler_open, simulation::coupler_broken_rear, true);
    a.add_velocity(-0.5f);
    simulation::Train* list[] = {&a, &b};
    bool coupled = false;
    for (int i = 0; i < 400 && !coupled; ++i) {
        for (const auto& r : simulation::resolve_contacts(list)) coupled = coupled || r.outcome == simulation::ContactOutcome::coupled;
        a.step();
        b.step();
    }
    check(!coupled, "a broken coupler does not couple");

    // Sand: the keyboard driver's limit and the car's adhesion.
    simulation::Train t(stock.cars(0));
    check(t.place(*network, 1, 500.0, true, &error), "placed: " + error);
    const auto driver = simulation::attach_keyboard_driver(t);
    driver->reverser = 1;
    driver->throttle = driver->notches;
    t.step();
    const float plain = std::abs(t.cars()[0].force);
    driver->sanding = true;
    t.step();
    const float sanded = std::abs(t.cars()[0].force);
    check(t.cars()[0].sanding && sanded > plain, "sand raises the force the adhesion allows: " + std::to_string(plain) + " -> " + std::to_string(sanded));
}

// Wheel slip under traction (боксование, RTS 0x424488): the locomotive's
// force against mass x 10 x Adheasion[1], x Adheasion[0] after a slip; by
// motor when the .eng names their axles.
void test_wheel_slip(const Stock& stock) {
    struct Puller final : simulation::LocomotiveController {
        float force = 0.0f;
        std::array<float, 8> motors{};
        std::uint8_t count = 0;
        void step(double, const simulation::EngineInputs&, simulation::EngineDemands& d) override {
            d.force = force;
            d.engine_forces = motors;
            d.engine_force_count = count;
        }
    };
    const auto network = simulation::straight_test_track(3000.0);
    std::string error;
    {
        // 100 t, Adheasion ( 0.2 0.3 2 0 ): 300 kN, 60 kN once slipping.
        simulation::Train t(stock.cars(0));
        check(t.place(*network, 1, 500.0, true, &error), "placed: " + error);
        auto made = std::make_unique<Puller>();
        auto* p = made.get();
        t.set_controller(0, std::move(made));
        p->force = 350000.0f;
        t.step();
        check(t.cars()[0].wheelslip == 15 && t.cars()[0].force == 0.0f, "over the adhesion it slips (Wheelslip 15) and pulls nothing");
        p->force = 250000.0f;
        t.step();
        check(t.cars()[0].wheelslip == 15, "slipping, the adhesion x Adheasion[0]: below the first limit it slips on");
        p->force = 50000.0f;
        t.step();
        check(t.cars()[0].wheelslip == 0 && std::abs(t.cars()[0].force) > 40000.0f, "the force down far enough, the wheels grip again");
        p->force = 250000.0f;
        t.step();
        check(t.cars()[0].wheelslip == 0 && std::abs(t.cars()[0].force) > 240000.0f, "gripping, it holds up to the adhesion");
    }
    {
        // Two motors of two axles each (EngineAxles): 75 kN an axle.
        Stock motors = stock;
        motors.engine.engine->num_engines = 2;
        motors.engine.engine->num_axles = 4;
        motors.engine.engine->engine_axles = {{0, 1}, {2, 3}};
        simulation::Train t(motors.cars(0));
        check(t.place(*network, 1, 500.0, true, &error), "placed: " + error);
        auto made = std::make_unique<Puller>();
        auto* p = made.get();
        t.set_controller(0, std::move(made));
        p->force = 200000.0f;
        p->motors = {170000.0f, 30000.0f};
        p->count = 2;
        t.step();
        const auto& car = t.cars()[0];
        check(car.wheelslip == 0b0011u, "the first motor's axles slip, the second's grip");
        check(std::abs(std::abs(car.force) - (200000.0f - 0.75f * 170000.0f)) < 1.0f, "the slipping motor pulls a quarter");
        check(car.slip_ratio[0] > 1.1f && car.slip_ratio[2] == 0.0f, "its axles' force over their adhesion, the wheels' spin");
        p->motors = {0.0f, 0.0f};
        p->force = 900000.0f;
        t.step();
        check(t.cars()[0].wheelslip == 0, "motors with no force of their own: no slip (RTS)");
    }
    {
        // The RTS air: a brake far over the adhesion stands the wheels.
        Stock heavy = stock;
        heavy.wagon.brakes.max_brake_force = 2.0e6f;
        Run r(heavy, {}, 1500.0);
        r.driver->reverser = 1;
        r.driver->throttle = r.driver->notches;
        r.run(60.0);
        r.driver->throttle = 0;
        r.driver->brake = simulation::DriverHandles::BrakeHandle::emergency;
        bool locked = false, stood = true;
        for (int i = 0; i < 2000 && r.train.speed() > 1.0; ++i) {
            const auto before = r.train.cars()[5].rolled;
            r.train.step();
            const auto& car = r.train.cars()[5];
            if (!car.brake_locked) continue;
            locked = true;
            if (car.rolled[0] != before[0] || car.rolled[1] != before[1]) stood = false;
        }
        check(locked && stood, "the RTS air: braked over the adhesion, the wheels stand");
    }
}

// Wheel flats (ползуны): a slide leaves them; each turn after, each of the
// bogie's axles strikes the rail once; new wheel sets end it.
void test_wheel_flats(const Stock& stock) {
    using simulation::RailCondition;
    Run r(stock, {.rails = RailCondition::dry}, 1500.0);
    r.train.set_brake_model(simulation::BrakeModel::realistic);
    r.driver->reverser = 1;
    r.driver->throttle = r.driver->notches;
    r.run(60.0);
    r.train.set_rails(RailCondition::ice);
    r.driver->throttle = 0;
    r.driver->brake = simulation::DriverHandles::BrakeHandle::emergency;
    for (int i = 0; i < 6000 && r.train.speed() > 0.05; ++i) r.train.step();
    std::size_t car = SIZE_MAX, bogie = 0;
    float deepest = 0.0f;
    for (std::size_t k = 0; k < r.train.cars().size(); ++k) {
        for (std::size_t b = 0; b < 2; ++b) {
            if (r.train.cars()[k].flat[b] > deepest) {
                deepest = r.train.cars()[k].flat[b];
                car = k;
                bogie = b;
            }
        }
    }
    // About 1 mm in 15 m slid: the slide here is short.
    check(car != SIZE_MAX && deepest > 0.05f && deepest <= simulation::flat_deepest, "a slide on ice leaves a flat: " + std::to_string(deepest) + " mm");
    if (car == SIZE_MAX) return;
    r.train.set_wheel_flat(car, bogie, 1.5f);
    // Away again: count the knocks of that bogie over the run.
    r.driver->brake = simulation::DriverHandles::BrakeHandle::running;
    r.run(120.0);
    r.driver->throttle = r.driver->notches;
    const double from = r.train.cars()[car].rolled[bogie];
    std::size_t knocks = 0;
    for (int i = 0; i < 1200; ++i) {
        r.train.step();
        for (const auto& e : r.train.take_events()) {
            if (e.kind == simulation::TrainEventKind::wheel_flat && e.car == car && e.bogie == bogie) ++knocks;
        }
    }
    const double turns = std::abs(r.train.cars()[car].rolled[bogie] - from) /
                         (2.0 * 3.14159265358979 * (stock.wagon.wheel_radius > 0.05f ? stock.wagon.wheel_radius : 0.5));
    check(turns > 5.0 && std::abs(static_cast<double>(knocks) - 2.0 * turns) <= 2.5,
          "each axle strikes once a turn: " + std::to_string(knocks) + " knocks in " + std::to_string(turns) + " turns");
    r.train.replace_wheelsets(car, bogie);
    knocks = 0;
    for (int i = 0; i < 200; ++i) {
        r.train.step();
        for (const auto& e : r.train.take_events()) {
            if (e.kind == simulation::TrainEventKind::wheel_flat && e.car == car && e.bogie == bogie) ++knocks;
        }
    }
    check(knocks == 0 && r.train.cars()[car].flat[bogie] == 0.0f, "new wheel sets: no flat, no knock");
}

void test_engine_keys() {
    const auto engine = formats::parse_wagon(engine_text.substr(0, engine_text.size() - 1) + R"(
        MaxSandingTime ( 600 )
)
EngExtended (
    ExcludePantograph ( 2 )
    AlwaysUsePanto ( 4 )
    LowerPantoOnStop ( 1 )
    RaiseSecondPantoWhenSpeedBelow ( 6 )
    MaxLineCurrent ( 2000 )
    MaxLineCurrentVelocityIncrease ( 10 500 )
    MaxLineCurrentCatenaryBurnTime ( 12100 )
    ORTSTrainPlugin ( "cab/x.dll" )
))");
    check(engine.ok() && engine.engine, "engine with EngExtended parses");
    if (!engine.engine) return;
    const auto& e = *engine.engine;
    check(e.exclude_pantographs == 2 && e.always_use_panto == 4 && e.lower_panto_on_stop == 1, "pantograph masks");
    check(near(e.raise_second_panto_below, 6.0, 1e-6), "RaiseSecondPantoWhenSpeedBelow");
    check(e.line_current_cooling[0] == 10.0f && e.line_current_cooling[1] == 500.0f, "MaxLineCurrentVelocityIncrease");
    check(e.train_plugin == "cab/x.dll", "ORTSTrainPlugin");
    check(near(e.max_sanding_time, 600.0, 1e-6), "MaxSandingTime");
    check(near(engine.pantograph_raise_time, 3.0, 1e-6), "PantographRaiseTime defaults to 3 s");
    {
        // The compressor governor from the .eng (psi, or with a unit).
        const auto emu = formats::parse_wagon(engine_text.substr(0, engine_text.size() - 1) +
                                              "\n    AirBrakesMainMaxAirPressure ( 116 )\n    AirBrakesCompressorRestartPressure ( 6.5bar )\n)");
        check(emu.engine && near(emu.engine->main_reservoir_max, 7.90, 0.01) && near(emu.engine->compressor_restart, 6.42, 0.01),
              "AirBrakesMainMaxAirPressure in psi, AirBrakesCompressorRestartPressure in bar");
        const auto range = simulation::compressor_range(emu.engine ? &*emu.engine : nullptr);
        check(near(range.on, 6.42, 0.01) && near(range.off, 7.90, 0.01), "the governor by the .eng");
        const auto none = simulation::compressor_range(engine.engine ? &*engine.engine : nullptr);
        check(none.on == 7.5f && none.off == 9.0f, "no governor given: 7.5 / 9.0");
        simulation::Train train({{.definition = &emu}});
        check(near(train.cars()[0].main_reservoir, 7.90, 0.01), "a new engine's main reservoir at its cut-out");
        simulation::EngineInputs in;
        in.definition = &emu;
        bool running = false;
        in.main_reservoir = 7.0f;
        check(simulation::compressor_rate(running, in) == 0.0f, "between the cut-in and the cut-out a stopped compressor stays off");
        in.main_reservoir = 6.3f;
        check(simulation::compressor_rate(running, in) > 0.0f && running, "below the cut-in it starts");
        in.main_reservoir = 7.5f;
        check(simulation::compressor_rate(running, in) > 0.0f, "and runs on to the cut-out");
        in.main_reservoir = 7.95f;
        check(simulation::compressor_rate(running, in) == 0.0f && !running, "at the cut-out it stops");
    }

    const auto consist = formats::parse_consist(R"(SIMISA@@@@@@@@@@JINX0D0t______
Train ( TrainCfg ( "c"
    Wagon ( UiD ( 1 ) WagonData ( car folder )
        FreightAnim ( FreightAnimShape ( "coal.s" 0 ) FreightAnimPosition ( 0 1.5 0 ) FreightAnimMass ( 60t ) FreightAnimCargoType ( Coal ) )
        PlaceDrag ( 2 ) )
) )
)");
    check(consist.ok() && consist.entries.size() == 1 && consist.entries[0].freight.size() == 1, "FreightAnim in a consist");
    check(consist.ok() && consist.entries.size() == 1 && consist.entries[0].place_drag == 2, "PlaceDrag in a consist: a chock at the car's rear");
    {
        const auto own = formats::parse_wagon("Wagon ( w\n Type ( Freight )\n Mass ( 80t )\n MaxBrakeForce ( 100kN )\n ORTSBrakeForce ( 74kN )\n"
                                              " TripleValveRatio ( 2.5 )\n BrakeCylinderPressureForMaxBrakeBrakeForce ( 55 )\n)");
        check(near(own.brakes.realistic_brake_force, 74000.0, 1.0) && near(own.brakes.max_brake_force, 100000.0, 1.0),
              "ORTSBrakeForce beside MaxBrakeForce");
    }
    {
        // ORTSBrakeDistributor: a built-in name, or a Luau file.
        const auto named = formats::parse_wagon("Wagon ( w\n Type ( Carriage )\n ORTSBrakeDistributor ( \"305-001\" )\n)");
        check(named.brakes.distributor == formats::BrakeDistributor::evr305_001 && named.brakes.distributor_script.empty(),
              "ORTSBrakeDistributor ( 305-001 ): built in");
        check(simulation::distributor_kind(named) == simulation::DistributorKind::evr305_001 &&
                  simulation::inferred_distributor_kind(named) == simulation::DistributorKind::vr292,
              "the named one wins over the inferred 292");
        const auto freight = formats::parse_wagon("Wagon ( w\n Type ( Freight )\n ORTSBrakeDistributor ( VR292 )\n)");
        check(simulation::distributor_kind(freight) == simulation::DistributorKind::vr292, "a freight car given a 292");
        const auto scripted = formats::parse_wagon("Wagon ( w\n ORTSBrakeDistributor ( \"my.luau\" )\n)");
        check(scripted.brakes.distributor == formats::BrakeDistributor::inferred && scripted.brakes.distributor_script == "my.luau",
              "ORTSBrakeDistributor ( my.luau ): a script");
        // The car menu's companion text: set, changed, taken out.
        auto text = formats::set_override_distributor("", formats::BrakeDistributor::vr483);
        check(text.find("ORTSBrakeDistributor ( \"483\" )") != std::string::npos && text.find("Wagon (") != std::string::npos,
              "a companion written");
        text = formats::set_override_distributor("Comment ( \"mine\" )\r\n" + text, formats::BrakeDistributor::evr305_001);
        check(text.find("\"305-001\"") != std::string::npos && text.find("\"483\"") == std::string::npos && text.find("mine") != std::string::npos,
              "a companion changed, the rest kept");
        check(formats::set_override_distributor(text, formats::BrakeDistributor::inferred).empty(), "taken out: no file left");
        const auto kept = formats::set_override_distributor("Wagon (\r\n\tORTSBrakeShoeType ( composite )\r\n)\r\n", formats::BrakeDistributor::inferred);
        check(kept.find("ORTSBrakeShoeType") != std::string::npos, "taken out: another key kept");
        // load_wagon: the companion (a mod's) over the file.
        const auto work = std::filesystem::temp_directory_path() / "openrts-train-test-companion";
        std::filesystem::remove_all(work);
        std::filesystem::create_directories(work / "TRAINS" / "TRAINSET" / "f");
        std::ofstream(work / "TRAINS" / "TRAINSET" / "f" / "w.wag", std::ios::binary)
            << "SIMISA@@@@@@@@@@JINX0D0t______\r\nWagon ( w\r\n Type ( Carriage )\r\n Size ( 3m 4m 24m )\r\n Mass ( 50t )\r\n"
               " TripleValveRatio ( 2.5 )\r\n ORTSBrakeDistributor ( \"my.luau\" )\r\n)\r\n";
        std::ofstream(work / "TRAINS" / "TRAINSET" / "f" / "w.wag.openrts", std::ios::binary)
            << formats::set_override_distributor("", formats::BrakeDistributor::vr483);
        core::Vfs vfs;
        vfs.mount_directory(work);
        vfs.build_index();
        const auto loaded = formats::load_wagon(vfs, "f", "w", false);
        check(loaded.ok() && loaded.brakes.distributor == formats::BrakeDistributor::vr483 && loaded.brakes.distributor_script.empty(),
              "load_wagon: the companion's distributor over the file's script (" + loaded.error + ")");
        std::filesystem::remove_all(work);
    }
    {
        // The stock rules (packs/stock): an ЭД4М's car gets the 305-001 and
        // the governor 6.5-8.0 (multiple_units.luau); a ВЛ80 the 483 and with
        // it the 418, a 2ТЭ10УТ (passenger) nothing (freight_locomotives.luau);
        // a companion's distributor and 418 win; a broken script is said and
        // left out.
        const auto work = std::filesystem::temp_directory_path() / "openrts-train-test-rules";
        std::filesystem::remove_all(work);
        const auto set = work / "TRAINS" / "TRAINSET";
        std::filesystem::create_directories(set / "mdd_ED4M-0001");
        std::filesystem::create_directories(set / "vl80");
        std::filesystem::create_directories(set / "mdd_VL10-0001");
        std::filesystem::create_directories(set / "RTS_2TE10Ut-0001");
        std::filesystem::create_directories(work / "TRAINS" / "OPENRTS" / "STOCK");
        auto eng = engine_text.substr(0, engine_text.size() - 1) +
                   "\n    AirBrakesMainMaxAirPressure ( 130 )\n    AirBrakesCompressorRestartPressure ( 105 )\n)";
        const std::string plain = "Wagon ( loco\n";
        eng.replace(eng.find(plain), plain.size(), "Wagon ( loco\n BrakeEquipmentType ( \"Triple_valve, Auxilary_reservoir, EP_brake\" )\n");
        std::ofstream(set / "mdd_ED4M-0001" / "mdd_ED4M-0001_G.eng", std::ios::binary) << eng;
        std::ofstream(set / "mdd_ED4M-0001" / "mdd_ED4M-0001_M.eng", std::ios::binary) << eng;
        std::ofstream(set / "mdd_ED4M-0001" / "mdd_ED4M-0001_M.eng.openrts", std::ios::binary)
            << formats::set_override_distributor("", formats::BrakeDistributor::vr292);
        std::ofstream(set / "vl80" / "vl80.eng", std::ios::binary) << eng;
        std::ofstream(set / "mdd_VL10-0001" / "mdd_VL10-0001A.eng", std::ios::binary) << eng;
        std::ofstream(set / "mdd_VL10-0001" / "mdd_VL10-0001A.eng.openrts", std::ios::binary)
            << formats::set_override_sensor418("", false);
        std::ofstream(set / "RTS_2TE10Ut-0001" / "RTS_2te10ut-0001a.eng", std::ios::binary) << eng;
        for (const char* script : {"multiple_units.luau", "freight_locomotives.luau"}) {
            std::filesystem::copy_file(std::filesystem::path(OPENRTS_SOURCE_DIR) / "packs" / "stock" / script,
                                       work / "TRAINS" / "OPENRTS" / "STOCK" / script);
        }
        std::ofstream(work / "TRAINS" / "OPENRTS" / "STOCK" / "zz_broken.luau", std::ios::binary) << "function car(c) error('сломано') end\n";
        core::Vfs vfs;
        vfs.mount_directory(work);
        vfs.build_index();
        std::string messages;
        const auto rules = simulation::StockRules::load(vfs, messages);
        check(rules->scripts() == 3 && messages.empty(), "three stock rules scripts load (" + messages + ")");
        const auto emu = formats::load_wagon(vfs, "mdd_ED4M-0001", "mdd_ED4M-0001_G", true, rules->hook());
        check(emu.ok() && emu.brakes.distributor == formats::BrakeDistributor::evr305_001 &&
                  emu.brakes.default_distributor == formats::BrakeDistributor::evr305_001 && !emu.brakes.distributor_in_companion,
              "the rules: an ЭД4М's car with the ЭПТ gets the 305-001 (" + emu.error + ")");
        check(emu.engine && near(emu.engine->compressor_restart, 6.5, 1e-4) && near(emu.engine->main_reservoir_max, 8.0, 1e-4),
              "the rules: an ЭД4М's governor 6.5-8.0 over the file's 130 / 105 psi");
        check(emu.engine && near(emu.engine->compressor_capacity, 0.37, 1e-4), "the rules: an ЭД4М's compressor 0.37 m³/min");
        check(!simulation::sensor418_fitted(emu), "an ЭД4М's car (its 305) has no 418");
        check(rules->take_messages().find("zz_broken.luau") != std::string::npos, "a broken script is said");
        const auto chosen = formats::load_wagon(vfs, "mdd_ED4M-0001", "mdd_ED4M-0001_M", true, rules->hook());
        check(chosen.brakes.distributor == formats::BrakeDistributor::vr292 && chosen.brakes.distributor_in_companion &&
                  chosen.brakes.default_distributor == formats::BrakeDistributor::evr305_001,
              "a companion's distributor wins over the rules");
        const auto loco = formats::load_wagon(vfs, "vl80", "vl80", true, rules->hook());
        check(loco.brakes.distributor == formats::BrakeDistributor::vr483 && loco.brakes.default_distributor == formats::BrakeDistributor::vr483 &&
                  loco.engine && near(loco.engine->main_reservoir_max, 8.85, 0.01) && loco.engine->compressor_capacity == 0.0f,
              "the rules: a ВЛ80 (EP flag and all) gets the 483, its governor as its file gives it");
        check(simulation::sensor418_fitted(loco), "a ВЛ80 with the 483 has the 418");
        const auto vl10 = formats::load_wagon(vfs, "mdd_VL10-0001", "mdd_VL10-0001A", true, rules->hook());
        check(vl10.brakes.distributor == formats::BrakeDistributor::vr483 && vl10.brakes.sensor418 == std::optional<bool>(false) &&
                  !simulation::sensor418_fitted(vl10),
              "a ВЛ10 with the 483 and ORTSBrakeSensor418 ( 0 ) in its companion: no 418 (" + vl10.error + ")");
        const auto te10ut = formats::load_wagon(vfs, "RTS_2TE10Ut-0001", "RTS_2te10ut-0001a", true, rules->hook());
        check(te10ut.brakes.distributor == formats::BrakeDistributor::inferred && !simulation::sensor418_fitted(te10ut),
              "the rules leave a 2ТЭ10УТ (passenger) as its file gives it, no 418");
        check(formats::set_override_sensor418(formats::set_override_sensor418("", false), std::nullopt).empty(),
              "ORTSBrakeSensor418 taken out again: no companion left");
        std::filesystem::remove_all(work);
    }
    const auto shod = formats::parse_consist(R"(SIMISA@@@@@@@@@@JINX0D0t______
Train ( TrainCfg ( "c"
    Wagon ( UiD ( 1 ) WagonData ( car folder ) ORTSBrakeShoes ( Composite cast_iron ) )
    Wagon ( UiD ( 2 ) WagonData ( car folder ) ORTSBrakeShoes ( disc ) )
    Wagon ( UiD ( 3 ) WagonData ( car folder ) )
) )
)");
    check(shod.ok() && shod.entries.size() == 3 && shod.entries[0].shoes[0] == formats::BrakeShoe::composite &&
              shod.entries[0].shoes[1] == formats::BrakeShoe::cast_iron,
          "ORTSBrakeShoes: the front and the rear bogie");
    check(shod.ok() && shod.entries.size() == 3 && shod.entries[1].shoes[0] == formats::BrakeShoe::disc &&
              shod.entries[1].shoes[1] == formats::BrakeShoe::disc,
          "ORTSBrakeShoes: one value for both bogies");
    check(shod.ok() && shod.entries.size() == 3 && shod.entries[2].shoes[0] == formats::BrakeShoe::unknown, "no ORTSBrakeShoes: the car's own");
    if (consist.ok() && !consist.entries.empty() && !consist.entries[0].freight.empty()) {
        const auto& f = consist.entries[0].freight[0];
        check(f.shape == "coal.s" && near(f.position[1], 1.5, 1e-6) && near(f.mass, 60000.0, 1.0) && (f.cargo_type == "coal" || f.cargo_type == "Coal"),
              "FreightAnim shape, position, mass, cargo type");
    }
}

// I by the train's length (testers 2026-11-19): the pipe at the engine
// runs ahead of the УР, and the longer the train the slower it rises.
void test_charge_by_length(const Stock& stock) {
    using Handle = simulation::DriverHandles::BrakeHandle;
    simulation::TrainSettings settings;
    settings.brake_model = simulation::BrakeModel::realistic;
    float rise[3]{};
    const std::size_t lengths[3] = {0, 10, 60};
    for (int k = 0; k < 3; ++k) {
        const auto wagons = lengths[k];
        Run r(stock, settings, 2500.0, wagons);
        r.run(wagons > 20 ? 600.0 : 200.0);
        r.driver->brake = Handle::service;
        r.run(4.0);
        r.driver->brake = Handle::lap;
        r.run(10.0);
        const float before = r.train.cars().front().pipe;
        r.driver->brake = Handle::charge;
        r.run(2.0);
        const float head = r.train.cars().front().pipe;
        rise[k] = head - before;
        check(head > r.driver->equalising + 0.1f,
              "realistic, " + std::to_string(wagons) + " wagons: in I the pipe at the engine runs ahead of the УР (" +
                  std::to_string(head) + " / " + std::to_string(r.driver->equalising) + ")");
        if (wagons == 60)
            check(head < r.train.cars().front().main_reservoir - 1.0f,
                  "realistic, 60 wagons: in I the pipe does not leap to the main reservoir (" + std::to_string(head) + ")");
    }
    check(rise[0] > rise[1] && rise[1] > rise[2], "realistic: in I the longer the train the slower the pipe rises");
}

int main() {
    test_units();
    const Stock stock;
    test_charge_by_length(stock);
    test_parse(stock);
    test_brakes(stock);
    test_buffer_stop(stock);
    test_turn_around(stock);
    test_sway(stock);
    test_body_settles(stock);
    test_travel_after_stop(stock);
    test_save_restore(stock, simulation::BrakeModel::rts);
    test_save_restore(stock, simulation::BrakeModel::realistic);
    test_rail_joints(stock);
    test_slide_off(stock);
    test_overturn(stock);
    test_hit(stock);
    test_joint_forces(stock);
    test_brake_shoes(stock);
    test_adhesion(stock);
    test_uncouple(stock);
    test_contacts(stock);
    test_car_ids(stock);
    test_ept();
    test_realistic_air(stock);
    test_brake_condition(stock);
    test_distributor_stability(stock);
    test_car_menu(stock);
    test_broken_and_sand(stock);
    test_wheel_slip(stock);
    test_wheel_flats(stock);
    test_engine_keys();
    if (failures == 0) std::cout << "train tests passed\n";
    return failures == 0 ? 0 : 1;
}
