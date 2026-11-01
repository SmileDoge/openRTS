#pragma once

// Consists (.con) and rolling stock (.wag, .eng with their .rtswag / .rtseng
// companions and RTSInclude files), read as rts1.exe reads them: values,
// defaults and units follow the RTS loaders (0x4770E0 / 0x477928 consists,
// 0x44247C wagons and engines, 0x566944 file merging). See
// docs/reverse/rolling-stock.md.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

// ---- consists ---------------------------------------------------------------

struct ConsistEntry {
    bool engine = false;
    std::uint32_t uid{};
    std::string name;   // file stem, e.g. vl10k-1461a
    std::string folder; // TRAINSET subfolder
    bool flipped = false;
    bool cold = false;                  // engines
    std::optional<float> handbrake;     // HandbrakePercent
};

struct ConsistDefinition {
    std::string name;
    float max_velocity{};  // MaxVelocity, first value (m/s)
    float durability = 1.0f;
    std::vector<ConsistEntry> entries; // front to back
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] ConsistDefinition parse_consist(std::string_view bytes);
// `name` as RTS takes it (TRAINS\CONSISTS_RTS\<name>.con, then
// TRAINS\CONSISTS\<name>.con), or a virtual path to a .con file.
[[nodiscard]] ConsistDefinition load_consist(const core::Vfs& vfs, std::string_view name);
[[nodiscard]] std::optional<core::VirtualPath> find_consist(const core::Vfs& vfs, std::string_view name);

// ---- wagons and engines -----------------------------------------------------

enum class WagonKind : std::uint8_t { freight, passenger, engine, tender, unknown };
enum class EngineKind : std::uint8_t { electric, diesel, steam, unknown };

// One coupling end (RTS 40-byte block).
struct Coupling {
    std::uint8_t type{};  // 0 automatic, 1 bar, 2 chain, 3 other
    bool rigid = false;   // CouplingHasRigidConnection
    float stiffness_tension{}, stiffness_compression{}; // N/m
    float damping_tension{}, damping_compression{};     // N/m/s
    float break_tension{}, break_compression{};         // N
    float r0_tension{}, r0_compression{};               // m
    float velocity{};
};

// MSTS Friction: the first five values (the second row is not used by RTS).
struct Friction {
    float c1{}, e1{}, transition{}, c2{}, e2{};
};

struct BrakeEquipment {
    std::uint16_t equipment{}; // 1 triple_valve, 2 auxilary_reservoir, 4 ep_brake, 8 handbrake, 0x10 vacuum
    std::uint16_t system{};    // 1 air_single_pipe, 2 ep, 4 twin / charge pipe, 8 vacuum_single_pipe
    float max_handbrake_force{};        // N
    float max_brake_force{};            // N
    float triple_valve_ratio = 1.0f;
    float max_release_rate{};           // atm/s
    float max_application_rate{};       // atm/s
    float max_auxiliary_charging_rate{}; // atm/s
    float cylinder_pressure_for_max_force{}; // atm
    float train_pipe_leak_rate{};       // atm/s
};

struct EngineDefinition {
    EngineKind kind = EngineKind::unknown;
    float max_power{};            // W
    float max_force{};            // N
    float max_continuous_force{}; // N
    float max_velocity{};         // m/s (used for the overspeed penalty)
    float max_current{};
    float sanding_speed{};
    float main_reservoir_volume{};
    float dynamic_brake_max_force{};
    std::uint32_t throttle_notches{};
    std::string library_module; // EngExtended LibraryModule (legacy DLL)
    std::string cabin;
    std::uint32_t variable_stack{};
};

struct WagonDefinition {
    std::string name;
    WagonKind kind = WagonKind::unknown;
    std::string shape;          // RTSWagonShape, else WagonShape
    std::string freight_shape;  // FreightAnim
    float width{}, height{}, length{}; // Size, m
    float mass{};               // kg (RTS reads tonnes)
    float wheel_radius{};       // m
    std::uint8_t wheels{};      // NumWheels
    // Coupling blocks in file order; the second is a copy of the first when
    // the file has one. RTS uses [0] at the joint behind a car and [1] at the
    // joint ahead of it (swapped for a flipped car), see 0x505414.
    std::array<Coupling, 2> couplings;
    Friction friction;
    std::array<float, 4> adhesion{};
    float derail_rail_height{};
    float derail_rail_force = 240000.0f;
    float derail_buffer_force{};
    // Body on its springs (RTS 0x44247C, used by 0x4F5B34).
    double frame_spring_stiffness{}; // FrameSpringStiffness, default mass * 30
    double frame_spring_damping{};   // FrameSpringDamping, default mass * 1.25
    // FrameOscCorrection: lateral amplitude, lateral frequency, vertical
    // amplitude factors (default 1, negative read as 0).
    float frame_osc_correction[3]{1.0f, 1.0f, 1.0f};
    BrakeEquipment brakes;
    std::string sound;          // RTSExteriorSound, else Sound
    std::string cab_sound;      // RTSCabinSound
    std::optional<EngineDefinition> engine;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Parses one wagon / engine text (already merged with its companions).
[[nodiscard]] WagonDefinition parse_wagon(std::string_view bytes);
// TRAINS\TRAINSET\<folder>\<name>.wag or .eng with its .rtswag / .rtseng
// companion and RTSInclude files merged as RTS does.
[[nodiscard]] WagonDefinition load_wagon(const core::Vfs& vfs, std::string_view folder, std::string_view name,
                                         bool engine);
[[nodiscard]] core::VirtualPath wagon_path(std::string_view folder, std::string_view name, bool engine);

// RTS value readers, exposed for tests.
[[nodiscard]] double rts_quantity_value(std::string_view token);
[[nodiscard]] double rts_product_value(std::string_view token);

} // namespace openrts::formats
