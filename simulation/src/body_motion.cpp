#include <openrts/simulation/body_motion.hpp>

#include <openrts/formats/rolling_stock.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace openrts::simulation {
namespace {

constexpr double pi = 3.14159265358979323846;

// SplitMix64.
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

constexpr std::uint64_t track_salt = 0x7A11E5ull;
constexpr int waves = 5;      // sines a channel
constexpr double taper = 12.0; // m: the errors fade in from a node's ends

// One channel of a node's errors: `waves` sines between `shortest` and
// `longest` m, the longer ones larger (as measured track: the error grows
// with the wavelength), `rms` m in all; each averaged over `wheelbase`.
double channel(Sequence& s, const double offset, const double wheelbase, const double rms, const double shortest,
               const double longest) {
    double sum = 0.0;
    double power = 0.0;
    for (int k = 0; k < waves; ++k) {
        const double wavelength = shortest * std::pow(longest / shortest, (k + s.unit()) / waves);
        const double phase = 2.0 * pi * s.unit();
        const double weight = std::pow(wavelength / longest, 0.6);
        const double x = pi * wheelbase / wavelength;
        const double average = x > 1e-6 ? std::sin(x) / x : 1.0;
        sum += weight * average * std::sin(2.0 * pi * offset / wavelength + phase);
        power += weight * weight * 0.5;
    }
    return power > 0.0 ? rms * sum / std::sqrt(power) : 0.0;
}

[[nodiscard]] float smoothstep(const float from, const float to, const float x) {
    const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

TrackIrregularity track_irregularity(const std::uint32_t node, const double length, const double offset) {
    return bogie_irregularity(node, length, offset, 0.0);
}

TrackIrregularity bogie_irregularity(const std::uint32_t node, const double length, const double offset, const double wheelbase) {
    const double edge = (std::min)(offset, length - offset);
    if (!(edge > 0.0)) return {};
    double fade = (std::min)(edge / taper, 1.0);
    fade = fade * fade * (3.0 - 2.0 * fade);
    Sequence s{(static_cast<std::uint64_t>(node) << 20) ^ track_salt};
    // This node's track: from good to worn; jointed track (a short node)
    // rougher than welded.
    const double quality = 0.6 + s.unit();
    const double scale = quality * (length < 1000.0 ? 1.3 : 1.0) * fade;
    TrackIrregularity result;
    result.vertical = static_cast<float>(channel(s, offset, wheelbase, 0.0025, 6.0, 40.0) * scale);
    result.lateral = static_cast<float>(channel(s, offset, wheelbase, 0.0025, 8.0, 50.0) * scale);
    // The rails' level difference over the 1.5 m between them.
    result.cross_level = static_cast<float>(channel(s, offset, wheelbase, 0.0025, 6.0, 35.0) * scale / 1.5);
    return result;
}

float joint_depth(const std::uint32_t node, const double index) {
    Sequence s{(static_cast<std::uint64_t>(node) << 24) ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(index) * 2654435761ll) ^ 0x10147ull};
    return static_cast<float>(0.0012 + 0.0026 * s.unit());
}

BodyModes body_modes(const formats::WagonDefinition& wagon, const float mass, const std::uint64_t seed) {
    Sequence s{seed};
    const auto spread = [&](const double amount) { return 1.0 + amount * (2.0 * s.unit() - 1.0); };
    const double m = (std::max)(static_cast<double>(mass), 1000.0);
    double hz = 1.5;
    double damping = 0.2;
    if (wagon.frame_spring_stiffness > 0.0) {
        // The author's springs: their bounce frequency and damping.
        const double k = wagon.frame_spring_stiffness;
        hz = std::clamp(std::sqrt(k / m) / (2.0 * pi), 0.8, 2.6);
        damping = wagon.frame_spring_damping > 0.0 ? std::clamp(wagon.frame_spring_damping / (2.0 * std::sqrt(k * m)), 0.12, 0.5) : 0.25;
    } else {
        switch (wagon.kind) {
        case formats::WagonKind::passenger:
            hz = 1.1;
            damping = 0.25;
            break;
        case formats::WagonKind::engine:
            hz = 1.45;
            damping = 0.25;
            break;
        case formats::WagonKind::freight:
            // Stiff, and lower when loaded.
            hz = std::clamp(2.3 * std::sqrt((std::max)(static_cast<double>(wagon.mass), 1000.0) / m), 1.3, 2.6);
            damping = 0.15;
            break;
        default:
            break;
        }
    }
    const double w = 2.0 * pi * hz;
    BodyModes modes;
    modes.bounce = static_cast<float>(w * spread(0.08));
    modes.pitch = static_cast<float>(w * 1.2 * spread(0.08));
    modes.sway = static_cast<float>(w * 0.8 * spread(0.1));
    modes.yaw = static_cast<float>(w * 0.9 * spread(0.1));
    modes.roll = static_cast<float>(w * 0.6 * spread(0.1));
    modes.surge = static_cast<float>(2.0 * pi * 2.0 * spread(0.1));
    modes.vertical_damping = static_cast<float>(damping * spread(0.15));
    // Twice the damping aside: the smooth sway at half its size (testers,
    // 2026-11-12: "поменять демпфера"); the junctions throw it instead.
    modes.lateral_damping = static_cast<float>(damping * 1.8 * spread(0.15));
    modes.roll_damping = static_cast<float>(damping * 0.8 * spread(0.15));
    modes.surge_damping = 0.35f;
    modes.hunting_wavelength = static_cast<float>(16.0 + 12.0 * s.unit());
    modes.hunting_amplitude = static_cast<float>(0.005 * (0.7 + 0.6 * s.unit()));
    modes.swell_length = static_cast<float>(120.0 + 220.0 * s.unit());
    return modes;
}

void step_body(BodyMotion& motion, const BodyModes& modes, const float dt) {
    auto& in = motion.input;
    if (!motion.started) {
        motion.last[0] = in.bogie[0];
        motion.last[1] = in.bogie[1];
        motion.started = true;
    }
    const float base = (std::max)(in.base, 2.0f);
    const float v = in.speed;
    // Kicks at the bogies: a joint or a junction under one end pitches the
    // body, under both lifts it; a lateral one yaws and sways it.
    {
        const float down = in.kick_vertical[0] + in.kick_vertical[1];
        motion.shift_rate[1] -= down * 0.5f;
        motion.turn_rate[1] += (in.kick_vertical[1] - in.kick_vertical[0]) / base;
        const float side = in.kick_lateral[0] + in.kick_lateral[1];
        motion.shift_rate[0] += side * 0.5f;
        motion.turn_rate[2] += (in.kick_lateral[0] - in.kick_lateral[1]) / base;
        motion.turn_rate[0] += side * 0.5f * 0.25f;
    }
    // The bogies' hunting: from 5 m/s it grows to its full size at 25 m/s,
    // swelling and fading along the way; the rear bogie at its own phase
    // and a slightly longer wave.
    const float hunting = modes.hunting_amplitude * smoothstep(5.0f, 25.0f, v);
    const float swell_angle = static_cast<float>(2.0 * pi) * motion.travelled / (std::max)(modes.swell_length, 1.0f) +
                              static_cast<float>(motion.seed % 628) * 0.01f;
    const float swell_wave = 0.5f + 0.5f * std::sin(swell_angle);
    const float swell = 0.35f + 0.65f * swell_wave * swell_wave;
    const float wavelengths[2] = {(std::max)(modes.hunting_wavelength, 4.0f), (std::max)(modes.hunting_wavelength, 4.0f) * 1.04f};
    // The curve: the body out on its springs and rolled outwards.
    const float pull = std::clamp(in.lateral_acceleration, -2.5f, 2.5f);
    const float sway_curve = modes.sway > 0.0f ? -pull / (modes.sway * modes.sway) : 0.0f;
    const float roll_curve = -0.015f * pull;
    // Braking dips the front, pulling lifts it.
    const float acceleration = std::clamp(in.acceleration, -5.0f, 5.0f);
    const float pitch_pull = 0.004f * acceleration;

    constexpr int steps = 4;
    const float h = dt / static_cast<float>(steps);
    const auto at = [&](const int k, const float f) {
        TrackIrregularity t;
        t.vertical = motion.last[k].vertical + (in.bogie[k].vertical - motion.last[k].vertical) * f;
        t.lateral = motion.last[k].lateral + (in.bogie[k].lateral - motion.last[k].lateral) * f;
        t.cross_level = motion.last[k].cross_level + (in.bogie[k].cross_level - motion.last[k].cross_level) * f;
        return t;
    };
    for (int j = 0; j < steps; ++j) {
        const float f0 = static_cast<float>(j) / steps;
        const float f1 = static_cast<float>(j + 1) / steps;
        const TrackIrregularity a0 = at(0, f0), b0 = at(1, f0), a1 = at(0, f1), b1 = at(1, f1);
        float lateral[2], lateral_rate[2];
        for (int k = 0; k < 2; ++k) {
            const float rate = static_cast<float>(2.0 * pi) * v / wavelengths[k];
            motion.hunting[k] += rate * h;
            if (motion.hunting[k] > static_cast<float>(pi)) motion.hunting[k] -= static_cast<float>(2.0 * pi);
            const float y = hunting * swell * std::sin(motion.hunting[k]);
            const float y_rate = hunting * swell * std::cos(motion.hunting[k]) * rate;
            const TrackIrregularity& now = k == 0 ? a1 : b1;
            const TrackIrregularity& then = k == 0 ? a0 : b0;
            lateral[k] = now.lateral + y;
            lateral_rate[k] = (now.lateral - then.lateral) / h + y_rate;
        }
        const float bounce_in = (a1.vertical + b1.vertical) * 0.5f;
        const float bounce_rate = ((a1.vertical + b1.vertical) - (a0.vertical + b0.vertical)) * 0.5f / h;
        const float pitch_in = (a1.vertical - b1.vertical) / base;
        const float pitch_rate = ((a1.vertical - b1.vertical) - (a0.vertical - b0.vertical)) / base / h;
        const float sway_in = (lateral[0] + lateral[1]) * 0.5f;
        const float sway_rate = (lateral_rate[0] + lateral_rate[1]) * 0.5f;
        const float yaw_in = (lateral[0] - lateral[1]) / base;
        const float yaw_rate = (lateral_rate[0] - lateral_rate[1]) / base;
        const float roll_in = (a1.cross_level + b1.cross_level) * 0.5f;
        const float roll_rate = ((a1.cross_level + b1.cross_level) - (a0.cross_level + b0.cross_level)) * 0.5f / h;

        const auto spring = [h](float& x, float& rate, const float w, const float zeta, const float target, const float target_rate,
                                const float extra) {
            const float a = w * w * (target - x) + 2.0f * zeta * w * (target_rate - rate) + extra;
            rate += a * h;
            x += rate * h;
        };
        auto& s = motion.shift;
        auto& sr = motion.shift_rate;
        auto& t = motion.turn;
        auto& tr = motion.turn_rate;
        spring(s[1], sr[1], modes.bounce, modes.vertical_damping, bounce_in, bounce_rate, 0.0f);
        spring(t[1], tr[1], modes.pitch, modes.vertical_damping, pitch_in + pitch_pull, pitch_rate, 0.0f);
        spring(s[0], sr[0], modes.sway, modes.lateral_damping, sway_in + sway_curve, sway_rate, 0.0f);
        spring(t[2], tr[2], modes.yaw, modes.lateral_damping, yaw_in, yaw_rate, 0.0f);
        // Roll with the sway: the body swings about a centre below it, its
        // top further out than its floor.
        spring(t[0], tr[0], modes.roll, modes.roll_damping, roll_in + roll_curve + 0.6f * s[0], roll_rate, 0.0f);
        spring(s[2], sr[2], modes.surge, modes.surge_damping, 0.0f, 0.0f, -acceleration);
    }
    // The bump stops.
    const auto stop = [](float& x, float& rate, const float limit) {
        if (x > limit) {
            x = limit;
            if (rate > 0.0f) rate = 0.0f;
        } else if (x < -limit) {
            x = -limit;
            if (rate < 0.0f) rate = 0.0f;
        }
    };
    stop(motion.shift[0], motion.shift_rate[0], 0.06f);
    stop(motion.shift[1], motion.shift_rate[1], 0.05f);
    stop(motion.shift[2], motion.shift_rate[2], 0.05f);
    stop(motion.turn[0], motion.turn_rate[0], 0.05f);
    stop(motion.turn[1], motion.turn_rate[1], 0.02f);
    stop(motion.turn[2], motion.turn_rate[2], 0.01f);

    // The throws and knocks: a start at a size's pace (the spring's first
    // swing peaks at about the size), stiff and well damped, in sub-steps
    // short enough for their frequencies.
    {
        constexpr float throw_w = 2.0f * static_cast<float>(pi) * 3.5f, throw_zeta = 0.55f;
        constexpr float knock_w = 2.0f * static_cast<float>(pi) * 7.0f, knock_zeta = 0.5f;
        for (int k = 0; k < 2; ++k) {
            motion.jolt_rate[k] += 1.92f * throw_w * in.throw_lateral[k];
            motion.knocked_rate[k] += 1.83f * knock_w * in.knock[k];
        }
        constexpr int fast_steps = 20;
        const float fh = dt / static_cast<float>(fast_steps);
        for (int j = 0; j < fast_steps; ++j) {
            for (int k = 0; k < 2; ++k) {
                motion.jolt_rate[k] += (-throw_w * throw_w * motion.jolt[k] - 2.0f * throw_zeta * throw_w * motion.jolt_rate[k]) * fh;
                motion.jolt[k] += motion.jolt_rate[k] * fh;
                motion.knocked_rate[k] += (-knock_w * knock_w * motion.knocked[k] - 2.0f * knock_zeta * knock_w * motion.knocked_rate[k]) * fh;
                motion.knocked[k] += motion.knocked_rate[k] * fh;
            }
        }
        for (int k = 0; k < 2; ++k) {
            if (std::abs(motion.jolt[k]) < 1e-7f && std::abs(motion.jolt_rate[k]) < 1e-6f) motion.jolt[k] = motion.jolt_rate[k] = 0.0f;
            if (std::abs(motion.knocked[k]) < 1e-7f && std::abs(motion.knocked_rate[k]) < 1e-6f) motion.knocked[k] = motion.knocked_rate[k] = 0.0f;
        }
    }
    // The pose: aside by the ends' mean, yawed by their difference; down by
    // the knocks' mean, pitched (the front down) by theirs; a little roll
    // with the throw.
    for (int axis = 0; axis < 3; ++axis) {
        motion.pose_shift[axis] = motion.shift[axis];
        motion.pose_turn[axis] = motion.turn[axis];
    }
    motion.pose_shift[0] += 0.5f * (motion.jolt[0] + motion.jolt[1]);
    motion.pose_turn[2] += (motion.jolt[0] - motion.jolt[1]) / base;
    motion.pose_turn[0] += 0.25f * 0.5f * (motion.jolt[0] + motion.jolt[1]);
    motion.pose_shift[1] -= 0.5f * (motion.knocked[0] + motion.knocked[1]);
    motion.pose_turn[1] += (motion.knocked[1] - motion.knocked[0]) / base;

    motion.last[0] = in.bogie[0];
    motion.last[1] = in.bogie[1];
    motion.travelled += v * dt;
    if (motion.travelled > 1.0e6f) motion.travelled -= 1.0e6f;
    for (int k = 0; k < 2; ++k) {
        in.kick_vertical[k] = 0.0f;
        in.kick_lateral[k] = 0.0f;
        in.throw_lateral[k] = 0.0f;
        in.knock[k] = 0.0f;
    }
}

void reverse_body(BodyMotion& motion) {
    // The lateral and the longitudinal axes turn about; roll and pitch with
    // them, the yaw stays.
    for (auto* values : {motion.shift, motion.shift_rate}) {
        values[0] = -values[0];
        values[2] = -values[2];
    }
    for (auto* values : {motion.turn, motion.turn_rate}) {
        values[0] = -values[0];
        values[1] = -values[1];
    }
    std::swap(motion.hunting[0], motion.hunting[1]);
    for (auto* pair : {motion.last, motion.input.bogie}) {
        std::swap(pair[0], pair[1]);
        for (int k = 0; k < 2; ++k) {
            pair[k].lateral = -pair[k].lateral;
            pair[k].cross_level = -pair[k].cross_level;
        }
    }
    std::swap(motion.input.kick_vertical[0], motion.input.kick_vertical[1]);
    std::swap(motion.input.kick_lateral[0], motion.input.kick_lateral[1]);
    motion.input.kick_lateral[0] = -motion.input.kick_lateral[0];
    motion.input.kick_lateral[1] = -motion.input.kick_lateral[1];
    std::swap(motion.input.throw_lateral[0], motion.input.throw_lateral[1]);
    motion.input.throw_lateral[0] = -motion.input.throw_lateral[0];
    motion.input.throw_lateral[1] = -motion.input.throw_lateral[1];
    std::swap(motion.input.knock[0], motion.input.knock[1]);
    // The ends swap; aside is the other way.
    std::swap(motion.jolt[0], motion.jolt[1]);
    std::swap(motion.jolt_rate[0], motion.jolt_rate[1]);
    for (int k = 0; k < 2; ++k) {
        motion.jolt[k] = -motion.jolt[k];
        motion.jolt_rate[k] = -motion.jolt_rate[k];
    }
    std::swap(motion.knocked[0], motion.knocked[1]);
    std::swap(motion.knocked_rate[0], motion.knocked_rate[1]);
    // The pose follows the next step; until then the springs alone, turned.
    for (int axis = 0; axis < 3; ++axis) {
        motion.pose_shift[axis] = motion.shift[axis];
        motion.pose_turn[axis] = motion.turn[axis];
    }
}

} // namespace openrts::simulation
