#pragma once

// Where openRTS's grass does not grow (docs/grass.md): under the objects
// standing on the ground (platforms, buildings, fences, yards), as boxes
// in route metres (tile * 2048 + the tile's own metres). The viewer sets
// them from the objects placed around the camera; the grass grows again
// when they change.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace openrts::renderer {

class GrassBlockers {
public:
    // A box's footprint: its middle, its first axis (a unit vector; the
    // second at right angles), its half sizes along them, m.
    void add(double cx, double cz, double ax, double az, double half_a, double half_b);
    // Whether (x, z) is under a box (`margin` m round each).
    [[nodiscard]] bool covers(double x, double z, double margin = 0.2) const;
    [[nodiscard]] std::size_t size() const noexcept { return boxes_.size(); }

private:
    struct Box {
        double cx, cz, ax, az, half_a, half_b;
    };
    static constexpr double cell = 16.0;
    [[nodiscard]] static std::uint64_t key(std::int64_t cx, std::int64_t cz) noexcept;
    std::vector<Box> boxes_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

void set_grass_blockers(std::shared_ptr<const GrassBlockers> blockers);
[[nodiscard]] std::shared_ptr<const GrassBlockers> grass_blockers();
// Counts each set_grass_blockers.
[[nodiscard]] std::uint64_t grass_blockers_generation();

} // namespace openrts::renderer
