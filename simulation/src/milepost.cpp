#include <openrts/simulation/milepost.hpp>

#include <openrts/formats/track_database.hpp>

namespace openrts::simulation {

namespace {

constexpr double reach = 5000.0; // RTS's search, m

// A kilometre post: a speed post without a limit (RTS 0x47FBE4 skips flag 2).
bool is_milepost(const formats::TrackItem& item) {
    const auto post = formats::speed_post(item);
    return post && !post->limit;
}

} // namespace

void update_milepost(MilepostState& state, const TrackNetwork& network, const std::uint32_t node, const double offset, const bool forward) {
    state.known = true;
    state.base = -1.0f;
    state.decreasing = false;
    const auto& items = network.database().items;
    const auto first = [&](const bool way, float& value, float& km) {
        const auto found = look_ahead(network, node, offset, way, reach, is_milepost, 1);
        if (found.items.empty()) return false;
        const auto& at = found.items.front();
        if (at.item >= items.size()) return false;
        value = formats::speed_post(items[at.item])->number;
        km = static_cast<float>(at.distance * 0.001);
        return true;
    };
    float behind = 0.0f, behind_km = 0.0f, ahead = 0.0f, ahead_km = 0.0f;
    if (first(!forward, behind, behind_km)) {
        // With none ahead RTS looks behind again from the train and finds
        // the same post: the numbers grow.
        if (!first(forward, ahead, ahead_km) || ahead >= behind) {
            state.base = behind + behind_km;
        } else {
            state.decreasing = true;
            state.base = behind - behind_km;
        }
    } else if (first(forward, ahead, ahead_km)) {
        // Its second search ahead finds the same post again.
        state.base = ahead - ahead_km;
    }
}

float car_milepost(const MilepostState& state, const double along) {
    if (state.base < 0.0f) return -1.0f;
    return state.decreasing ? static_cast<float>(state.base - along * 0.001) : static_cast<float>(along * 0.001 + state.base);
}

} // namespace openrts::simulation
