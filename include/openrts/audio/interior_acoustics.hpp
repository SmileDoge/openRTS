#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <openrts/formats/cab_description.hpp>

namespace openrts::audio {

// The rooms of a cab or salon (InteriorAreas) and how much of a sound gets
// from one to another through the doors and windows between them, as RTS
// works it out (rts1.exe 0x51434C, 0x5143DC, 0x513E8C; docs/sound.md,
// "Rooms and sound proofing"). Coordinates are the cab shape's.
class InteriorAcoustics {
public:
    static constexpr int outside = -1;

    InteriorAcoustics() = default;
    explicit InteriorAcoustics(const formats::CabDescription& cab);

    [[nodiscard]] bool empty() const noexcept { return rooms_.empty(); }
    [[nodiscard]] std::size_t doors() const noexcept { return doors_.size(); }
    [[nodiscard]] std::size_t windows() const noexcept { return windows_.size(); }
    // The room at (x, z): the last InteriorArea whose rectangle holds it
    // (strictly), else `outside`.
    [[nodiscard]] int room_at(float x, float z) const noexcept;
    // The share (0..1) of a sound in room `from` that reaches room `to`
    // (`outside`: the open air). `door_open` / `window_open`: how far each
    // door and window of the description is open, 0..1 (missing: shut).
    [[nodiscard]] float transmission(int from, int to, std::span<const float> door_open,
                                     std::span<const float> window_open) const;

private:
    // Connects ( a b ) ends (RTS 0x4D47A4): EXTERIOR -1, EXT_WAG -2,
    // INTERIOR -3, WAGON_BACK -4 / WAGON_FRONT -5, INTERIORPASS -7, a room
    // -6. The open air is EXTERIOR, EXT_WAG and the two car ends.
    enum class End : std::uint8_t { room, exterior, other };
    struct Side {
        End kind = End::other;
        int room = 0; // End::room; RTS reads the others as room 0
    };
    struct Rect {
        float x1, z1, x2, z2;
    };
    struct Door {
        Side a, b;
        float closed, open;
    };
    struct Window {
        int room;
        float closed, open;
    };
    [[nodiscard]] float pass(int from, int to, std::vector<int>& visited, bool chained, std::span<const float> door_open,
                             std::span<const float> window_open) const;

    std::vector<Rect> rooms_;
    std::vector<Door> doors_;
    std::vector<Window> windows_;
};

} // namespace openrts::audio
