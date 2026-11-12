#pragma once

#include <vector>

namespace openrts::viewer {

// A key going down or up, for cab keys bound by the content (AssignKey).
struct KeyEvent {
    int key{};     // GLFW key code
    bool down = false;
    bool shift = false, ctrl = false, alt = false;
};

// Cab keys held down work their switch again, as RTS's do (0x414340, which
// polls the keys every frame): once per key delay (config.ini KeyDelay, ms;
// 600 without it, the installations here 200). A press works at once, a
// held key again each delay after its last step (openRTS: RTS also lets a
// press of the same key within the delay do nothing; here a quick tap
// always counts).
class KeyRepeat {
public:
    // This frame's key events, the time `now` (s, rising) and the delay (s):
    // the events to act on, each press as it came and a press again for
    // every key held a delay since its last one, and the releases.
    [[nodiscard]] std::vector<KeyEvent> step(const std::vector<KeyEvent>& events, double now, double delay);
    // Everything let go (the cab lost the keyboard).
    void clear() noexcept { held_.clear(); }
    [[nodiscard]] bool holding() const noexcept { return !held_.empty(); }

private:
    struct Held {
        KeyEvent press;
        double last{};
    };
    std::vector<Held> held_;
};

} // namespace openrts::viewer
