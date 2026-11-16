#include <openrts/simulation/alsn.hpp>

#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::simulation {
namespace {

// The codes' speeds: a yellow one is the 60 km/h the driver must be down to
// before the next signal, a silent track (no code, a broken track circuit)
// the 40 km/h of the "no code" order, red-yellow the stop.
constexpr float yellow_speed = 60.0f / 3.6f;
constexpr float silent_speed = 40.0f / 3.6f;

} // namespace

AlsnCode alsn_from_aspect(const formats::SignalAspect aspect) noexcept {
    switch (aspect) {
    case formats::SignalAspect::clear_1:
    case formats::SignalAspect::clear_2:
        return AlsnCode::green;
    case formats::SignalAspect::approach_1:
    case formats::SignalAspect::approach_2:
    case formats::SignalAspect::approach_3:
        return AlsnCode::yellow;
    case formats::SignalAspect::stop:
    case formats::SignalAspect::stop_and_proceed:
    case formats::SignalAspect::restricting:
    case formats::SignalAspect::block_obstructed:
    case formats::SignalAspect::failed:
        return AlsnCode::red_yellow;
    }
    return AlsnCode::none;
}

float alsn_speed(const AlsnCode code) noexcept {
    switch (code) {
    case AlsnCode::green:
        return -1.0f;
    case AlsnCode::yellow:
        return yellow_speed;
    case AlsnCode::red_yellow:
        return 0.0f;
    case AlsnCode::none:
        return silent_speed;
    }
    return silent_speed;
}

void AlsnDecoder::reset() noexcept {
    window_ = {AlsnCode::none, AlsnCode::none, AlsnCode::none};
    valid_ = {false, false, false};
    shown_ = AlsnCode::none;
    last_ = AlsnCode::none;
    elapsed_ = 0.0;
    changed_ = false;
}

void AlsnDecoder::push(const AlsnCode code) noexcept {
    window_[0] = window_[1];
    window_[1] = window_[2];
    window_[2] = code;
    valid_[0] = valid_[1];
    valid_[1] = valid_[2];
    valid_[2] = true;
    // The first code has nothing to compare with: it is taken at once.
    if (!valid_[0] && !valid_[1]) {
        if (shown_ != code) {
            shown_ = code;
            changed_ = true;
        }
        return;
    }
    // 2 of 3: a code seen twice in the window is what the driver sees.
    for (const AlsnCode candidate : {AlsnCode::green, AlsnCode::yellow, AlsnCode::red_yellow, AlsnCode::none}) {
        int seen = 0;
        for (std::size_t i = 0; i < window_.size(); ++i)
            if (valid_[i] && window_[i] == candidate) ++seen;
        if (seen >= 2) {
            if (shown_ != candidate) {
                shown_ = candidate;
                changed_ = true;
            }
            return;
        }
    }
}

void AlsnDecoder::feed(const AlsnCode code, const double dt) {
    last_ = code;
    elapsed_ += dt;
    if (elapsed_ < interval && valid_[2]) return;
    elapsed_ = 0.0;
    push(code);
}

void AlsnSystem::update(const SignalSystem& signals, std::span<const Train* const> trains, const double dt) {
    for (const auto* train : trains) {
        if (!train) continue;
        AlsnCode raw = AlsnCode::none;
        const auto ahead = signals.ahead(*train);
        if (ahead.valid) raw = alsn_from_aspect(ahead.aspect);
        auto& entry = trains_[train->id()];
        entry.raw = raw;
        entry.decoder.feed(raw, dt);
    }
}

void AlsnSystem::forget(const std::uint64_t train) { trains_.erase(train); }

void AlsnSystem::clear() noexcept { trains_.clear(); }

AlsnCode AlsnSystem::code(const Train& train) const noexcept {
    const auto it = trains_.find(train.id());
    return it == trains_.end() ? AlsnCode::none : it->second.decoder.code();
}

AlsnCode AlsnSystem::raw(const Train& train) const noexcept {
    const auto it = trains_.find(train.id());
    return it == trains_.end() ? AlsnCode::none : it->second.raw;
}

bool AlsnSystem::known(const Train& train) const noexcept { return trains_.find(train.id()) != trains_.end(); }

const char* alsn_name(const AlsnCode code) noexcept {
    switch (code) {
    case AlsnCode::green: return "green";
    case AlsnCode::yellow: return "yellow";
    case AlsnCode::red_yellow: return "red-yellow";
    default: return "no code";
    }
}

} // namespace openrts::simulation
