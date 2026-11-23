#pragma once

// The renderer's WGSL, kept as files under renderer/shaders (one per piece,
// named by its path there without the extension: "model/interface") and
// built into the program (cmake/embed_shaders.cmake). A shader folder set by
// set_shader_directory (the viewer's --shader-dir, or OPENRTS_SHADER_DIR)
// is read instead: a shader edited there takes effect when the program
// starts again, without a build. docs/shaders.md.

#include <string_view>

namespace openrts::renderer::detail {

// The piece's text, zero-terminated, alive for the program's life. An
// unknown name is a programming error: it throws std::out_of_range.
[[nodiscard]] const char* shader_code(std::string_view name);

} // namespace openrts::renderer::detail
