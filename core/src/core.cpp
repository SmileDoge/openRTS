#include <openrts/core/core.hpp>

namespace openrts::core {

std::string_view module_name() noexcept {
    return "openrts_core";
}

std::string_view version() noexcept {
    return OPENRTS_VERSION;
}

std::string_view build_label() noexcept {
#ifdef OPENRTS_BUILD_LABEL
    return OPENRTS_BUILD_LABEL;
#else
    return {};
#endif
}

std::string version_text() {
    std::string text(version());
    if (!build_label().empty()) text += " (" + std::string(build_label()) + ")";
    return text;
}

} // namespace openrts::core
