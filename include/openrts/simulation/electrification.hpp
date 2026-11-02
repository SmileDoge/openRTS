#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/electrification.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/track_geometry.hpp>

// The overhead line as RTS gives it to electric engines (RTS 0x503CE4 with
// 0x5B53A8; docs/electrification.md, docs/reverse/train-dll.md §14): the
// voltage and frequency under a car, and the train's supply for its DLLs.
namespace openrts::simulation {

class TrackNetwork;
class Train;

struct LineSupply {
    float voltage{};   // V, 0: no voltage
    float frequency{}; // Hz, 0 for DC
};

enum class LineState : std::uint8_t {
    no_wire, // no catenary over the track here
    dead,    // a wire without voltage: a neutral or switched-off stretch
    live,
};

struct LinePoint {
    LineState state = LineState::no_wire;
    LineSupply supply;
    int section = -1; // index into data().database.sections
    int source = -1;  // the power source used, index into that section's sources
};

class Electrification {
public:
    enum class Mode : std::uint8_t {
        sections,    // an .electrodb: the electrosection under the car
        single_type, // an .electrotype only: its first supply type everywhere
        by_engine,   // Electrified in the .trk only: each engine the middle of its first WorksOnLineVoltages
        none,        // not electrified: no voltage
    };

    Electrification(formats::RouteElectrification data, const TrackNetwork& network);
    [[nodiscard]] static std::unique_ptr<Electrification> load(const core::Vfs& vfs, const core::VirtualPath& route,
                                                               const TrackNetwork& network);

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] const formats::RouteElectrification& data() const noexcept { return data_; }

    // The line over (node, offset); `where` is the point there (the voltage
    // drop counts from the feeder), `engine` the engine asking (by_engine,
    // and the automatic choice of a source).
    [[nodiscard]] LinePoint at(std::uint32_t node, double offset, const TrackPoint& where,
                               const formats::EngineDefinition* engine) const;

    // The train's supply as RTS sets it before the DLLs: the highest voltage
    // under its electric engines with a pantograph up (`raised[car]`), given
    // to every electric engine of the train (Train::set_line_supply). No
    // engine with a pantograph up: 0 V. Returns what it set.
    LineSupply apply(Train& train, const std::vector<bool>& raised) const;

    // The section's chosen source (the dispatcher's "Switch power source"):
    // the CurrentSource of the file at first; -1 lets the sources' Active
    // flags decide.
    [[nodiscard]] int current_source(std::size_t section) const;
    void set_current_source(std::size_t section, int source);
    // As RTS in an activity: a section with several sources and a chosen one
    // feeds the first source an engine works on (WorksOnLineVoltages order).
    bool automatic_sources = false;
    // openRTS option: the route's line ignored, every electric engine gets
    // its own voltage (as RTS on an electrified route without .electrotype),
    // e.g. to drive a DC engine on an AC route.
    bool own_voltage = false;

    // The supply type a section's chosen (else first) source gives; null when
    // it has none.
    [[nodiscard]] const formats::ElectricSupplyType* supply_type(std::size_t section) const;
    [[nodiscard]] const formats::ElectricSubstation* substation(std::uint32_t id) const;

    // The node in stretches for the map, in order along it.
    struct Stretch {
        double from{}, to{};
        LineState state = LineState::no_wire;
        int section = -1;
    };
    [[nodiscard]] std::vector<Stretch> stretches(std::uint32_t node) const;

    // Whether `engine` (electric, without a DLL: openRTS's default logic) can
    // take power from `supply`: some voltage of a kind one of its
    // WorksOnLineVoltages has (by the frequency: AC or DC), or of any kind
    // when it lists none. The level is a DLL's business: the lines run well
    // outside the listed ranges near substations (Chernyshevsk up to 3.8 kV
    // against the VL10k's 2.5..3.5 kV).
    [[nodiscard]] static bool works_on(const formats::EngineDefinition& engine, const LineSupply& supply);

private:
    [[nodiscard]] float source_voltage(const formats::ElectroPowerSource& source, const TrackPoint& where) const;
    [[nodiscard]] float source_frequency(const formats::ElectroPowerSource& source) const;
    [[nodiscard]] const formats::ElectricSupplyType* type_of(const formats::ElectroPowerSource& source) const;
    [[nodiscard]] bool wire_at(const formats::NodeElectrification& node, double offset) const;
    [[nodiscard]] const formats::ElectricityPiece* piece_at(const formats::NodeElectrification& node, double offset) const;

    formats::RouteElectrification data_;
    const TrackNetwork* network_ = nullptr;
    Mode mode_ = Mode::none;
    std::vector<const formats::NodeElectrification*> nodes_;  // by TDB node index
    std::vector<int> section_by_id_;                           // electrosection id -> index
    std::vector<int> current_;                                 // per section
    std::vector<std::vector<std::uint32_t>> active_;           // per section, per source (Active flags)
};

} // namespace openrts::simulation
