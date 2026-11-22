#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace openrts::viewer {

// Where openRTS keeps what it writes for the player: viewer_state.ini, the
// game's log (openrts.log), imgui.ini, saved views (debug_views) and Track
// control's paths (user_paths). %LOCALAPPDATA%\openRTS on Windows
// ($XDG_DATA_HOME/openRTS elsewhere), made on first use, with the settings
// an older version kept next to the program or in the working directory
// copied in once; next to the program instead when a file `portable.txt`
// lies there.
[[nodiscard]] const std::filesystem::path& user_data_directory();

// What the viewer remembers between runs: viewer_state.ini in
// user_data_directory(), one `key=value` per line, UTF-8 (the consist folder
// of Track control's file dialog, where the HUD panels were dragged to).
[[nodiscard]] std::optional<std::string> remembered(std::string_view key);
void remember(std::string_view key, std::string_view value);

// Once per player (2026-11-22, testers: most new players asked why the
// train does not go): the brakes set to openRTS's realistic preset with
// openRTS's brakes and КЛУБ and no Brakes.dll where all of it is openRTS's,
// over whatever was remembered; later choices are kept. Called by the
// launcher and the viewer before they read the settings.
void apply_settings_defaults();

[[nodiscard]] std::filesystem::path remembered_folder(std::string_view key);
void remember_folder(std::string_view key, const std::filesystem::path& folder);

} // namespace openrts::viewer
