// Trains: RTS value units, wagon parsing, brakes with and without distributor
// stability, and the train ends staying a train length apart at a buffer stop.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <array>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/brake_shoes.hpp>
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

    Run(const Stock& stock, const simulation::TrainSettings settings, const double front)
        : train(stock.cars(10), settings) {
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
        Run r(stock, {.rails = RailCondition::ice}, 1500.0);
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
        r.run(60.0);
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
        check(hottest > 15.0f, "braking warms the shoes");
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

int main() {
    test_units();
    const Stock stock;
    test_parse(stock);
    test_brakes(stock);
    test_buffer_stop(stock);
    test_turn_around(stock);
    test_sway(stock);
    test_rail_joints(stock);
    test_slide_off(stock);
    test_overturn(stock);
    test_hit(stock);
    test_joint_forces(stock);
    test_brake_shoes(stock);
    test_adhesion(stock);
    test_uncouple(stock);
    test_contacts(stock);
    test_ept();
    test_realistic_air(stock);
    test_distributor_stability(stock);
    test_car_menu(stock);
    test_broken_and_sand(stock);
    test_engine_keys();
    if (failures == 0) std::cout << "train tests passed\n";
    return failures == 0 ? 0 : 1;
}
