#include <openrts/simulation/brake_shoes.hpp>

#include <algorithm>

namespace openrts::simulation {

namespace {

// A full shoe's pressing force, kN.
float full_force(const formats::BrakeShoe shoe) noexcept {
    return shoe == formats::BrakeShoe::composite ? 15.0f : shoe == formats::BrakeShoe::wood ? 20.0f : 30.0f;
}

} // namespace

formats::BrakeShoe shoe_of(const formats::WagonDefinition& wagon) noexcept {
    if (wagon.brakes.shoe != formats::BrakeShoe::unknown) return wagon.brakes.shoe;
    return wagon.kind == formats::WagonKind::freight ? formats::BrakeShoe::composite : formats::BrakeShoe::cast_iron;
}

float shoe_friction(const formats::BrakeShoe shoe, const float speed_kmh, const float force_kn) noexcept {
    const float v = (std::max)(speed_kmh, 0.0f);
    const float k = (std::max)(force_kn, 0.0f);
    switch (shoe) {
    case formats::BrakeShoe::composite:
        return 0.44f * (0.1f * k + 20.0f) / (0.4f * k + 20.0f) * (v + 150.0f) / (2.0f * v + 150.0f);
    case formats::BrakeShoe::disc:
        return 0.35f;
    case formats::BrakeShoe::wood:
        return 0.45f * (v + 60.0f) / (2.0f * v + 60.0f);
    default:
        return 0.6f * (1.6f * k + 100.0f) / (8.0f * k + 100.0f) * (v + 100.0f) / (5.0f * v + 100.0f);
    }
}

float shoe_force_limit(const formats::BrakeShoe shoe) noexcept {
    constexpr float standing = 0.165f;
    const float rise = shoe_factor(shoe, 0.0f, 1.0f);
    return rise > 0.0f ? standing / rise : standing;
}

float shoe_factor(const formats::BrakeShoe shoe, const float speed_kmh, const float share) noexcept {
    const float full = full_force(shoe);
    const float reference = shoe_friction(shoe, shoe_reference_speed, full);
    if (reference <= 0.0f) return 1.0f;
    return shoe_friction(shoe, speed_kmh, full * std::clamp(share, 0.0f, 1.5f)) / reference;
}

} // namespace openrts::simulation
