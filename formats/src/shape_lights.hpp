#pragma once

// The LightSource entries of an ESD_LightSources block (a shape's .sd) or of
// the LightSources block of openRTS's override of a shape (shape_override).
// Internal to openrts_formats.

#include <vector>

#include <openrts/formats/shape_description.hpp>

#include "text_tree.hpp"

namespace openrts::formats::detail {

[[nodiscard]] std::vector<ShapeLight> read_light_sources(const TextNode& list);

} // namespace openrts::formats::detail
