// Paths, the dispatcher's junction reservations, speed limits and the test
// driver (docs/paths.md) on a synthetic station with a passing loop.
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/simulation/auto_driver.hpp>
#include <openrts/simulation/dispatcher.hpp>
#include <openrts/simulation/speed_limits.hpp>
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

void test_speed_limits() {
    auto net = station();
    simulation::SpeedLimits limits(*net, 25.0f);
    check(limits.posts().size() == 1, "the limit post is on node 4");
    check(near(limits.limit_at(4, 300.0, true, false), 40.0 / 3.6, 1e-3), "eastbound beyond the post: 40 km/h");
    check(near(limits.limit_at(4, 100.0, true, false), 25.0, 1e-3), "before the post: the route's limit");
    check(near(limits.limit_at(4, 300.0, false, false), 25.0, 1e-3), "the post is not for westbound trains");
    check(near(limits.limit_at(1, 500.0, true, false), 25.0, 1e-3), "elsewhere: the route's limit");
    const auto post = formats::speed_post(net->database().items[0]);
    check(post && post->limit && !post->resume && near(post->speed, 40.0 / 3.6, 1e-4), "speed post data as RTS reads it");
}

} // namespace

int main() {
    auto net = station();
    test_routes(*net);
    test_reservations();
    test_speed_limits();
    if (failures == 0) std::cout << "path tests passed\n";
    return failures == 0 ? 0 : 1;
}
