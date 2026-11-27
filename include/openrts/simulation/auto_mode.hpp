#pragma once

// The АР-265 auto mode (авторежим) of a freight car's brakes: a device
// between the distributor and the brake cylinder that lowers the cylinder
// pressure by the car's load. It never raises it above the distributor's
// pressure: the 483's load mode still sets the ceiling, the auto mode only
// reduces below it. The model follows the RRS АР-265 device: a working
// chamber filled by the distributor, a damper piston moved by the load, and
// a calibrated passage from the chamber to the brake cylinder. Pressures are
// MPa and flows are MPa·m³/s, as in the RRS device; the air model converts
// its atm at the boundary.

namespace openrts::simulation {

// 1 atm in MPa.
inline constexpr float mpa_per_atm = 0.101325f;

class AutoMode265 {
public:
    // The device's settings (the RRS ar265.xml values: its 60 s
    // <dempfer_time> and <A> are not read by the RRS loader, so its defaults
    // are used here too).
    float work_volume = 0.003f;       // m³, the working chamber
    float flow_coefficient = 0.05f;   // m³/s, the chamber-to-cylinder passage
    float motion_time = 60.0f;        // s, the damper's full stroke
    float lock_pressure = 0.05f;      // MPa, above it the damper is locked
    float payload_min = 0.1f;         // the lower load-sensitivity limit
    float payload_max = 0.8f;         // the upper load-sensitivity limit
    float reduction_min = 0.4f;       // the cylinder-pressure coefficient at payload_min
    float reduction_max = 1.0f;       // the cylinder-pressure coefficient at payload_max
    float gain = 1.0f;                // m³/(s·MPa), the passage's response

    // false until the first step or restore: the working pressure then comes
    // from the car's current brake cylinder.
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] float work_pressure() const noexcept { return work_pressure_; }
    [[nodiscard]] float damper() const noexcept { return damper_; }
    [[nodiscard]] float payload() const noexcept { return payload_; }

    // Start from `work_pressure` MPa and a damper position. The damper is
    // clamped to 0..1; the working pressure is clamped to >= 0.
    void reset(float work_pressure, float damper) noexcept;
    // The car's load: 0 empty .. 1 fully loaded.
    void set_payload(float payload) noexcept;

    // One sub-step of `h` s. `distributor_flow` is the air the distributor
    // sends into the working chamber (MPa·m³/s, negative when it vents it);
    // `brake_cylinder_pressure` is the real brake cylinder's pressure (MPa).
    // Returns the flow into the brake cylinder (MPa·m³/s, negative when it
    // vents the cylinder to the atmosphere).
    [[nodiscard]] float step(float distributor_flow, float brake_cylinder_pressure, float h) noexcept;

    // The brake-cylinder pressure the damper asks for at the current working
    // pressure (MPa). Never above the working pressure.
    [[nodiscard]] float target_brake_pressure() const noexcept;

private:
    float work_pressure_ = -1.0f;
    float damper_ = 0.0f;
    float payload_ = 0.0f;
    bool initialized_ = false;
};

} // namespace openrts::simulation
