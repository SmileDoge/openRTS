#pragma once

// A route's grass settings (`<route>/<route>.rtst`, RTS's: NoGrassInSeason,
// GrassDistanceFromTrack / Road, GrassTextures, TerrainTextures). How RTS
// draws them is not traced (testers: half done there); openRTS's own grass
// (renderer, terrain) takes only where grass grows from it: on which ground
// textures, how tall, how dense, in which seasons, how far from the ways.
// docs/grass.md.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

struct GroundGrass {
    float height = 1.0f;  // GrassHeight, m (the tallest blades)
    float density = 1.0f; // the sum of its UseGrassTextures' TextureSpread
};

struct RouteGrass {
    std::set<int> no_grass_seasons; // 0 spring .. 3 winter
    float from_track = 0.0f;        // GrassDistanceFromTrack's first value, m from the axis
    float from_road = 0.0f;         // GrassDistanceFromRoad, m
    // By the ground texture's file name, lower case ("трава 45 сухая.ace").
    std::map<std::string, GroundGrass> grounds;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] RouteGrass parse_route_grass(std::string_view text);
// The route's .rtst (its folder's name + .rtst); nullopt when there is none.
[[nodiscard]] std::optional<RouteGrass> load_route_grass(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
