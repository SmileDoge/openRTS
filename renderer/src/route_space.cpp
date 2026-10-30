#include <openrts/renderer/route_space.hpp>

#include <cmath>
#include <cstdint>

#include <glm/gtc/quaternion.hpp>

namespace openrts::renderer {

glm::mat4 placement_transform(const formats::WorldObjectPlacement& object, const formats::RoutePosition& origin) {
    constexpr double tile_size = formats::route_tile_size;
    const auto& q = object.rotation;

    // QDirection is a rotation in MSTS's left-handed frame; applied to the
    // same left-handed coordinates with right-handed quaternion maths it is
    // the conjugate. Checked on Kuban_RTS: oblique straight track pieces end
    // at the next piece's origin only with the conjugate.
    auto result = glm::mat4_cast(glm::quat{q[3], -q[0], -q[1], -q[2]});
    const auto& position = object.position;
    result[3] = glm::vec4{
        static_cast<float>((position.tile_x - origin.tile_x) * tile_size + position.x - origin.x),
        static_cast<float>(position.y - origin.y),
        static_cast<float>((position.tile_z - origin.tile_z) * tile_size + position.z - origin.z),
        1.0f,
    };
    return result;
}

formats::RoutePosition route_position(const glm::vec3& local, const formats::RoutePosition& origin) {
    constexpr double tile_size = formats::route_tile_size;
    const auto tile_offset = [](const double coordinate) {
        return static_cast<std::int32_t>(std::floor((coordinate + tile_size * 0.5) / tile_size));
    };

    const auto offset_x = tile_offset(origin.x + local.x);
    const auto offset_z = tile_offset(origin.z + local.z);
    return {
        .tile_x = origin.tile_x + offset_x,
        .tile_z = origin.tile_z + offset_z,
        .x = origin.x + local.x - offset_x * tile_size,
        .y = origin.y + local.y,
        .z = origin.z + local.z - offset_z * tile_size,
    };
}

} // namespace openrts::renderer
