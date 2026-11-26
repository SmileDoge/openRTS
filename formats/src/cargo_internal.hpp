#pragma once

// Internal to openrts_formats: ESD_Pickup_Object read from a .sd's tree.

#include <openrts/formats/cargo.hpp>

#include "text_tree.hpp"

namespace openrts::formats::detail {

[[nodiscard]] PickupDescription read_pickup_description(const TextNode& node);

} // namespace openrts::formats::detail
