#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/server/game_server.hpp>
#include <openrts/server/server_route.hpp>
#include <openrts/server/transport.hpp>

// A running openRTS server (docs/server.md): its route loaded, its port
// open, its game. The console program and the server's window both run one.
namespace openrts::server {

struct SessionSettings {
    std::filesystem::path root;  // the RTS installation
    std::string route;           // a folder under ROUTES
    std::filesystem::path data;  // where the server keeps its files (empty: openRTS's user data folder)
    std::uint16_t port = 800;
    std::uint8_t season = 1;     // 0 spring .. 3 winter
    std::uint8_t weather = 0;    // 0 clear, 1 snow, 2 rain, 3 storm
    bool weather_dynamic = false; // openRTS's dynamic weather from the start
    double time_of_day = 12.0 * 3600.0;
    bool admit = true;
    bool local = false;          // listens on 127.0.0.1 alone (tests: no firewall prompt)
    // The dispatcher's console from another computer logs in with it (empty:
    // only from this one; ServerOptions::dispatcher_password).
    std::string dispatcher_password;
    // The players' Discord "Join" (ServerOptions::discord_join) and the
    // address it hands over (empty: the one each player connected to).
    bool discord_join = false;
    std::string public_address;
    std::uint32_t max_players = 0; // places (ServerOptions::max_players), 0 no limit
    bool fast_positions = true;    // ServerOptions::fast_positions
    // Every train on the track saved as the placement "autosave" so often
    // and when the server stops (0: never); at the start the last run's is
    // kept as "autosave-previous" (docs/server.md, "Placements").
    double autosave_minutes = 2.0;
};

class ServerSession {
public:
    // The route loaded, the port open, the game made. Nothing and `error`
    // when not; `lines` gets what the loading tells (notes, the route).
    [[nodiscard]] static std::unique_ptr<ServerSession> start(SessionSettings settings, std::string& error, std::vector<std::string>& lines);
    ~ServerSession();
    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    // The network's events, then the game's step; `now`: seconds since the start.
    void step(double now);
    // A console line (help, trains, signal ...); "quit" sets `quit`.
    [[nodiscard]] std::string command(std::string_view line, bool& quit);

    [[nodiscard]] GameServer& game() noexcept { return *game_; }
    [[nodiscard]] ServerRoute& route() noexcept { return *route_; }
    [[nodiscard]] const SessionSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] std::uint16_t port() const;

private:
    ServerSession() = default;
    SessionSettings settings_;
    std::unique_ptr<ServerRoute> route_;
    std::unique_ptr<TcpServer> tcp_;
    std::unique_ptr<GameServer> game_;
    double autosaved_at_ = 0.0; // `now` of the last autosave
};

} // namespace openrts::server
