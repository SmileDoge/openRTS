#include <openrts/simulation/derailment.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double curve_load_coefficient = 2.0;
constexpr double curve_reference_speed = 25.0;
constexpr double curve_maximum_gain = 4.0;
constexpr double curve_reference_radius = 272.0;
constexpr double substep = 0.001;

} // namespace

double curve_load_gain(const double curvature, const double speed) noexcept {
    if (std::abs(curvature) <= 1e-6) return 1.0;
    const double ratio = speed / curve_reference_speed;
    return (std::min)(curve_maximum_gain,
                      1.0 + curve_load_coefficient * ratio * ratio * ratio * std::abs(curvature) * curve_reference_radius);
}

WheelLoads wheel_loads(const double mass, const double gauge, const double height, const double offset,
                       const double curvature, const double cant, const double speed) noexcept {
    WheelLoads loads;
    if (mass <= 0.0 || gauge <= 0.0) return loads;
    const double a = speed * speed * curvature;
    loads.lateral = mass * (a * std::cos(cant) - derail_gravity * std::sin(cant));
    loads.down = mass * (derail_gravity * std::cos(cant) + a * std::sin(cant));
    loads.moment = loads.lateral * height + loads.down * offset;
    loads.left = loads.down * 0.5 - loads.moment / gauge;
    loads.right = loads.down * 0.5 + loads.moment / gauge;
    return loads;
}

void apply_curve_load(WheelLoads& loads, const double gain, const double gauge, const double height,
                      const double offset) noexcept {
    if (gauge <= 0.0) return;
    loads.lateral *= gain;
    loads.moment = loads.lateral * height + loads.down * offset;
    loads.left = loads.down * 0.5 - loads.moment / gauge;
    loads.right = loads.down * 0.5 + loads.moment / gauge;
}

void step_roll(const double mass, const double gauge, const double height, const double offset, const double inertia,
               const WheelLoads& loads, const int side, const double seconds, double& angle,
               double& angular_speed) noexcept {
    if (seconds <= 0.0 || seconds > 1.0 || mass <= 0.0 || gauge <= 0.0 || height <= 0.0 || inertia <= 0.0
        || loads.down <= 0.0 || (side != 1 && side != -1)) {
        return;
    }
    const double arm = gauge * 0.5 - static_cast<double>(side) * offset;
    const double moment_of_inertia = inertia + mass * (arm * arm + height * height);
    if (moment_of_inertia <= 0.0) return;
    const int steps = static_cast<int>(std::ceil(seconds / substep));
    if (steps <= 0) return;
    const double h = seconds / static_cast<double>(steps);
    for (int i = 0; i < steps; ++i) {
        const double lateral_arm = -arm * std::cos(angle) + height * std::sin(angle);
        const double vertical_arm = arm * std::sin(angle) + height * std::cos(angle);
        angular_speed +=
            (static_cast<double>(side) * loads.lateral * vertical_arm + loads.down * lateral_arm) / moment_of_inertia * h;
        angle += angular_speed * h;
        if (angle <= 0.0) {
            angle = 0.0;
            angular_speed = (std::max)(0.0, angular_speed);
        }
        if (angle >= pi * 0.5) {
            angle = pi * 0.5;
            angular_speed = 0.0;
            break;
        }
    }
}

bool step_flange(const double lateral, const double vertical, const double effective_mass, const double beta,
                 const double mu, const double flange_height, const double approach_speed, const double seconds,
                 double& height, double& vertical_speed) noexcept {
    if (vertical <= 0.0 || effective_mass <= 0.0 || beta <= 0.0 || beta >= pi * 0.5 || flange_height <= 0.0
        || seconds <= 0.0 || seconds > 1.0) {
        return false;
    }
    const double sn = std::sin(beta);
    const double cs = std::cos(beta);
    const double accel =
        (lateral * (cs + mu * sn) - vertical * (sn - mu * cs)) / effective_mass * sn;
    const double limit = (std::max)(0.0, approach_speed) * std::tan(beta);
    const int steps = static_cast<int>(std::ceil(seconds / substep));
    if (steps <= 0) return false;
    const double h = seconds / static_cast<double>(steps);
    for (int i = 0; i < steps; ++i) {
        vertical_speed = (std::min)(limit, vertical_speed + accel * h);
        height = (std::max)(0.0, height + vertical_speed * h);
        if (height == 0.0) vertical_speed = (std::max)(0.0, vertical_speed);
        if (height >= flange_height) {
            height = flange_height;
            return true;
        }
    }
    return false;
}

void combine_axle_roll(const std::span<const AxleRoll> axles, double& angle, double& speed) noexcept {
    if (axles.empty()) return;
    double sum_angle = 0.0;
    double sum_speed = 0.0;
    for (const auto& axle : axles) {
        const double side = static_cast<double>(axle.side);
        sum_angle += axle.lift_angle * side;
        sum_speed += axle.lift_speed * side;
    }
    const double count = static_cast<double>(axles.size());
    angle = sum_angle / count;
    speed = sum_speed / count;
}

} // namespace openrts::simulation
