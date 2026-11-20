#pragma once

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
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
// way (out of its station onto the line); it leaves it ("без ЭК") when the
// train passes the signal of another track (an exit or a route signal at
// another track's end), or the entry signal of a station without the
// entered track, or when no station of the route has the number at all. It
// comes back by itself before the entry signal of another station with the
// track (the entry its target; where the entries are not known, one train
// signal or none before the track). Track 99: "без ЭК" (the instructions' way into it).
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
    struct Span {
        std::uint32_t node{};
        double from{}, to{}; // offsets along the node, from <= to
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

    // How far the map's way is looked along, m.
    static constexpr double horizon = 10000.0;

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
        std::size_t track{};          // the entered track's the way leads onto (tracks_)
    };
    [[nodiscard]] Found search(const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                               std::uint32_t number) const;
    // The first train signal along MainRoute from (node, offset, forward),
    // `already` metres on, within the horizon.
    [[nodiscard]] Found along_main(const TrackNetwork& network, std::uint32_t node, double offset, bool forward, double already,
                                   bool strictly_ahead) const;

    std::vector<MapTrack> tracks_;
    std::unordered_map<std::uint32_t, std::vector<Signal>> signals_; // by node, by offset
    std::unordered_map<std::uint32_t, std::vector<SpanRef>> spans_;  // by node
    std::unordered_map<std::uint32_t, std::size_t> signal_track_;    // object -> track
    std::unordered_map<std::uint32_t, std::string> signal_entry_;    // object -> the station it enters
    std::set<std::pair<std::string, std::uint32_t>> station_numbers_;
    std::set<std::uint32_t> numbers_;
};

} // namespace openrts::simulation
