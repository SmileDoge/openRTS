#include <openrts/audio/interior_acoustics.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace openrts::audio {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

float between(const float closed, const float open, const std::span<const float> fractions, const std::size_t index) {
    const float f = index < fractions.size() ? std::clamp(fractions[index], 0.0f, 1.0f) : 0.0f;
    return closed + (open - closed) * f;
}

} // namespace

InteriorAcoustics::InteriorAcoustics(const formats::CabDescription& cab) {
    std::vector<std::string> names;
    for (const auto& area : cab.areas) {
        rooms_.push_back({area.x1, area.z1, area.x2, area.z2});
        names.push_back(lower(area.name));
    }
    // RTS 0x4D47A4 reads each end of Connects; the file is lower-cased first.
    const auto side = [&](const std::string& text) {
        const auto name = lower(text);
        if (name == "exterior" || name == "ext_wag" || name == "wagon_back" || name == "wagon_front")
            return Side{End::exterior, 0};
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == name) return Side{End::room, static_cast<int>(i)};
        // INTERIOR, INTERIORPASS or an unknown name: never matched as the
        // side a sound comes from; as the other side, RTS reads its unset
        // room number, room 0.
        return Side{End::other, 0};
    };
    for (const auto& door : cab.doors) doors_.push_back({side(door.from), side(door.to), door.sound_closed, door.sound_open});
    // A window belongs to the room its Position is in (0x51CD88 -> 0x51434C);
    // one without a Position to none.
    for (const auto& window : cab.windows) {
        const int room = window.has_position ? room_at(window.position[0], window.position[2]) : outside;
        windows_.push_back({room, window.sound_closed, window.sound_open});
    }
}

int InteriorAcoustics::room_at(const float x, const float z) const noexcept {
    // RTS keeps the rectangles with X negated and tests -X1 > -x > -X2 and
    // Z1 > z > Z2; the last area that holds the point wins.
    int found = outside;
    for (std::size_t i = 0; i < rooms_.size(); ++i) {
        const auto& r = rooms_[i];
        if (r.x1 < x && x < r.x2 && r.z2 < z && z < r.z1) found = static_cast<int>(i);
    }
    return found;
}

float InteriorAcoustics::transmission(const int from, const int to, const std::span<const float> door_open,
                                      const std::span<const float> window_open) const {
    std::vector<int> visited;
    return pass(from, to, visited, false, door_open, window_open);
}

// 0x5143DC: through every door of room `from` (0x513E8C picks the doors and
// their other side) to room `to`, the best way; a way's share is the
// product of its doors'. Towards the open air the room's windows count too.
// `visited` (with `chained`): the rooms the way came through, which it does
// not enter again (the open air as `outside`).
float InteriorAcoustics::pass(const int from, const int to, std::vector<int>& visited, const bool chained,
                              const std::span<const float> door_open, const std::span<const float> window_open) const {
    if (from == to) return 1.0f;
    const auto exterior_like = [](const Side& s) { return s.kind == End::exterior; };
    float best = 0.0f;
    for (std::size_t k = 0; k < doors_.size(); ++k) {
        const auto& door = doors_[k];
        const Side* other = nullptr;
        if (from != outside) {
            if (door.a.kind == End::room && door.a.room == from) other = &door.b;
            else if (door.b.kind == End::room && door.b.room == from) other = &door.a;
        } else {
            if (exterior_like(door.a)) other = &door.b;
            else if (exterior_like(door.b)) other = &door.a;
        }
        if (!other) continue;
        if (chained) {
            if (from != outside ? other->kind == End::room && other->room == from : exterior_like(*other)) continue;
            bool seen = false;
            for (const int room : visited) {
                if (room == outside ? exterior_like(*other) : other->kind == End::room && other->room == room) seen = true;
            }
            if (seen) continue;
        }
        float share = between(door.closed, door.open, door_open, k);
        if (share > best) {
            const int next = exterior_like(*other) ? outside : other->room;
            visited.push_back(from);
            share *= pass(next, to, visited, true, door_open, window_open);
            visited.pop_back();
        }
        best = (std::max)(best, share);
    }
    if (to == outside) {
        for (std::size_t k = 0; k < windows_.size(); ++k) {
            if (windows_[k].room != from) continue;
            best = (std::max)(best, between(windows_[k].closed, windows_[k].open, window_open, k));
        }
    }
    return best;
}

} // namespace openrts::audio
