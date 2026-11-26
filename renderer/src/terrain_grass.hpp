#pragma once

// openRTS's own grass (not RTS's, docs/grass.md): blades of grass grown on
// the ground near the eye, each a few triangles the vertex shader bends
// (terrain.wgsl, vs_grass), coloured by the ground texture under its root.
// Where it grows: a route's .rtst (its ground textures with grass, the
// seasons without it), else on any ground whose texture is green enough
// (the shader looks); never on the tracks and roads (their axes, the
// forests' clearance) nor in winter or snow.
// Internal to openrts_renderer.

#include <cstdint>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <openrts/formats/route_grass.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/terrain_renderer.hpp>

namespace openrts::renderer::detail {

struct GrassInstance {
    float position[3]{}; // in its tile, as the tile's vertices
    float height{};      // m
    float uv[2]{};       // the ground texture's at the root
    float seed{};        // 0..1
    float width{};       // m at the root
};
static_assert(sizeof(GrassInstance) == 32);

struct GrassGroup {
    std::pair<std::int32_t, std::int32_t> tile;
    std::size_t patch{}; // TerrainTileMesh::patches
    std::uint32_t first{}, count{};
    bool test_colour = true; // its ground not in the .rtst: the shader looks at its colour
};

class GrassField {
public:
    // The route's .rtst (none: the shader's colour test).
    void set_settings(std::optional<formats::RouteGrass> settings);
    [[nodiscard]] bool has_settings() const noexcept { return settings_.has_value(); }
    void add_tile(const TerrainTileMesh& mesh);
    void remove_tile(std::int32_t tile_x, std::int32_t tile_z);
    // The blades within `radius` m of the eye; true when they changed.
    bool update(const formats::RoutePosition& eye, rts::Season season, rts::Weather weather, float radius, float density);
    [[nodiscard]] const std::vector<GrassInstance>& instances() const noexcept { return instances_; }
    [[nodiscard]] const std::vector<GrassGroup>& groups() const noexcept { return groups_; }

private:
    struct Ground {
        std::uint32_t patches_per_side{}, stride{}; // patches a side; vertices a patch's side
        float sample_size{}, half_extent{};
        std::vector<float> heights;          // the tile's vertices' y
        std::vector<float> uvs;              // their uv, two each
        std::vector<std::string> textures;   // the patches' texture names, lower case
    };
    struct Cell {
        std::size_t patch{};
        bool test_colour = true;
        std::vector<GrassInstance> blades;
    };
    using CellKey = std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int32_t>;
    void grow(const std::pair<std::int32_t, std::int32_t>& tile, const Ground& ground, std::int32_t cx, std::int32_t cz, Cell& cell) const;

    std::optional<formats::RouteGrass> settings_;
    std::map<std::pair<std::int32_t, std::int32_t>, Ground> grounds_;
    std::map<CellKey, Cell> cells_;
    std::vector<GrassInstance> instances_;
    std::vector<GrassGroup> groups_;
    formats::RoutePosition last_eye_{};
    bool built_ = false;
    int last_season_ = -1;
    bool last_off_ = false;
    float last_density_ = -1.0f;
    float density_ = 1.0f;
    bool flowers_ = true;
    std::uint64_t last_blockers_ = 0;
    float flower_share_ = 1.0f;
};

} // namespace openrts::renderer::detail
