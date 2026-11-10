// Paths, the dispatcher's junction reservations, speed limits and the test
// driver (docs/paths.md) on a synthetic station with a passing loop.
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/simulation/auto_driver.hpp>
#include <openrts/simulation/dispatcher.hpp>
#include <openrts/simulation/speed_limits.hpp>
#include <openrts/simulation/traffic.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/simulation/train_route.hpp>

namespace {

using namespace openrts;
using simulation::PathPlan;
using simulation::PlanPoint;
using simulation::PlanPointKind;
using simulation::TrackNetwork;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

bool near(const double a, const double b, const double tolerance = 1e-3) { return std::abs(a - b) <= tolerance; }

const char* sections_text = R"(SIMISA@@@@@@@@@@JINX0T0t______
TrackSections ( 1
    TrackSection ( 1 SectionSize ( 1.5 100 ) )
)
TrackShapes ( 1
    TrackShape ( 7
        FileName ( Switch.s )
        NumPaths ( 2 )
        MainRoute ( 0 )
        ClearanceDist ( 25 )
        SectionIdx ( 1 0 0 0 0 1 )
        SectionIdx ( 1 1.5 0 0.1 5 1 )
    )
)
)";

// `count` straight 100 m sections along +z from (x, z).
std::string straight(const int count, const double x, const double z) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += "1 0 0 0 0 1 1 00 0 0 " + std::to_string(x) + " 0 " + std::to_string(z + 100.0 * i) + " 0 0 0 ";
    }
    return text;
}

std::string vector_node(const int index, const int count, const double x, const double z, const std::string& pins,
                        const std::string& items = {}) {
    return "TrackNode ( " + std::to_string(index) + " TrVectorNode ( TrVectorSections ( " + std::to_string(count) + " "
        + straight(count, x, z) + ") " + items + ") TrPins ( 1 1 " + pins + ") )\n";
}

std::string junction_node(const int index, const std::string& pins, const int uid, const double z) {
    return "TrackNode ( " + std::to_string(index) + " TrJunctionNode ( 0 7 0 ) UiD ( 0 0 " + std::to_string(uid)
        + " 0 0 0 0 0 " + std::to_string(z) + " 0 0 0 ) TrPins ( 1 2 " + pins + ") )\n";
}

std::string end_node(const int index, const std::string& pin) {
    return "TrackNode ( " + std::to_string(index) + " TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 "
        + pin + ") )\n";
}

std::string pin(const int node, const int direction) {
    return "TrPin ( " + std::to_string(node) + " " + std::to_string(direction) + " ) ";
}

// end 10 - [1: 1000 m] - junction 20 (z 1000) - main [2] / loop [3] (400 m)
// - junction 21 (z 1400) - [4: 1000 m, a 40 km/h post at 200 m for
// eastbound trains] - end 11.
std::unique_ptr<TrackNetwork> station() {
    const std::string nodes =
        vector_node(1, 10, 0, 0, pin(10, 1) + pin(20, 1)) + junction_node(20, pin(1, 0) + pin(2, 1) + pin(3, 1), 55, 1000)
        + vector_node(2, 4, 0, 1000, pin(20, 0) + pin(21, 0)) + vector_node(3, 4, 5, 1000, pin(20, 0) + pin(21, 0))
        + junction_node(21, pin(4, 1) + pin(2, 0) + pin(3, 0), 56, 1400)
        + vector_node(4, 10, 0, 1400, pin(21, 1) + pin(11, 1), "TrItemRefs ( 1 TrItemRef ( 0 ) ) ")
        + end_node(10, pin(1, 1)) + end_node(11, pin(4, 0));
    const std::string items = "SpeedPostItem ( TrItemId ( 0 ) TrItemSData ( 200 00000006 ) "
                              "SpeedpostTrItemData ( 642 40 1.5708 ) )\n";
    const std::string tdb = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackDB ( Serial ( 1 ) TrackNodes ( 21\n" + nodes
        + ") TrItemTable ( 1\n" + items + ") )\n";
    auto sections = formats::parse_track_sections(sections_text);
    auto db = formats::parse_track_database(tdb);
    check(sections.ok() && db.ok(), "synthetic station parses: " + db.error);
    return std::make_unique<TrackNetwork>(std::move(db), std::move(sections));
}

PlanPoint point(const PlanPointKind kind, const std::uint32_t node, const double offset,
                std::map<std::uint32_t, std::uint32_t> branches = {}) {
    PlanPoint p;
    p.kind = kind;
    p.node = node;
    p.offset = offset;
    p.branches = std::move(branches);
    return p;
}

void test_routes(const TrackNetwork& net) {
    PathPlan plan{.name = "east", .start = {1, 100.0, true}, .points = {point(PlanPointKind::end, 4, 500.0)}};
    auto route = simulation::build_route(net, plan);
    check(route.ok(), "route east builds: " + route.error);
    if (!route.ok()) return;
    const auto& leg = route.legs.front();
    check(near(leg.length(), 900.0 + 400.0 + 500.0), "route east is 1800 m");
    check(leg.junctions.size() == 2 && leg.junctions[0].junction == 20 && leg.junctions[0].facing
              && leg.junctions[1].junction == 21 && !leg.junctions[1].facing
              && leg.junctions[0].branch == leg.junctions[1].branch,
          "facing 20, trailing 21 from the same track");
    check(leg.locate(2, 100.0) && near(*leg.locate(2, 100.0), 1000.0) == (leg.junctions[0].branch == 0),
          "a place on the way is found at its distance");

    plan.points.front().branches = {{20, 1}};
    route = simulation::build_route(net, plan);
    check(route.ok() && route.legs.front().junctions[0].branch == 1 && route.legs.front().junctions[1].branch == 1,
          "a chosen branch takes the loop");

    // Into the main track, back out to the start line.
    PathPlan shunt{.name = "shunt",
                   .start = {1, 100.0, true},
                   .points = {point(PlanPointKind::reverse, 2, 200.0, {{20, 0}}), point(PlanPointKind::end, 1, 300.0)}};
    route = simulation::build_route(net, shunt);
    check(route.ok() && route.legs.size() == 2, "a reverse point splits the route: " + route.error);
    if (route.ok() && route.legs.size() == 2) {
        check(near(route.legs[0].length(), 1100.0) && near(route.legs[1].length(), 900.0), "leg lengths");
        check(route.legs[1].junctions.size() == 1 && !route.legs[1].junctions[0].facing, "back out trailing through 20");
    }

    // Through a .pat and back: the loop (branch 1) reads back as RTS reads
    // it (the last branch that reaches the next junction); the main track
    // (branch 0) needs a point on it, which the writer adds.
    for (const std::uint32_t branch : {1u, 0u}) {
        plan.points.front().branches = {{20, branch}};
        route = simulation::build_route(net, plan);
        const auto pat = simulation::path_from_route(net, plan, route);
        const auto parsed = formats::parse_train_path(formats::write_train_path(pat));
        check(parsed.ok() && parsed.points.size() == (branch == 1 ? 4u : 5u),
              "the written .pat reads back, branch " + std::to_string(branch));
        std::string error;
        const auto again = simulation::plan_from_path(net, parsed, &error);
        check(again.has_value(), "the .pat resolves on the network: " + error);
        if (!again) continue;
        const auto rebuilt = simulation::build_route(net, *again);
        check(rebuilt.ok() && rebuilt.legs.front().junctions.size() == 2 && rebuilt.legs.front().junctions[0].branch == branch
                  && again->start.forward && near(rebuilt.legs.front().length(), route.legs.front().length(), 0.5),
              "the .pat gives the same way, branch " + std::to_string(branch));
    }
}

const std::string car_common = R"(
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
    DerailBufferForce ( 732kN )
)";

struct Stock {
    formats::WagonDefinition engine = formats::parse_wagon("Wagon ( loco\n Type ( Engine )\n Mass ( 100t )\n" + car_common
                                                           + R"()
Engine ( loco
    Type ( Electric )
    MaxPower ( 3000kW )
    MaxForce ( 300kN )
    MaxVelocity ( 100km/h )
    AirBrakesMainResVolume ( 30 )
    EngineControllers ( Throttle ( 0 1 0.1 0 NumNotches ( 10 ) ) )
))");
    formats::WagonDefinition wagon = formats::parse_wagon("Wagon ( car\n Type ( Freight )\n Mass ( 60t )\n" + car_common + ")");
    [[nodiscard]] std::vector<simulation::CarSetup> cars() const {
        return {{.definition = &engine}, {.definition = &wagon}, {.definition = &wagon}};
    }
};

void test_reservations() {
    auto net = station();
    Stock stock;
    simulation::Train a(stock.cars());
    simulation::Train b(stock.cars());
    std::string error;
    check(a.place(*net, 1, 300.0, true, &error), "train A placed: " + error);
    check(b.place(*net, 4, 300.0, false, &error), "train B placed: " + error);
    simulation::Dispatcher dispatcher(*net);
    // A: east through the loop; B: west through the main track.
    PathPlan east{.name = "A", .start = {1, 200.0, true}, .points = {point(PlanPointKind::end, 4, 500.0, {{20, 1}})}};
    PathPlan west{.name = "B", .start = {4, 400.0, false}, .points = {point(PlanPointKind::end, 1, 500.0, {{21, 0}})}};
    auto east_route = simulation::build_route(*net, east);
    auto west_route = simulation::build_route(*net, west);
    check(east_route.ok() && west_route.ok(), "both routes build");
    dispatcher.assign(a.id(), east, east_route);
    const std::vector<const simulation::Train*> trains{&a, &b};
    dispatcher.update(trains);
    const auto* j20 = net->junction(20);
    const auto* j21 = net->junction(21);
    check(j20->reservations.size() == 1 && j20->reservations.front().first == a.id() && j20->selected == 1,
          "A reserves junction 20 and sets it for the loop");
    check(j21->reservations.size() == 1 && j21->selected == 1, "A reserves junction 21 trailing from the loop");
    const auto* pa = dispatcher.progress(a.id());
    check(pa && pa->on_route && near(pa->head, 100.0, 0.5) && near(pa->tail, 40.0, 1.0), "A is found on its path");
    check(dispatcher.authority_ahead(a.id()) && near(*dispatcher.authority_ahead(a.id()), 1600.0, 1.0),
          "A's way is reserved to its end");
    check(!net->throw_junction(20, 0, b.id()), "a reserved junction cannot be thrown by hand");

    dispatcher.assign(b.id(), west, west_route);
    dispatcher.update(trains);
    dispatcher.update(trains);
    const auto* pb = dispatcher.progress(b.id());
    // A waits for B to leave the line beyond 21, and lets B have 21; B
    // waits at 20 for A to leave the line it needs.
    check(j21->reservations.front().first == b.id() && j21->selected == 0, "B takes junction 21 for the main track");
    check(j20->reservations.front().first == a.id() && j20->selected == 1, "A keeps junction 20");
    check(pa->blocked_at == 21u && pb && pb->blocked_at == 20u, "each waits at the far junction of the loop");

    // Drive both with the test driver: they cross in the loop and arrive.
    simulation::AutoDriver driver_a(simulation::attach_keyboard_driver(a));
    simulation::AutoDriver driver_b(simulation::attach_keyboard_driver(b));
    const auto orders = [&](const simulation::Train& train) {
        simulation::DriveOrders o;
        const auto* p = dispatcher.progress(train.id());
        const auto* route = dispatcher.route(train.id());
        if (!p || !route || !p->on_route) {
            o.hold = true;
            return o;
        }
        const auto& leg = route->legs[p->leg];
        o.direction = p->front_leads ? 1 : -1;
        o.stop_distance = (std::min)(p->authority, leg.length()) - p->head;
        if (p->finished) o.hold = true;
        return o;
    };
    std::vector<simulation::Train*> moving{&a, &b};
    std::size_t contacts = 0;
    double closest = 1e9;
    for (int i = 0; i < static_cast<int>(900.0 / simulation::train_step); ++i) {
        driver_a.step(simulation::train_step, a, orders(a));
        driver_b.step(simulation::train_step, b, orders(b));
        contacts += simulation::resolve_contacts(moving).size();
        a.step();
        b.step();
        dispatcher.update(trains);
        if (const auto fa = a.front_end(); fa && b.front_end()) {
            const auto pa_point = fa->pose();
            const auto pb_point = b.front_end()->pose();
            closest = (std::min)(closest, simulation::distance(pa_point, pb_point));
        }
        if (pa->finished && pb->finished) break;
    }
    std::cerr << "  crossing: A head " << pa->head << " finished " << pa->finished << ", B head " << pb->head
              << " finished " << pb->finished << ", closest fronts " << closest << " m\n";
    check(contacts == 0, "the trains never touch");
    check(pa->finished && pb->finished, "both trains reach their ends");
    check(!a.derailed() && !b.derailed(), "nobody derails");
}

// Traffic after RTS (docs/traffic.md): one body on its path, RTS's
// controller choosing the acceleration.
void test_traffic() {
    using simulation::TrafficDriver;
    using simulation::TrafficSettings;
    const auto& table = simulation::rts_traffic_limits();
    check(simulation::traffic_curve_speed(table, 7000.0) < 0.0f && simulation::traffic_curve_speed(table, 5500.0) == 75.0f
              && simulation::traffic_curve_speed(table, 250.0) == 11.0f && simulation::traffic_curve_speed(table, 150.0) == 7.0f,
          "RTS's curve table: the tightest radius it is under wins");
    check(!TrafficSettings{}.problem().empty() && TrafficSettings{.max_speed = 10.0f, .acceleration = 0.3f}.problem().empty(),
          "traffic needs a positive top speed and acceleration (RTS message 0x88)");

    // One train east through the loop to its end: up at its acceleration, at
    // its top speed, down to the 40 km/h post, standing at its end point.
    {
        auto net = station();
        Stock stock;
        simulation::Train a(stock.cars());
        std::string error;
        check(a.place(*net, 1, 300.0, true, &error), "traffic train placed: " + error);
        a.set_kinematic(true);
        simulation::Dispatcher dispatcher(*net);
        PathPlan east{.name = "A", .start = {1, 200.0, true}, .points = {point(PlanPointKind::end, 4, 500.0, {{20, 1}})}};
        auto route = simulation::build_route(*net, east);
        check(route.ok(), "the traffic route builds");
        const double end = route.legs.front().length();
        dispatcher.assign(a.id(), east, route);
        const simulation::SpeedLimits limits(*net, 0.0f);
        TrafficDriver driver(TrafficSettings{.max_speed = 20.0f, .acceleration = 0.5f, .deceleration = 0.5f});
        const std::vector<const simulation::Train*> trains{&a};
        dispatcher.update(trains);
        float wait = -1.0f;
        double top = 0.0, after_post = 0.0, first_second = 0.0;
        const double post = 1000.0 + 400.0 + 200.0 - 200.0; // the 40 km/h post on node 4, as a leg distance from the start
        for (int i = 0; i < static_cast<int>(600.0 / simulation::train_step); ++i) {
            const auto orders = simulation::traffic_orders(a, dispatcher, nullptr, limits, simulation::TrainCategory::freight,
                                                           driver.settings(), wait);
            driver.step(simulation::train_step, a, orders);
            a.step();
            dispatcher.update(trains);
            const auto* p = dispatcher.progress(a.id());
            top = (std::max)(top, a.speed());
            if (i == static_cast<int>(1.0 / simulation::train_step)) first_second = a.speed();
            if (p && p->head > post + 5.0 && p->head < end - 50.0) after_post = (std::max)(after_post, a.speed());
            if (p && p->finished) break;
        }
        const auto* p = dispatcher.progress(a.id());
        std::cerr << "  traffic run: top " << top << " m/s, after the post " << after_post << ", head " << (p ? p->head : -1.0) << " of "
                  << end << ", first second " << first_second << '\n';
        check(std::abs(first_second - 0.5) < 0.06, "it starts at its acceleration (0.5 m/s^2)");
        // The 40 km/h post 1.4 km on: RTS starts braking for it at four
        // braking distances, so it does not get to 20 m/s here.
        check(top > 15.0 && top <= 20.0 + 1e-3, "it runs up towards its top speed, no faster");
        check(after_post > 3.0 && after_post <= 40.0 / 3.6 + 0.05, "past the 40 km/h post it keeps to it");
        check(p && p->finished && a.speed() == 0.0 && p->head <= end + 0.01 && p->head > end - 2.0,
              "it stands at its end point (within 1 m short of it)");
        for (const auto& car : a.cars()) check(car.velocity == 0.0f, "its cars stand as one");
    }

    // A block point holds it short of the point; released, it goes on.
    {
        auto net = station();
        Stock stock;
        simulation::Train a(stock.cars());
        std::string error;
        check(a.place(*net, 1, 300.0, true, &error), "blocked train placed: " + error);
        a.set_kinematic(true);
        simulation::Dispatcher dispatcher(*net);
        PathPlan east{.name = "A", .start = {1, 200.0, true}, .points = {point(PlanPointKind::end, 4, 500.0, {{20, 1}})}};
        dispatcher.assign(a.id(), east, simulation::build_route(*net, east));
        dispatcher.set_blocks(a.id(), {{.key = 7, .leg = 0, .distance = 600.0}});
        const simulation::SpeedLimits limits(*net, 0.0f);
        TrafficDriver driver(TrafficSettings{.max_speed = 20.0f, .acceleration = 0.5f, .deceleration = 0.5f});
        const std::vector<const simulation::Train*> trains{&a};
        dispatcher.update(trains);
        const auto* p = dispatcher.progress(a.id());
        check(p && p->blocked_point == 7u && std::abs(p->authority - 600.0) < 1e-6, "the way ends at the block point");
        check(net->junction(20)->reservations.empty(), "no junction past the block point is reserved");
        float wait = -1.0f;
        const auto run = [&](const double seconds) {
            for (int i = 0; i < static_cast<int>(seconds / simulation::train_step); ++i) {
                driver.step(simulation::train_step, a,
                            simulation::traffic_orders(a, dispatcher, nullptr, limits, simulation::TrainCategory::freight, driver.settings(), wait));
                a.step();
                dispatcher.update(trains);
                if (p->finished) break;
            }
        };
        run(180.0);
        std::cerr << "  held at " << p->head << " (the point at 600)\n";
        check(a.speed() == 0.0 && p->head < 600.0 - simulation::traffic_stop_margin + 0.5 && p->head > 600.0 - simulation::traffic_stop_margin - 5.0,
              "it stands 10 m short of the block point");
        dispatcher.enable_block(7, false);
        run(300.0);
        check(p->finished, "released, it runs to its end");

        // Enabled again while the way runs past it: a train within its
        // distance of no return goes on, one farther is held.
        simulation::Dispatcher d2(*net);
        simulation::Train b(stock.cars());
        check(b.place(*net, 1, 300.0, true, &error), "train B placed: " + error);
        const std::vector<const simulation::Train*> only_b{&b};
        d2.assign(b.id(), east, simulation::build_route(*net, east));
        d2.set_blocks(b.id(), {{.key = 9, .leg = 0, .distance = 600.0}});
        d2.enable_block(9, false);
        d2.update(only_b);
        check(d2.progress(b.id())->authority > 1000.0, "its way runs past the released point");
        d2.enable_block(9, true, 1000.0);
        d2.update(only_b);
        check(d2.progress(b.id())->authority > 1000.0, "enabled within its distance of no return: it goes on");
        d2.enable_block(9, false);
        d2.update(only_b);
        simulation::Dispatcher d3(*net);
        simulation::Train c(stock.cars());
        b.vacate();
        check(c.place(*net, 1, 300.0, true, &error), "train C placed: " + error);
        const std::vector<const simulation::Train*> only_c{&c};
        d3.assign(c.id(), east, simulation::build_route(*net, east));
        d3.set_blocks(c.id(), {{.key = 9, .leg = 0, .distance = 600.0}});
        d3.enable_block(9, false);
        d3.update(only_c);
        d3.enable_block(9, true, 100.0);
        d3.update(only_c);
        check(std::abs(d3.progress(c.id())->authority - 600.0) < 1e-6, "enabled farther than its distance of no return: held");
    }

    // Two traffic trains cross in the loop and arrive.
    {
        auto net = station();
        Stock stock;
        simulation::Train a(stock.cars());
        simulation::Train b(stock.cars());
        std::string error;
        check(a.place(*net, 1, 300.0, true, &error) && b.place(*net, 4, 300.0, false, &error), "crossing trains placed: " + error);
        a.set_kinematic(true);
        b.set_kinematic(true);
        simulation::Dispatcher dispatcher(*net);
        PathPlan east{.name = "A", .start = {1, 200.0, true}, .points = {point(PlanPointKind::end, 4, 500.0, {{20, 1}})}};
        PathPlan west{.name = "B", .start = {4, 400.0, false}, .points = {point(PlanPointKind::end, 1, 500.0, {{21, 0}})}};
        dispatcher.assign(a.id(), east, simulation::build_route(*net, east));
        dispatcher.assign(b.id(), west, simulation::build_route(*net, west));
        const simulation::SpeedLimits limits(*net, 0.0f);
        TrafficDriver da(TrafficSettings{.max_speed = 20.0f, .acceleration = 0.5f, .deceleration = 0.6f});
        TrafficDriver db(TrafficSettings{.max_speed = 15.0f, .acceleration = 0.4f, .deceleration = 0.5f});
        const std::vector<const simulation::Train*> trains{&a, &b};
        std::vector<simulation::Train*> moving{&a, &b};
        dispatcher.update(trains);
        float wa = -1.0f, wb = -1.0f;
        std::size_t contacts = 0;
        const auto* pa = dispatcher.progress(a.id());
        const auto* pb = dispatcher.progress(b.id());
        for (int i = 0; i < static_cast<int>(900.0 / simulation::train_step); ++i) {
            da.step(simulation::train_step, a,
                    simulation::traffic_orders(a, dispatcher, nullptr, limits, simulation::TrainCategory::freight, da.settings(), wa, moving));
            db.step(simulation::train_step, b,
                    simulation::traffic_orders(b, dispatcher, nullptr, limits, simulation::TrainCategory::freight, db.settings(), wb, moving));
            contacts += simulation::resolve_contacts(moving).size();
            a.step();
            b.step();
            dispatcher.update(trains);
            if (pa->finished && pb->finished) break;
        }
        std::cerr << "  traffic crossing: A " << pa->head << " finished " << pa->finished << ", B " << pb->head << " finished " << pb->finished << '\n';
        check(contacts == 0, "the traffic trains never touch");
        check(pa->finished && pb->finished, "both traffic trains reach their ends");
    }
}

// A STATIC consist (RTS train type 0) is not simulated: given a push it
// stays where it is; a train running into it moves it (the owner then wakes
// it: RTS turns it LOOSE).
void test_static() {
    auto net = station();
    Stock stock;
    simulation::Train standing(stock.cars());
    simulation::Train moving(stock.cars());
    std::string error;
    check(standing.place(*net, 4, 600.0, true, &error), "standing placed");
    check(moving.place(*net, 4, 520.0, true, &error), "moving placed behind it");
    standing.set_asleep(true);
    const auto before = standing.front_end()->offset();
    standing.add_velocity(5.0f);
    standing.set_asleep(true);
    for (int i = 0; i < 100; ++i) standing.step();
    check(std::abs(standing.front_end()->offset() - before) < 1e-6, "a STATIC train does not move by itself");
    moving.add_velocity(3.0f);
    std::vector<simulation::Train*> both{&moving, &standing};
    std::optional<simulation::ContactOutcome> outcome;
    for (int i = 0; i < 1200 && !outcome; ++i) {
        for (const auto& result : simulation::resolve_contacts(both)) outcome = result.outcome;
        moving.step();
        standing.step();
    }
    // Couplers meeting gently couple (the moving train takes the cars);
    // otherwise the push moves the static one.
    check(outcome && (*outcome == simulation::ContactOutcome::coupled ? standing.cars().empty() : std::abs(standing.speed()) > 0.01),
          "a train running into it couples it or moves it (to wake it by)");
}

// Track control's preview lays the cars back from the front of car 0 by the
// train's own car offsets (TrainView::consist_layout); a train put there
// must stand exactly on the preview.
void test_layout_preview() {
    auto net = station();
    Stock stock;
    auto short_car = stock.wagon;
    short_car.length = 12.0f;
    std::vector<simulation::CarSetup> setups{{.definition = &stock.engine}, {.definition = &short_car}, {.definition = &stock.wagon}};
    simulation::Train train(setups);
    std::string error;
    check(train.place(*net, 4, 500.0, true, &error), "train placed for the preview check: " + error);
    const auto& cars = train.cars();
    const double head = cars.front().offset + cars.front().definition->length * 0.5;
    const auto placed = train.placements();
    double last_rear = 0.0;
    for (std::size_t i = 0; i < cars.size(); ++i) {
        const double length = cars[i].definition->length;
        const double front = head - (cars[i].offset + length * 0.5);
        simulation::TrackTraveller centre(*net, 4, 500.0, true);
        check(centre.move(-(front + length * 0.5)).ok(), "preview car centre on the track");
        const auto at = centre.pose();
        const auto& car = placed[i];
        const double x = (car.front.tile_x * 2048.0 + car.front.x + car.rear.tile_x * 2048.0 + car.rear.x) * 0.5;
        const double z = (car.front.tile_z * 2048.0 + car.front.z + car.rear.tile_z * 2048.0 + car.rear.z) * 0.5;
        check(std::hypot(at.tile_x * 2048.0 + at.x - x, at.tile_z * 2048.0 + at.z - z) < 0.05,
              "preview car " + std::to_string(i) + " is where the train puts it");
        last_rear = front + length;
    }
    check(std::abs(last_rear - train.length()) < 1e-6, "the preview is as long as the train");
}

void test_speed_limits() {
    auto net = station();
    simulation::SpeedLimits limits(*net, 25.0f);
    check(limits.posts().size() == 1, "the limit post is on node 4");
    check(near(limits.limit_at(4, 300.0, true, simulation::TrainCategory::passenger), 40.0 / 3.6, 1e-3), "eastbound beyond the post: 40 km/h");
    check(near(limits.limit_at(4, 100.0, true, simulation::TrainCategory::passenger), 25.0, 1e-3), "before the post: the route's limit");
    check(near(limits.limit_at(4, 300.0, false, simulation::TrainCategory::passenger), 25.0, 1e-3), "the post is not for westbound trains");
    check(near(limits.limit_at(1, 500.0, true, simulation::TrainCategory::passenger), 25.0, 1e-3), "elsewhere: the route's limit");
    const auto post = formats::speed_post(net->database().items[0]);
    check(post && post->limit && !post->resume && near(post->speed, 40.0 / 3.6, 1e-4), "speed post data as RTS reads it");

    // Which posts a train obeys by its category (rts1.exe 0x47F7B4).
    using C = simulation::TrainCategory;
    const auto obeys = [](const std::uint32_t flags, const C category) {
        simulation::SpeedPostOnTrack p;
        p.post.flags = flags;
        return simulation::SpeedLimits::applies(p, true, category);
    };
    check(obeys(0x20, C::other) && obeys(0x40, C::other) && obeys(0x60, C::other), "\"Прочий\" obeys every post");
    check(obeys(0x20, C::passenger) && !obeys(0x40, C::passenger) && obeys(0x80, C::passenger) && obeys(0, C::passenger),
          "a passenger train: passenger posts and those for all");
    check(!obeys(0x20, C::freight) && obeys(0x40, C::freight) && obeys(0x80, C::freight), "a freight train: freight posts and those for all");
    check(!obeys(0x60, C::passenger) && !obeys(0x60, C::freight), "a post marked both passenger and freight: only \"Прочий\" (as RTS)");
}

} // namespace

int main() {
    auto net = station();
    test_routes(*net);
    test_reservations();
    test_traffic();
    test_speed_limits();
    test_static();
    test_layout_preview();
    if (failures == 0) std::cout << "path tests passed\n";
    return failures == 0 ? 0 : 1;
}
