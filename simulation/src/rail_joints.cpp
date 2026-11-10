#include <openrts/simulation/rail_joints.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace openrts::simulation {
namespace {

constexpr double welded_from = 1000.0; // m: a node this long is welded (openRTS)
constexpr double rail = 25.0;          // m: jointed track's rails
constexpr double short_rail = 12.5;    // m: an equalizing span's rails
constexpr double string_length = 800.0; // m: a welded string about this long
constexpr double end_margin = 0.25;    // m: a joint this near a node end is its junction's

// SplitMix64: a node's own sequence from its index.
struct Sequence {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // In [0, 1).
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};

// RTS (0x4F5B34): the stretch a node offset lies in, and the metres since
// its joint (towards greater offsets).
double rts_index(const double offset, const double length) {
    if (length > 2000.0) {
        const double group = std::floor(offset / 600.0);
        const double in = offset - group * 600.0;
        return group * 4.0 + (in < 12.5 ? 0.0 : in < 25.0 ? 1.0 : in < 37.5 ? 2.0 : 3.0);
    }
    return std::floor(offset / 50.0);
}

double rts_since(const double offset, const double length) {
    if (length > 2000.0) {
        double in = offset - std::floor(offset / 600.0) * 600.0;
        if (in < 37.5) {
            if (in >= 25.0) in -= 25.0;
            else if (in >= 12.5) in -= 12.5;
        } else {
            in -= 37.5;
        }
        return in;
    }
    return offset - std::floor(offset / 50.0) * 50.0;
}

} // namespace

std::vector<double> RailJoints::lay_out(const std::uint32_t node, const double length) {
    std::vector<double> joints;
    if (!(length > 0.0)) return joints;
    const auto add = [&](const double at) {
        if (at > end_margin && at < length - end_margin) joints.push_back(at);
    };
    if (length < welded_from) {
        // Jointed: rails from the node's start, the last one cut to fit.
        for (double at = rail; at < length; at += rail) add(at);
        return joints;
    }
    // Welded: span, string, span, ..., string, span; each span two or three
    // short rails, the strings sharing what is left about evenly.
    Sequence random{0x6A0157ull * (static_cast<std::uint64_t>(node) + 1)};
    const int strings = (std::max)(1, static_cast<int>(std::lround(length / string_length)));
    std::vector<int> spans(static_cast<std::size_t>(strings) + 1);
    double spanned = 0.0;
    for (auto& rails : spans) {
        rails = 2 + static_cast<int>(random.next() & 1u);
        spanned += rails * short_rail;
    }
    const double room = length - spanned;
    if (room < strings * 200.0) {
        for (double at = rail; at < length; at += rail) add(at);
        return joints;
    }
    std::vector<double> weights(static_cast<std::size_t>(strings));
    double weight = 0.0;
    for (auto& w : weights) {
        w = 0.8 + 0.4 * random.unit();
        weight += w;
    }
    double at = 0.0;
    for (std::size_t s = 0; s < spans.size(); ++s) {
        for (int r = 0; r <= spans[s]; ++r) add(at + r * short_rail);
        at += spans[s] * short_rail;
        if (s < weights.size()) at += room * weights[s] / weight;
    }
    return joints;
}

void RailJoints::set_layout(const RailJointLayout layout, const std::vector<double>& lengths) {
    layout_ = layout;
    joints_.clear();
    if (layout != RailJointLayout::openrts) return;
    joints_.resize(lengths.size());
    for (std::size_t n = 0; n < lengths.size(); ++n) joints_[n] = lay_out(static_cast<std::uint32_t>(n), lengths[n]);
}

RailJointPlace RailJoints::place(const std::uint32_t node, const double length, const double offset, const bool increasing) const {
    if (layout_ == RailJointLayout::rts || node >= joints_.size()) return {rts_index(offset, length), rts_since(offset, length)};
    const auto& joints = joints_[node];
    const auto after = std::upper_bound(joints.begin(), joints.end(), offset);
    RailJointPlace place{.index = static_cast<double>(after - joints.begin()), .since = std::numeric_limits<double>::infinity()};
    if (increasing) {
        if (after != joints.begin()) place.since = offset - *(after - 1);
    } else {
        const auto ahead = std::lower_bound(joints.begin(), joints.end(), offset);
        if (ahead != joints.end()) place.since = *ahead - offset;
    }
    return place;
}

} // namespace openrts::simulation
