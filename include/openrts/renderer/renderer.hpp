#pragma once

#include <string_view>

namespace openrts::renderer {

[[nodiscard]] std::string_view backend_name() noexcept;

[[nodiscard]] bool webgpu_available() noexcept;

} // namespace openrts::renderer
