#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace openrts::viewer {

// The system's "Open" dialog (Windows: the Explorer one; elsewhere nothing),
// starting in `folder`, showing files that match `pattern` (e.g. "*.con")
// under `description`. Blocks until it closes; nothing when cancelled. The
// working directory is left as it was.
[[nodiscard]] std::optional<std::filesystem::path> open_file_dialog(const std::filesystem::path& folder,
                                                                     std::wstring_view description, std::wstring_view pattern);

// The system's folder picker (Windows: the Explorer one; elsewhere nothing),
// starting in `folder`, titled `title`. Blocks until it closes; nothing when
// cancelled.
[[nodiscard]] std::optional<std::filesystem::path> open_folder_dialog(const std::filesystem::path& folder, std::wstring_view title);

} // namespace openrts::viewer
