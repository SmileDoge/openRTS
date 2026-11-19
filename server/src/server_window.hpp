#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <openrts/server/session.hpp>

namespace openrts::server {

// The server's window, as rts_server.exe's (docs/server.md, "The window"):
// its start form (route, season, weather, time, port, players taken), then
// the dispatcher's console on the route's map: trains, station routes,
// signals, players, the log and the console's command line. `commands`:
// console lines run once the server is up. `capture`: the window's picture
// to that file `capture_at` s after the server started (a check), then it
// closes. The program's exit code.
int run_server_window(SessionSettings settings, const std::vector<std::string>& commands, const std::filesystem::path& capture,
                      double capture_at);

} // namespace openrts::server
