#pragma once

#include <filesystem>
#include <optional>

namespace openrts::renderer {

// Where the renderer reads its WGSL from instead of the copy built into the
// program (renderer/shaders of a source tree, say): a piece found there is
// taken from it, one missing there from the built-in copy. Set it before
// the renderer makes its pipelines; empty: the built-in copy alone. Unset,
// the environment's OPENRTS_SHADER_DIR is taken. docs/shaders.md.
void set_shader_directory(const std::filesystem::path& folder);

// The folder in use, if any.
[[nodiscard]] std::optional<std::filesystem::path> shader_directory();

} // namespace openrts::renderer
