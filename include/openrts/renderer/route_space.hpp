#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/formats/route_world.hpp>

namespace openrts::renderer {

// Rendering happens in single-precision space relative to a route origin.
// These helpers convert between that space and tile-addressed route positions.

// Model matrix of a world-file object relative to `origin`.
[[nodiscard]] glm::mat4 placement_transform(const formats::WorldObjectPlacement& object,
                                            const formats::RoutePosition& origin);

// Route position of a point given relative to `origin`; the tile is chosen so
// that the local x/z coordinates stay within half a tile of its centre.
[[nodiscard]] formats::RoutePosition route_position(const glm::vec3& local, const formats::RoutePosition& origin);

} // namespace openrts::renderer
