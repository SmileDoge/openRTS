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

struct TrackProfile {
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
[[nodiscard]] TrackProfile build_profile(const Train& train, const TrackNetwork& network, const SpeedLimits* limits,
                                         TrainCategory category, const SignalSystem* signals,
                                         const std::vector<const Train*>& others, const ProfileRequest& request = {});

} // namespace openrts::simulation
