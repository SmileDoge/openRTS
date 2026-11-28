#pragma once

// Builds the cargo pile and stream models (openrts/renderer/cargo_shape.hpp).
// Internal to openrts_renderer.

#include <optional>
#include <string>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/shape_binary.hpp>

namespace openrts::renderer {

struct CargoShape {
    formats::ShapeModel model;
    std::optional<core::VirtualPath> texture; // its texture file
    std::string error;
};

[[nodiscard]] CargoShape build_cargo_shape(const core::VirtualPath& path);

} // namespace openrts::renderer
