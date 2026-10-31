// Trains: RTS value units, wagon parsing, brakes with and without distributor
// stability, and the train ends staying a train length apart at a buffer stop.
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
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

int main() {
    test_units();
    const Stock stock;
    test_parse(stock);
    test_brakes(stock);
    test_buffer_stop(stock);
    test_ept();
    if (failures == 0) std::cout << "train tests passed\n";
    return failures == 0 ? 0 : 1;
}
