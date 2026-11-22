#pragma once

#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>

// The train's coordinate as RTS keeps it for Engine::CurrentMilepost (km;
// the КЛУБ's and САУТ's, the speed recorder's tape): its train update
// (0x508520) while the train moves looks for the nearest kilometre post
// (a speed post that is not a limit: its number) within 5000 m behind its
// reference point (the rear) and ahead (0x47FBE4); the post behind plus the
// metres to it, or minus them where the numbers fall the way the train
// runs (the post ahead lower); with none behind, the post ahead minus the
// metres to it; none within 5 km either way: −1. Each engine's is that ±
// its offset along the train (0x50B690).
namespace openrts::simulation {

struct MilepostState {
    float base = -1.0f;      // Train +112, km at the rear
    bool decreasing = false; // Train +4 0x800000: the numbers fall towards the front
    bool known = false;      // worked out once (RTS: when the train is made)
};

// One step: from the train's rear, facing its front. `moved`: the metres the
// rear went towards the front since the last update. openRTS (2026-11-22):
// where the search finds no post (looking back through a switch that leads
// away from where the train came, a dead end ahead) the coordinate it had
// goes on by `moved` instead of falling to -1 (the САВПЭ's distance stood:
// Novokuznetsk, testers).
void update_milepost(MilepostState& state, const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                     double moved = 0.0);

// A car's (RTS 0x50B690): `along` its centre's metres from the rear.
[[nodiscard]] float car_milepost(const MilepostState& state, double along);

} // namespace openrts::simulation
