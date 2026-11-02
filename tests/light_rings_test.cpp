// RTS's rings of further lights (renderer::LightRings; rts1.exe 0x4AF7D4,
// 0x4B009C, 0x4B042C): which rings a light goes into, a full ring's
// replacement, and the ring an object takes.
#include <iostream>
#include <string>

#include <openrts/renderer/model_renderer.hpp>

namespace {

using openrts::renderer::LightRings;
using openrts::renderer::ModelExtraLight;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cout << "FAIL: " << what << '\n';
    }
}

ModelExtraLight light(const float tag) {
    ModelExtraLight result;
    result.spot_exponent = tag; // to tell the lights apart
    return result;
}

} // namespace

int main() {
    LightRings rings;
    // The installed config: MaxLightDistance 400, ViewDistance 1300.
    rings.reset({0.0f, 0.0f, 0.0f}, 400.0f, 1300.0f);
    check(rings.light_step == 40.0f && rings.object_step == 130.0f, "steps");

    // Nothing in ring 0: every object takes ring 0.
    check(rings.ring_for({500.0f, 0.0f, 0.0f}, 1.0f) == 0, "ring 0 while it is empty");

    // A light 50 m away reaching 10 m: rings centred 40 and 80 m (|c - d| <= 10 + 40).
    rings.add(light(1.0f), {50.0f, 0.0f, 0.0f}, 10.0f);
    check(rings.lights[0].size() == 1 && rings.lights[1].size() == 1 && rings.lights[2].empty(), "a light in the rings it reaches");
    check(rings.placed() == 1, "placed once");

    // Objects: distance less radius in 130 m steps, ring 0 when not positive, the last beyond.
    check(rings.ring_for({300.0f, 0.0f, 0.0f}, 10.0f) == 2, "290 m: ring 2");
    check(rings.ring_for({50.0f, 0.0f, 0.0f}, 60.0f) == 0, "inside its radius: ring 0");
    check(rings.ring_for({5000.0f, 0.0f, 0.0f}, 0.0f) == 9, "far away: the last ring");

    // Nine a ring; a full ring gives its farthest light's place to a nearer one only.
    LightRings full;
    full.reset({0.0f, 0.0f, 0.0f}, 400.0f, 1300.0f);
    for (int i = 0; i < 9; ++i) full.add(light(10.0f + static_cast<float>(i)), {30.0f + static_cast<float>(i), 0.0f, 0.0f}, 1.0f);
    check(full.lights[0].size() == 9, "nine in ring 0");
    full.add(light(99.0f), {45.0f, 0.0f, 0.0f}, 1.0f);
    bool farther_in = false;
    for (const auto& each : full.lights[0]) farther_in |= each.spot_exponent == 99.0f;
    check(!farther_in, "a farther light stays out of a full ring");
    full.add(light(77.0f), {20.0f, 0.0f, 0.0f}, 1.0f);
    bool nearer_in = false, farthest_out = true;
    for (const auto& each : full.lights[0]) {
        nearer_in |= each.spot_exponent == 77.0f;
        farthest_out &= each.spot_exponent != 18.0f; // the one 38 m away
    }
    check(nearer_in && farthest_out, "a nearer light takes the farthest one's place");

    // Without MaxLightDistance the lights are ringed by ViewDistance.
    LightRings plain;
    plain.reset({0.0f, 0.0f, 0.0f}, 0.0f, 1000.0f);
    check(plain.light_step == 100.0f && plain.object_step == 100.0f, "ViewDistance for both without MaxLightDistance");

    std::cout << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
