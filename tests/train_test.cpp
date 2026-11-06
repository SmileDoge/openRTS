// Trains: RTS value units, wagon parsing, brakes with and without distributor
// stability, and the train ends staying a train length apart at a buffer stop.
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <array>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
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
        FreightAnim ( FreightAnimShape ( "coal.s" 0 ) FreightAnimPosition ( 0 1.5 0 ) FreightAnimMass ( 60t ) FreightAnimCargoType ( Coal ) ) )
) )
)");
    check(consist.ok() && consist.entries.size() == 1 && consist.entries[0].freight.size() == 1, "FreightAnim in a consist");
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
    test_slide_off(stock);
    test_overturn(stock);
    test_hit(stock);
    test_uncouple(stock);
    test_contacts(stock);
    test_ept();
    test_realistic_air(stock);
    test_car_menu(stock);
    test_broken_and_sand(stock);
    test_engine_keys();
    if (failures == 0) std::cout << "train tests passed\n";
    return failures == 0 ? 0 : 1;
}
