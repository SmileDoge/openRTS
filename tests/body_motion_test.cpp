// openRTS's body motion (simulation/body_motion.hpp): the track's errors
// laid along a node (the same every time, nothing at the ends, millimetres),
// cars of one kind not swaying in step, a curve leaning the body outwards,
// a joint's knock dying away, a turned car's motion.
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/body_motion.hpp>

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

constexpr float dt = 0.05f;

} // namespace

int main() {
    using namespace openrts;
    using namespace openrts::simulation;

    // The track: from the node's index, nothing at its ends, a few mm.
    {
        const auto a = track_irregularity(7, 3000.0, 1234.5);
        const auto b = track_irregularity(7, 3000.0, 1234.5);
        check(a.vertical == b.vertical && a.lateral == b.lateral && a.cross_level == b.cross_level, "the same every time");
        const auto other = track_irregularity(8, 3000.0, 1234.5);
        check(other.vertical != a.vertical, "another node has its own");
        const auto end = track_irregularity(7, 3000.0, 0.0);
        const auto far_end = track_irregularity(7, 3000.0, 3000.0);
        check(end.vertical == 0.0f && end.lateral == 0.0f && far_end.cross_level == 0.0f, "nothing at the node's ends");
        double sum = 0.0, most = 0.0;
        int n = 0;
        for (double s = 20.0; s < 2980.0; s += 0.5, ++n) {
            const float v = track_irregularity(7, 3000.0, s).vertical;
            sum += static_cast<double>(v) * v;
            most = (std::max)(most, static_cast<double>(std::abs(v)));
        }
        const double rms = std::sqrt(sum / n);
        check(rms > 0.0005 && rms < 0.004 && most < 0.012, "millimetres: rms " + std::to_string(rms) + ", most " + std::to_string(most));
        const auto bogie = bogie_irregularity(7, 3000.0, 1234.5, 2.4);
        check(std::abs(bogie.vertical) <= std::abs(a.vertical) + 0.003f, "a bogie rides over the short waves");
        check(joint_depth(3, 5.0) == joint_depth(3, 5.0) && joint_depth(3, 5.0) > 0.001f && joint_depth(3, 5.0) < 0.004f, "a joint's dip");
    }

    formats::WagonDefinition wagon;
    wagon.kind = formats::WagonKind::passenger;
    wagon.mass = 40000.0f;
    wagon.length = 22.0f;

    // Three cars of one kind running at 20 m/s over the same track, 22 m
    // apart: each its own springs, so they do not sway in step.
    {
        std::vector<BodyMotion> cars(3);
        std::vector<BodyModes> modes;
        for (std::size_t c = 0; c < cars.size(); ++c) {
            cars[c].seed = 1000 + c * 77;
            modes.push_back(body_modes(wagon, wagon.mass, cars[c].seed));
        }
        check(modes[0].bounce != modes[1].bounce && modes[0].sway != modes[1].sway, "each car its own springs");
        std::vector<std::vector<float>> sway(cars.size()), roll(cars.size()), bounce(cars.size()), roof(cars.size());
        constexpr float speed = 20.0f;
        for (int step = 0; step < 1200; ++step) {
            const double front = 400.0 + speed * dt * step;
            for (std::size_t c = 0; c < cars.size(); ++c) {
                auto& in = cars[c].input;
                const double centre = front - 22.0 * static_cast<double>(c);
                in.bogie[0] = bogie_irregularity(11, 5000.0, centre + 7.5, 2.4);
                in.bogie[1] = bogie_irregularity(11, 5000.0, centre - 7.5, 2.4);
                in.speed = speed;
                in.base = 15.0f;
                step_body(cars[c], modes[c], dt);
                if (step >= 200) {
                    sway[c].push_back(cars[c].shift[0]);
                    roof[c].push_back(cars[c].shift[0] + cars[c].turn[0] * 3.0f);
                    roll[c].push_back(cars[c].turn[0]);
                    bounce[c].push_back(cars[c].shift[1]);
                }
            }
        }
        const auto rms = [](const std::vector<float>& x) {
            double sum = 0.0;
            for (const float v : x) sum += static_cast<double>(v) * v;
            return std::sqrt(sum / static_cast<double>(x.size()));
        };
        const auto correlation = [](const std::vector<float>& a, const std::vector<float>& b) {
            double ab = 0.0, aa = 0.0, bb = 0.0;
            for (std::size_t k = 0; k < a.size(); ++k) {
                ab += static_cast<double>(a[k]) * b[k];
                aa += static_cast<double>(a[k]) * a[k];
                bb += static_cast<double>(b[k]) * b[k];
            }
            return ab / std::sqrt(aa * bb + 1e-30);
        };
        for (std::size_t c = 0; c < cars.size(); ++c) {
            const double s = rms(sway[c]), r = rms(roll[c]), z = rms(bounce[c]);
            std::cout << "car " << c << " at 72 km/h: sway " << s * 1000.0 << " mm, roll " << r * 57.29578 << " deg, bounce "
                      << z * 1000.0 << " mm, roof aside " << rms(roof[c]) * 1000.0 << " mm (rms)\n";
            check(s > 0.0005 && s < 0.03, "car " + std::to_string(c) + " sways a little: " + std::to_string(s) + " m");
            check(r > 0.0001 && r < 0.02, "car " + std::to_string(c) + " rolls: " + std::to_string(r) + " rad");
            check(z > 0.0002 && z < 0.02, "car " + std::to_string(c) + " bounces: " + std::to_string(z) + " m");
        }
        const double same = correlation(sway[0], sway[1]);
        check(same < 0.8, "two cars do not sway in step (correlation " + std::to_string(same) + ")");
        std::cout << "sway correlation of cars 1 and 2: " << same << '\n';
    }

    // A curve: the body out and rolled outwards (against the lateral
    // acceleration), then still.
    {
        BodyMotion body;
        body.seed = 5;
        const auto modes = body_modes(wagon, wagon.mass, body.seed);
        for (int step = 0; step < 400; ++step) {
            body.input = {};
            body.input.lateral_acceleration = 1.0f;
            body.input.base = 15.0f;
            step_body(body, modes, dt);
        }
        const float expected = -1.0f / (modes.sway * modes.sway);
        check(std::abs(body.shift[0] - expected) < 0.002f, "out on the springs: " + std::to_string(body.shift[0]) + " m, " +
                                                              std::to_string(expected) + " expected");
        check(body.turn[0] < -0.01f && body.turn[0] > -0.05f, "rolled outwards: " + std::to_string(body.turn[0]) + " rad");
    }

    // A joint under the front bogie: the front dips, the knock dies away.
    {
        BodyMotion body;
        body.seed = 9;
        const auto modes = body_modes(wagon, wagon.mass, body.seed);
        body.input.base = 15.0f;
        body.input.kick_vertical[0] = 0.03f;
        step_body(body, modes, dt);
        check(body.turn[1] < 0.0f && body.shift[1] < 0.0f, "the front dips");
        check(body.input.kick_vertical[0] == 0.0f, "the kick is taken");
        float most = 0.0f;
        for (int step = 0; step < 100; ++step) {
            step_body(body, modes, dt);
            if (step > 80) most = (std::max)(most, std::abs(body.turn[1]));
        }
        check(most < 1e-4f, "and dies away");
    }

    // A junction's throw: 1.5-2 cm aside within a fifth of a second, over
    // within a second (not the slow sway); a joint's knock: down and back
    // within a few hundredths, gone in half a second.
    {
        BodyMotion body;
        body.seed = 11;
        const auto modes = body_modes(wagon, wagon.mass, body.seed);
        body.input.base = 15.0f;
        body.input.throw_lateral[0] = 0.018f;
        body.input.throw_lateral[1] = 0.018f;
        float most = 0.0f, late = 0.0f;
        int peak_step = 0;
        for (int step = 0; step < 40; ++step) {
            step_body(body, modes, dt);
            if (std::abs(body.pose_shift[0]) > most) {
                most = std::abs(body.pose_shift[0]);
                peak_step = step;
            }
            if (step * dt > 1.0f) late = (std::max)(late, std::abs(body.jolt[0]));
        }
        check(most > 0.012f && most < 0.025f, "a junction throws the body aside: " + std::to_string(most) + " m");
        check((peak_step + 1) * dt <= 0.2f, "sharply: at its most after " + std::to_string((peak_step + 1) * dt) + " s");
        check(late < 0.001f, "and the throw is over within a second: " + std::to_string(late) + " m");

        BodyMotion knock;
        knock.seed = 12;
        knock.input.base = 15.0f;
        knock.input.knock[0] = 0.003f;
        float deepest = 0.0f, after = 0.0f;
        for (int step = 0; step < 20; ++step) {
            step_body(knock, modes, dt);
            if (step == 0) deepest = -knock.pose_shift[1];
            if (step * dt >= 0.5f) after = (std::max)(after, std::abs(knock.knocked[0]));
        }
        check(deepest > 0.0003f, "a joint knocks the front down at once: " + std::to_string(deepest) + " m");
        check(after < 0.0002f, "and the knock is gone in half a second: " + std::to_string(after) + " m");
    }

    // Turned twice: the same motion.
    {
        BodyMotion body;
        body.shift[0] = 0.01f;
        body.shift[2] = 0.002f;
        body.turn[0] = 0.003f;
        body.turn[1] = -0.001f;
        body.turn[2] = 0.0005f;
        body.hunting[0] = 1.0f;
        body.last[0].lateral = 0.001f;
        reverse_body(body);
        check(body.shift[0] == -0.01f && body.turn[0] == -0.003f && body.turn[1] == 0.001f && body.turn[2] == 0.0005f,
              "turned: lateral, roll and pitch the other way, the yaw kept");
        check(body.hunting[1] == 1.0f && body.last[1].lateral == -0.001f, "the bogies swap");
        reverse_body(body);
        check(body.shift[0] == 0.01f && body.hunting[0] == 1.0f && body.last[0].lateral == 0.001f, "turned back");
    }

    if (failures == 0) std::cout << "body motion: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
