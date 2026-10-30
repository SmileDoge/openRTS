#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

struct ShapeDescription {
    std::filesystem::path shape_file;
};

struct ShapeDescriptionResult {
    std::optional<ShapeDescription> value;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

// Extracts the model filename from the root Shape(...) node of an MSTS/RTS
// text SD file. Other ESD_* nodes are deliberately not interpreted yet.
[[nodiscard]] ShapeDescriptionResult parse_shape_description(std::string_view text);
[[nodiscard]] ShapeDescriptionResult load_shape_description(const std::filesystem::path& file);
[[nodiscard]] ShapeDescriptionResult load_shape_description(const core::Vfs& vfs, const core::VirtualPath& file);

} // namespace openrts::formats
