#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace openrts::viewer {

// The mouse's moves the view has yet to take (pixels, y down), taken up
// smoothly: each frame's move spread evenly over the next `window` seconds.
// At sensitivity 1 a count of the mouse turns the view about half a
// degree, a dozen pixels on the screen, and slow moves came out in jumps
// (2026-11-20). A lag that took a share of what was left (1 - e^(-t/lag))
// went on turning the view long after the hand stopped, its tail a quarter
// of a second, and made the player seasick; spread over a window the turn
// ends when the window does. The whole move is taken, none is lost.
struct MouseLook {
    struct Turn {
        double x = 0.0;
        double y = 0.0;
    };

    void add(const double dx, const double dy) {
        fresh_.x += dx;
        fresh_.y += dy;
    }

    [[nodiscard]] bool idle() const {
        if (fresh_.x != 0.0 || fresh_.y != 0.0) return false;
        return std::none_of(spreads_.begin(), spreads_.end(), [](const Spread& s) { return s.left > 0.0; });
    }

    // This frame's part; window 0: all of it at once.
    Turn take(const float seconds, const float window) {
        Turn turn{};
        const double dt = seconds > 0.0f ? seconds : 0.0;
        // The older moves first, each its even share of what it has left.
        for (auto& s : spreads_) {
            if (s.left <= 0.0) continue;
            const double share = dt >= s.left ? 1.0 : dt / s.left;
            turn.x += s.x * share;
            turn.y += s.y * share;
            s.x -= s.x * share;
            s.y -= s.y * share;
            s.left -= dt;
            if (s.left <= 0.0) s = {};
        }
        if (fresh_.x != 0.0 || fresh_.y != 0.0) {
            if (window <= 0.0f) {
                turn.x += fresh_.x;
                turn.y += fresh_.y;
            } else {
                // This frame takes its share at once; the rest waits, or
                // all at once when no slot is free (more frames than slots
                // within a window: the frames are short enough anyway).
                const double w = window;
                const double share = dt >= w ? 1.0 : (dt > 0.0 ? dt / w : 0.0);
                turn.x += fresh_.x * share;
                turn.y += fresh_.y * share;
                const Spread rest{fresh_.x * (1.0 - share), fresh_.y * (1.0 - share), w - dt};
                if (rest.left > 0.0) {
                    auto free = std::find_if(spreads_.begin(), spreads_.end(), [](const Spread& s) { return s.left <= 0.0; });
                    if (free != spreads_.end()) {
                        *free = rest;
                    } else {
                        turn.x += rest.x;
                        turn.y += rest.y;
                    }
                }
            }
            fresh_ = {};
        }
        return turn;
    }

private:
    struct Spread {
        double x = 0.0;
        double y = 0.0;
        double left = 0.0; // s
    };

    Turn fresh_{};                    // moved since the last frame
    std::array<Spread, 32> spreads_{}; // the frames' moves still being spread
};

} // namespace openrts::viewer
