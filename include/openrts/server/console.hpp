#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/net/placement.hpp>
#include <openrts/net/play.hpp>
#include <openrts/server/game_server.hpp>
#include <openrts/server/server_route.hpp>
#include <openrts/simulation/train_route.hpp>

// The openRTS server's console: the dispatcher's commands typed at the
// server (docs/server.md), until the dispatcher console of its own
// (roadmap step 6, stage 5).
namespace openrts::server {

// A train from a consist, standing with its rear at a path's start; `slot`
// n > 0 puts it n of its lengths and 30 m each further back (the next place
// when one stands there).
[[nodiscard]] std::optional<NewTrain> train_on_path(ServerRoute& route, std::string_view consist, std::string_view path,
                                                    std::uint32_t number, std::string& error, std::uint32_t slot = 0);

// A train from a consist with the front of its first car at `front` (the
// way it faces is the way the train faces): the console's map's placing.
[[nodiscard]] std::optional<NewTrain> train_at(ServerRoute& route, std::string_view consist, const simulation::TrackLocation& front,
                                               std::uint32_t number, std::string& error);

// A console's consist by its cars (console_place) put on the track: at a
// path's start or at a place; what to tell the console.
[[nodiscard]] std::string place_cars(GameServer& server, ServerRoute& route, const net::ConsolePlace& place,
                                     std::uint32_t* placed = nullptr);
// A standing train put anew (2026-11-21, the organiser): at another place
// (the front of its first car, facing the way it runs) and/or with chocks
// under its ends (1 at its head, 2 at its tail; RTS's PlaceDrag), its
// number, names, kind, state and cars kept. Removed and placed again (a new
// id, `placed`), left where it was when the new place is refused; not a
// train a player drives.
[[nodiscard]] std::string replace_train(GameServer& server, ServerRoute& route, std::uint32_t train,
                                        const std::optional<simulation::TrackLocation>& at, std::optional<std::uint8_t> chocks,
                                        std::uint32_t* placed = nullptr);
// The chocks under a train's ends as replace_train takes them (1 head, 2 tail).
[[nodiscard]] std::uint8_t train_chocks(const GameServer& server, std::uint32_t train);

// Placements (docs/server.md, "Placements"): every train on the track
// saved under a name in the route's placements folder, and put back (the
// trains on the track removed first, unless `add`); the names saved. What
// to tell the console.
[[nodiscard]] std::string save_placement(GameServer& server, ServerRoute& route, const std::string& name);
[[nodiscard]] std::string load_placement(GameServer& server, ServerRoute& route, const std::string& name, bool add);
[[nodiscard]] std::vector<std::string> placement_names(ServerRoute& route);
// The trains on the track now as a placement (net/placement.hpp); a saved
// one's file; every train removed (how many).
[[nodiscard]] std::vector<net::PlacementTrain> standing_placement(const GameServer& server);
[[nodiscard]] std::optional<std::string> placement_text(ServerRoute& route, const std::string& name);
std::size_t clear_trains(GameServer& server);
// A saved placement as it is, and written (whole, then put in place of the
// old file: a crash leaves the old one); with its included placements'
// trains (`from` theirs), what was wrong with them in `problems`.
[[nodiscard]] std::optional<net::Placement> placement_file_read(ServerRoute& route, const std::string& name, std::string& error);
[[nodiscard]] std::string placement_file_write(ServerRoute& route, const std::string& name, const net::Placement& placement);
[[nodiscard]] std::vector<net::PlacementTrain> placement_resolved(ServerRoute& route, const net::Placement& placement, const std::string& name,
                                                                  std::vector<std::string>& problems);
// A placement's includes: another one added or taken out.
[[nodiscard]] std::string placement_include(ServerRoute& route, const std::string& name, const std::string& other, bool add);
// The autosave (docs/server.md): every train on the track as the placement
// "autosave" (none on the track: the last one kept); at the start the last
// run's kept as "autosave-previous".
inline constexpr const char* autosave_name = "autosave";
inline constexpr const char* autosave_previous_name = "autosave-previous";
std::string autosave_placement(GameServer& server, ServerRoute& route);
void keep_previous_autosave(ServerRoute& route);

// Splits a command line into words; "double quotes" keep spaces (consist
// names have them).
[[nodiscard]] std::vector<std::string> command_words(std::string_view line);

// Runs one command; returns what to print. `quit` is set by "quit".
[[nodiscard]] std::string run_command(GameServer& server, ServerRoute& route, std::string_view line, bool& quit);

// "HH:MM[:SS]" as seconds of the day.
[[nodiscard]] std::optional<double> parse_time_of_day(std::string_view text);

// The season's and weather's index from a name or a number; the weather's
// 3 (storm, гроза) is openRTS's thunderstorm: rain with lightning.
[[nodiscard]] std::optional<std::uint8_t> parse_season(std::string_view text);
[[nodiscard]] std::optional<std::uint8_t> parse_weather(std::string_view text);
// "Маршрут: <id>\r\nСезон: <season>", as rts_server.exe describes its game.
[[nodiscard]] std::u16string game_description(std::string_view route_id, std::uint8_t season);

} // namespace openrts::server
