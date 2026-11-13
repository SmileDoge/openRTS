#include <openrts/simulation/adhesion.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {

namespace {

// A bogie's shoes and the treads' working skin take this heat per kelvin
// (four wheels), J/K, and give it to the air in this time, s: a light
// application at speed warms them by tens of kelvin in ten seconds, as the
// drivers' drying of the shoes ("просушка колодок") does.
constexpr float heat_capacity = 25000.0f;
constexpr float cooling_time = 200.0f;
constexpr float hottest = 800.0f;
// Wet in about 1.5 min of rain or snow; dry in the air in 10 min.
constexpr float wetting_time = 90.0f;
constexpr float air_drying_time = 600.0f;
// Warm shoes dry: per s and per K above 5 K.
constexpr float warm_drying = 0.004f;
constexpr float drying_from = 5.0f;
// A sliding wheel's friction on the rail, as a share of the adhesion.
constexpr float sliding_share = 0.75f;
// Wood: half the heat capacity (it keeps the heat at its face), a fire
// warming it 4 K a second, burning away in 120 s, charring from
// wood_smoulders over 10 min at +500 K.
constexpr float wood_capacity_share = 0.5f;
constexpr float fire_heat = 4.0f;
constexpr float burn_time = 120.0f;
constexpr float char_time = 600.0f;

} // namespace

float rail_factor(const RailCondition rails) noexcept {
    switch (rails) {
    case RailCondition::wet: return 0.65f;
    case RailCondition::snow: return 0.5f;
    case RailCondition::ice: return 0.35f;
    case RailCondition::leaves: return 0.3f;
    default: return 1.0f;
    }
}

float rail_wetness(const RailCondition rails) noexcept {
    switch (rails) {
    case RailCondition::wet: return 1.0f;
    case RailCondition::snow: return 0.8f;
    case RailCondition::ice: return 0.8f;
    case RailCondition::leaves: return 0.6f;
    default: return 0.0f;
    }
}

RailCondition rails_for_weather(const std::uint8_t weather) noexcept {
    return weather == 2 ? RailCondition::wet : weather == 1 ? RailCondition::snow : RailCondition::dry;
}

float wheel_rail_adhesion(const float speed_kmh) noexcept {
    return 0.161f + 7.5f / ((std::max)(speed_kmh, 0.0f) + 44.0f);
}

float wet_shoe_factor(const formats::BrakeShoe shoe, const float wet) noexcept {
    const float loss = shoe == formats::BrakeShoe::composite ? 0.5f
                       : shoe == formats::BrakeShoe::wood    ? 0.4f
                       : shoe == formats::BrakeShoe::disc    ? 0.1f
                                                             : 0.2f;
    return 1.0f - loss * std::clamp(wet, 0.0f, 1.0f);
}

float tread_factor(const float rails, const float wet) noexcept {
    return rails + 0.3f * (1.0f - rails) * (1.0f - std::clamp(wet, 0.0f, 1.0f));
}

float step_bogie_brake(ShoeState& state, const BogieBrake& bogie, const float dt) noexcept {
    const bool wood = bogie.shoe == formats::BrakeShoe::wood;
    float grip = wet_shoe_factor(bogie.shoe, state.wet);
    // Wood chars hot (down to 0.35 of its grip at +800 K) and what burnt
    // away no longer presses.
    if (wood) grip *= std::clamp(1.0f - (state.heat - 200.0f) / 600.0f, 0.35f, 1.0f) * (1.0f - std::clamp(state.wear, 0.0f, 1.0f));
    const float rolling = bogie.force * grip;
    const float adhesion = wheel_rail_adhesion(bogie.speed_kmh) * tread_factor(bogie.rails, state.wet) * bogie.weight;
    const float sliding = sliding_share * adhesion;
    // The wheels lock when the shoes hold them harder than the rail turns
    // them; a locked wheel's shoe holds with its standing grip, so they roll
    // again only once the brake is let off that far, or the car stops.
    const float speed = std::abs(bogie.speed_kmh);
    if (speed < 1.0f) {
        state.sliding = false;
    } else if (!state.sliding && rolling > adhesion) {
        state.sliding = true;
    } else if (state.sliding && bogie.standing_force * grip < sliding) {
        state.sliding = false;
    }
    // Rolling, the wheels hold no more than the adhesion (standing: the most
    // they hold the car against the rail).
    const float force = state.sliding ? sliding : (std::min)(rolling, adhesion);
    // The shoes take the friction's heat while the wheel turns under them
    // (a locked wheel heats the rail instead); they cool to the air.
    const float power = state.sliding ? 0.0f : force * speed / 3.6f;
    const float capacity = wood ? heat_capacity * wood_capacity_share : heat_capacity;
    state.heat += (power / capacity - state.heat / cooling_time + (state.burning ? fire_heat : 0.0f)) * dt;
    state.heat = std::clamp(state.heat, 0.0f, hottest);
    // Wood: chars from wood_smoulders, catches fire at wood_ignites, burns
    // away; other shoes never burn.
    if (wood) {
        if (!state.burning && state.heat >= wood_ignites && state.wear < 1.0f) state.burning = true;
        if (state.burning) {
            state.wear += dt / burn_time;
        } else if (state.heat > wood_smoulders) {
            state.wear += (state.heat - wood_smoulders) / 500.0f * dt / char_time;
        }
        if (state.wear >= 1.0f) {
            state.wear = 1.0f;
            state.burning = false;
        }
    } else {
        state.burning = false;
    }
    // Wet from the rails' wetness, dry in the air and fast when warm.
    if (bogie.wetness > state.wet) {
        state.wet += (bogie.wetness - state.wet) * dt / wetting_time;
    } else {
        state.wet += (bogie.wetness - state.wet) * dt / air_drying_time;
    }
    state.wet -= warm_drying * (std::max)(state.heat - drying_from, 0.0f) * state.wet * dt;
    state.wet = std::clamp(state.wet, 0.0f, 1.0f);
    return force;
}

} // namespace openrts::simulation
