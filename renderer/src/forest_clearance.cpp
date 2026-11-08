#include <openrts/renderer/forest_clearance.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "forest_clearance_internal.hpp"

namespace openrts::renderer {

namespace {

std::mutex setting_mutex;
std::shared_ptr<const ForestClearance> setting_axes;
float setting_distance = 0.0f;
std::atomic<std::uint64_t> planted_trees{0}, cleared_trees{0};

} // namespace

std::uint64_t ForestClearance::key(const std::int64_t cx, const std::int64_t cz) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) | static_cast<std::uint32_t>(cz);
}

void ForestClearance::add(const double ax, const double az, const double bx, const double bz, const float half_width) {
    const auto index = static_cast<std::uint32_t>(pieces_.size());
    pieces_.push_back({ax, az, bx, bz, half_width});
    // Every cell the piece's box touches (pieces are a few metres long).
    const auto x0 = static_cast<std::int64_t>(std::floor((std::min)(ax, bx) / cell));
    const auto x1 = static_cast<std::int64_t>(std::floor((std::max)(ax, bx) / cell));
    const auto z0 = static_cast<std::int64_t>(std::floor((std::min)(az, bz) / cell));
    const auto z1 = static_cast<std::int64_t>(std::floor((std::max)(az, bz) / cell));
    for (auto cx = x0; cx <= x1; ++cx)
        for (auto cz = z0; cz <= z1; ++cz) cells_[key(cx, cz)].push_back(index);
}

double ForestClearance::nearest(const double x, const double z, const double within) const {
    double best = within;
    const auto x0 = static_cast<std::int64_t>(std::floor((x - within) / cell));
    const auto x1 = static_cast<std::int64_t>(std::floor((x + within) / cell));
    const auto z0 = static_cast<std::int64_t>(std::floor((z - within) / cell));
    const auto z1 = static_cast<std::int64_t>(std::floor((z + within) / cell));
    for (auto cx = x0; cx <= x1; ++cx) {
        for (auto cz = z0; cz <= z1; ++cz) {
            const auto found = cells_.find(key(cx, cz));
            if (found == cells_.end()) continue;
            for (const auto index : found->second) {
                const auto& p = pieces_[index];
                const double dx = p.bx - p.ax, dz = p.bz - p.az;
                const double length2 = dx * dx + dz * dz;
                double t = length2 > 0.0 ? ((x - p.ax) * dx + (z - p.az) * dz) / length2 : 0.0;
                t = std::clamp(t, 0.0, 1.0);
                const double ex = p.ax + t * dx - x, ez = p.az + t * dz - z;
                best = (std::min)(best, std::sqrt(ex * ex + ez * ez) - p.half_width);
            }
        }
    }
    return best;
}

void set_forest_clearance_axes(std::shared_ptr<const ForestClearance> axes) {
    const std::lock_guard lock(setting_mutex);
    setting_axes = std::move(axes);
}

void set_forest_clearance_distance(const float metres) {
    const std::lock_guard lock(setting_mutex);
    setting_distance = (std::max)(metres, 0.0f);
}

float forest_clearance_distance() {
    const std::lock_guard lock(setting_mutex);
    return setting_distance;
}

ForestClearanceUse forest_clearance_use() {
    const std::lock_guard lock(setting_mutex);
    if (setting_distance <= 0.0f || !setting_axes || setting_axes->size() == 0) return {};
    return {setting_axes, setting_distance};
}

void count_forest_trees(const std::uint64_t planted, const std::uint64_t cleared) {
    planted_trees += planted;
    cleared_trees += cleared;
}

ForestTreeCounts forest_tree_counts() { return {planted_trees.load(), cleared_trees.load()}; }

} // namespace openrts::renderer
