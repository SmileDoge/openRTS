#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>

// RTS route electrification (docs/electrification.md, docs/reverse/train-dll.md
// §14): `<route>/<RouteID>.electrotype` (SIMRTS…JINX0D0e: kinds of supply,
// catenary types, substations) and `<route>/<RouteID>.electrodb`
// (SIMRTS…JINX0D1e: electrosections with their power sources, and which
// track nodes carry which section and which catenary). Both UTF-16 text.
namespace openrts::formats {

// ElectricSupplyType: a kind of current (RTS 0x491668).
struct ElectricSupplyType {
    std::uint32_t id{};
    std::string name;
    std::uint32_t colour{};   // ARGB as written (ff0000ff)
    float voltage{};          // V
    float frequency{};        // Hz, 0 for DC
    float voltage_drop{};     // V per km from the feeder
};

// CatenaryType: a kind of overhead line.
struct CatenaryType {
    std::uint32_t id{};
    std::string name;
    std::uint32_t colour{};
    float position[2]{};      // Position ( x height ) of the wire over the track, m
    std::uint32_t type{};
    std::uint32_t num_wires{};
    float burn_q{};           // BurnQ
};

struct ElectricSubstation {
    std::uint32_t id{};
    std::string name;
    std::uint32_t electro_type{}; // ElectricSupplyType id
    RoutePosition position;
    std::uint32_t phases{};       // PhaseNum
    float power{};                // MW
    float voltage{};              // V
    float power_decline[2]{};
};

// PowerSource of an electrosection (RTS 0x491DC8, 32 bytes).
struct ElectroPowerSource {
    std::uint32_t substation{};   // ElectricSubstation id
    std::uint32_t phase{};
    // Active ( n ): bit 0 the source is switched in, bit 1 it is enabled.
    // CurrentSource sets bit 0 on the chosen one only and bit 1 on all.
    std::uint32_t active{};
    std::optional<RoutePosition> feeder; // FeederPos
    bool use_substation_position = false; // UseSubstationPos: the voltage drop counts from the substation
    float feeder_q = 100.0f;      // FeederQ, % of the substation voltage
};

struct ElectroSection {
    std::uint32_t id{};
    std::string name;
    std::uint32_t basic_catenary_type{};
    std::vector<ElectroPowerSource> sources;
    int current_source = -1;      // CurrentSource, -1 when not given (or out of range)
};

// Electricity piece of a track node: from `start` metres on, the node is in
// electrosection `section` (0: none, a dead stretch).
struct ElectricityPiece {
    std::uint32_t section{};
    float start{};
    std::uint32_t flags{};
};

// Catenary piece of a track node: wire of `type` over [start, start + length].
// A node's only piece with start <= 0 and length < 0 covers the whole node.
struct CatenaryPiece {
    std::uint32_t type{};
    float start{};
    float length{};
    float unknown{};  // the fourth value (-1 in the installed routes)
    std::uint32_t flags{};
};

struct NodeElectrification {
    std::uint32_t node{}; // TDB track node (Section ( n ), 1-based like TrackNode)
    std::vector<ElectricityPiece> electricity;
    std::vector<CatenaryPiece> catenary;
};

struct ElectricTypes {
    std::vector<ElectricSupplyType> supply_types;
    std::vector<CatenaryType> catenary_types;
    std::vector<ElectricSubstation> substations;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

struct ElectroDatabase {
    std::vector<ElectroSection> sections;
    std::vector<NodeElectrification> nodes;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] ElectricTypes parse_electric_types(std::string_view bytes);
[[nodiscard]] ElectroDatabase parse_electro_database(std::string_view bytes);

struct RouteElectrification {
    bool electrified = false;     // .trk Electrified
    bool has_types = false;       // an .electrotype was read
    bool has_database = false;    // an .electrodb was read
    ElectricTypes types;
    ElectroDatabase database;
    std::string error;            // a file that exists but did not read
};

// The route's files, named after its RouteID as RTS builds them, else the
// only .electrotype / .electrodb of the folder.
[[nodiscard]] RouteElectrification load_route_electrification(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
