#pragma once

// Cars as entities (docs/car-architecture.md): a car's components, plain
// data kept by its CarId in its world's CarRegistry. They live as long as
// the car: a coupling, a cut, a train handed over do not touch them; the car
// released, they go with it. Step 2: the doors, the lights, the
// pantographs. Step 3: the cab's (or salon's) own state.

#include <array>
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

// The car's pantographs (1..4), bit n - 1 for pantograph n.
struct CarPantographs {
    std::uint8_t wanted{}; // wanted up (the DLL's, the rules', the owner's)
    std::uint8_t heard{};  // as the sounds last knew them (no DLL: 45 / 46, 66 / 67)
    std::uint8_t sent{};   // another computer's car: as its owner sends them
    // How far each has risen, 0 down .. 1 up (its shape's animation: RTS
    // takes a pantograph as up when its animation has risen, 0x5031D8).
    std::array<float, 4> rise{};

    // Each one towards up or down by `wanted`, a whole rise in `raise_time`
    // s (PantographRaiseTime).
    void step(float elapsed, float raise_time);
};

// A door or window of the car's cab or salon (its .sd's InteriorDoors,
// InteriorWindows), by position there.
struct CabOpening {
    float open{};        // 0 shut .. 1 open
    bool target = false; // where it is going
};

// A FreeAnimation the DLL runs (the wipers, the ЭП20's mirrors): where it
// is in its round.
struct CabAnimation {
    float phase{};       // 0 .. 1 of a round
    bool down = false;   // going back towards frame 0
    bool moving = false;
};

// The cab's or salon's state (cars as entities, step 3): what the engineer
// and the DLL left it as, kept with the car whatever view draws it (a cab
// left, come back to, drawn anew: as it was). The switches' states are the
// DLL's (its host keeps them by the car).
struct CarCab {
    bool made = false; // its .sd's starting states taken (the first view of it)
    std::vector<CabOpening> doors, windows;
    std::vector<float> blink_clock; // by display: DisplayFade's clock (RTS display +72)
    std::vector<CabAnimation> animations;
};

// Everything a car has besides its physics (that is the train's CarState).
struct CarComponents {
    CarDoors doors;
    CarLights lights;
    CarPantographs pantographs;
    CarCab cab;
};

} // namespace openrts::simulation
