#pragma once

// The other players of a network game and the engineer's helpers
// (помощник машиниста; docs/net.md, "Players and helpers"): their names
// over the world, a list in the panel with "ask to be the helper", and the
// engineer's window that takes or refuses one.

#include <vector>

#include <glm/vec3.hpp>

#include <openrts/formats/route_world.hpp>

#include "viewer_scene.hpp"

namespace openrts::viewer {

class NetSession;
class TrainView;

// Each player's name where his camera is (the server relays the places),
// with how far he is; within `range` m of the eye.
void collect_player_labels(const NetSession& net, std::vector<WorldLabel>& labels, const glm::vec3& eye,
                           const formats::RoutePosition& origin, float range);

// The panel's section: the players, asking to help one, one's help and
// helpers. `names`: the names over the world (the caller remembers it).
// True when `names` changed.
bool draw_players_section(NetSession& net, const TrainView& trains, const glm::vec3& eye, const formats::RoutePosition& origin,
                          bool& names);

// Windows of their own, the panel shown or not: a player asking to help
// the engineer (take him or not); the helper's waiting and the answer.
void draw_helper_windows(NetSession& net);
// The train just became ours and states of it are kept here for this
// server (less than a day old): put one back or none.
void draw_state_offer(NetSession& net, const TrainView& trains);

} // namespace openrts::viewer
