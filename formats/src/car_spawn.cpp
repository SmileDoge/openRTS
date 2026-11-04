#include <openrts/formats/car_spawn.hpp>

#include "text_tree.hpp"

namespace openrts::formats {

CarSpawnList parse_car_spawn_list(const std::string_view bytes) {
    CarSpawnList list;
    std::string error;
    const auto text = detail::decode_text(bytes, error);
    if (!text) {
        list.error = error;
        return list;
    }
    detail::TextNode root;
    detail::read_text_tree(*text, root);
    // CarSpawnerItem ( "shape.s" distance [CarSpread ( n ) CarAcceleration ( a d )
    // CarMaxVelocity ( v ) CarSoundFile ( "x.sms" )] ), defaults and limits as 0x492BE4.
    for (const auto& item : root.children) {
        if (item.name != "carspawneritem" || item.values.empty()) continue;
        CarSpawnType type;
        type.shape = item.values[0];
        type.distance = static_cast<float>(item.number(1, 0.0));
        if (type.distance <= 0.0f) type.distance = 5.0f;
        if (const auto* spread = item.child("carspread")) type.spread = static_cast<float>(spread->number(0, 0.0));
        if (type.spread <= 0.0f) type.spread = 1.0f;
        if (const auto* acceleration = item.child("caracceleration")) {
            type.acceleration = static_cast<float>(acceleration->number(0, 0.0));
            type.deceleration = static_cast<float>(acceleration->number(1, 0.0));
        }
        if (type.acceleration <= 0.0f) type.acceleration = 0.5f;
        if (type.deceleration <= 0.0f) type.deceleration = 1.0f;
        if (const auto* velocity = item.child("carmaxvelocity")) type.max_velocity = static_cast<float>(velocity->number(0, 0.0));
        if (type.max_velocity <= 0.0f) type.max_velocity = 33.0f;
        if (const auto* sound = item.child("carsoundfile"); sound && !sound->values.empty()) type.sound = sound->values[0];
        list.types.push_back(std::move(type));
    }
    return list;
}

CarSpawnList load_car_spawn_list(const core::Vfs& vfs, const core::VirtualPath& route) {
    const auto file = route.resolve(std::string_view("carspawn.dat"));
    if (!file) return {};
    const auto bytes = vfs.read(*file);
    if (!bytes) return {};
    return parse_car_spawn_list(*bytes);
}

} // namespace openrts::formats
