// The frame limit a frame takes (viewer/src/frame_limiter): none while the
// start loads, the setting in front, at most 30 frames/s in the background.
#include <iostream>
#include <string>

#include "../viewer/src/frame_limiter.hpp"

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
    check(frame_limit(0, false, false) == 0, "in front, no limit: none");
    check(frame_limit(144, false, false) == 144, "in front: the setting");
    check(frame_limit(0, false, true) == 30, "in the background, no limit: 30");
    check(frame_limit(144, false, true) == 30, "in the background, 144: 30");
    check(frame_limit(30, false, true) == 30, "in the background, 30: 30");
    check(frame_limit(144, true, true) == 0, "loading: none, even in the background");
    check(frame_limit(9999, false, false) == 0, "beyond the range: none, as the setting takes it");
    if (failures == 0) std::cout << "frame limiter: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
