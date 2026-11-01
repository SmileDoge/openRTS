#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace openrts::viewer {

// What the viewer remembers between runs: viewer_state.ini in the working
// directory, one `key=value` per line, UTF-8 (the consist folder of Track
// control's file dialog, where the HUD panels were dragged to).
[[nodiscard]] std::optional<std::string> remembered(std::string_view key);
void remember(std::string_view key, std::string_view value);

[[nodiscard]] std::filesystem::path remembered_folder(std::string_view key);
void remember_folder(std::string_view key, const std::filesystem::path& folder);

} // namespace openrts::viewer
