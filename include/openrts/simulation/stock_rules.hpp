#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/rolling_stock.hpp>

// openRTS's stock rules in Luau (docs/scripting.md, "Stock rules"): the
// scripts TRAINS/OPENRTS/STOCK/*.luau see every car as it loads and may name
// what its file does not say right, by its series rather than file by file:
// its brake distributor and its compressor's governor. openRTS's own pack
// openrts-emu-air brings one for the multiple units (ЭД4М, ЭР9...); a mod
// adds its own scripts there, or replaces ours by the same path.
//
// A script defines `car(c)`. `c` holds the car: `folder`, `file` (with its
// .eng / .wag), `name` (the file's Name), `engine` (an .eng), `type`
// ("engine", "passenger", "freight", "tender", "other"), `engine_type`
// ("electric", "diesel", "steam", nil), `ep` (the ЭПТ fitted), `mass` (t),
// `distributor` (the one named so far: "483", "292", "305-000", "305-001",
// nil), `compressor_on` / `compressor_off` (the governor, atm; nil when the
// file gives none), `compressor_capacity` (m³ of free air a minute, nil: a
// КТ-6). The script sets `distributor`, `compressor_on`, `compressor_off`,
// `compressor_capacity`; scripts run in their names' order, each seeing what
// the one before set.
namespace openrts::simulation {

class StockRules {
public:
    ~StockRules();
    // Every TRAINS/OPENRTS/STOCK/*.luau of `vfs` (in name order). A script
    // that does not compile or run, or has no car(c), is left out and said
    // in `messages`.
    [[nodiscard]] static std::shared_ptr<StockRules> load(const core::Vfs& vfs, std::string& messages);
    [[nodiscard]] std::size_t scripts() const noexcept;
    // What the scripts make of a car (a script's error is said once in
    // take_messages() and the script left out from then on).
    [[nodiscard]] formats::WagonOverrides apply(const formats::WagonDefinition& car, std::string_view folder, std::string_view file,
                                                bool engine);
    // As load_wagon takes them.
    [[nodiscard]] formats::WagonRules hook();
    [[nodiscard]] std::string take_messages();

    // The rules of `vfs`, read once (tools: every load_wagon of one
    // installation); their messages go to stderr.
    [[nodiscard]] static formats::WagonRules of(const core::Vfs& vfs);

private:
    StockRules();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::weak_ptr<StockRules> self_;
};

} // namespace openrts::simulation
