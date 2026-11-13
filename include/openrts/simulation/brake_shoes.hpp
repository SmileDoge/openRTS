#pragma once

// Brake shoes under the realistic air model (docs/train.md): the friction of
// a shoe on the wheel changes with the speed and with how hard the shoe is
// pressed, by the friction formulas of the Russian traction rules (Правила
// тяговых расчётов): cast-iron shoes grip twice as hard near a stop as at
// 40 km/h, composite ones only a fifth more. RTS's model keeps a brake force
// that does not depend on the speed.

#include <openrts/formats/rolling_stock.hpp>

namespace openrts::simulation {

// The shoes a car has: its ORTSBrakeShoeType, else cast iron on engines and
// passenger cars and composite on freight cars.
[[nodiscard]] formats::BrakeShoe shoe_of(const formats::WagonDefinition& wagon) noexcept;

// The friction coefficient of one shoe at `speed_kmh` pressed with
// `force_kn` kN (Правила тяговых расчётов):
// - cast iron: 0.6 · (1.6K + 100) / (8K + 100) · (v + 100) / (5v + 100);
// - composite: 0.44 · (0.1K + 20) / (0.4K + 20) · (v + 150) / (2v + 150);
// - disc pads: 0.35 at any speed;
// - wood (no rule for it: a guess): 0.45 · (v + 60) / (2v + 60).
[[nodiscard]] float shoe_friction(formats::BrakeShoe shoe, float speed_kmh, float force_kn) noexcept;

// The content's brake force is taken as the force at 40 km/h with the full
// cylinder (BrakeCylinderPressureForMaxBrakeBrakeForce); this is what the
// shoes give instead, as a factor on it: the friction now over the friction
// at 40 km/h, the shoe pressed by `share` of its full force (the cylinder
// over the full one). A full shoe presses 30 kN (cast iron) or 15 kN
// (composite: their rigging presses about half as hard).
[[nodiscard]] float shoe_factor(formats::BrakeShoe shoe, float speed_kmh, float share) noexcept;

// The speed the content's brake force is taken at, km/h.
inline constexpr float shoe_reference_speed = 40.0f;

// The most a full brake gives at 40 km/h as a share of the weight on the
// shoes, with TrainSettings::limit_brake_force: what keeps real stock's stop
// at about half the dry adhesion (0.165 of the weight at no speed, as a
// locomotive's shoe pressing gives): cast iron 0.077, composite 0.136, disc
// pads 0.165. Some content brakes far harder (the MaDaDev ВЛ80Т's 246.6 kN a
// 96 t section, 0.26), so its wheels slide on dry rails.
[[nodiscard]] float shoe_force_limit(formats::BrakeShoe shoe) noexcept;

} // namespace openrts::simulation
