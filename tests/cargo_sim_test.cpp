// Cargo on cars and pickups (simulation/cargo.hpp, docs/reverse/cargo.md).

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <sstream>

#include <openrts/formats/cargo.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/cargo.hpp>
#include <openrts/simulation/state_text.hpp>
#include <openrts/simulation/train.hpp>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

bool near(float a, float b, float tolerance = 1e-3f) {
    return std::abs(a - b) < tolerance;
}

using namespace openrts;

formats::WagonCargoDescription gondola() {
    formats::WagonCargoDescription d;
    d.name = "Gondola";
    d.loading = formats::cargo_pile | formats::cargo_container;
    formats::CargoLoadingArea area;
    area.box.low = {-1.5f, 1.3f, -6.0f};
    area.box.high = {1.5f, 3.3f, 6.0f};
    d.areas.push_back(area); // 3 x 2 x 12 = 72 m³
    d.maximum_weight = 300.0f;
    d.dump_speed = 2.0f;
    d.dump_max_velocity = 0.0f;
    return d;
}

void bulk_loading() {
    formats::CargoKinds kinds;
    const auto description = gondola();
    simulation::CarCargo car(&description, {3.2f, 4.0f, 14.0f}); // box 179.2 m³: limit 53 760 kg
    check(near(car.room(), 72.0f) && near(car.room(1), 72.0f), "the room is the area's box");
    check(car.area_at(5.9f, 0.2f) == 1 && !car.area_at(6.5f, 0.2f) && car.area_at(6.1f, 0.2f) == 1, "the area under a point");

    formats::PickupDescription coal_loader;
    coal_loader.type = 2;
    coal_loader.speed = 2.0f; // m³/s
    simulation::Pickup pickup(coal_loader, kinds);
    check(pickup.cargo() && pickup.cargo()->id == "coal", "type 2 loads coal");
    check(pickup.stock() < 0.0f, "no CargoTable: no limit");

    std::vector<simulation::PickupCar> cars{{.cargo = &car, .speed = 0.0f, .area = 1, .in_reach = true}};
    auto step = pickup.step(1.0f, cars);
    check(step.car == -1 && car.empty(), "off: nothing");
    check(std::string(pickup.toggle(false)) == "Pickup ready", "Num3: ready");
    step = pickup.step(1.0f, cars);
    check(pickup.state() == simulation::PickupState::active, "ready becomes active over a car with areas");
    check(step.pouring && near(step.volume, 2.0f) && near(car.bulk_volume(), 2.0f) && near(car.load_mass(), 3000.0f),
          "2 m³ of coal, 3 t");
    for (int i = 0; i < 100; ++i) pickup.step(1.0f, cars);
    // 53 760 kg / 1500 = 35.84 m³: the mass limit before the room.
    check(near(car.load_mass(), 53760.0f, 1.0f) && near(car.fill_percent(), 100.0f, 0.01f), "full by mass: 53.76 t");
    check(pickup.state() == simulation::PickupState::done, "a full car: done");

    cars[0].speed = 1.0f;
    simulation::CarCargo second(&description, {3.2f, 4.0f, 14.0f});
    cars[0].cargo = &second;
    step = pickup.step(1.0f, cars);
    check(step.too_fast && second.empty(), "SpeedRange 0: a moving car is not loaded");
    cars[0].speed = 0.0f;
    pickup.step(1.0f, cars);
    check(pickup.state() == simulation::PickupState::active && near(second.bulk_volume(), 2.0f), "the next car loads");

    formats::PickupDescription trestle;
    trestle.type = 13;
    simulation::Pickup unloading(trestle, kinds);
    unloading.toggle(false);
    cars[0].cargo = &car;
    step = unloading.step(1.0f, cars);
    check(near(step.volume, -2.0f) && near(car.bulk_volume(), 35.84f - 2.0f, 0.01f), "the trestle takes the car's DumpSpeed");
    check(near(car.load_mass(), 53760.0f - 3000.0f, 1.0f), "and its mass");
    for (int i = 0; i < 40; ++i) unloading.step(1.0f, cars);
    check(car.empty(), "emptied");

    formats::PickupDescription limited = coal_loader;
    limited.table.push_back({.loading = formats::cargo_pile, .cargo = "coal", .count = -1, .mass = 4500.0f});
    simulation::Pickup small(limited, kinds);
    check(near(small.stock(), 4500.0f), "a CargoTable's mass is the stock");
    small.toggle(false);
    for (int i = 0; i < 5; ++i) small.step(1.0f, cars);
    check(near(car.load_mass(), 4500.0f, 1.0f) && small.state() == simulation::PickupState::empty, "the stock runs out");

    formats::WagonCargoDescription tank;
    tank.loading = formats::cargo_liquid;
    tank.capacity = 0.5f;
    simulation::CarCargo tanker(&tank, {3.0f, 4.0f, 10.0f}); // 60 m³
    formats::PickupDescription rack;
    rack.type = 16;
    rack.cargo = "oil";
    rack.speed = 100.0f;
    simulation::Pickup oil(rack, kinds);
    oil.toggle(false);
    oil.toggle(false);
    std::vector<simulation::PickupCar> tankers{{.cargo = &tanker, .area = 0, .in_reach = true}};
    oil.step(1.0f, tankers);
    check(near(tanker.bulk_volume(), 1.0f) && near(tanker.load_mass(), 850.0f), "a rack fills at a hundredth of its speed");
    oil.toggle(true);
    oil.step(1.0f, tankers);
    check(tanker.empty(), "Ctrl+Num3 drains it");
    std::vector<simulation::PickupCar> hoppers{{.cargo = &car, .area = 1, .in_reach = true}};
    simulation::Pickup grain_on_tank(coal_loader, kinds);
    grain_on_tank.toggle(false);
    std::vector<simulation::PickupCar> wrong{{.cargo = &tanker, .area = 0, .in_reach = true}};
    check(grain_on_tank.step(1.0f, wrong).unsuitable && tanker.empty(), "a tank takes no coal");
}

void objects() {
    formats::WagonCargoDescription flat;
    flat.loading = formats::cargo_container;
    formats::CargoLoadingArea area;
    area.box.low = {-1.5f, 1.3f, -12.26f};
    area.box.high = {1.5f, 6.0f, 12.26f}; // RTS's 13-7024 flat car
    flat.areas.push_back(area);
    simulation::CarCargo car(&flat, {3.0f, 4.0f, 25.0f});
    formats::CargoObject container;
    container.id = "Container1_1";
    container.loading = formats::cargo_container;
    container.size = formats::CargoBox{{-1.3f, 0.0f, -6.05f}, {1.3f, 2.8f, 6.05f}};
    container.mass = 5000.0f;
    std::string why;
    check(car.place_objects(container, 304, 5000.0f, 1, 0, simulation::CargoPlacement::front, true, &why) == 1, "one container");
    check(car.loads().size() == 1 && near(car.loads()[0].position[2], 12.26f - 6.05f) && near(car.loads()[0].position[1], 1.3f),
          "Front: at the front end, on the floor");
    check(car.place_objects(container, 304, 5000.0f, 1, 0, simulation::CargoPlacement::front, true, &why) == 1, "a second container");
    check(car.loads().size() == 2 && near(car.loads()[1].position[2], 12.26f - 6.05f - 12.1f), "the second behind the first");
    check(car.place_objects(container, 304, 5000.0f, 1, 0, simulation::CargoPlacement::center, true, &why) == 0
              && why == "Failed to place cargo in the selected area",
          "no room for a third");
    car.unload_all();
    check(car.empty(), "unload all");

    formats::CargoObject rails;
    rails.id = "zaprail";
    rails.loading = formats::cargo_stack;
    rails.size = formats::CargoBox{{-1.0f, 0.0f, -6.25f}, {1.0f, 0.5f, 6.25f}};
    rails.stackable = 9;
    check(car.place_objects(rails, 57, 50.0f, 3, 0, simulation::CargoPlacement::center, true, &why) == 3, "three rails");
    check(car.loads().size() == 1 && car.loads()[0].count == 3 && near(car.load_mass(), 150.0f), "stacked, Stackable 9");
    check(car.place_objects(rails, 57, 50.0f, 2, 0, simulation::CargoPlacement::manual, true, &why) == 1, "manual: one item");

    formats::WagonCargoDescription light = flat;
    light.maximum_weight = 50.0f; // 3 x 4 x 25 = 300 m³: 15 t
    simulation::CarCargo weak(&light, {3.0f, 4.0f, 25.0f});
    check(weak.place_objects(container, 304, 20000.0f, 1, 0, simulation::CargoPlacement::center, true, &why) == 0
              && why == "Wagon maximum weight limit reached",
          "too heavy");
}

// A state's line `<key> <value>` set to another value (the tank emptied).
std::string with_value(const std::string& text, const std::string& key_tail, const std::string& value) {
    std::istringstream in(text);
    std::string out;
    for (std::string line; std::getline(in, line);) {
        const auto space = line.find(' ');
        const auto key = line.substr(0, space);
        if (key.size() >= key_tail.size() && key.compare(key.size() - key_tail.size(), key_tail.size(), key_tail) == 0) line = key + " " + value;
        out += line + "\n";
    }
    return out;
}

void engines() {
    formats::CargoKinds kinds;
    formats::WagonDefinition diesel;
    diesel.name = "test diesel";
    diesel.mass = 100000.0f;
    diesel.length = 18.0f;
    diesel.engine.emplace();
    diesel.engine->kind = formats::EngineKind::diesel;
    diesel.engine->max_diesel_level = 6000.0f;
    diesel.engine->max_sanding_time = 120.0f;
    simulation::Train train({{.definition = &diesel}});
    check(near(train.cars()[0].fuel_left, 6000.0f) && near(train.cars()[0].sand_left, 120.0f), "a new diesel: full of fuel and sand");
    check(train.add_fuel(0, 10.0f) == 0.0f, "a full tank takes none");

    formats::PickupDescription rack;
    rack.type = 7;
    rack.speed = 50.0f; // l/s
    simulation::Pickup fuel(rack, kinds);
    fuel.toggle(false);
    fuel.toggle(false);
    std::vector<simulation::PickupCar> at{{.speed = 0.0f, .area = 0, .in_reach = true, .train = &train, .index = 0, .engine = true, .diesel = true}};
    auto step = fuel.step(1.0f, at);
    check(step.engine && step.volume == 0.0f && fuel.state() == simulation::PickupState::done, "full: the rack is done");

    simulation::StateWriter out;
    train.save_state(out);
    train.restore_state(simulation::StateReader(with_value(out.text(), "fuel", "1000")));
    check(near(train.cars()[0].fuel_left, 1000.0f), "the fuel kept in a saved state");
    fuel.toggle(false);
    fuel.toggle(false);
    fuel.toggle(false);
    step = fuel.step(2.0f, at);
    check(step.engine && near(step.volume, 100.0f) && near(train.cars()[0].fuel_left, 1100.0f), "the rack fills the tank at its speed");

    formats::PickupDescription box;
    box.type = 10;
    box.speed = 5.0f;
    simulation::Pickup sand(box, kinds);
    sand.toggle(false);
    sand.toggle(false);
    train.restore_state(simulation::StateReader(with_value(out.text(), "sand", "100 -1")));
    step = sand.step(2.0f, at);
    check(step.engine && near(step.volume, 10.0f) && near(train.cars()[0].sand_left, 110.0f), "the sand box fills the sand");
    for (int i = 0; i < 5; ++i) sand.step(2.0f, at);
    check(near(train.cars()[0].sand_left, 120.0f) && sand.state() == simulation::PickupState::done, "up to MaxSandingTime");

    // A diesel rack with no diesel at it fills a tank car instead.
    at[0].engine = at[0].diesel = false;
    formats::WagonCargoDescription tank;
    tank.loading = formats::cargo_liquid;
    tank.capacity = 0.5f;
    simulation::CarCargo tanker(&tank, {3.0f, 4.0f, 10.0f});
    at[0].cargo = &tanker;
    at[0].train = nullptr;
    simulation::Pickup oil(rack, kinds);
    oil.toggle(false);
    oil.toggle(false);
    oil.step(1.0f, at);
    check(!tanker.empty() && tanker.loads()[0].cargo == kinds.find("diesel")->code, "no diesel there: the tank car takes diesel");
}

} // namespace

int main() {
    bulk_loading();
    objects();
    engines();
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
