#pragma once

#include <cstdint>

namespace openrts::renderer::rts {

// Order of the .trk Environment block and of RTS's season variable.
enum class Season : std::uint8_t { spring = 0, summer = 1, autumn = 2, winter = 3 };

// RTS weather variable (MSTS activity order).
enum class Weather : std::uint8_t { clear = 0, snow = 1, rain = 2 };

} // namespace openrts::renderer::rts
