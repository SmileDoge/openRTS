#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
[[nodiscard]] std::string place_cars(GameServer& server, ServerRoute& route, const net::ConsolePlace& place);

// Splits a command line into words; "double quotes" keep spaces (consist
// names have them).
[[nodiscard]] std::vector<std::string> command_words(std::string_view line);

// Runs one command; returns what to print. `quit` is set by "quit".
[[nodiscard]] std::string run_command(GameServer& server, ServerRoute& route, std::string_view line, bool& quit);

// "HH:MM[:SS]" as seconds of the day.
[[nodiscard]] std::optional<double> parse_time_of_day(std::string_view text);

// The season's and weather's index from a name or a number.
[[nodiscard]] std::optional<std::uint8_t> parse_season(std::string_view text);
[[nodiscard]] std::optional<std::uint8_t> parse_weather(std::string_view text);
// "Маршрут: <id>\r\nСезон: <season>", as rts_server.exe describes its game.
[[nodiscard]] std::u16string game_description(std::string_view route_id, std::uint8_t season);

} // namespace openrts::server
