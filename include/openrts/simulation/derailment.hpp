#pragma once

#include <span>

namespace openrts::simulation {

inline constexpr double derail_gravity = 9.80665;
inline constexpr double derail_flange_length = 0.0254;
inline constexpr double derail_flange_angle = 1.2217304763960306;
inline constexpr double derail_flange_friction = 0.3;

struct WheelLoads {
    double lateral{};
    double down{};
    double moment{};
    double left{};
    double right{};
};

struct AxleRoll {
    double lift_angle{};
    double lift_speed{};
    double climb_height{};
    double climb_speed{};
    double travel{};
    int side{};
};

[[nodiscard]] double curve_load_gain(double curvature, double speed) noexcept;

[[nodiscard]] WheelLoads wheel_loads(double mass, double gauge, double height, double offset, double curvature,
                                     double cant, double speed) noexcept;

void apply_curve_load(WheelLoads& loads, double gain, double gauge, double height, double offset) noexcept;

void step_roll(double mass, double gauge, double height, double offset, double inertia, const WheelLoads& loads,
               int side, double seconds, double& angle, double& angular_speed) noexcept;

[[nodiscard]] bool step_flange(double lateral, double vertical, double effective_mass, double beta, double mu,
                               double flange_height, double approach_speed, double seconds, double& height,
                               double& vertical_speed) noexcept;

void combine_axle_roll(std::span<const AxleRoll> axles, double& angle, double& speed) noexcept;

} // namespace openrts::simulation
