// A snapshot in openRTS terms on a synthetic track (docs/net.md): distances
// along the track, junctions set the server's way, a train's front from the
// rear end RTS gives, and a remote train drawn one interval behind.
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/net/world.hpp>
#include <openrts/simulation/track_network.hpp>

namespace {

using namespace openrts;
using simulation::TrackLocation;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

bool near(const double a, const double b, const double tolerance = 1e-2) { return std::abs(a - b) <= tolerance; }

const char* sections_text = R"(SIMISA@@@@@@@@@@JINX0T0t______
_INFO ( header )
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
        SectionIdx ( 1 1.5 0 0.1 0 1 )
    )
)
)";

std::string section(const double x, const double z) {
    return "1 0 0 0 0 1 1 00 0 0 " + std::to_string(x) + " 0 " + std::to_string(z) + " 0 0 0 ";
}

std::string pin(const int node, const int direction) {
    return "TrPin ( " + std::to_string(node) + " " + std::to_string(direction) + " ) ";
}

std::string vector_node(const int index, const std::string& sections, const std::string& pins) {
    return "TrackNode ( " + std::to_string(index) + " TrVectorNode ( TrVectorSections ( 1 " + sections + ") ) TrPins ( 1 1 " +
           pins + ") )\n";
}

// end 10 - [1: 100 m] - junction 20; branch 0 -> [2] -> end 11; branch 1 -> [3] -> end 12.
std::unique_ptr<simulation::TrackNetwork> switch_track() {
    const std::string nodes =
        vector_node(1, section(0, 0), pin(10, 1) + pin(20, 1)) +
        "TrackNode ( 20 TrJunctionNode ( 0 7 0 ) UiD ( 0 0 55 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 2 " + pin(1, 0) + pin(2, 1) +
        pin(3, 1) + ") )\n" + vector_node(2, section(0, 100), pin(20, 0) + pin(11, 1)) +
        vector_node(3, section(1.5, 100), pin(20, 0) + pin(12, 1)) +
        "TrackNode ( 10 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 " + pin(1, 1) + ") )\n" +
        "TrackNode ( 11 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 " + pin(2, 0) + ") )\n" +
        "TrackNode ( 12 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 " + pin(3, 0) + ") )\n";
    const std::string tdb = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackDB ( Serial ( 1 ) TrackNodes ( 20\n" + nodes +
                            ") TrItemTable ( 0\n) )\n";
    auto sections = formats::parse_track_sections(sections_text);
    auto db = formats::parse_track_database(tdb);
    check(sections.ok() && db.ok(), "synthetic track parses");
    return std::make_unique<simulation::TrackNetwork>(std::move(db), std::move(sections));
}

void distances() {
    auto net = switch_track();
    const TrackLocation from{1, 10.0, true};
    const auto ahead = net::track_distance(*net, from, {1, 60.0, true}, 300.0);
    check(ahead && near(*ahead, 50.0), "ahead on the same node");
    const auto behind = net::track_distance(*net, from, {1, 5.0, true}, 300.0);
    check(behind && near(*behind, -5.0), "behind on the same node");
    const auto across = net::track_distance(*net, from, {2, 30.0, true}, 300.0);
    check(across && near(*across, 120.0, 0.05), "across the junction on its branch");
    check(!net::track_distance(*net, from, {3, 30.0, true}, 300.0), "not the branch the junction is not set to");
    check(!net::track_distance(*net, from, {1, 60.0, false}, 300.0), "not a place facing the other way");
    check(!net::track_distance(*net, from, {2, 30.0, true}, 50.0), "nothing past the limit");

    // The server's junction position, even where this side holds it.
    check(net::set_junction(*net, {19, 1, 0}), "junction index 19 is node 20");
    const auto branch = net::track_distance(*net, from, {3, 30.0, true}, 300.0);
    check(branch && near(*branch, 120.0, 0.05), "across the junction after the server threw it");
    check(!net::set_junction(*net, {0, 0, 0}), "node 1 is no junction");
    check(!net::set_junction(*net, {19, 5, 0}), "no such branch");
}

void front() {
    auto net = switch_track();
    net::SnapshotTrain train;
    net::TrainCar car;
    car.length = 16.0f;
    train.cars = {car, car};
    net::TrainPos pos;
    pos.node = 1;
    pos.offset = 20.0f;
    pos.direction = 0;
    pos.cars = {{24.15, 0.0f, 0.0f}, {8.05, 0.0f, 0.0f}};
    train.pos = pos;
    const auto front = net::train_front(*net, train);
    check(front && front->node == 1 && near(front->offset, 52.15) && front->forward, "the front 32.15 m ahead of the rear");
    train.pos->direction = 1;
    train.pos->offset = 60.0f;
    const auto back = net::train_front(*net, train);
    check(back && near(back->offset, 27.85) && !back->forward, "facing against the node");
    train.pos->offset = 20.0f;
    check(!net::train_front(*net, train), "the track ends before the front");
}

void motion() {
    auto net = switch_track();
    net::RemoteMotion m;
    check(!m.at(*net, 0.0), "nothing before the first position");
    m.add(*net, 0.0, {1, 10.0, true}, 10.0f);
    m.add(*net, 1.5, {1, 25.0, true}, 10.0f);
    m.add(*net, 3.0, {1, 40.0, true}, 10.0f);
    check(near(m.delay(), 1.5), "the interval is the senders' 1.5 s");
    // Drawn 1.5 s behind: at 3.75 s the train is where it was at 2.25 s.
    const auto at = m.at(*net, 3.75);
    check(at && near(at->offset, 32.5), "interpolated between two positions");
    check(near(m.speed_at(3.75), 10.0f), "the speed between them");
    // Past the last position: on for at most one interval.
    const auto later = m.at(*net, 6.0);
    check(later && near(later->offset, 55.0), "on at the last speed for one interval");
    m.stand(6.0);
    const auto stood = m.at(*net, 9.0);
    check(stood && near(stood->offset, 40.0), "a standing train stays where it was last");
    // A jump that the track does not join: put there once it is due.
    net::RemoteMotion jump;
    jump.add(*net, 0.0, {1, 10.0, true}, 0.0f);
    jump.add(*net, 1.5, {3, 50.0, true}, 0.0f);
    const auto before = jump.at(*net, 2.0);
    const auto after = jump.at(*net, 3.1);
    check(before && before->node == 1 && after && after->node == 3, "a jump waits, then puts the train there");
}

} // namespace

int main() {
    distances();
    front();
    motion();
    if (failures == 0) std::cout << "net world tests passed\n";
    return failures == 0 ? 0 : 1;
}
