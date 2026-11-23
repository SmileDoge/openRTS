#pragma once

// Cars as entities (docs/car-architecture.md): a car's components, plain
// data kept by its CarId in its world's CarRegistry. They live as long as
// the car: a coupling, a cut, a train handed over do not touch them; the car
// released, they go with it. Step 2: the doors, the lights.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <openrts/simulation/train_lights.hpp>

namespace openrts::simulation {

// One of the car's outer doors with its own animation (the shape .sd's
// ExteriorDoors whose node the shape has).
struct CarDoor {
    std::string name;       // the .sd's door name (a cab door of the same name works it)
    std::size_t index{};    // in the .sd's ExteriorDoors (the DLL host's door order)
    std::uint32_t group{};  // GlobalID: 1 the right doors, 2 the left ones, 0 none
    float time = 1.0f;      // seconds from shut to open (AnimTime)
    float open{};           // 0 shut .. 1 open
    std::optional<bool> wanted; // the engineer opened or shut it from outside (no cab door of its name)
};

// The car's doors: the outer ones (its shape's), their passenger groups.
struct CarDoors {
    std::vector<CarDoor> doors;
    // Every ExteriorDoor's GlobalID, in .sd order (a door without a node of
    // its own too: the DLL's door states go by this order).
    std::vector<std::uint32_t> groups;
    // Bit n: the doors of GlobalID n + 1 open, as another computer's train
    // sends them (0x45FC) or a test sets them.
    std::uint8_t groups_open{};

    // A door's leaf moves towards `target` (open or shut) for `elapsed` s.
    void step(std::size_t k, bool target, float elapsed);
    // The groups open as drawn (a door half open or more), bit n GlobalID n + 1.
    [[nodiscard]] std::uint8_t open_groups() const noexcept;
    // GlobalID `group` is set in groups_open.
    [[nodiscard]] bool group_sent_open(std::uint32_t group) const noexcept;
    [[nodiscard]] bool has_group(std::uint32_t group) const noexcept;
    // The door of .sd index `index` (none: no node of its own).
    [[nodiscard]] const CarDoor* at_index(std::size_t index) const noexcept;
    [[nodiscard]] CarDoor* at_index(std::size_t index) noexcept;
};

// The car's lights (its definition's Lights, by LightID).
struct CarLights {
    std::vector<LightRunner> runners; // fading and the States shown
    // Each light's wanted state at the last update, for the network.
    std::vector<std::uint8_t> lit;
    // Another computer's car: its lights as its owner sends them.
    std::vector<std::uint8_t> sent;

    // Light `k` sent on or off (the list grows to it).
    void set_sent(std::size_t k, bool on);
    // The lights shining (fading in or out too).
    [[nodiscard]] std::size_t shining() const noexcept;
};

// Everything a car has besides its physics (that is the train's CarState).
struct CarComponents {
    CarDoors doors;
    CarLights lights;
};

} // namespace openrts::simulation
