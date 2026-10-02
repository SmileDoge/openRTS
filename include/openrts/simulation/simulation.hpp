#pragma once

#include <string_view>

namespace openrts::simulation {

[[nodiscard]] std::string_view module_name() noexcept;

} // namespace openrts::simulation
