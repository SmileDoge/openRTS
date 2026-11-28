// Cargo formats: RTSCargo.dat's WagonTypes, RTSObjects.dat, a .sd's
// ESD_Pickup_Object, a world Pickup, the cargo kinds (docs/reverse/cargo.md).

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/formats/cargo.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/shape_description.hpp>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

bool near(float a, float b) {
    return std::abs(a - b) < 1e-3f;
}

void wagon_types() {
    using namespace openrts::formats;
    const std::string text = R"(SIMISA@@@@@@@@@@JINX0D0t______

CargoTypes (
	Cargo ( ID ( COAL ) Name ( "Coal" ) Density ( 5000.0 ) Texture ( "coal.ace" ) SMSTrigger ( 1 2 ) )
)
WagonTypes (
	WagonType (
		Name ( "Hopper" )
		WagonName (
			Pattern ( "*Hopper*e.wag" )
			Pattern ( "R_HopperDos_CNII-DVZ*e.wag" )
		)
		LoadingType ( PILE )
		LoadingType ( DUMP )
		IntakePoints (
			IntakePoint ( 0.0 4.35 0.0 2.0 )
			DumpPoint ( 0.0 1.0 -1.8  0.2 )
			DumpPoint ( 0.0 1.0  1.8  0.2 )
		)
		Capacity ( 0.55 )
		DumpSpeed ( 0.72 )
		MaximumWeight ( 370.0 )
	)
	WagonType (
		Name ( "Gondola" )
		WagonName ( Pattern ( "*Gondola*e.wag" ) Filename ( "RRT_TD_MailCar_0001.wag" ) )
		LoadingType ( PILE )
		LoadingType ( CONTAINER )
		LoadingAreas (
		LoadingArea (
			BoundingBox ( -1.45 1.30 -6.40 1.50 3.40 6.40 )
				DumpFromSide ( BOTTOM )
			)
		)
		MaximumWeight ( 658.0 )
		DumpSpeed ( 2.5 )
		DumpMaxVelocity ( 0.0 )
	)
	WagonType (
		Name ( Flatcar )
		WagonName ( Filename ( mdd_Flatcar_trainz-13-7024-512e.wag ) )
		LoadingType ( CONTAINER )
		LoadingAreas ( 3
			LoadingArea ( BoundingBox ( -1.5 1.30 -12.26 1.5 6.0 12.26 ) )
			LoadingArea ( BoundingBox ( -1.5 1.30 0.04 1.5 6.0 12.25 ) LoadingType ( STACK ) )
			LoadingArea ( BoundingBox ( -1.5 1.30 -12.25 1.5 6.0 -0.04 ) Invisible ( 1 ) )
		)
	)
	WagonType (
		Name ( "Dumpcar" )
		WagonName ( Filename ( dump_anim.wag ) )
		LoadingType ( PILE ) LoadingType ( DUMP )
		LoadingAreas ( LoadingArea ( BoundingBox ( -1.45 1.30 -6.40 1.50 3.40 6.40 )
			LoadingAnimation ( LoadingUnloading ( 2 ) Nodes ( 1 BODY ) AnimTime ( 4 ) ) ) )
		LoadingAnimation ( LoadingUnloading ( 3 ) Nodes ( 2 HATCH_L HATCH_R ) ForLoadingType ( PILE ) AnimFrame ( 10 ) )
	)
)
)";
    CargoDescriptions descriptions;
    parse_cargo_descriptions(text, descriptions);
    check(descriptions.wagons.size() == 4, "four WagonTypes, CargoTypes skipped: " + std::to_string(descriptions.wagons.size()));
    if (descriptions.wagons.size() != 4) return;
    const auto& dump = descriptions.wagons[3];
    check(dump.animations.size() == 2, "a dump car's two LoadingAnimations");
    if (dump.animations.size() == 2) {
        check(dump.animations[0].loading_unloading == 2 && dump.animations[0].nodes == std::vector<std::string>{"BODY"}
                  && near(dump.animations[0].time, 4.0f) && dump.animations[0].areas == 1,
              "an area's: tipping while unloading, for its own area");
        check(dump.animations[1].nodes.size() == 2 && dump.animations[1].nodes[1] == "HATCH_R" && dump.animations[1].frame == 10
                  && dump.animations[1].loading == cargo_pile && near(dump.animations[1].time, 2.0f),
              "the car's: its hatches, frame 10, AnimTime 2 s by default");
    }
    const auto& hopper = descriptions.wagons[0];
    check(hopper.name == "Hopper", "the hopper's name");
    check(hopper.loading == (cargo_pile | cargo_dump), "the hopper's loading types");
    check(hopper.intakes.size() == 1 && near(hopper.intakes[0].position[1], 4.35f) && near(hopper.intakes[0].tolerance, 2.0f),
          "the hopper's intake point");
    check(hopper.dumps.size() == 2 && near(hopper.dumps[1].position[2], 1.8f), "the hopper's dump points");
    check(near(hopper.capacity, 0.55f) && near(hopper.maximum_weight, 370.0f) && near(hopper.dump_speed, 0.72f), "the hopper's numbers");
    check(hopper.dump_max_velocity < 0.0f, "no DumpMaxVelocity: -1");
    const auto& gondola = descriptions.wagons[1];
    check(gondola.areas.size() == 1 && gondola.areas[0].dump_side == "bottom", "the gondola's area");
    check(gondola.areas.size() == 1 && near(gondola.areas[0].box.low[2], -6.4f) && near(gondola.areas[0].box.high[1], 3.4f),
          "the gondola's box");
    check(near(gondola.areas[0].box.volume(), 2.95f * 2.1f * 12.8f), "the box's volume");
    check(gondola.dump_max_velocity == 0.0f, "DumpMaxVelocity 0");
    const auto& flat = descriptions.wagons[2];
    check(flat.areas.size() == 3 && flat.areas[1].loading == cargo_stack && flat.areas[2].invisible, "the flat car's areas");
    check(flat.loading == cargo_container, "an area's LoadingType is not the car's");

    check(descriptions.find("ru_Hopper_X_0001e.wag") == &hopper, "a pattern matches, any case");
    check(descriptions.find("TRAINS/TRAINSET/x/R_HopperDos_CNII-DVZ_3e.wag") == &hopper, "a path's file name matches");
    check(descriptions.find("rrt_td_mailcar_0001.wag") == &gondola, "a Filename matches, any case");
    check(descriptions.find("mdd_Flatcar_trainz-13-7024-512e.wag") == &flat, "an unquoted Filename");
    check(descriptions.find("ru_Hopper_X_0001.wag") == nullptr, "the pattern's tail must match");
}

void cargo_objects() {
    using namespace openrts::formats;
    const std::string text = R"(SIMISA@@@@@@@@@@JINX0C3t______
ServiceObjects ( ServiceObject ( ID ( DRAG_NORMAL ) Type ( DRAG ) Shape ( drag.s ) ) )
CargoObjects (
	CargoObject ( ID ( Container1_1 ) Name ( "Container 1_1 loaded" ) LoadingType ( CONTAINER )
		Shape ( t_container1_1.s ) Size ( -1.3 2.8 6.05 1.3 0.0 -6.05 ) Stackable ( 3 ) Mass ( 50t ) MassMaximum ( 150t ) )
	CargoObject ( ID ( zaprail ) LoadingType ( STACK ) LoadingType ( CONTAINER ) Shape ( zaprail.s )
		Size ( -1.000 0.000 -6.250 1.000 0.500 6.250 ) Stackable ( 9 ) Mass ( 0.05t ) )
	CargoObject ( ID ( "flour bag" ) Name ( "Мука, мешок" ) LoadingType ( STACK ) Volume ( 0.06 ) Mass ( 50 ) )
)
)";
    std::vector<CargoObject> objects;
    parse_cargo_objects(text, objects);
    check(objects.size() == 3, "three cargo objects, the service object skipped: " + std::to_string(objects.size()));
    if (objects.size() != 3) return;
    check(objects[0].id == "Container1_1" && objects[0].loading == cargo_container, "the container's id and type");
    check(near(objects[0].mass, 50000.0f) && near(objects[0].mass_maximum, 150000.0f), "50t: 50000 kg");
    check(objects[0].size && near(objects[0].size->low[1], 0.0f) && near(objects[0].size->high[2], 6.05f), "Size sorted");
    check(objects[0].stackable == 3, "Stackable");
    const auto e = objects[0].extent();
    check(near(e[0], 2.6f) && near(e[1], 2.8f) && near(e[2], 12.1f), "the container's extent");
    check(objects[1].loading == (cargo_stack | cargo_container) && near(objects[1].mass, 50.0f), "rails: two types, 0.05t");
    check(objects[2].id == "flour bag" && !objects[2].size && near(objects[2].volume, 0.06f), "a quoted id, Volume");
    check(near(objects[2].extent()[0], std::cbrt(0.06f)), "a Volume's extent is its cube");

    const std::string route = R"(CargoObjects ( CargoObject ( ID ( ZAPRAIL ) LoadingType ( STACK ) Mass ( 1t ) ) ))";
    parse_cargo_objects(route, objects);
    check(objects.size() == 3 && near(objects[1].mass, 1000.0f), "the route's object replaces the global one of its id");
}

void pickup_shape() {
    using namespace openrts::formats;
    const std::string sd = R"(SIMISA@@@@@@@@@@JINX0t1t______

shape ( BGM_CargoType_MAIL.s
	ESD_Detail_Level ( 0 )
	ESD_Pickup_Object ( PickupCraneOriented ( 1 ) PickupCraneType ( 1 ) PickupType ( 12 0.0 )
		CargoTable ( Item ( CONTAINER Container mail1 -1 -1 ) Item ( PILE COAL COAL 10 5t ) )
		SpeedRange ( 0.0 0.5 ) PickupArea ( -1.25 -0.1 -8 1.25 12.4 8 )
		PickupCraneShapeNodes ( Bridge ( MOST ) Hook ( KRUK ) HookInitialHeight ( 6.5 ) ) )
)
)";
    const auto parsed = parse_shape_description(sd);
    check(parsed.ok() && parsed.value->pickup.has_value(), "ESD_Pickup_Object read");
    if (!parsed.ok() || !parsed.value->pickup) return;
    const auto& pickup = *parsed.value->pickup;
    check(pickup.type == 12 && pickup.crane_type == 1 && pickup.crane_oriented == 1, "a crane's type");
    check(near(pickup.speed_max, 0.5f), "SpeedRange");
    check(pickup.area && near(pickup.area->low[2], -8.0f) && near(pickup.area->high[1], 12.4f), "PickupArea");
    check(pickup.table.size() == 2, "two CargoTable items");
    if (pickup.table.size() == 2) {
        check(pickup.table[0].loading == cargo_container && pickup.table[0].object == "mail1" && pickup.table[0].count == -1,
              "a container item, no limit");
        check(pickup.table[1].loading == cargo_pile && pickup.table[1].cargo == "coal" && pickup.table[1].object.empty()
                  && pickup.table[1].count == 10 && near(pickup.table[1].mass, 5000.0f),
              "a bulk item names no object, its mass in tonnes");
    }
    check(pickup.crane_nodes && pickup.crane_nodes->bridge == "MOST" && near(pickup.crane_nodes->hook_height, 6.5f), "the crane's nodes");
    check(pickup_loading(12) == (cargo_container | cargo_stack | cargo_box), "a crane loads containers, stacks and boxes");

    const std::string fuel = R"(shape ( terminal.s ESD_Pickup_Object ( SpeedRange ( 0.0 0.0 ) PickupType ( 7 0 ) PickupAnimData ( 0 1 )
		PickupCapacity ( 10000 1.25 ) CargoType ( GASOLINE ) PickupPoints ( 2 1 2 3 4 5 6 ) PickupLength ( 15.0 ) ) ))";
    const auto rack = parse_shape_description(fuel);
    check(rack.ok() && rack.value->pickup && rack.value->pickup->type == 7, "a rack's type");
    if (rack.ok() && rack.value->pickup) {
        const auto& p = *rack.value->pickup;
        check(p.cargo == "gasoline" && near(p.capacity, 10000.0f) && near(p.speed, 1.25f) && near(p.length, 15.0f), "a rack's numbers");
        check(p.points.size() == 2 && near(p.points[1][0], 4.0f) && near(p.points[1][2], 6.0f), "PickupPoints after their count");
    }
    check(!parse_shape_description("shape ( plain.s ESD_Detail_Level ( 0 ) )").value->pickup, "no pickup");
}

void world_pickup() {
    using namespace openrts::formats;
    const std::string text = R"(SIMISA@@@@@@@@@@JINX0w0t______
Tr_Worldfile (
	Pickup (
		UiD ( 7343 )
		SpeedRange ( 0 5 )
		PickupType ( 4 1 )
		PickupAnimData ( 0 0 )
		PickupCapacity ( 1000 2 )
		TrItemId ( 0 3339 )
		CollideFlags ( 576 )
		FileName ( rm_univ-zapravka.s )
		Position ( 10 20 30 )
		QDirection ( 0 0 0 1 )
	)
)
)";
    const auto tile = parse_world_tile(text, "w+000000+000000.w");
    check(tile.ok() && tile.objects.size() == 1, "a world pickup read");
    if (!tile.ok() || tile.objects.size() != 1) return;
    const auto& object = tile.objects[0];
    check(object.kind == WorldObjectKind::pickup && object.pickup_type == 4 && near(object.pickup_type_value, 1.0f), "PickupType");
    check(near(object.pickup_speed_range[1], 5.0f) && near(object.pickup_capacity[0], 1000.0f) && near(object.pickup_capacity[1], 2.0f),
          "SpeedRange and PickupCapacity");
    check(object.rail_items.size() == 1 && object.rail_items[0] == 3339 && object.uid == 7343, "the common fields still read");
}

void kinds() {
    using namespace openrts::formats;
    CargoKinds kinds;
    check(kinds.find("COAL") && kinds.find("coal")->code == 10 && near(kinds.find("coal")->density, 1500.0f), "coal: code 10, 1500");
    check(kinds.find(54) && kinds.find(54)->id == "iron_ore" && near(kinds.find(54)->pile, 0.2f), "iron ore by code");
    check(kinds.find("coal")->texture == "RTSCargo_coal", "RTS's texture stem");
    const std::string own = "\xEF\xBB\xBF" R"(CargoTypes (
	Cargo ( ID ( ASH ) Name ( "Ash" ) RussianName ( "Зола" ) Density ( 1000 ) Pile ( 0.4 ) )
	Cargo ( ID ( CASTIRON ) Name ( "Cast iron" ) Density ( 7200 ) LoadingType ( STACK ) Texture ( "oRTSCargo_castiron.ace" ) )
	Cargo ( ID ( coal ) Name ( "Not RTS's coal" ) Density ( 1 ) )
))";
    check(kinds.add_own(own) == 2, "two own kinds, RTS's coal kept");
    const auto* ash = kinds.find("ash");
    check(ash && ash->own && ash->code >= 1000 && ash->russian_name == "Зола" && ash->loading == cargo_pile, "ash read");
    check(ash && ash->texture == "oRTSCargo_ash" && near(ash->pile, 0.4f), "ash's texture by its id");
    const auto* iron = kinds.find("CastIron");
    check(iron && iron->texture == "oRTSCargo_castiron" && iron->loading == cargo_stack && iron->code == ash->code + 1, "cast iron");
    check(near(kinds.find("coal")->density, 1500.0f), "coal unchanged");
}

} // namespace

int main() {
    wagon_types();
    cargo_objects();
    pickup_shape();
    world_pickup();
    kinds();
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
