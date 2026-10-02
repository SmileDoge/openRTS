#pragma once

#include <string_view>

#include <openrts/assets/content_resolver.hpp>

namespace openrts::assets {

[[nodiscard]] std::string_view module_name() noexcept;

} // namespace openrts::assets
