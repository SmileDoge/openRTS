#pragma once

// Trees kept off tracks and roads: an openRTS option, not RTS. RTS plants a
// forest's trees over its whole Area (rts1.exe 0x4B3C40, no test against the
// track), so a forest drawn across a line stands on the rails
// (docs/world-objects.md).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace openrts::renderer {

// The axes of a route's tracks and roads as straight pieces, in route metres
// (tile * 2048 + the tile's own metres, MSTS axes), found by a grid.
class ForestClearance {
public:
    // `half_width`: how far the way reaches from its axis (a road is wider
    // than a track); a tree's crown keeps the distance from that edge.
    void add(double ax, double az, double bx, double bz, float half_width);
    // How far (x, z) is from the nearest edge, if one is closer than
    // `within`; else `within`.
    [[nodiscard]] double nearest(double x, double z, double within) const;
    [[nodiscard]] std::size_t size() const noexcept { return pieces_.size(); }

private:
    struct Piece {
        double ax, az, bx, bz;
        float half_width;
    };
    static constexpr double cell = 32.0;
    [[nodiscard]] static std::uint64_t key(std::int64_t cx, std::int64_t cz) noexcept;
    std::vector<Piece> pieces_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells_;
};

// The route's axes, from its track and road databases (the viewer sets them
// when a route is loaded; null: none). Forests built afterwards use them.
void set_forest_clearance_axes(std::shared_ptr<const ForestClearance> axes);
// How far a tree's crown keeps from a track's or road's edge, metres; 0:
// trees anywhere, as RTS. Forests built afterwards use it (the viewer
// reloads the loaded ones when it changes).
void set_forest_clearance_distance(float metres);
[[nodiscard]] float forest_clearance_distance();

// Trees planted and left out by the clearance since the start, for reports.
struct ForestTreeCounts {
    std::uint64_t planted{};
    std::uint64_t cleared{};
};
[[nodiscard]] ForestTreeCounts forest_tree_counts();

} // namespace openrts::renderer
