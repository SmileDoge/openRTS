#pragma once

#include <cstdint>
#include <vector>

#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>

// The track's profile along a train's way, for the profile strip (the
// viewer's Ctrl+F4; testers' requests 2026-11-19): the track's height at
// every joint of its sections ahead of the leading end and behind it,
// through the junctions as they are set (simulation::TrackTraveller,
// JunctionMode::plain: nothing is changed on the way). A section is of one
// grade (its placement's pitch), so the profile between two joints is a
// straight line and its elements are the track's own: tied to the track,
// not to where the train stands, and not smoothed.
namespace openrts::simulation {

struct TrackProfilePoint {
    double distance{}; // m from the leading end: ahead +, behind -
    double height{};   // m, the track's (TrackTraveller::pose().y)
};

// An element of the profile (a regime map's): a stretch of one grade.
struct TrackProfileElement {
    double from{}, to{}; // m along the way
    double grade{};      // per mille: + rising the way the train goes
};

struct TrackProfile {
    std::vector<TrackProfilePoint> points; // the joints, by distance, behind first
    double ahead{}, behind{};              // how far it got each way (an end of track stops it)

    // The height at `distance`, between the points (the nearest end past them).
    [[nodiscard]] double height_at(double distance) const;
    // The grade from `from` to `to` (m along the way), per mille: + rising
    // the way the train goes, - falling. 0 for a span under a metre.
    [[nodiscard]] double grade(double from, double to) const;
    // Its elements: the stretches between the joints, the neighbours whose
    // grades differ by `tolerance` per mille or less joined (the sections of
    // one element), a stretch under a metre (a joint a centimetre off, the
    // ends) joined to its neighbour.
    [[nodiscard]] std::vector<TrackProfileElement> elements(double tolerance = 0.05) const;
};

// The profile from `lead` (the train's leading end, facing the way it goes)
// `ahead` m forward and `behind` m back.
[[nodiscard]] TrackProfile track_profile(const TrackNetwork& network, const TrackTraveller& lead, double ahead, double behind);

} // namespace openrts::simulation
