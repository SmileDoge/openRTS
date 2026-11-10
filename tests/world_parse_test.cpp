// A world tile's objects (formats/route_world): every kind takes its
// QDirection, also those whose own fields come first (a level crossing, a
// forest, a car spawner). Pavlovsk's rm_vorota2.s gates (LevelCr) had lost
// their 20 degrees and stood square to the tile.
#include <cmath>
#include <iostream>
#include <string>

#include <openrts/formats/route_world.hpp>

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

[[nodiscard]] bool near(const float a, const float b) { return std::abs(a - b) < 1e-5f; }

} // namespace

int main() {
    using namespace openrts::formats;
    const std::string text = R"(SIMISA@@@@@@@@@@JINX0w0t______

Tr_Worldfile (
	LevelCr (
		UiD ( 9277 )
		LevelCrParameters ( 30 20 )
		CrashProbability ( 0 )
		LevelCrData ( 00000000 1 )
		LevelCrTiming ( 60 60 4 )
		TrItemId ( 0 678 )
		TrItemId ( 1 58 )
		FileName ( rm_vorota2.s )
		Position ( -121.124 25.4521 -619.382 )
		QDirection ( 0 0.174108 0 0.984726 )
		VDbId ( 4294967295 )
	)
	Forest (
		UiD ( 12 )
		TreeTexture ( tree.ace )
		ScaleRange ( 1 2 )
		Area ( 100 50 )
		Population ( 40 )
		TreeSize ( 5 8 )
		Position ( 10 20 30 )
		QDirection ( 0 0.5 0 0.866025 )
		VDbId ( 1 )
	)
	CarSpawner (
		UiD ( 13 )
		CarFrequency ( 5 )
		CarAvSpeed ( 10 )
		TrItemId ( 1 1 )
		TrItemId ( 1 2 )
		Position ( 1 2 3 )
		QDirection ( 0 -0.7071 0 0.7071 )
		VDbId ( 1 )
	)
	Static (
		UiD ( 14 )
		FileName ( house.s )
		Position ( 0 0 0 )
		QDirection ( 0 1 0 0 )
		VDbId ( 1 )
	)
)
)";
    const auto tile = parse_world_tile(text, "w-006153+014889.w");
    const auto find = [&](const std::uint32_t uid) -> const WorldObjectPlacement* {
        for (const auto& object : tile.objects)
            if (object.uid == uid) return &object;
        return nullptr;
    };
    const auto* gates = find(9277);
    check(gates && gates->kind == WorldObjectKind::level_crossing, "the level crossing is read");
    if (gates) {
        check(near(gates->rotation[1], 0.174108f) && near(gates->rotation[3], 0.984726f), "the level crossing keeps its QDirection");
        check(near(gates->crossing_parameters[0], 30.0f) && near(gates->crossing_timing[2], 4.0f), "and its own fields");
        check(gates->rail_items.size() == 1 && gates->road_items.size() == 1, "and its items");
    }
    const auto* forest = find(12);
    check(forest && near(forest->rotation[1], 0.5f) && near(forest->forest_area[0], 100.0f), "the forest keeps its QDirection and area");
    const auto* spawner = find(13);
    check(spawner && near(spawner->rotation[1], -0.7071f), "the car spawner keeps its QDirection");
    const auto* house = find(14);
    check(house && near(house->rotation[1], 1.0f), "a static object keeps its QDirection");
    if (failures == 0) std::cout << "world parse: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
