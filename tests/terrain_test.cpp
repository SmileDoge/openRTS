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

    if (failures == 0) std::printf("terrain: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
