#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <openrts/simulation/speed_limits.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>

// The track profile around a train (docs/paths.md, Shift+F7): the way ahead
// and behind its leading end, flattened out, with the rail height at every
// step, the grades, and what stands along it — signals, speed posts,
// stations and the other trains' cars. RTS shows the same window in its
// track monitor (reverse/dispatcher.md); this is openRTS's own reading of
// the same data, not the old window's code.
namespace openrts::simulation {

class Train;
class SignalSystem;

// One sample of the way: where it is from the train's leading end and how
// high the rails are there.
struct ProfileVertex {
    double distance{};  // m, negative behind the leading end
    double elevation{}; // m, the rail top
    float grade{};      // per mille, positive uphill towards the head
};

enum class ProfileMarkKind : std::uint8_t { signal, speed_post, station, platform, siding };

struct ProfileMark {
    ProfileMarkKind kind{};
    double distance{}; // m, negative behind the leading end
    float speed = -1.0f; // m/s for a speed post or a signal that names one
    bool facing = false; // a signal whose face the train passes
    bool at_danger = false;
    std::string name;
};

// A stretch of the way, as distances from the leading end: a car of another
// train standing on it, in order from behind to ahead.
struct ProfileSpan {
    double from{};
    double to{};
};

struct TrackWindow {
    std::vector<ProfileVertex> vertices;
    std::vector<ProfileMark> marks;
    std::vector<ProfileSpan> occupied;
    double behind{}; // how far the walk reached, m
    double ahead{};
    bool cut_behind = false; // stopped at a junction set against the way or the end of track
    bool cut_ahead = false;
    float current_limit = -1.0f; // m/s at the head, from the posts behind it
    double elevation_at_head{};
    float grade_at_head{}; // per mille
};

struct ProfileRequest {
    double behind = 600.0;  // m
    double ahead = 2400.0;  // m
    double step = 10.0;     // m between samples
    bool marks = true;
    bool occupancy = true;
};

// Walks `behind` metres back from the train's leading end and `ahead` metres
// forward, sampling the rails; `limits` and `signals` may be null, `others`
// are the trains whose cars show as occupied.
[[nodiscard]] TrackWindow build_track_window(const Train& train, const TrackNetwork& network, const SpeedLimits* limits,
                                         TrainCategory category, const SignalSystem* signals,
                                         const std::vector<const Train*>& others, const ProfileRequest& request = {});
#include <vector>

#include <openrts/simulation/track_traveller.hpp>

// The track's profile along a train's way, for the profile strip (the
// viewer's Ctrl+F4; testers' request 2026-11-19-4): the track's height every
// `step` metres from the leading end ahead and behind it, through the
// junctions as they are set (simulation::TrackTraveller, JunctionMode::plain:
// nothing is changed on the way).

struct TrackProfilePoint {
    double distance{}; // m from the leading end: ahead +, behind -
    double height{};   // m, the track's (TrackTraveller::pose().y)
};

struct TrackProfile {
    std::vector<TrackProfilePoint> points; // by distance, behind first
    double ahead{}, behind{};              // how far it got each way (an end of track stops it)

    // The height at `distance`, between the points (the nearest end past them).
    [[nodiscard]] double height_at(double distance) const;
    // The grade from `from` to `to` (m along the way), per mille: + rising
    // the way the train goes, - falling. 0 for a span under a metre.
    [[nodiscard]] double grade(double from, double to) const;
};

// The profile from `lead` (the train's leading end, facing the way it goes)
// `ahead` m forward and `behind` m back, a point every `step` m.
[[nodiscard]] TrackProfile track_profile(const TrackTraveller& lead, double ahead, double behind, double step);

} // namespace openrts::simulation
