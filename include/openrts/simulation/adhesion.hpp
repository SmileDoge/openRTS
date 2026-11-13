#pragma once

// Wheel to rail adhesion by the rails' state, wheel slide (юз) and the brake
// shoes warming and drying (openRTS, docs/train.md "Adhesion, wheel slide
// and the shoes' warmth"). RTS knows none of it: its traction limit is the
// .eng's Adheasion whatever the weather, its brakes never slide.

#include <cstdint>

#include <openrts/formats/rolling_stock.hpp>

namespace openrts::simulation {

// The rails: as RTS (no weather), or dry, wet (rain), snow, ice, leaves.
enum class RailCondition : std::uint8_t { rts, dry, wet, snow, ice, leaves };

// The adhesion on such rails as a share of the dry one: wet 0.65, snow 0.5,
// ice 0.35, leaves 0.3 (rts and dry: 1).
[[nodiscard]] float rail_factor(RailCondition rails) noexcept;
// How wet such rails make the shoes and wheels in the end (0 .. 1): rain 1,
// snow and ice 0.8 (the shoes pack with snow and ice), leaves 0.6, dry and
// rts 0.
[[nodiscard]] float rail_wetness(RailCondition rails) noexcept;
// The rails the weather gives (0 clear, 1 snow, 2 rain; the scene's).
[[nodiscard]] RailCondition rails_for_weather(std::uint8_t weather) noexcept;

// Dry wheel to rail adhesion at `speed_kmh` (Curtius and Kniffler):
// 0.161 + 7.5 / (v + 44): 0.33 standing, 0.25 at 40 km/h, 0.21 at 100.
[[nodiscard]] float wheel_rail_adhesion(float speed_kmh) noexcept;

// A bogie's shoes and wheel treads: how wet (0 dry .. 1 soaked) and how
// warm (K above the air) they are, whether its wheels slide (locked, юз).
// Wooden shoes also char and burn: how much of them is gone (0 new .. 1
// burnt away: no brake left on the bogie) and whether they are alight.
struct ShoeState {
    float wet{};
    float heat{};
    bool sliding = false;
    float wear{};
    bool burning = false;
};
// Wooden shoes smoulder from this warmth (K above the air) and catch fire
// at the second; alight they burn away in two minutes.
inline constexpr float wood_smoulders = 250.0f;
inline constexpr float wood_ignites = 400.0f;

// One bogie's braking for a step: its shoes, their pressing (`share` of the
// full force: the cylinder over BrakeCylinderPressureForMaxBrakeBrakeForce),
// the force its shoes give rolling at full grip (N, the content's half
// brake force times shoe_factor), its weight (N), the rails' adhesion
// factor (rail_factor, with sand), the car's speed (km/h). Updates `state`
// (slide, warmth, wetness towards `wetness`) and gives the force that holds
// the car back (N).
struct BogieBrake {
    formats::BrakeShoe shoe{};
    float share{};
    float force{};           // N, rolling, the shoes dry
    float standing_force{};  // N, the same at no speed (a locked wheel's shoe)
    float weight{};          // N
    float rails = 1.0f;      // rail_factor x sand
    float wetness{};         // rail_wetness
    float speed_kmh{};
};
[[nodiscard]] float step_bogie_brake(ShoeState& state, const BogieBrake& bogie, float dt) noexcept;

// What wetness leaves of a shoe's grip: composite shoes lose up to half,
// wood two fifths, cast iron a fifth, disc pads a tenth.
[[nodiscard]] float wet_shoe_factor(formats::BrakeShoe shoe, float wet) noexcept;
// The adhesion factor a bogie's wheels have on such rails: dry treads
// (warmed by the shoes) win back 30 % of what wet rails take.
[[nodiscard]] float tread_factor(float rails, float wet) noexcept;

} // namespace openrts::simulation
