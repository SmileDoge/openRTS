#pragma once

// The forest builder's side of forest_clearance.hpp. Internal to
// openrts_renderer.

#include <cstdint>
#include <memory>

#include <openrts/renderer/forest_clearance.hpp>

namespace openrts::renderer {

// What a forest built now keeps its trees from: no axes when off.
struct ForestClearanceUse {
    std::shared_ptr<const ForestClearance> axes;
    float distance = 0.0f;
};
[[nodiscard]] ForestClearanceUse forest_clearance_use();
void count_forest_trees(std::uint64_t planted, std::uint64_t cleared);

} // namespace openrts::renderer
