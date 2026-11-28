#include <openrts/renderer/grass_blockers.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace openrts::renderer {

std::uint64_t GrassBlockers::key(const std::int64_t cx, const std::int64_t cz) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) | static_cast<std::uint32_t>(cz);
}

void GrassBlockers::add(const double cx, const double cz, const double ax, const double az, const double half_a, const double half_b) {
    const auto index = static_cast<std::uint32_t>(boxes_.size());
    boxes_.push_back({cx, cz, ax, az, half_a, half_b});
    // The cells its bounding square touches.
    const double reach = std::abs(ax) * half_a + std::abs(az) * half_b + 1.0;
    const double reach_z = std::abs(az) * half_a + std::abs(ax) * half_b + 1.0;
    const auto x0 = static_cast<std::int64_t>(std::floor((cx - reach) / cell)), x1 = static_cast<std::int64_t>(std::floor((cx + reach) / cell));
    const auto z0 = static_cast<std::int64_t>(std::floor((cz - reach_z) / cell)), z1 = static_cast<std::int64_t>(std::floor((cz + reach_z) / cell));
    for (auto z = z0; z <= z1; ++z)
        for (auto x = x0; x <= x1; ++x) cells_[key(x, z)].push_back(index);
}

bool GrassBlockers::covers(const double x, const double z, const double margin) const {
    const auto found = cells_.find(key(static_cast<std::int64_t>(std::floor(x / cell)), static_cast<std::int64_t>(std::floor(z / cell))));
    if (found == cells_.end()) return false;
    for (const auto i : found->second) {
        const auto& b = boxes_[i];
        const double dx = x - b.cx, dz = z - b.cz;
        const double a = dx * b.ax + dz * b.az;
        const double c = -dx * b.az + dz * b.ax;
        if (std::abs(a) <= b.half_a + margin && std::abs(c) <= b.half_b + margin) return true;
    }
    return false;
}

namespace {
std::mutex blockers_mutex;
std::shared_ptr<const GrassBlockers> blockers_set;
std::uint64_t blockers_generation = 0;
} // namespace

void set_grass_blockers(std::shared_ptr<const GrassBlockers> blockers) {
    const std::lock_guard lock(blockers_mutex);
    blockers_set = std::move(blockers);
    ++blockers_generation;
}

std::shared_ptr<const GrassBlockers> grass_blockers() {
    const std::lock_guard lock(blockers_mutex);
    return blockers_set;
}

std::uint64_t grass_blockers_generation() {
    const std::lock_guard lock(blockers_mutex);
    return blockers_generation;
}

} // namespace openrts::renderer
