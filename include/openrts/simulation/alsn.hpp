// ALSN - the cab signalling the track gives the locomotive (the Russian
// automatic locomotive signalling, АЛСН). The track circuits carry a code
// that says how many blocks ahead are free; the locomotive's coils read it
// and the decoder shows it to the driver. The codes follow the aspects of the
// signal ahead: a green signal means the next blocks are free, a yellow one
// that the next signal is closed, a red one that this block's end is closed.
//
// The decoded code is what the driver sees: the decoder accepts a change only
// when the new code is read twice out of three checks (2 of 3), as the real
// ALSN decoder does, so a short loss of the code does not throw the light.
#ifndef OPENRTS_SIMULATION_ALSN_HPP
#define OPENRTS_SIMULATION_ALSN_HPP

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

#include <openrts/formats/signal_config.hpp>

namespace openrts::simulation {

class SignalSystem;
class Train;

// The codes of the ALSN: no code at all, green, yellow, red-yellow.
enum class AlsnCode : std::uint8_t { none = 0, green = 1, yellow = 2, red_yellow = 3 };

// The code the track gives at a signal of this aspect.
[[nodiscard]] AlsnCode alsn_from_aspect(formats::SignalAspect aspect) noexcept;

// The speed the code allows, m/s; -1 when the code sets none (green).
[[nodiscard]] float alsn_speed(AlsnCode code) noexcept;

// The code as the driver's ALSN lamp shows it: "green", "yellow", "red-yellow",
// "no code".
[[nodiscard]] const char* alsn_name(AlsnCode code) noexcept;

// The decoder: 2 of 3 checks, half a second apart.
class AlsnDecoder
{
public:
    static constexpr double interval = 0.5; // s between the checks

    void reset() noexcept;
    void feed(AlsnCode code, double dt);
    // What the driver sees.
    [[nodiscard]] AlsnCode code() const noexcept { return shown_; }
    // What the track gives right now, before the filter.
    [[nodiscard]] AlsnCode raw() const noexcept { return last_; }
    [[nodiscard]] bool changed() const noexcept { return changed_; }
    void clear_changed() noexcept { changed_ = false; }

private:
    void push(AlsnCode code) noexcept;

    std::array<AlsnCode, 3> window_{AlsnCode::none, AlsnCode::none, AlsnCode::none};
    std::array<bool, 3> valid_{};
    AlsnCode shown_ = AlsnCode::none;
    AlsnCode last_ = AlsnCode::none;
    double elapsed_ = 0.0;
    bool changed_ = false;
};

// The codes of every train of the world: the track gives each locomotive the
// code of its next signal, the decoder filters it.
class AlsnSystem
{
public:
    // Reads the signals ahead of the trains and feeds their decoders.
    void update(const SignalSystem& signals, std::span<const Train* const> trains, double dt);
    // A train has left the world.
    void forget(std::uint64_t train);
    void clear() noexcept;

    [[nodiscard]] AlsnCode code(const Train& train) const noexcept;
    [[nodiscard]] AlsnCode raw(const Train& train) const noexcept;
    [[nodiscard]] bool known(const Train& train) const noexcept;

private:
    struct Entry
    {
        AlsnDecoder decoder;
        AlsnCode raw = AlsnCode::none;
    };

    std::unordered_map<std::uint64_t, Entry> trains_;
};

} // namespace openrts::simulation

#endif
