#pragma once

#include <string_view>

namespace openrts::platform {

[[nodiscard]] std::string_view module_name() noexcept;

} // namespace openrts::platform
