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
// closes. `given`: what the command line set, kept over the form as it was
// last started (the start menu passes the RTS folder and the port). The
// program's exit code.
struct WindowGiven {
    bool root = false, port = false, password = false;
};
int run_server_window(SessionSettings settings, const std::vector<std::string>& commands, const std::filesystem::path& capture,
                      double capture_at, WindowGiven given = {});

} // namespace openrts::server
