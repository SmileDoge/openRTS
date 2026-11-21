#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <openrts/formats/route_database.hpp>
#include <openrts/simulation/track_network.hpp>

// openRTS's КЛУБ-У electronic map (ЭК), its first version
// (docs/brakes-dll-replacement.md, "openRTS's electronic map").
// The real КЛУБ-У does not know which track the train is on: the driver
// enters the track number and the map gives the target, the signal of that
// track, and the distance to it, whatever way the switches lie (a wrong
// number: the distance to the entered track's signal). Brakes.dll's КЛУБ has
// no map: a number entered only turns its "ЭК" on, the target always the next
// signal as the switches stand.
//
// The map is the route database's (.rtsdb): its station tracks (RoutePoints
// of PointType 2, named by their number: "3", "1А", "2БП") from one end to
// the other, and the stations' ends at the lines (PointType 1, flag 2: where
// a station's entry signals stand). From the train's front the map's way is
// the way to the nearest track of the entered number ahead (all branches
// looked at), then on through the switches' MainRoute; the target is the
// first train signal (a NORMAL head) along it. Without such a track ahead
// the ЭК goes on along MainRoute while the train is on the entered track's
// way (out of its station onto the line); a track other than the main ones
// (1, 2: the line's track of the number then, below) leaves it ("без ЭК")
// past the first signal on past its own along its way (past its route
// signal НМ3 the exit Н1 after the crossover, past its exit Н3 the first
// block signal; the train on another track: past the first block signal,
// its station's other signals passed on, and once the entered track
// abreast is behind the target that block signal, not another park's track
// of the number; testers, 2026-11-21), the entry signal of a station
// without the entered track too, and with no station of the route having
// the number at all. It
// comes back by itself before the entry signal of another station with the
// track (the entry its target; where the entries are not known, one train
// signal or none before the track). Track 99: "без ЭК" (the instructions' way into it).
// The ЭК takes a track whose way turns off MainRoute (a side track) only
// in front of the signal the turn lies beyond (testers, 2026-11-21: an
// entry Н with a route signal НМ past it and the switches past НМ: "без
// ЭК" up to НМ); once taken it keeps it as above.
// The front on another track of a station that has the entered one (at its
// platform, the switches set for the train's own): the entered track's
// signal the train's way all the same, as the real КЛУБ-У's by its
// coordinate (testers, 2026-11-20), measured from the point of that track
// abreast of the front; so too in its throat past its entry signal, once
// the entered track is no more ahead. On the line (testers, 2026-11-21) the
// lines' tracks carry the numbers of the station main tracks (1, 2) they go
// on from (along MainRoute, not where a side track joins one): the front on one
// with another number entered takes the next train signal of the entered
// one's abreast that faces the way most of its node's do (its block
// signals, with their backs to the train on a double line), measured from
// its point abreast.
// Not yet: the ALSN frequency, the limits, switches and crossings of the
// map, К280 / К282.
namespace openrts::simulation {

// A train signal (a NORMAL head) the map knows: where it stands and the way
// it faces (trains running that way on the node approach it).
struct MapSignal {
    std::uint32_t object{}; // SignalSystem object
    std::uint32_t node{};
    double offset{};
    bool forward = true;
};

// A station track of the map.
struct MapTrack {
    std::string station;
    std::string name;
    std::uint32_t number{}; // the name's leading digits (1 .. 98)
    // Its park: the capital letter after the number ("7Г", "3А", "1ГЛ": Г,
    // А, Г), empty none ("1", "17", "6н" / "6ч" the halves of a track).
    std::string park;
    struct Span {
        std::uint32_t node{};
        double from{}, to{}; // offsets along the node, from <= to
        bool forward = true; // from the track's first end to its second the offset grows
    };
    std::vector<Span> spans;
};

// What the map keeps of a КЛУБ's ЭК between steps.
struct EkState {
    std::uint32_t number{};     // the track number it is for (another: started afresh)
    bool started = false;
    bool matched = false;       // the ЭК mode
    bool latched_off = false;   // left the entered track's way: back only near another station with it
    std::string left_at;        // the station it was left at
    // A number other than the main tracks' (1, 2) entered, past a track's
    // signal leaving a station: the station; "без ЭК" from the next block
    // signal passed, or (`leaving_own`: past the entered track's own) from
    // the next signal of any kind.
    std::string leaving;
    bool leaving_own = false;
    // The station whose track of the number the train has passed abreast
    // (its signals behind): the target the first block signal along the
    // train's way from then, not another park's track of the number.
    std::string done_at;
    // The station with the entered track the front is in: past its entry
    // signal, or on one of its tracks, until it passes a track's signal.
    std::string inside;
};

struct EkTarget {
    bool matched = false;    // the ЭК mode (false: "без ЭК")
    bool has_target = false; // a train signal ahead along the map's way
    std::uint32_t object{};  // the target's SignalSystem object
    double distance{};       // m from the front along the map's way
    std::int32_t unit = -1;  // its route database unit (set by the caller, for its name)
};

class ElectronicMap {
public:
    ElectronicMap() = default;
    // The route database's station tracks and station ends placed on the
    // network (each end within 10 m of a track), and the train signals.
    ElectronicMap(const TrackNetwork& network, const formats::RouteDatabase& database, std::vector<MapSignal> signals);

    [[nodiscard]] bool empty() const noexcept { return tracks_.empty(); }
    // The line track's number of vector node `node` (0: none, or more
    // than one led onto it).
    [[nodiscard]] std::uint32_t line_number(std::uint32_t node) const;
    [[nodiscard]] const std::vector<MapTrack>& tracks() const noexcept { return tracks_; }
    // The map's track of `object` (the signal at its end or on it), else none.
    [[nodiscard]] const MapTrack* track_of(std::uint32_t object) const;

    // This step's target for track `number` from the front at `offset` on
    // vector node `node` facing `forward`; `passed`: the train signals the
    // front passed this step (as the switches lie). `network`: the one the
    // map was made on (its junctions' MainRoute).
    EkTarget update(EkState& state, const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                    std::uint32_t number, std::span<const std::uint32_t> passed) const;

    // A station track's number: its name's leading digits (1 .. 98), else 0.
    [[nodiscard]] static std::uint32_t track_number(std::string_view name);
    // Its park letter (MapTrack::park).
    [[nodiscard]] static std::string track_park(std::string_view name);

    // How far the map's way is looked along, m.
    static constexpr double horizon = 10000.0;
    // A line track's number carried at most this far from its station, m.
    static constexpr double line_carry = 40000.0;
    // A line track of the number entered taken at most this far aside, m.
    static constexpr double line_reach = 200.0;

private:
    struct Signal {
        std::uint32_t object{};
        double offset{};
        bool forward = true;
    };
    struct SpanRef {
        std::size_t track{};
        double from{}, to{};
    };
    struct Found {
        bool found = false;
        bool has_target = false;
        std::uint32_t object{};
        double distance{};
        std::size_t signals_before{}; // train signals before the entered track
        // Train signals before the way first turns off MainRoute (a facing
        // junction's other branch) on to the track; 0 where it does not.
        std::size_t signals_before_turn{};
        std::size_t track{};          // the entered track's the way leads onto (tracks_)
        bool passed = false;          // abreast(): the nearest track of the number behind the front
    };
    [[nodiscard]] Found search(const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                               std::uint32_t number) const;
    // The first train signal along MainRoute from (node, offset, forward),
    // `already` metres on, within the horizon.
    [[nodiscard]] Found along_main(const TrackNetwork& network, std::uint32_t node, double offset, bool forward, double already,
                                   bool strictly_ahead, bool line_way = false, bool as_set = false) const;
    // The station track whose span holds (node, offset), else none.
    [[nodiscard]] std::optional<std::size_t> track_at(std::uint32_t node, double offset) const;
    // Track `number` of `station`: its first train signal the way the front
    // at (node, offset, forward) runs, from its point abreast of the front.
    [[nodiscard]] Found abreast(const TrackNetwork& network, std::uint32_t node, double offset, bool forward, const std::string& station,
                                std::uint32_t number, const std::string* park = nullptr) const;
    // `station`'s park `park` has a track `number` (the main ones, 1 and 2:
    // its unlettered tracks too).
    [[nodiscard]] bool in_park(const std::string& station, const std::string& park, std::uint32_t number) const;
    // Line track `number` abreast of the front (within line_reach): its
    // next train signal the way the front runs (`along_main`'s `line_way`:
    // facing as most of its node's signals do, either way on a node where
    // as many face each way).
    [[nodiscard]] Found line_abreast(const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                                     std::uint32_t number) const;
    // The first train signal along MainRoute from (node, offset, forward)
    // that is not `station`'s (its tracks' signals, its entries): the first
    // block signal leaving it, else the next station's entry.
    [[nodiscard]] Found first_block(const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                                    const std::string& station) const;

    std::vector<MapTrack> tracks_;
    std::unordered_map<std::uint32_t, std::vector<Signal>> signals_; // by node, by offset
    std::unordered_map<std::uint32_t, std::vector<SpanRef>> spans_;  // by node
    std::unordered_map<std::uint32_t, std::size_t> signal_track_;    // object -> track
    std::unordered_map<std::uint32_t, MapSignal> signal_place_;      // object -> where it stands
    std::unordered_map<std::uint32_t, std::string> signal_entry_;    // object -> the station it enters
    std::set<std::pair<std::string, std::uint32_t>> station_numbers_;
    std::set<std::tuple<std::string, std::string, std::uint32_t>> park_numbers_; // station, park, number
    std::set<std::uint32_t> numbers_;
    // The lines' tracks: vector node -> number (0: more than one), and
    // their points every 10 m by 64 m cells.
    struct LinePoint {
        std::uint32_t node{};
        double offset{};
    };
    std::unordered_map<std::uint32_t, std::uint32_t> line_numbers_;
    std::unordered_map<std::uint32_t, int> line_ways_; // line node -> its signals' way: 1 forward, -1 back (most of them)
    std::unordered_map<std::uint64_t, std::vector<LinePoint>> line_cells_;
};

} // namespace openrts::simulation
