#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace openrts::formats {

struct ShapeImageTable {
    std::vector<std::filesystem::path> filenames;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Reads only the canonical binary shape root's images table. `body` begins
// with JINX0s1b________ (the output of narrow shape inflation). Unknown root
// siblings are skipped using their validated bounded block lengths.
[[nodiscard]] ShapeImageTable extract_image_filenames_from_binary_shape(std::string_view body);

} // namespace openrts::formats
