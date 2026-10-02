#pragma once

#include <string_view>

#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_file.hpp>

namespace openrts::formats {

[[nodiscard]] std::string_view module_name() noexcept;

} // namespace openrts::formats
