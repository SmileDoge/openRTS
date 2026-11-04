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
#include <utility>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

// ---- consists ---------------------------------------------------------------

// FreightAnim of a consist entry (RTS "FreightAnim description"): a cargo or
// another object fastened to the car, with its mass.
struct ConsistFreight {
    std::string shape;                 // FreightAnimShape, relative to the car's folder
    std::uint32_t shape_location{};    // its second value
    std::array<float, 3> position{};   // FreightAnimPosition, in the car
    float mass{};                      // FreightAnimMass, kg
    float volume{};                    // FreightAnimVolume, m³
    std::string cargo_type;            // FreightAnimCargoType
    std::int32_t cargo_id{};           // FreightAnimCargoID
};

struct ConsistEntry {
    bool engine = false;
    std::uint32_t uid{};
    std::string name;   // file stem, e.g. vl10k-1461a
    std::string folder; // TRAINSET subfolder
    bool flipped = false;
    bool cold = false;                  // engines
    std::optional<float> handbrake;     // HandbrakePercent
    std::vector<ConsistFreight> freight; // FreightAnim blocks
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

// EngExtended EngineParameters / AuxLibraryParameters entry (names lower
// case, as RTS reads the file).
struct EngineParameter {
    std::string name;
    float value{};
    std::string text; // the quoted form, when the value is a string
};

// EngExtended AuxLibraries ( AuxLibrary ( ... ) ): an extra DLL run around
// the main one (docs/reverse/train-dll.md section 7).
// One state of a light (RTS keeps the first for a steady light).
struct LightState {
    std::uint32_t colour = 0xFFFFFFFFu; // LightColour, AARRGGBB
    float duration{};
    std::array<float, 3> position{};    // in the car's shape
    std::array<float, 3> azimuth{};     // degrees: [1] turns it about the vertical (180: backwards)
    std::array<float, 3> elevation{};   // degrees: [0] up / down
    float transition{};
    float radius{};                     // a glow's half size; a beam's reach, m
    float angle{};                      // a beam's half angle, degrees
    float source_radius{};
};

// An exterior light of a car (Lights / RTSLights). Type 0 is a glow sprite
// (LightTexture), 1 a spotlight, 2 a sphere, 3 orthogonal, 4 a cone: the
// light sources RTS's shaders take. A DLL switches them (LightObj Flags 8).
struct LightDefinition {
    std::uint8_t type{};
    // Conditions: Headlight, Control, Service, Unit, TimeOfDay, Weather.
    std::uint8_t headlight{}, control{}, service{}, unit{}, time_of_day{}, weather{};
    float fade_in{}, fade_out{}, cycle{};
    std::string texture;                // relative to TRAINS/TRAINSET
    std::vector<LightState> states;
    std::array<float, 3> attenuation{2.0f, 0.6f, 0.4f}; // kind (2 linear), start share, length share
    float spot_exponent = 64.0f;        // SpotExponent x 128
};

// AuxLibrarySoundType: AMBIENT (no position), WAGON (at the car, with its
// sound variables), ROUTEFORWAGON (a wagon sound looked up in the route's
// SOUND first, then next to the engine).
enum class AuxSoundType : std::uint8_t { ambient, wagon, route_for_wagon };

struct AuxLibraryDefinition {
    std::string name;   // AuxLibraryName
    std::string module; // AuxLibraryModule, relative to the .eng folder
    std::uint32_t variable_stack{};
    std::vector<EngineParameter> parameters;
    std::vector<std::pair<std::string, int>> calls; // AuxCall ( "run" order ), lower case
    std::string sound;  // AuxLibrarySound, relative to the .eng folder
    AuxSoundType sound_type = AuxSoundType::wagon;
    std::array<float, 3> sound_position{}; // AuxLibrarySoundPosition, in the car
    bool sound_external = false;    // AuxLibrarySoundExternal: fades with distance
    bool sound_own_stack = false;   // AuxLibrarySoundOwnStack: no Var1..8 from the engine
    bool sound_single_wagon = false; // AuxLibrarySoundSingleWagon: only with the player in the car
};

// One LineVoltage ( MinVoltage MaxVoltage MinFreq MaxFreq ) of
// WorksOnLineVoltages: V and Hz (0 0 for DC).
struct LineVoltageRange {
    float min_voltage{}, max_voltage{};
    float min_frequency{}, max_frequency{};
    [[nodiscard]] bool contains(const float voltage, const float frequency) const noexcept {
        return voltage >= min_voltage && voltage <= max_voltage && frequency >= min_frequency && frequency <= max_frequency;
    }
};

// DieselSpecialEffects ( ExhaustN ( x y z  dx dy dz  size  [RTS:] Colour
// Density Mass Magnitude Rate Dispersion Texture ) ) as rts1.exe 0x44247C
// reads it, its defaults and limits; the smoke itself: 0x50D3B4.
struct DieselExhaust {
    std::array<float, 3> position{};
    std::array<float, 3> direction{};
    float size{};                       // the nozzle's width, m
    std::uint32_t colour = 0xCF656577u; // ARGB
    float density{};
    float mass = 1.0f;                  // 0.5..5 when used
    std::array<float, 2> magnitude{};   // idle, full load
    std::array<float, 2> rate{};        // puffs a second: idle, full load
    float dispersion = 1.0f;
    std::string texture;                // relative to the engine's folder; empty: GLOBAL t_diesel_smoke.ace
};

struct EngineDefinition {
    EngineKind kind = EngineKind::unknown;
    float max_power{};            // W
    float max_force{};            // N
    float max_continuous_force{}; // N
    float max_velocity{};         // m/s (used for the overspeed penalty)
    float max_current{};
    float sanding_speed{};
    float max_sanding_time = -1.0f; // MaxSandingTime, s of sand (-1: unlimited)
    float main_reservoir_volume{};
    float dynamic_brake_max_force{};
    std::uint32_t throttle_notches{};
    std::string library_module; // EngExtended LibraryModule (legacy DLL), relative to the .eng folder
    std::uint32_t library_param{}; // its second value (Locomotive::LibParam)
    std::string cabin;          // EngExtended Cabin: the cab shape
    std::uint32_t cabin_param{};
    // EngExtended DoNotHideExtInCabin ( n ) & 3 (RTS 0x44247C): non-zero
    // keeps the exterior shape drawn while the player is in the cab.
    std::uint8_t keep_exterior_in_cab{};
    std::uint32_t variable_stack{};
    std::string series;         // LocomotiveSeries (CanWorkWith)
    std::uint32_t num_engines{}; // NumEngines
    std::uint32_t num_axles{};   // NumAxles
    std::uint32_t max_slaves{};  // MaxNumSlaveLoco
    std::array<std::uint8_t, 2> sme_cable{}; // SMECable ( front rear ): multiple-unit control cable
    std::array<std::uint8_t, 2> hv_bus{};    // HVBus ( front rear )
    // Pantographs without a DLL (rts1.exe 0x44247C, used by the AI logic in
    // 0x424488): bit n-1 is pantograph n.
    std::uint8_t exclude_pantographs{};  // ExcludePantograph ( n ), repeated: never raised
    std::uint8_t always_use_panto{};     // AlwaysUsePanto ( bitmask ): raised whatever the direction
    std::uint8_t lower_panto_on_stop{};  // LowerPantoOnStop ( bitmask ): lowered at a stop
    float raise_second_panto_below{};    // RaiseSecondPantoWhenSpeedBelow, m/s (0: never)
    // MaxLineCurrentVelocityIncrease ( MLC_B MLC_A ): the wire cools by
    // MLC_B + MLC_A * speed a second (negative read as none).
    std::array<float, 2> line_current_cooling{};
    // EngExtended ORTSTrainPlugin: a native openRTS plugin (train_plugin.h),
    // relative to the .eng folder. With it the legacy LibraryModule is not run.
    std::string train_plugin;
    std::vector<EngineParameter> parameters; // EngineParameters
    std::vector<AuxLibraryDefinition> aux_libraries;
    // EngExtended WorksOnLineVoltages: the supplies it works on, in file
    // order (RTS gives the middle of the first on a route without an
    // .electrotype, docs/electrification.md).
    std::vector<LineVoltageRange> line_voltages;
    float max_line_current{};          // MaxLineCurrent, A per raised pantograph
    float max_line_current_burn_time{}; // MaxLineCurrentCatenaryBurnTime, s
    // DieselSmokeEffect* (defaults for the exhausts' Magnitude and Rate).
    float smoke_initial_magnitude{}, smoke_max_magnitude{}, smoke_initial_rate{}, smoke_max_rate{};
    std::vector<DieselExhaust> exhausts;
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
    float pantograph_raise_time = 3.0f; // PantographRaiseTime, s (RTS default 3; <= 0 is 3)
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
    // Lights ( n Light ( ... ) ) and RTSLights: by LightID (RTS 0x44247C,
    // 0x50036C); an entry that was never given has no states.
    std::vector<LightDefinition> lights;
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
