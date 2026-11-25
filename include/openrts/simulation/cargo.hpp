#pragma once

// Cargo on cars and the pickups that load them, as RTS does it
// (docs/reverse/cargo.md, rts1.exe 0x4E59A0 and its helpers). Geometry-free:
// the caller says which car is in a pickup's reach and where; this keeps the
// rules so a server can run them as well as the game.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <openrts/formats/cargo.hpp>

namespace openrts::simulation {

class Train;

// One load on a car: a bulk cargo in an area, or cargo objects.
struct CarLoad {
    std::uint16_t cargo{};      // the kind's code (formats::CargoKind), 0 none
    std::string object;         // a CargoObject's id; "" for bulk
    int area{};                 // 1..n the car's loading area, 0 the car as a whole
    float volume{};             // m³ of bulk
    float mass{};               // kg, all of it (count objects)
    int count = 1;              // objects stacked here
    std::array<float, 3> position{}; // objects: the stack's base centre, in the car
    std::array<float, 3> extent{};   // objects: one object's size
    // Seen, not kept: how many of the stack's top objects are still on a
    // crane's hook (not drawn in place), and how high over its place the
    // lowest of them hangs, m (the crane's lowering, cargo_view.hpp).
    int arriving{};
    float lift{};
};

// Where the crane puts an object along an area (the "Load cargo" dialog's
// Placement: Center, Front, Back, Manual).
enum class CargoPlacement : std::uint8_t { center, front, back, manual };

// A car's cargo: its loading description (RTSCargo.dat's WagonType), its
// Size box and what lies in it.
class CarCargo {
public:
    CarCargo() = default;
    CarCargo(const formats::WagonCargoDescription* description, std::array<float, 3> size);

    [[nodiscard]] const formats::WagonCargoDescription* description() const noexcept { return description_; }
    [[nodiscard]] bool loadable() const noexcept { return description_ != nullptr; }
    [[nodiscard]] std::uint32_t loading() const noexcept;
    [[nodiscard]] const std::vector<CarLoad>& loads() const noexcept { return loads_; }
    [[nodiscard]] bool empty() const noexcept { return loads_.empty(); }

    // The room for bulk (rts1.exe 0x501D80): an area's box (1..n), else the
    // areas' summed volume, else Capacity times the Size box.
    [[nodiscard]] float room(int area = 0) const;
    [[nodiscard]] float bulk_volume(int area = 0) const;
    // How full, 0..100 (RTS's percent, by volume or by mass, the larger).
    [[nodiscard]] float fill_percent() const;
    // The load's mass and its limit (MaximumWeight times the Size box; 0
    // none).
    [[nodiscard]] float load_mass() const;
    [[nodiscard]] float mass_limit() const;
    // The area a bulk load falls in at car-local z (with `margin`), 1..n; 0
    // the car as a whole (no areas); nullopt none there.
    [[nodiscard]] std::optional<int> area_at(float z, float margin) const;

    // Adds bulk of `kind` (positive) or takes it (negative), capped by the
    // room and the mass limit; returns the volume moved (signed).
    float move_bulk(const formats::CargoKind& kind, float volume, int area = 0);
    // Places `count` objects; returns how many went on, and why the rest
    // did not ("Wagon maximum weight limit reached", "Failed to place cargo
    // in the selected area").
    int place_objects(const formats::CargoObject& object, std::uint16_t cargo, float mass, int count, int area,
                      CargoPlacement placement, bool centred, std::string* why = nullptr);
    // Takes one load (its index in loads()), or every load of an area (0:
    // all).
    bool unload(std::size_t index);
    void unload_all(int area = 0);
    // A load as it was saved or relayed.
    void restore(std::vector<CarLoad> loads) { loads_ = std::move(loads); }
    // The loads to change in place (a crane's lowering: CarLoad::arriving
    // and lift only).
    [[nodiscard]] std::vector<CarLoad>& loads_seen() noexcept { return loads_; }
    // How far its LoadingAnimation has played (0 shut .. 1 open: hatches,
    // a dump car tipped), seen only; and which of them plays (1 loading,
    // 2 unloading).
    float opening{};
    std::uint32_t opening_for = 2;

private:
    [[nodiscard]] formats::CargoBox area_box(int area) const;
    [[nodiscard]] float box_volume() const noexcept { return size_[0] * size_[1] * size_[2]; }

    const formats::WagonCargoDescription* description_ = nullptr;
    std::array<float, 3> size_{}; // width, height, length
    std::vector<CarLoad> loads_;
};

// A pickup's state (rts1.exe pickup +65): Num3 cycles off, ready, active;
// Ctrl+Num3 unloads; done and empty are set by the loading.
enum class PickupState : std::uint8_t { off = 0, ready = 1, active = 2, done = 3, empty = 4, unloading = 5 };

// A car as a pickup sees it this frame.
struct PickupCar {
    CarCargo* cargo = nullptr;
    float speed{};             // |m/s|
    std::optional<int> area;   // bulk: the area under the pickup's point (0 the whole car), nullopt not under it
    bool in_reach = false;     // under it / beside it / in the crane's area
    // A locomotive: its train and place in it (a fuel rack fills a diesel,
    // a sand box any engine; rts1.exe 0x4E59A0 types 7 and 10).
    Train* train = nullptr;
    std::size_t index{};
    bool engine = false;
    bool diesel = false;
};

// What a pickup did this frame.
struct PickupStep {
    int car = -1;           // the car it worked on (index into the cars given)
    float volume{};         // m³ moved (+ loaded, - unloaded)
    bool pouring = false;   // bulk falling now (its stream drawn, its sound)
    bool too_fast = false;  // a car in reach outside SpeedRange
    bool unsuitable = false; // a car in reach that cannot take it
    bool engine = false;    // it fuelled or sanded a locomotive (volume: litres / seconds)
    bool dialog = false;    // a crane / platform with a car standing in reach: its dialog may open
};

class Pickup {
public:
    Pickup(formats::PickupDescription description, const formats::CargoKinds& kinds);

    [[nodiscard]] const formats::PickupDescription& description() const noexcept { return description_; }
    [[nodiscard]] PickupState state() const noexcept { return state_; }
    void set_state(PickupState state) noexcept { state_ = state; }
    // The cargo kind it loads (bulk and liquid pickups), else nullptr.
    [[nodiscard]] const formats::CargoKind* cargo() const noexcept { return cargo_; }
    // Its stock, kg; negative: no limit (RTS: CargoTable's mass, else none).
    [[nodiscard]] float stock() const noexcept { return stock_; }
    [[nodiscard]] bool crane() const noexcept;  // 12, 14, 15: the dialog
    [[nodiscard]] bool unloads() const noexcept; // 13 empties cars
    // Num3 (rts1.exe 0x4210FC): off, ready, active, off; `unload` (Ctrl)
    // asks the unloading state where the type allows it. Returns the text
    // RTS shows ("Pickup ready" ...), or nullptr.
    const char* toggle(bool unload);
    // One frame of loading (rts1.exe 0x4E59A0's bulk, liquid and trestle
    // branches; the cranes only report a standing car).
    PickupStep step(float dt, std::span<PickupCar> cars);
    // The stock after a crane took `mass` of an item (CargoTable's).
    void take(float mass);

private:
    formats::PickupDescription description_;
    const formats::CargoKind* cargo_ = nullptr;
    PickupState state_ = PickupState::off;
    float stock_ = -1.0f;
};

} // namespace openrts::simulation
