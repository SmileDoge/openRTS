#include "key_repeat.hpp"

#include <algorithm>

namespace openrts::viewer {

std::vector<KeyEvent> KeyRepeat::step(const std::vector<KeyEvent>& events, const double now, const double delay) {
    std::vector<KeyEvent> out;
    out.reserve(events.size() + held_.size());
    for (const auto& event : events) {
        out.push_back(event);
        std::erase_if(held_, [&](const Held& held) { return held.press.key == event.key; });
        if (event.down) held_.push_back({event, now});
    }
    if (delay > 0.0) {
        for (auto& held : held_) {
            if (now - held.last < delay) continue;
            out.push_back(held.press);
            held.last = now;
        }
    }
    return out;
}

} // namespace openrts::viewer
