#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
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
    // The wire's BurnQ (RTS 0x5B53A8: of the type-1 catenary pieces over the
    // point, 0 if any has 0, else the largest; -1 without one): the burning
    // heat grows by current / BurnQ. 1 where the route has no .electrodb.
    float burn_q = 1.0f;
};

// A pantograph that burnt the wire this step (LineSupply::burnt).
struct LineBurn {
    std::uint64_t train{};
    std::size_t car{};
    float current{}; // A per raised pantograph when it burnt
};

// A substation's load (RTS 0x5B6030): the power its feeders gave the trains,
// averaged over 2 s windows, by phase.
struct SubstationLoad {
    std::uint32_t substation{};
    std::uint32_t phase{};
    float power{};      // W, the last full window
    float energy{};     // J in the running window
    float window{};     // s of the running window
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

    // The train's line as RTS sets it before the DLLs (0x503CE4), by groups
    // of cars joined by the high-voltage bus (an engine alone without one):
    // the highest voltage under the group's electric engines with a
    // pantograph up (`pantographs[car]`, the raised ones as bits) and not
    // burnt, given to every electric engine of the group
    // (Train::set_line_supply). Then the line current: the group's power
    // (CarState::power) over that voltage, split by raised pantographs
    // (Train::set_line_load); over `dt` seconds the wire heats and cools and
    // may burn (MaxLineCurrent, MaxLineCurrentCatenaryBurnTime,
    // MaxLineCurrentVelocityIncrease); and the substations feeding it are
    // loaded. Returns the highest voltage it set (0: none).
    LineSupply apply(Train& train, const std::vector<std::uint8_t>& pantographs, double dt = 0.0);
    // Pantographs that burnt the wire since the last call.
    [[nodiscard]] std::vector<LineBurn> take_burns() { return std::exchange(burns_, {}); }
    // Every substation phase that fed a train, with its load.
    [[nodiscard]] std::vector<SubstationLoad> substation_loads() const;
    // Once a step before the trains: closes the 2 s windows (RTS 0x5B6030).
    void advance_loads(double dt);
    // The windows closed since the last call, with what they summed (what
    // rts1.exe sends the dispatchers as 0x4649).
    struct ClosedLoad {
        std::uint32_t substation{};
        std::uint32_t phase{};
        float energy{}; // power x s
        float window{}; // s
    };
    [[nodiscard]] std::vector<ClosedLoad> take_closed_loads() { return std::exchange(closed_, {}); }

    // The line as a network server keeps it (the console's
    // "Электроснабжение", docs/net.md):
    // a substation switched off (0x4646 flags bit 0) gives no voltage; its
    // voltage as the server gives it, by phase when it gives phases (0x4648).
    void set_substation(std::uint32_t id, bool off, float voltage);
    void set_phase_voltages(std::uint32_t id, std::vector<float> voltages);
    // A section's source by its id (0x4647): >= 0 that one, -1 its own (as
    // the route file has it), -2 / -3 none ("отключено").
    void set_section_source(std::uint32_t section_id, int source);
    // A piece of track's own supply (0x4645, "Отдельные пути"): node and
    // the piece's index in its Electricity list; >= 0 that source of its
    // section, -1 the section's choice, -2 / -3 none; a dead piece fed from
    // a neighbour, 1 the piece before, 2 the piece after.
    void set_piece_supply(std::uint32_t node, std::uint32_t piece, int source);
    // Off: pantographs never burn the wire (an openRTS option; on as RTS).
    bool wire_burning = true;

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
    std::vector<int> file_current_;                            // per section, as the route file has it
    std::vector<std::vector<std::uint32_t>> file_active_;      // per section, per source
    struct SubstationState {
        bool off = false;
        float voltage = -1.0f; // < 0: the file's
        std::vector<float> phases;
    };
    std::map<std::uint32_t, SubstationState> substation_state_;
    std::map<std::uint64_t, int> piece_supply_;                // node << 32 | piece -> source
    std::vector<ClosedLoad> closed_;
    [[nodiscard]] const SubstationState* state_of(std::uint32_t substation) const {
        const auto it = substation_state_.find(substation);
        return it == substation_state_.end() ? nullptr : &it->second;
    }
    std::vector<std::vector<std::uint32_t>> active_;           // per section, per source (Active flags)
    std::vector<LineBurn> burns_;
    std::map<std::uint64_t, SubstationLoad> loads_;            // substation << 32 | phase
};

} // namespace openrts::simulation
