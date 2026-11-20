// The engineer's keys in the prompts (viewer/src/key_layout): on foot in
// openRTS's keys Enter shows as E and Ctrl+Enter as Shift+E; seated, and in
// RTS's keys, as written; what Enter alone is offered.
#include <iostream>
#include <string>

#include "../viewer/src/key_layout.hpp"

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
    using namespace openrts::viewer;
    const std::string both = "Enter — открыть дверь, Ctrl+Enter — подняться";
    set_key_layout(KeyLayout::openrts);
    check(engineer_keys(both, true) == "E — открыть дверь, Shift+E — подняться", "on foot: E and Shift+E");
    check(engineer_keys("Ctrl+Enter: climb up", true) == "Shift+E: climb up", "on foot: Ctrl+Enter alone");
    check(engineer_keys("Enter: stand up", false) == "Enter: stand up", "seated: Enter stays (E is the cab's)");
    set_key_layout(KeyLayout::rts);
    check(engineer_keys(both, true) == both, "RTS's keys: as written");

    check(offers_enter(both), "a door and the steps: Enter offered");
    check(offers_enter("Enter: throw the switch"), "a switch: Enter offered");
    check(!offers_enter("Ctrl+Enter: climb up"), "a ladder only: no Enter (E opens the car menu)");
    check(!offers_enter("Ctrl+Enter — пройти"), "go through only: no Enter");
    check(!offers_enter(""), "nothing at hand");
    if (failures == 0) std::cout << "key_layout_test: ok\n";
    return failures == 0 ? 0 : 1;
}
