// Cab keys held down (viewer/src/key_repeat): a press at once, again once
// per key delay while held, nothing after the release; the press's
// modifiers kept; two keys each on their own.
#include <iostream>
#include <string>
#include <vector>

#include "../viewer/src/key_repeat.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

int presses(const std::vector<openrts::viewer::KeyEvent>& events, const int key) {
    int n = 0;
    for (const auto& e : events) n += e.key == key && e.down ? 1 : 0;
    return n;
}

} // namespace

int main() {
    using namespace openrts::viewer;
    constexpr int e_key = 69;  // GLFW_KEY_E
    constexpr int r_key = 82;  // GLFW_KEY_R
    constexpr double delay = 0.2; // the installations' KeyDelay = 200
    KeyRepeat repeat;

    auto out = repeat.step({{.key = e_key, .down = true, .shift = true}}, 0.0, delay);
    check(presses(out, e_key) == 1, "a press works at once");
    check(repeat.step({}, 0.1, delay).empty(), "held, nothing before the delay");
    out = repeat.step({}, 0.2, delay);
    check(presses(out, e_key) == 1 && out[0].shift, "held a delay: once more, with the press's Shift");
    check(repeat.step({}, 0.3, delay).empty(), "and not again until the next delay");
    check(presses(repeat.step({}, 0.41, delay), e_key) == 1, "the next delay");

    // A second key held alongside, on its own clock.
    out = repeat.step({{.key = r_key, .down = true}}, 0.5, delay);
    check(presses(out, r_key) == 1 && presses(out, e_key) == 0, "another key at once, the first not yet");
    out = repeat.step({}, 0.62, delay);
    check(presses(out, e_key) == 1 && presses(out, r_key) == 0, "each on its own delay");

    // Let go: the release comes through, the repeats stop.
    out = repeat.step({{.key = e_key, .down = false}}, 0.7, delay);
    check(out.size() == 1 && !out[0].down, "the release passes");
    out = repeat.step({}, 1.5, delay);
    check(presses(out, e_key) == 0 && presses(out, r_key) == 1, "no repeats after the release; the other still held");

    // A quick tap always counts (RTS ignores one within the delay).
    repeat.clear();
    check(!repeat.holding(), "cleared");
    out = repeat.step({{.key = e_key, .down = true}, {.key = e_key, .down = false}, {.key = e_key, .down = true}}, 2.0, delay);
    check(presses(out, e_key) == 2, "two taps in a frame, two presses");
    check(repeat.step({}, 2.05, delay).empty(), "held after the second tap: waits for the delay");

    // No delay: no repeats.
    KeyRepeat none;
    (void)none.step({{.key = e_key, .down = true}}, 0.0, 0.0);
    check(none.step({}, 5.0, 0.0).empty(), "a zero delay repeats nothing");

    if (failures == 0) std::cout << "key repeat: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
