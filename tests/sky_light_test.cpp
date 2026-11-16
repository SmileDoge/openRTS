// The sky's light (renderer/sky_light.hpp, PBR stage 2) on the CPU: the SH9
// basis and its Lambert light (a uniform sky shows itself on a white surface;
// the lower hemisphere's coefficients light a surface facing down fully and
// one facing up not at all), the split sum's table (a mirror reflects all at
// normal incidence; the bias grows towards grazing angles; nothing above 1),
// and the scale of openRTS's sky's light against the sun's and against the
// experimental path's old sky light, printed (R11: the ranges checked by
// numbers).
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <openrts/renderer/native_sky.hpp>
#include <openrts/renderer/sky_light.hpp>

namespace {

using namespace openrts::renderer;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

[[nodiscard]] bool near(const glm::vec3& a, const glm::vec3& b, const float tolerance) {
    return glm::length(a - b) <= tolerance;
}

// The SH9 of radiance given by `sky` over the sphere (a fine grid).
template <typename Sky>
[[nodiscard]] Sh9 project(const Sky& sky, const int steps = 96) {
    Sh9 c{};
    const double pi = 3.14159265358979;
    for (int i = 0; i < steps; ++i) {
        for (int j = 0; j < 2 * steps; ++j) {
            const double theta = (i + 0.5) / steps * pi;
            const double phi = (j + 0.5) / (2 * steps) * 2.0 * pi;
            const glm::vec3 d{static_cast<float>(std::sin(theta) * std::cos(phi)), static_cast<float>(std::cos(theta)),
                              static_cast<float>(std::sin(theta) * std::sin(phi))};
            const double solid = std::sin(theta) * (pi / steps) * (2.0 * pi / (2 * steps));
            const auto b = sh9_basis(d);
            const glm::vec3 l = sky(d);
            for (int k = 0; k < 9; ++k) c[static_cast<std::size_t>(k)] += l * static_cast<float>(b[static_cast<std::size_t>(k)] * solid);
        }
    }
    return c;
}

} // namespace

int main() {
    // A uniform sky of radiance L: a white surface any way shows L.
    {
        const auto c = project([](const glm::vec3&) { return glm::vec3{0.4f, 0.5f, 0.6f}; });
        check(near(sh9_light(c, {0.0f, 1.0f, 0.0f}), {0.4f, 0.5f, 0.6f}, 0.01f), "a uniform sky's light facing up");
        check(near(sh9_light(c, glm::normalize(glm::vec3{1.0f, -1.0f, 0.3f})), {0.4f, 0.5f, 0.6f}, 0.01f), "a uniform sky's light any way");
    }
    // The ground alone (radiance 1 under the horizon): facing down all of it,
    // facing up none; projected or by the closed form alike.
    {
        const auto ground = sh9_lower_hemisphere(glm::vec3{1.0f});
        const auto projected = project([](const glm::vec3& d) { return d.y < 0.0f ? glm::vec3{1.0f} : glm::vec3{0.0f}; });
        for (int k = 0; k < 9; ++k) {
            check(near(ground[static_cast<std::size_t>(k)], projected[static_cast<std::size_t>(k)], 0.01f),
                  "the lower hemisphere's coefficient " + std::to_string(k));
        }
        const auto down = sh9_light(ground, {0.0f, -1.0f, 0.0f}).x;
        const auto up = sh9_light(ground, {0.0f, 1.0f, 0.0f}).x;
        const auto side = sh9_light(ground, {1.0f, 0.0f, 0.0f}).x;
        std::cout << "the ground's light (radiance 1): facing down " << down << ", up " << up << ", sideways " << side << '\n';
        check(down > 0.9f && down < 1.1f, "facing down, nearly all the ground's light");
        check(up < 0.1f, "facing up, nearly none");
        check(std::abs(side - 0.5f) < 0.05f, "sideways, half");
    }
    // The split sum's table.
    {
        const auto mirror = split_sum_brdf(1.0f, 0.0f);
        check(std::abs(mirror.x + mirror.y - 1.0f) < 0.02f, "a mirror at normal incidence reflects F0 and none of the bias");
        check(mirror.y < 0.02f, "no bias at normal incidence");
        const auto grazing = split_sum_brdf(0.1f, 0.2f);
        check(grazing.y > split_sum_brdf(0.9f, 0.2f).y, "the bias (Fresnel) grows towards grazing angles");
        for (const float r : {0.1f, 0.5f, 1.0f}) {
            for (const float v : {0.05f, 0.5f, 1.0f}) {
                const auto ab = split_sum_brdf(v, r);
                check(ab.x >= 0.0f && ab.y >= 0.0f && ab.x + ab.y <= 1.02f, "the table within 0..1");
            }
        }
        check(split_sum_brdf(0.5f, 1.0f).x + split_sum_brdf(0.5f, 1.0f).y < split_sum_brdf(0.5f, 0.2f).x + split_sum_brdf(0.5f, 0.2f).y,
              "a rough surface reflects less in all (Smith's shadowing)");
        const auto mid = split_sum_brdf(0.5f, 0.5f);
        std::cout << "split sum at n.v 0.5, roughness 0.5: " << mid.x << ", " << mid.y << '\n';
    }
    // openRTS's sky's light on a white surface facing up (its sky over the
    // horizon only) against the experimental path's old sky light at noon
    // (a white environment ambient, 35 % of it by full sun): printed.
    for (const float elevation : {60.0f, 30.0f, 10.0f, 2.0f}) {
        const float e = glm::radians(elevation);
        const glm::vec3 to_sun{0.0f, std::sin(e), std::cos(e)};
        const auto c = project([&](const glm::vec3& d) {
            if (d.y < 0.0f) return glm::vec3{0.0f};
            return atmosphere_radiance(300.0f, d, to_sun);
        }, 48);
        const auto up = sh9_light(c, {0.0f, 1.0f, 0.0f});
        const auto side = sh9_light(c, {0.0f, 0.0f, 1.0f});
        std::printf("sun %4.0f deg: openRTS's sky's light facing up %.3f %.3f %.3f, facing the sun's side %.3f %.3f %.3f\n", elevation,
                    up.x, up.y, up.z, side.x, side.y, side.z);
        if (elevation == 60.0f) {
            const float luminance = glm::dot(up, glm::vec3{0.2126f, 0.7152f, 0.0722f});
            check(luminance > 0.05f && luminance < 1.0f, "the sky's light at noon within a twentieth and all of a white surface in the sun");
        }
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "sky light: ok\n";
    return 0;
}
