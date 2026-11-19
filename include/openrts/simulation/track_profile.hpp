#pragma once

#include <cstdint>
#include <vector>

#include <openrts/simulation/track_traveller.hpp>

// The track's profile along a train's way, for the profile strip (the
// viewer's Ctrl+F4; testers' request 2026-11-19-4): the track's height every
// `step` metres from the leading end ahead and behind it, through the
// junctions as they are set (simulation::TrackTraveller, JunctionMode::plain:
// nothing is changed on the way).
namespace openrts::simulation {

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
