#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// openRTS's own edits kept as a mod (MODS_OPENRTS/<mod>/, docs/vfs-tutorial-ru.md):
// the inspector's shape edits and the car menu's distributors. A mod is
// mounted over the content, so its files change nothing of the content's.
namespace openrts::viewer {

inline constexpr const char* default_edits_mod = "openrts_edits";

// The mod the edits go to: the one last saved into (the inspector's field
// sets it), else openrts_edits.
[[nodiscard]] std::string edits_mod();

// <root>/MODS_OPENRTS/<mod>/<virtual_file>.
[[nodiscard]] std::filesystem::path mod_file(const std::filesystem::path& root, std::string_view mod, std::string_view virtual_file);

// Adds `mod` to the installation's openrts.ini [Mods] (after the others,
// numbered when they are); false when it was there already. A mod listed
// now is mounted from the next start.
bool list_mod(const std::filesystem::path& root, const std::string& mod);

// The path as UTF-8 with forward slashes (for messages).
[[nodiscard]] std::string utf8_path(const std::filesystem::path& path);

} // namespace openrts::viewer
