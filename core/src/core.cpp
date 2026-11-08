#include <openrts/core/core.hpp>

namespace openrts::core {

std::string_view module_name() noexcept {
    return "openrts_core";
}

std::string_view version() noexcept {
    return OPENRTS_VERSION;
}

} // namespace openrts::core
