#pragma once

// openRTS's photometric lamps (the inspector's lamps, docs/inspector.md
// "Photometric lamps"): a light given in real units, for the experimental
// path's physical light (light_shapes.wgsl): candela (a beam's intensity on
// its axis) or lumen (its whole flux, spread over its cone or every way),
// its colour by its temperature in kelvin. It falls off as the inverse
// square of the distance (lux = cd / m²); the scene's light is lux over
// lux_per_unit(). The RTS path keeps the lamp's own colour and fall-off.

#include <cmath>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace openrts::renderer {

// Lux a unit of the scene's light stands for (a night-adapted eye: some
// 20 lx look well lit). Read each frame.
inline float& lux_per_unit() {
    static float lux = 20.0f;
    return lux;
}

// A black body's colour at `kelvin` (1000..40000), linear, its luminance 1
// (Tanner Helland's fit, taken to linear).
[[nodiscard]] inline glm::vec3 kelvin_colour(float kelvin) noexcept {
    const float t = (kelvin < 1000.0f ? 1000.0f : kelvin > 40000.0f ? 40000.0f : kelvin) / 100.0f;
    const auto clamp01 = [](const float v) { return v < 0.0f ? 0.0f : v > 255.0f ? 1.0f : v / 255.0f; };
    float r, g, b;
    if (t <= 66.0f) {
        r = 1.0f;
        g = clamp01(99.4708025861f * std::log(t) - 161.1195681661f);
        b = t <= 19.0f ? 0.0f : clamp01(138.5177312231f * std::log(t - 10.0f) - 305.0447927307f);
    } else {
        r = clamp01(329.698727446f * std::pow(t - 60.0f, -0.1332047592f));
        g = clamp01(288.1221695283f * std::pow(t - 60.0f, -0.0755148492f));
        b = 1.0f;
    }
    glm::vec3 c{std::pow(r, 2.2f), std::pow(g, 2.2f), std::pow(b, 2.2f)};
    const float y = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    return y > 0.0f ? c / y : glm::vec3{1.0f};
}

// A lamp's own colour (display, 0..1) in the hue of `kelvin`, its
// lightness kept: the temperature without candela or lumen (both paths).
// 0 kelvin: as it is.
[[nodiscard]] inline glm::vec3 kelvin_tint(const glm::vec3& display, const float kelvin) noexcept {
    if (kelvin <= 0.0f) return display;
    const auto linear = [](const float v) { return std::pow(v < 0.0f ? 0.0f : v, 2.2f); };
    const glm::vec3 lin{linear(display.x), linear(display.y), linear(display.z)};
    const float y = 0.2126f * lin.x + 0.7152f * lin.y + 0.0722f * lin.z;
    glm::vec3 c = kelvin_colour(kelvin) * y;
    const float top = c.x > c.y ? (c.x > c.z ? c.x : c.z) : (c.y > c.z ? c.y : c.z);
    if (top > 1.0f) c /= top;
    return {std::pow(c.x, 1.0f / 2.2f), std::pow(c.y, 1.0f / 2.2f), std::pow(c.z, 1.0f / 2.2f)};
}

// A lamp's intensity on its axis, cd: `candela` when given, else `lumen`
// over its cone (half angle `half_angle` radians; <= 0: every way, 4 pi).
[[nodiscard]] inline float lamp_candela(const float candela, const float lumen, const float half_angle) noexcept {
    if (candela > 0.0f) return candela;
    if (lumen <= 0.0f) return 0.0f;
    constexpr float pi = 3.14159265f;
    const float solid = half_angle > 0.0f && half_angle < pi ? 2.0f * pi * (1.0f - std::cos(half_angle)) : 4.0f * pi;
    return lumen / (solid > 1e-6f ? solid : 1e-6f);
}

// What the shaders take (ModelExtraLight::photometric): rgb the light at 1 m
// in units of the scene (cd / lux_per_unit, its colour), w 1; none (w 0)
// without candela or lumen. `colour`: its own (linear) when no kelvin.
[[nodiscard]] inline glm::vec4 photometric_light(const float candela, const float lumen, const float kelvin, const float half_angle,
                                                 const glm::vec3& colour) noexcept {
    const float intensity = lamp_candela(candela, lumen, half_angle);
    if (intensity <= 0.0f) return glm::vec4{0.0f};
    glm::vec3 c = colour;
    if (kelvin > 0.0f) {
        c = kelvin_colour(kelvin);
    } else {
        const float y = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
        c = y > 1e-4f ? c / y : glm::vec3{1.0f};
    }
    return glm::vec4{c * (intensity / (lux_per_unit() > 1e-3f ? lux_per_unit() : 1e-3f)), 1.0f};
}

} // namespace openrts::renderer
