#pragma once

#include <string_view>

namespace openrts::compat {

[[nodiscard]] std::string_view module_name() noexcept;

} // namespace openrts::compat
