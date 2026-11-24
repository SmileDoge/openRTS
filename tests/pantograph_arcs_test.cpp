// openRTS's pantograph arcs (viewer/src/pantograph_arcs.hpp): an arc when
// a head touches the wire and when it leaves it, none on a dead line; on an
// icy wire sparks while running, at the leading pantograph only; the made
// crack is a sound. And the photometric lamps' numbers
// (renderer/photometric.hpp).

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/renderer/photometric.hpp>
#include <glm/trigonometric.hpp>

#include "../viewer/src/pantograph_arcs.hpp"

namespace {

int failures = 0;

void check(const bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

using openrts::viewer::ArcPantograph;
using openrts::viewer::PantographArcs;
namespace renderer = openrts::renderer;

// Arcs begun at `key` over `seconds` of frames.
int run(PantographArcs& arcs, std::vector<ArcPantograph> ps, const bool icy, const float seconds, const std::uint64_t key) {
    int begun = 0;
    for (int f = 0; f < static_cast<int>(seconds * 60.0f); ++f) {
        arcs.update(1.0f / 60.0f, ps, icy);
        for (const auto& s : arcs.started()) begun += s.key == key ? 1 : 0;
    }
    return begun;
}

} // namespace

int main() {
    {
        PantographArcs arcs;
        ArcPantograph p{.key = 1, .contact = false};
        std::vector<ArcPantograph> ps{p};
        arcs.update(0.016f, ps, false); // first seen: nothing
        check(arcs.started().empty() && arcs.arcs().empty(), "a pantograph first seen: no arc");
        ps[0].contact = true;
        arcs.update(0.016f, ps, false);
        check(arcs.started().size() == 1 && !arcs.arcs().empty(), "the head touches the wire: an arc");
        check(run(arcs, ps, false, 30.0f, 1) == 0, "summer, at the wire running: no more arcs");
        check(arcs.arcs().empty(), "the arc is over");
        ps[0].contact = false;
        arcs.update(0.016f, ps, false);
        check(arcs.started().size() == 1 && arcs.started()[0].length >= 0.3f, "leaving the wire: an arc, drawn out");
        ps[0].contact = true;
        ps[0].live = false;
        arcs.update(0.016f, ps, false);
        check(arcs.started().empty(), "a dead line: no arc");
    }
    {
        PantographArcs arcs;
        ArcPantograph front{.key = 1, .contact = true, .leading = true, .speed = 20.0f};
        ArcPantograph rear{.key = 2, .contact = true, .leading = false, .speed = 20.0f};
        std::vector<ArcPantograph> ps{front, rear};
        arcs.update(0.016f, ps, true);
        int at_front = 0, at_rear = 0;
        for (int f = 0; f < 60 * 60; ++f) {
            arcs.update(1.0f / 60.0f, ps, true);
            for (const auto& s : arcs.started()) (s.key == 1 ? at_front : at_rear) += 1;
        }
        std::cerr << "  ice, 72 km/h, a minute: " << at_front << " sparks at the leading pantograph, " << at_rear << " behind" << std::endl;
        check(at_front > 30 && at_front < 150, "ice: sparks at the leading pantograph, about one a second");
        check(at_rear == 0, "ice: none at the one behind");
        ps[0].speed = ps[1].speed = 0.0f;
        check(run(arcs, ps, true, 60.0f, 1) == 0, "ice, standing: none");
    }
    check(PantographArcs::icy_rate(33.0f) > PantographArcs::icy_rate(10.0f), "faster, more sparks");
    {
        // The thread: from the wire down; drawn out, it grows as the head drops.
        PantographArcs arcs;
        ArcPantograph p{.key = 1, .position = {0.0f, 6.25f, 0.0f}, .contact = true};
        std::vector<ArcPantograph> ps{p};
        arcs.update(0.016f, ps, false);
        ps[0].contact = false;
        arcs.update(0.016f, ps, false);
        const auto gap = [&] {
            const auto& path = arcs.arcs().front().path;
            return path.front().y - path.back().y;
        };
        check(!arcs.arcs().empty() && arcs.arcs().front().path.size() >= 5 && std::abs(arcs.arcs().front().path.front().y - 6.25f) < 1e-4f,
              "the thread starts at the wire");
        const float early = gap();
        for (int f = 0; f < 15; ++f) arcs.update(0.016f, ps, false);
        check(!arcs.arcs().empty() && gap() > early + 0.02f, "leaving the wire, the thread grows");
    }
    for (const auto& [name, clip] : {std::pair{"the snap", openrts::viewer::make_arc_snap()},
                                     std::pair{"the AC buzz", openrts::viewer::make_arc_buzz(true)},
                                     std::pair{"the DC buzz", openrts::viewer::make_arc_buzz(false)}}) {
        float peak = 0.0f;
        double energy = 0.0;
        for (const float s : clip->samples) {
            peak = (std::max)(peak, std::abs(s));
            energy += static_cast<double>(s) * s;
        }
        check(clip->channels == 1 && clip->frames > 4000 && peak > 0.5f && peak <= 1.0f && energy > 1.0,
              std::string(name) + ": heard, not clipped");
    }
    check(openrts::viewer::make_arc_buzz(true)->has_loop(), "the buzz loops");
    {
        // Photometric: lumen over a cone, kelvin's colour of luminance 1,
        // the scene's units by lux_per_unit.
        const float cd = renderer::lamp_candela(0.0f, 1000.0f, glm::radians(30.0f));
        check(std::abs(cd - 1000.0f / (2.0f * 3.14159265f * (1.0f - std::cos(glm::radians(30.0f))))) < 0.5f,
              "lumen over a 30-degree cone: its candela");
        check(std::abs(renderer::lamp_candela(0.0f, 4.0f * 3.14159265f, 0.0f) - 1.0f) < 1e-4f, "4 pi lumen every way: 1 cd");
        const auto warm = renderer::kelvin_colour(2000.0f), cold = renderer::kelvin_colour(8000.0f);
        check(warm.r > warm.b * 3.0f && cold.b > cold.r && std::abs(0.2126f * warm.r + 0.7152f * warm.g + 0.0722f * warm.b - 1.0f) < 1e-3f,
              "2000 K warm, 8000 K cold, luminance 1");
        const auto light = renderer::photometric_light(200.0f, 0.0f, 0.0f, 0.1f, glm::vec3{1.0f});
        check(light.w == 1.0f && std::abs(light.g - 200.0f / renderer::lux_per_unit()) < 1e-3f, "200 cd white: 200 / lux_per_unit at 1 m");
        check(renderer::photometric_light(0.0f, 0.0f, 3000.0f, 0.1f, glm::vec3{1.0f}).w == 0.0f, "no cd, no lm: no photometric light");
        // The temperature alone: the lamp's own colour in its hue.
        const auto warm_white = renderer::kelvin_tint(glm::vec3{1.0f}, 2000.0f), as_is = renderer::kelvin_tint(glm::vec3{0.5f}, 0.0f);
        check(warm_white.r > warm_white.b + 0.3f && warm_white.r <= 1.0f && as_is == glm::vec3{0.5f}, "2000 K tints a white lamp warm; 0 K leaves it");
    }
    if (failures == 0) std::cout << "pantograph arcs, photometric lamps: all passed" << std::endl;
    return failures == 0 ? 0 : 1;
}
