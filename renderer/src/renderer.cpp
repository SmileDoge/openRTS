#include <openrts/renderer/renderer.hpp>

namespace openrts::renderer {

std::string_view backend_name() noexcept {
#ifdef OPENRTS_HAS_WGPU
    return "wgpu-native";
#else
    return "null";
#endif
}

bool webgpu_available() noexcept {
#ifdef OPENRTS_HAS_WGPU
    return true;
#else
    return false;
#endif
}

} // namespace openrts::renderer
