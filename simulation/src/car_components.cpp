#include <openrts/simulation/car_components.hpp>

#include <algorithm>

namespace openrts::simulation {

void CarDoors::step(const std::size_t k, const bool target, const float elapsed) {
    if (k >= doors.size()) return;
    auto& door = doors[k];
    const float goal = target ? 1.0f : 0.0f;
    const float step = elapsed / door.time;
    door.open = goal > door.open ? (std::min)(goal, door.open + step) : (std::max)(goal, door.open - step);
}

std::uint8_t CarDoors::open_groups() const noexcept {
    std::uint8_t out = 0;
    for (const auto& door : doors)
        if (door.group >= 1 && door.group <= 8 && door.open >= 0.5f) out |= static_cast<std::uint8_t>(1u << (door.group - 1));
    return out;
}

bool CarDoors::group_sent_open(const std::uint32_t group) const noexcept {
    return group >= 1 && group <= 8 && ((groups_open >> (group - 1)) & 1u) != 0;
}

bool CarDoors::has_group(const std::uint32_t group) const noexcept {
    return std::find(groups.begin(), groups.end(), group) != groups.end();
}

const CarDoor* CarDoors::at_index(const std::size_t index) const noexcept {
    for (const auto& door : doors)
        if (door.index == index) return &door;
    return nullptr;
}

CarDoor* CarDoors::at_index(const std::size_t index) noexcept {
    for (auto& door : doors)
        if (door.index == index) return &door;
    return nullptr;
}

} // namespace openrts::simulation
