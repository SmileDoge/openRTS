#pragma once

// Cargo and loading as RTS has them (docs/reverse/cargo.md): the cargo
// kinds (built into rts1.exe, plus openRTS's own from openRTSCargo.dat),
// the cars' loading (RTSCargo.dat's WagonTypes, a car's own
// WagonCargoDescription), the cargo objects (RTSObjects.dat) and the
// pickups (a shape's ESD_Pickup_Object, a world Pickup).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

// CargoLoadingType as bits (rts1.exe's dialog filters on them: 1 pile,
// 2 liquid, 4 container, 8 stack, 0x20 box, 0x100 fridge; gas 0x10 and dump
// 0x80 as its loading code uses them; energy openRTS's own bit).
enum CargoLoading : std::uint32_t {
    cargo_pile = 0x1,
    cargo_liquid = 0x2,
    cargo_container = 0x4,
    cargo_stack = 0x8,
    cargo_gas = 0x10,
    cargo_box = 0x20,
    cargo_energy = 0x40,
    cargo_dump = 0x80,
    cargo_fridge = 0x100,
};

// One loading type by its name (PILE, CONTAINER ...), any case; 0 unknown.
[[nodiscard]] std::uint32_t cargo_loading_bit(std::string_view name);

// A kind of cargo. RTS's codes (0x45B930) and densities (0x50CD6C); its
// pile shape factor (0x50D1F0); openRTS's own kinds from 1000 on.
struct CargoKind {
    std::uint16_t code{};
    std::string id;           // as files name it: coal, iron_ore, ASH ...
    std::string name;         // English, a tr() key for RTS's own
    std::string russian_name; // openRTS's own kinds (their file's RussianName)
    float density{};          // kg/m³, 0: not bulk (objects, energy)
    float pile = 1.0f;        // pile shape factor (RTS 0x50D1F0)
    std::uint32_t loading{};  // how it is carried (CargoLoading bits)
    std::string texture;      // the texture's stem: RTSCargo_coal, oRTSCargo_ash
    bool own = false;         // openRTS's, not RTS's
};

// The kinds RTS knows, then openRTS's own read from openRTSCargo.dat.
class CargoKinds {
public:
    CargoKinds(); // RTS's built-in kinds only
    [[nodiscard]] const CargoKind* find(std::uint16_t code) const;
    [[nodiscard]] const CargoKind* find(std::string_view id) const; // any case
    [[nodiscard]] const std::vector<CargoKind>& all() const noexcept { return kinds_; }
    // openRTS's own kinds: CargoTypes ( Cargo ( ID ( ASH ) Name ( "Ash" )
    // RussianName ( "Зола" ) Density ( 1000 ) Pile ( 0.5 ) LoadingType ( PILE )
    // Texture ( oRTSCargo_ash ) ) ). A kind with an RTS id replaces nothing;
    // returns the number added.
    std::size_t add_own(std::string_view bytes, std::string* error = nullptr);

private:
    std::vector<CargoKind> kinds_;
};

// The kinds with openRTS's own from TRAINS/TRAINSET/DEFAULT/openRTSCargo.dat.
[[nodiscard]] CargoKinds load_cargo_kinds(const core::Vfs& vfs);

// A box as RTS writes it (x1 y1 z1 x2 y2 z2), the corners sorted.
struct CargoBox {
    std::array<float, 3> low{}, high{};
    [[nodiscard]] float volume() const noexcept {
        return (high[0] - low[0]) * (high[1] - low[1]) * (high[2] - low[2]);
    }
    [[nodiscard]] bool contains(const std::array<float, 3>& p) const noexcept {
        return p[0] >= low[0] && p[0] <= high[0] && p[1] >= low[1] && p[1] <= high[1] && p[2] >= low[2] && p[2] <= high[2];
    }
};

struct CargoLoadingArea {
    CargoBox box;
    std::string parent_node;              // ParentNode: the shape's node it is loaded into
    std::optional<std::array<float, 3>> pivot; // AreaPivot
    std::string dump_side;                // DumpFromSide: bottom, left ... (lower case)
    std::uint32_t loading{};              // its own LoadingType bits, 0: the car's
    bool invisible = false;               // filled without showing the load
};

struct CargoIntakePoint {
    std::array<float, 3> position{};
    float tolerance{};
};

struct CargoDumpPoint {
    std::array<float, 3> position{};
    float width{};
};

// A LoadingAnimation (RTS's reference: "the animation played while loading
// or unloading: hatches opening, a dump car tipping"): the shape's nodes
// played to their last frame (or AnimFrame) over AnimTime, for the areas and
// intake points it names (bits, 0: all).
struct CargoLoadingAnimation {
    std::uint32_t loading_unloading = 3; // LoadingUnloading: 1 loading, 2 unloading (PROBABLE)
    std::string side;                    // LoadingSide (lower case)
    std::uint32_t areas{};               // ForAreas
    std::uint32_t intakes{};             // ForIntakePoints
    std::vector<std::string> nodes;      // Nodes ( n name ... )
    std::uint32_t loading{};             // ForLoadingType bits, 0: any
    int frame = -1;                      // AnimFrame, -1: the node's last
    float time = 2.0f;                   // AnimTime, s
};

// A WagonType of RTSCargo.dat, or a car's WagonCargoDescription.
struct WagonCargoDescription {
    std::string name;
    std::vector<std::string> patterns;  // WagonName's Pattern ( "*Hopper*e.wag" ), lower case
    std::vector<std::string> filenames; // its Filename ( ... ), lower case
    std::uint32_t loading{};            // LoadingType bits
    std::vector<CargoIntakePoint> intakes;
    std::vector<CargoDumpPoint> dumps;
    std::vector<CargoLoadingArea> areas;
    std::vector<CargoLoadingAnimation> animations; // LoadingAnimation (WagonType's and its areas')
    float capacity{};        // Capacity: a part of the car's Size box (no areas)
    float maximum_weight{};  // MaximumWeight: kg per m³ of the Size box
    float dump_speed{};      // DumpSpeed, m³/s
    float dump_max_velocity = -1.0f; // DumpMaxVelocity, m/s (-1: none given)
    bool use_bounding_box = false;

    // Whether the car file (its name with .wag / .eng) is one of these.
    [[nodiscard]] bool matches(std::string_view file_name) const;
};

struct CargoDescriptions {
    std::vector<WagonCargoDescription> wagons;
    std::string error;

    // The first WagonType a car file matches (as RTS walks its list).
    [[nodiscard]] const WagonCargoDescription* find(std::string_view file_name) const;
};

// Appends the WagonType blocks of an RTSCargo.dat (only those: RTS 0x4D77E4
// reads no CargoTypes).
void parse_cargo_descriptions(std::string_view bytes, CargoDescriptions& into);
// RTSCargo.dat, then RTSCargoCustom.dat, from TRAINS/TRAINSET/DEFAULT.
[[nodiscard]] CargoDescriptions load_cargo_descriptions(const core::Vfs& vfs);
// A .wag's own WagonCargoDescription block (none in the build's cars).
[[nodiscard]] std::optional<WagonCargoDescription> parse_wagon_cargo_description(std::string_view wag_bytes);

// A CargoObject of RTSObjects.dat: a piece loaded onto cars.
struct CargoObject {
    std::string id; // as written
    std::string name;
    std::string description;
    std::uint32_t loading{}; // LoadingType bits
    std::string cargo;       // CargoType, lower case
    std::string shape;       // its .s, in TRAINS/OBJECTS/CARGO
    std::optional<CargoBox> size;
    float volume{};
    std::uint32_t stackable{}; // how many high
    float mass{};              // kg
    float mass_maximum{};      // kg

    // Its footprint and height (from Size, else a cube of Volume).
    [[nodiscard]] std::array<float, 3> extent() const noexcept;
};

// Appends the CargoObjects of an RTSObjects.dat (a later one with the same
// id replaces the earlier: the route's over the global).
void parse_cargo_objects(std::string_view bytes, std::vector<CargoObject>& into, std::string* error = nullptr);
// TRAINS/OBJECTS/RTSObjects.dat, then the route's RTSObjects.dat.
[[nodiscard]] std::vector<CargoObject> load_cargo_objects(const core::Vfs& vfs, const std::optional<core::VirtualPath>& route);

// What a pickup is: a shape's ESD_Pickup_Object, or a world Pickup's fields.
struct PickupDescription {
    std::uint16_t type{};     // PickupType (docs/reverse/cargo.md's table)
    float type_value{};       // its second value
    float speed_min{}, speed_max{}; // SpeedRange, m/s
    float capacity{};         // PickupCapacity: its stock (or a socket's VA)
    float speed{};            // and its rate (m³/s; a socket's volts)
    std::string cargo;        // CargoType, lower case ("" none)
    float length{};           // PickupLength, m
    float margin{};           // PlacementMargin, m
    std::vector<std::array<float, 3>> points; // PickupPoints, in the shape
    std::optional<CargoBox> area;             // PickupArea, in the shape
    struct Item {
        std::uint32_t loading{}; // its CargoLoadingType bit
        std::string cargo;       // a kind's id, lower case
        std::string object;      // a CargoObject's id ("" for bulk)
        int count = -1;          // -1: no limit
        float mass = -1.0f;      // kg, -1: no limit
    };
    std::vector<Item> table;  // CargoTable
    int crane_type{};         // PickupCraneType: 1 bridge, 2 jib, 3 platform
    int crane_oriented{};     // PickupCraneOriented: 0 Z, 1 X, 2 -Z, 3 -X
    struct CraneNodes {
        std::string bridge, cabin, winch, hook;
        float hook_height{};
    };
    std::optional<CraneNodes> crane_nodes; // PickupCraneShapeNodes
    bool from_world = false;  // a world Pickup's (MSTS), not a shape's
};

// What loading types a pickup of this type offers (rts1.exe 0x4E59A0:
// 12 container, stack, box; 14 pile, stack; 15 stack, box, fridge; the bulk
// and liquid ones their cargo's).
[[nodiscard]] std::uint32_t pickup_loading(std::uint16_t type);
// The English name of a pickup type (a tr() key).
[[nodiscard]] const char* pickup_type_name(std::uint16_t type);

} // namespace openrts::formats
