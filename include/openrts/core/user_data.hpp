#pragma once

#include <filesystem>

// Where openRTS keeps what it writes (settings, saves, the server's files):
// never inside an RTS installation.
// (program_directory(): installation.hpp.)
namespace openrts::core {

// %LOCALAPPDATA%\openRTS, or $XDG_DATA_HOME/openRTS (~/.local/share/openRTS);
// empty when the system gives none.
[[nodiscard]] std::filesystem::path per_user_directory();
// Next to the program when a portable.txt is there, else the per-user
// folder (made when missing), else the program's folder.
[[nodiscard]] std::filesystem::path find_user_data_directory();

} // namespace openrts::core
