#pragma once

#include <string_view>

namespace openrts::core {

[[nodiscard]] std::string_view module_name() noexcept;
// openRTS's version, CMake's project version ("0.4.2"): the windows' titles.
[[nodiscard]] std::string_view version() noexcept;

} // namespace openrts::core
