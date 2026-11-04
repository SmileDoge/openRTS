#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

// A route's carspawn.dat: the road vehicles its car spawners send out
// (rts1.exe 0x492BE4 reads it with the road database).
namespace openrts::formats {

struct CarSpawnType {
    std::string shape;          // in the route's SHAPES (else GLOBAL\SHAPES)
    float distance = 5.0f;      // the second value: room the car takes on the road, m (5 when not positive)
    float spread = 1.0f;        // CarSpread: weight when picking a type
    float acceleration = 0.5f;  // CarAcceleration ( accel decel ), m/s^2
    float deceleration = 1.0f;
    float max_velocity = 33.0f; // CarMaxVelocity, m/s
    std::string sound;          // CarSoundFile, in the route's SOUND
};

struct CarSpawnList {
    std::vector<CarSpawnType> types;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] CarSpawnList parse_car_spawn_list(std::string_view bytes);
// `route`: ROUTES/<name>; no file gives an empty list.
[[nodiscard]] CarSpawnList load_car_spawn_list(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
