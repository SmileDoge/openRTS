#include <cmath>
#include <iostream>
#include <numbers>
#include <string>

#include <openrts/formats/shape_animation.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/simulation/track_geometry.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>

namespace {

using namespace openrts;
using simulation::JunctionMode;
using simulation::LookAheadStop;
using simulation::MoveStop;
using simulation::TrackNetwork;
using simulation::TrackTraveller;

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-3) {
    return std::abs(a - b) <= tolerance;
}

const char* sections_text = R"(SIMISA@@@@@@@@@@JINX0T0t______
_INFO ( header )
TrackSections ( 4
    _Skip ( an example: TrackSection ( 99 SectionSize ( 1 1 ) ) )
    TrackSection ( 1 SectionSize ( 1.5 100 ) )
    TrackSection ( 2 SectionSize ( 1.5 0 ) SectionCurve ( 100 90 ) )
    TrackSection ( 3 SectionSize ( 1.5 0 ) SectionCurve ( 100 -90 ) )
)
TrackShapes ( 2
    TrackShape ( 7
        FileName ( Switch.s )
        NumPaths ( 2 )
        MainRoute ( 0 )
        ClearanceDist ( 25 )
        SectionIdx ( 1 0 0 0 0 1 )
        SectionIdx ( 1 1.5 0 0.1 5 2 )
    )
)
)";

// Placement of a section at (x, z) on tile 0,0 with a heading.
std::string section(const int index, const double x, const double z, const double heading = 0.0) {
    return std::to_string(index) + " 0 0 0 0 1 1 00 0 0 " + std::to_string(x) + " 0 " + std::to_string(z) + " 0 "
        + std::to_string(heading) + " 0 ";
}

std::string vector_node(const int index, const std::string& sections, const int count, const std::string& pins,
                        const std::string& items = {}) {
    return "TrackNode ( " + std::to_string(index) + " TrVectorNode ( TrVectorSections ( " + std::to_string(count) + " "
        + sections + ") " + items + ") TrPins ( 1 1 " + pins + ") )\n";
}

std::string junction_node(const int index, const std::string& pins, const int uid = 0) {
    return "TrackNode ( " + std::to_string(index) + " TrJunctionNode ( 0 7 0 ) UiD ( 0 0 " + std::to_string(uid)
        + " 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 2 " + pins + ") )\n";
}

std::string end_node(const int index, const std::string& pin) {
    return "TrackNode ( " + std::to_string(index) + " TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 "
        + pin + ") )\n";
}

std::string pin(const int node, const int direction) {
    return "TrPin ( " + std::to_string(node) + " " + std::to_string(direction) + " ) ";
}

std::string database(const std::string& nodes, const std::string& items, const int node_count, const int item_count) {
    return "SIMISA@@@@@@@@@@JINX0T0t______\nTrackDB ( Serial ( 1 ) TrackNodes ( " + std::to_string(node_count) + "\n"
        + nodes + ") TrItemTable ( " + std::to_string(item_count) + "\n" + items + ") )\n";
}

std::unique_ptr<TrackNetwork> network(const std::string& tdb) {
    auto sections = formats::parse_track_sections(sections_text);
    auto db = formats::parse_track_database(tdb);
    check(sections.ok() && db.ok(), "synthetic files parse");
    return std::make_unique<TrackNetwork>(std::move(db), std::move(sections));
}

void test_formats() {
    const auto library = formats::parse_track_sections(sections_text);
    check(library.ok() && library.sections.size() == 3, "sections parsed, _Skip ignored");
    const auto* curve = library.section(2);
    check(curve && curve->curved && near(curve->length, 100.0 * std::numbers::pi / 2.0), "arc length from radius and angle");
    const auto* shape = library.shape(7);
    check(shape && shape->file_name == "Switch.s" && shape->paths.size() == 2 && shape->main_route == 0u
              && near(shape->clearance_distance, 25.0) && shape->paths[1].sections.size() == 1
              && near(shape->paths[1].start[0], 1.5),
          "track shape parsed");

    const auto db = formats::parse_track_database(database(
        vector_node(1, section(1, 0, 0), 1, pin(2, 1) + pin(3, 1), "TrItemRefs ( 1 TrItemRef ( 0 ) ) ")
            + end_node(2, pin(1, 1)) + end_node(3, pin(1, 0)),
        "SignalItem ( TrItemId ( 0 ) TrItemSData ( 40 00000002 ) TrItemRData ( 1 2 3 0 0 ) "
        "TrSignalType ( 00000000 1 0.5 T_Head ) TrSignalDirs ( 1 TrSignalDir ( 5 1 0 0 ) ) )\n",
        3, 1));
    check(db.ok() && db.node(1) && db.node(1)->sections.size() == 1 && db.node(1)->pins.size() == 2
              && db.node(1)->in_pin_count == 1 && db.node(1)->out_pin_count == 1 && db.node(1)->items.size() == 1,
          "vector node parsed");
    check(db.items.size() == 1 && db.items[0].kind == formats::TrackItemKind::signal && near(db.items[0].distance, 40.0)
              && db.items[0].name == "T_Head" && db.items[0].signal_directions.size() == 1,
          "signal item parsed");
}

void test_geometry() {
    const auto library = formats::parse_track_sections(sections_text);
    formats::TrackVectorSection placement{.x = 10.0f, .z = 20.0f, .heading = 0.0f};

    const auto straight = simulation::evaluate_section(placement, *library.section(1), 50.0);
    check(near(straight.x, 10.0) && near(straight.z, 70.0), "straight goes along +z at heading 0");

    // A 90 degree right arc of radius 100: ends 100 m ahead and 100 m right
    // (+x in MSTS axes), heading +90 degrees.
    const auto right = simulation::evaluate_section(placement, *library.section(2), library.section(2)->length);
    check(near(right.x, 110.0) && near(right.z, 120.0) && near(right.heading, std::numbers::pi / 2.0), "right arc");
    const auto left = simulation::evaluate_section(placement, *library.section(3), library.section(3)->length);
    check(near(left.x, -90.0) && near(left.z, 120.0) && near(left.heading, -std::numbers::pi / 2.0), "left arc");

    placement.heading = static_cast<float>(std::numbers::pi / 2.0);
    const auto east = simulation::evaluate_section(placement, *library.section(1), 100.0);
    check(near(east.x, 110.0) && near(east.z, 20.0), "heading +90 degrees points along +x");

    placement.heading = 0.0f;
    placement.pitch = 0.1f;
    const auto slope = simulation::evaluate_section(placement, *library.section(1), 100.0);
    check(near(slope.y, -100.0 * std::sin(0.1)), "pitch lowers the end as in RTS");
}

// Line: end(10) - [1: 100 m] - junction 20 trunk; branch 0 -> [2] -> end 11;
// branch 1 -> [3] -> end 12.
std::string switch_layout() {
    return database(vector_node(1, section(1, 0, 0), 1, pin(10, 1) + pin(20, 1))
                        + junction_node(20, pin(1, 0) + pin(2, 1) + pin(3, 1), 55)
                        + vector_node(2, section(1, 0, 100), 1, pin(20, 0) + pin(11, 1))
                        + vector_node(3, section(1, 1.5, 100), 1, pin(20, 0) + pin(12, 1))
                        + end_node(10, pin(1, 1)) + end_node(11, pin(2, 0)) + end_node(12, pin(3, 0)),
                    "", 20, 0);
}

void test_junctions() {
    auto net = network(switch_layout());
    auto* state = net->junction(20);
    check(state && state->selected == 0 && state->main_route == 0, "junction starts at MainRoute");
    check(net->junction_for_object(0, 0, 55) == 20u, "junction found by its world object");

    // Facing move takes the selected branch.
    TrackTraveller facing(*net, 1, 90.0, true);
    auto result = facing.move(20.0, JunctionMode::occupy, 1);
    check(result.ok() && facing.node() == 2 && near(facing.offset(), 10.0) && result.junctions.size() == 1
              && result.junctions[0].facing && state->occupied,
          "facing occupy enters the selected branch and occupies");
    check(!net->throw_junction(20, 1), "an occupied junction cannot be thrown");

    // The tail passing facing keeps it for ClearanceDist.
    TrackTraveller tail(*net, 1, 95.0, true);
    tail.move(10.0, JunctionMode::release, 1);
    check(state->clearance.contains(1) && near(state->clearance[1], 25.0), "facing release starts clearance");

    // Trailing from branch 1 runs through and throws the switch.
    state->occupied = false;
    TrackTraveller trailing(*net, 3, 5.0, false);
    result = trailing.move(10.0, JunctionMode::occupy, 2);
    check(result.ok() && trailing.node() == 1 && !trailing.forward() && near(trailing.offset(), 95.0)
              && state->selected == 1 && !result.junctions[0].facing && result.junctions[0].branch == 1,
          "trailing occupy pushes the switch over");
    TrackTraveller trailing_tail(*net, 3, 5.0, false);
    trailing_tail.move(10.0, JunctionMode::release, 2);
    check(!state->occupied && state->selected == 0, "trailing release returns to MainRoute");

    // Probe stops at a trailing switch set against the route.
    TrackTraveller probe(*net, 3, 5.0, false);
    result = probe.move(10.0, JunctionMode::probe);
    check(result.stop == MoveStop::junction_obstructed && probe.node() == 3, "probe stops at a switch set against it");

    // A junction reserved by someone else is reported as forced.
    state->reservations.push_back({7, 0});
    TrackTraveller forced(*net, 1, 90.0, true);
    result = forced.move(20.0, JunctionMode::occupy, 1);
    check(result.junctions.size() == 1 && result.junctions[0].forced, "forcing a reserved junction is reported");

    // End of track.
    TrackTraveller end(*net, 2, 50.0, true);
    result = end.move(500.0);
    check(result.stop == MoveStop::end_of_track && near(result.moved, 50.0) && end.node() == 2, "end node stops a move");

    // Moving backwards keeps the facing direction.
    TrackTraveller back(*net, 2, 10.0, true);
    result = back.move(-30.0);
    check(result.ok() && back.node() == 1 && back.forward() && near(back.offset(), 80.0), "backward move across a junction");
}

void test_loops() {
    // Ring A-B-C-A of vector nodes linked end to start, a signal on B facing
    // the "wrong" way only.
    const auto ring = database(
        vector_node(1, section(1, 0, 0), 1, pin(3, 0) + pin(2, 1))
            + vector_node(2, section(1, 0, 100), 1, pin(1, 0) + pin(3, 1), "TrItemRefs ( 1 TrItemRef ( 0 ) ) ")
            + vector_node(3, section(1, 0, 200), 1, pin(2, 0) + pin(1, 1)),
        "SignalItem ( TrItemId ( 0 ) TrItemSData ( 50 00000002 ) TrSignalType ( 00000000 1 0 Reverse ) )\n", 3, 1);
    auto net = network(ring);
    const auto signals_forward = [](const formats::TrackItem& item) { return item.name == "Forward"; };
    auto ahead = simulation::look_ahead(*net, 1, 10.0, true, 1e9, signals_forward);
    check(ahead.stop == LookAheadStop::loop && ahead.items.empty() && near(ahead.searched, 300.0, 1.0),
          "ring without a matching signal ends after one lap");
    ahead = simulation::look_ahead(*net, 1, 10.0, true, 1e9);
    check(ahead.items.size() == 1 && near(ahead.items[0].distance, 140.0), "ring item found once");
    ahead = simulation::look_ahead(*net, 1, 10.0, true, 120.0);
    check(ahead.stop == LookAheadStop::distance && ahead.items.empty(), "distance limit checked on every node");

    // A traveller may lap the ring any number of times.
    TrackTraveller lap(*net, 1, 0.0, true);
    const auto laps = lap.move(3010.0);
    check(laps.ok() && lap.node() == 1 && near(lap.offset(), 10.0, 1e-6) && near(laps.moved, 3010.0), "ten laps of the ring");

    // Oval through a junction: trunk <- C, branch 0 -> A, branch 1 -> siding.
    const auto oval = database(
        vector_node(1, section(1, 0, 0), 1, pin(20, 0) + pin(2, 1))
            + vector_node(2, section(1, 0, 100), 1, pin(1, 0) + pin(3, 1))
            + vector_node(3, section(1, 0, 200), 1, pin(2, 0) + pin(20, 1))
            + junction_node(20, pin(3, 0) + pin(1, 1) + pin(4, 1))
            + vector_node(4, section(1, 5, 0), 1, pin(20, 0) + pin(10, 1)) + end_node(10, pin(4, 0)),
        "", 20, 0);
    net = network(oval);
    ahead = simulation::look_ahead(*net, 2, 0.0, true, 1e9, [](const auto&) { return false; });
    check(ahead.stop == LookAheadStop::loop, "loop through a junction ends after one lap");

    // Balloon: branch 0 and branch 1 of one junction are the two ends of one
    // node. Running round reverses the train; look-ahead stops at the switch.
    const auto balloon = database(
        vector_node(1, section(1, 0, 0), 1, pin(10, 1) + pin(20, 1))
            + junction_node(20, pin(1, 0) + pin(2, 1) + pin(2, 0))
            + vector_node(2, section(1, 0, 100), 1, pin(20, 0) + pin(20, 0)) + end_node(10, pin(1, 1)),
        "", 20, 0);
    net = network(balloon);
    TrackTraveller round(*net, 1, 50.0, true);
    const auto turn = round.move(200.0, JunctionMode::occupy, 1);
    check(turn.ok() && round.node() == 1 && !round.forward() && near(round.offset(), 50.0), "balloon turns the train");
    check(net->junction(20)->selected == 1, "balloon exit pushed the switch over");
    net->junction(20)->occupied = false;
    net->junction(20)->selected = 0;
    ahead = simulation::look_ahead(*net, 1, 50.0, true, 1e9);
    check(ahead.stop == LookAheadStop::junction_obstructed, "balloon look-ahead stops at the switch");

    // Wye: three junctions; every look-ahead ends.
    const auto wye = database(
        junction_node(20, pin(4, 0) + pin(1, 1) + pin(3, 0)) + junction_node(21, pin(5, 0) + pin(2, 1) + pin(1, 0))
            + junction_node(22, pin(6, 0) + pin(3, 1) + pin(2, 0))
            + vector_node(1, section(1, 0, 0), 1, pin(20, 0) + pin(21, 0))
            + vector_node(2, section(1, 0, 0), 1, pin(21, 0) + pin(22, 0))
            + vector_node(3, section(1, 0, 0), 1, pin(22, 0) + pin(20, 0))
            + vector_node(4, section(1, 0, 0), 1, pin(10, 1) + pin(20, 1))
            + vector_node(5, section(1, 0, 0), 1, pin(11, 1) + pin(21, 1))
            + vector_node(6, section(1, 0, 0), 1, pin(12, 1) + pin(22, 1)) + end_node(10, pin(4, 1))
            + end_node(11, pin(5, 1)) + end_node(12, pin(6, 1)),
        "", 22, 0);
    net = network(wye);
    bool all_end = true;
    for (std::uint32_t node = 1; node <= 6; ++node) {
        for (const bool forward : {true, false}) {
            for (std::uint32_t a = 0; a < 2; ++a) {
                for (std::uint32_t b = 0; b < 2; ++b) {
                    net->junction(20)->selected = a;
                    net->junction(21)->selected = b;
                    net->junction(22)->selected = a ^ b;
                    const auto walk = simulation::look_ahead(*net, node, 0.0, forward, 1e9);
                    all_end = all_end && walk.stop != LookAheadStop::distance;
                }
            }
        }
    }
    check(all_end, "wye look-aheads terminate for every switch setting");
}

// Switch blade: a node named like its matrix (case-insensitive binding is
// done by the parser), rotated 0.9 degrees about Y at frame 1, back at 2.
void test_animation() {
    formats::ShapeModel model;
    formats::ShapeMatrix identity;
    identity.values[0] = identity.values[4] = identity.values[8] = 1.0f;
    model.matrices = {identity, identity};
    model.matrices[1].name = "POINT_LEFT";
    model.matrices[1].values[9] = -0.793f;
    model.matrices[1].values[11] = 10.996f;
    model.hierarchy = {-1, 0};
    const float s = 0.0078f;
    const float c = std::sqrt(1.0f - s * s);
    formats::ShapeAnimationNode node{.name = "POINT_LEFT", .matrix_index = 1};
    node.rotations = {{0, {0, 0, 0, 1}}, {1, {0, s, 0, c}}, {2, {0, 0, 0, 1}}};
    model.animations.push_back({.frame_count = 2, .frame_rate = 30, .nodes = {node}});

    const auto rest = formats::animate_matrices(model, 0, 0.0f);
    check(near(rest[1].values[0], 1.0) && near(rest[1].values[9], -0.793), "frame 0 keeps the rest pose and position");

    // A blade point 8.6 m behind the pivot (local z = -8.6) at frame 1: its x
    // (row-vector maths: x' = x*m0 + y*m3 + z*m6) must move towards the
    // track centre (+x for the left blade).
    const auto thrown = formats::animate_matrices(model, 0, 1.0f);
    const double tip_x = -8.6 * thrown[1].values[6];
    check(tip_x > 0.12 && tip_x < 0.15, "left blade tip opens 0.13 m towards the centre");
    const auto half = formats::animate_matrices(model, 0, 0.5f);
    check(near(-8.6 * half[1].values[6], tip_x / 2.0, 0.005), "keys are interpolated");
    const auto back = formats::animate_matrices(model, 0, 2.0f);
    check(near(back[1].values[6], 0.0, 1e-5), "frame 2 returns to the rest pose");
    check(near(formats::animate_matrices(model, 0, 9.0f)[1].values[6], 0.0, 1e-5), "frames past the last key clamp");
}

} // namespace

int main() {
    test_formats();
    test_geometry();
    test_junctions();
    test_loops();
    test_animation();
    if (failures == 0) std::cout << "track: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
