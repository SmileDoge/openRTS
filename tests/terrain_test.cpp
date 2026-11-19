#include <array>
#include <cstdio>
#include <string_view>

#include <openrts/formats/terrain.hpp>
#include <openrts/renderer/rts_sky.hpp>

namespace {

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    namespace rts = openrts::renderer::rts;
    using openrts::formats::terrain_tile_name;

    // Names of real Kuban_RTS terrain tiles around its start tile.
    check(terrain_tile_name(-4496, 14554) == "-11ac64cc", "start tile name");
    check(terrain_tile_name(-4497, 14553) == "-11ac61a4", "south-west neighbour name");
    check(terrain_tile_name(-4495, 14555) == "-11ac64c4", "north-east neighbour name");

    check(std::string_view(rts::environment_condition(rts::Season::summer, rts::Weather::clear)) == "SummerClear",
          "summer clear condition");
    check(std::string_view(rts::environment_condition(rts::Season::winter, rts::Weather::snow)) == "WinterSnow",
          "winter snow condition");
    check(std::string_view(rts::environment_condition(rts::Season::autumn, rts::Weather::rain)) == "AutumnRain",
          "autumn rain condition");

    // The water's height over the tile (TerrainTile::water_height): the
    // corners stored SW, SE, NE, NW, spread from corner to corner.
    {
        openrts::formats::TerrainTile tile;
        const auto near = [](const float a, const float b) { return a > b - 1e-4f && a < b + 1e-4f; };
        check(tile.water_height(0.5f, 0.5f) == 0.0f, "water: no heights, 0");
        tile.water_heights = std::array<float, 4>{1.0f, 2.0f, 3.0f, 4.0f};
        check(near(tile.water_height(0.0f, 0.0f), 4.0f), "water: the north-west corner is the fourth");
        check(near(tile.water_height(1.0f, 0.0f), 3.0f), "water: the north-east corner is the third");
        check(near(tile.water_height(0.0f, 1.0f), 1.0f), "water: the south-west corner is the first");
        check(near(tile.water_height(1.0f, 1.0f), 2.0f), "water: the south-east corner is the second");
        check(near(tile.water_height(0.5f, 0.5f), 2.5f), "water: the middle is the corners' mean");
        check(near(tile.water_height(0.5f, 0.0f), 3.5f) && near(tile.water_height(0.0f, 0.5f), 2.5f),
              "water: along the edges, between their corners");
        // A corner given as 0 takes the SW one (Open Rails): only the first set, level.
        tile.water_heights = std::array<float, 4>{210.0f, 0.0f, 0.0f, 0.0f};
        check(near(tile.water_height(0.0f, 0.0f), 210.0f) && near(tile.water_height(1.0f, 0.3f), 210.0f),
              "water: only the first height given, level at it");
        // Novokuznetsk's -148c6eac: NE and NW 210, SW and SE 0 (no water patches there).
        tile.water_heights = std::array<float, 4>{0.0f, 0.0f, 210.0f, 210.0f};
        check(near(tile.water_height(0.7f, 0.0f), 210.0f) && near(tile.water_height(0.2f, 1.0f), 0.0f) &&
                  near(tile.water_height(0.4f, 0.5f), 105.0f),
              "water: sloped from the north edge to the south edge");
    }

    if (failures == 0) std::printf("terrain: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
