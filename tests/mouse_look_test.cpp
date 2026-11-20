// The mouse's look, smoothed (viewer/src/mouse_look.hpp): without a window
// the move at once; with one a mouse count's jump spread evenly over it,
// the whole move taken at any frame rate, and nothing left turning once
// the window after the hand's last move is over (the old lag's tail went
// on a quarter of a second and made the player seasick).
#include <cmath>
#include <iostream>
#include <string>

#include "../viewer/src/mouse_look.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

} // namespace

int main() {
    using openrts::viewer::MouseLook;
    {
        MouseLook look;
        look.add(12.0, -3.0);
        const auto turn = look.take(1.0f / 240.0f, 0.0f);
        check(turn.x == 12.0 && turn.y == -3.0 && look.idle(), "no smoothing: the move at once");
    }
    for (const float fps : {30.0f, 60.0f, 144.0f, 240.0f, 500.0f}) {
        // One count's jump (12 px), a 12 ms window: even parts, all of it
        // by the window's end, then still.
        const std::string at = " at " + std::to_string(static_cast<int>(fps)) + " fps";
        const float dt = 1.0f / fps;
        constexpr float window = 0.012f;
        MouseLook look;
        look.add(12.0, 0.0);
        double total = 0.0;
        double first = -1.0;
        int frames = 0;
        while (!look.idle() && frames < 1000) {
            const double part = look.take(dt, window).x;
            if (first < 0.0) first = part;
            total += part;
            ++frames;
        }
        check(std::abs(total - 12.0) < 1e-9, "the whole jump taken" + at);
        if (dt < window) check(first < 12.0 * dt / window + 1e-9, "a jump spread" + at);
        check(static_cast<float>(frames) * dt <= window + dt + 1e-6f, "still once the window is over" + at);
    }
    {
        // A steady move, 600 px/s at 240 frames/s: the camera at the
        // mouse's speed, half a window (6 ms, 3.6 px) behind; the hand
        // stops and the view stops within the window.
        MouseLook look;
        double turned = 0.0, last = 0.0;
        for (int f = 0; f < 240; ++f) {
            look.add(2.5, 0.0);
            last = look.take(1.0f / 240.0f, 0.012f).x;
            turned += last;
        }
        check(std::abs(last - 2.5) < 1e-9, "a steady move followed at its speed");
        check(600.0 - turned < 5.0, "a steady move a few pixels behind");
        int after = 0;
        while (!look.idle() && after < 100) {
            turned += look.take(1.0f / 240.0f, 0.012f).x;
            ++after;
        }
        check(after <= 3 && std::abs(turned - 600.0) < 1e-9, "the hand stops: the view within 12 ms, all of it");
    }
    if (failures == 0) std::cout << "mouse_look_test: ok\n";
    return failures == 0 ? 0 : 1;
}
