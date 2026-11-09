#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/net/play.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/net/wire.hpp>
#include <openrts/server/server_route.hpp>
#include <openrts/server/transport.hpp>

namespace openrts::simulation {
class SignalSystem;
class TrackNetwork;
class Train;
} // namespace openrts::simulation

// The openRTS server's game (docs/server.md), Legacy on: it speaks the RTS
// protocol (docs/reverse/multiplayer.md, multiplayer-protocol.md) so that
// rts1.exe and openRTS clients play together, as on rts_server.exe:
// - the login and its reply, the startup snapshot;
// - each client simulates its own trains and sends their places; the server
//   relays them, and the lights, looks and sounds to players within 3 km;
// - the server decides junctions, signals and the time of day: the signals
//   run their scripts on the server's network with every train on it;
// - couplings and cuts are carried out on the server's copy as RTS does
//   them, and passed on;
// - the dispatcher (the server's console) places trains, assigns them,
//   throws junctions, sets signals, sends messages and pauses.
// It knows no sockets: a Transport carries its bytes; the caller feeds it
// what came in and ticks it.
namespace openrts::server {

struct ServerOptions {
    std::string route_id;          // as the login reply names the route (rts_server.exe: lower case)
    std::u16string description;    // "Маршрут: …\r\nСезон: …"
    std::uint8_t season = 1;       // 0 spring .. 3 winter
    std::uint8_t weather = 0;      // 0 clear, 1 snow, 2 rain
    RouteCounts counts;
    double time_of_day = 12.0 * 3600.0; // s
    // rts_server.exe sends the time every 300 s (live capture 2026-11-05).
    double time_period = 300.0;
    bool admit = true;             // new players taken (else 0x4702)
    // Remote dispatcher consoles (0x4500: rts_server_DSP.exe, openRTS's in
    // the game). Off: refused with 0x4702. They come after the server
    // itself, which is the dispatcher's as rts_server.exe is (the user's
    // call, 2026-11-09); the code stays, the tests run it.
    bool consoles = false;
    // Nicks that may log in as a console from anywhere; from this computer
    // any nick may.
    std::vector<std::string> dispatchers;
};

// A train the dispatcher puts on the track.
struct NewTrain {
    std::vector<simulation::CarSetup> cars;
    std::vector<net::TrainCar> records;
    std::uint32_t node{};  // the front of car 0
    double offset{};
    bool forward = true;
    std::uint16_t kind = 0;    // 0 STATIC, 1 PLAYER, 3 LOOSE
    std::uint16_t subtype = 0; // 0 "Прочий", 1 "Пассажирский", 2 "Грузовой"
    std::uint8_t state = 0;    // 0 "Холодный", 1 "Включен", 2 "Движется"
    float top_speed{};         // m/s
    std::uint32_t number{};    // the train number (the cars' uids carry it)
    std::u16string name;       // the console's name, may be empty
};

// One line of a report for the console.
struct TrainSummary {
    std::uint32_t id{}, number{};
    std::size_t route_junctions{}; // held for its route
    std::uint16_t kind{};
    std::string owner;   // the player's nick, empty none
    std::size_t cars{};
    bool placed = false; // on the server's track
    bool stands = false;
    float speed{};
    std::string where;   // "node 12 at 340.1 m"
};

struct UserSummary {
    std::uint32_t id{};
    std::string nick;
    std::string address;
    bool connected = false;
    bool playing = false;
    bool console = false; // the dispatcher's console
    std::vector<std::uint32_t> trains;
    // The last 0x462D place: the camera's (tile and the place in it), and
    // how far it is from the user's trains; empty none.
    std::string where;
};

class GameServer {
public:
    // `signals` may be nullptr (a route without them). Both outlive the server.
    GameServer(Transport& transport, ServerOptions options, simulation::TrackNetwork& network, simulation::SignalSystem* signals);
    ~GameServer();
    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;

    // What the transport saw.
    void connected(ConnectionId connection, std::string address);
    void received(ConnectionId connection, std::uint16_t id, net::ByteView body);
    void closed(ConnectionId connection);
    // Steps the world to `now` (seconds of a steady clock): the time of day,
    // the trains between their places, the signals; sends what changed.
    // Call it at least 20 times a second (the signals' 0.05 s step).
    void tick(double now);

    // The dispatcher's actions. Each sends what RTS's server sends for it.
    // A train is not put where another stands ("occupied by train N").
    [[nodiscard]] std::optional<std::uint32_t> add_train(NewTrain train, std::string& error);
    bool assign(std::uint32_t train, std::string_view nick, std::string& error);
    bool unassign(std::uint32_t train, std::string& error);
    bool remove_train(std::uint32_t train, std::string& error);
    // To one player (by nick) or to everyone ("*").
    bool message(std::string_view nick, std::u16string_view text, std::string& error);
    void set_paused(bool paused);
    [[nodiscard]] bool paused() const noexcept { return paused_; }
    void set_time(double seconds);
    [[nodiscard]] double time_of_day() const noexcept { return time_; }
    // Throws junction `node` (TDB node number); `lock` set or cleared when given.
    bool set_junction(std::uint32_t node, std::optional<std::uint32_t> branch, std::optional<bool> lock, std::string& error);
    // Holds the heads of TDB signal item `item` at `aspect` (the console's
    // manual state), or gives them back to their scripts (nothing).
    bool set_signal(std::uint32_t item, std::optional<formats::SignalAspect> aspect, std::string& error);
    bool kick(std::string_view nick, std::string& error);
    // A train's route, as the console assembles one: from the train's front
    // (else its rear, when it runs rear first) through each stop in turn,
    // the shortest way; a stop is the TDB signal items it may be (one name
    // can stand at several stations: the nearest along the way is taken).
    // Every junction on it is set, facing and trailing alike, and held:
    // players cannot throw it until the train's rear has passed it. The
    // signals open by their own scripts. `result`: what was set, or why not.
    bool set_route(std::uint32_t train, const std::vector<std::vector<std::uint32_t>>& stops, std::string& result);
    bool cancel_route(std::uint32_t train, std::string& error);
    // RTS's hand route (the console's "Задать маршрут", 0x4619): `length` m
    // from the train's head along the junctions as they stand (from its rear
    // when `rear_first`, the console's direction flag). Its junctions are held
    // as set_route's; the distance left goes to everyone as 0x4619 every 0.5
    // s while it changes, -1 when it ends.
    bool lay_route(std::uint32_t train, double length, bool rear_first, std::string& result);
    // The junctions a train's route holds (TDB nodes), in its order.
    [[nodiscard]] std::vector<std::uint32_t> route_junctions(std::uint32_t train) const;
    void set_admit(bool admit) noexcept { options_.admit = admit; }
    [[nodiscard]] bool admits() const noexcept { return options_.admit; }
    // What a console's command line (console_command) runs: the server's
    // console commands; its answer goes back as console_reply.
    void set_console_commands(std::function<std::string(std::string_view line)> run) { console_commands_ = std::move(run); }
    // What a console's consist (console_place) does: put on the track.
    void set_console_place(std::function<std::string(const net::ConsolePlace&)> place) { console_place_ = std::move(place); }

    [[nodiscard]] std::vector<TrainSummary> trains() const;
    [[nodiscard]] std::vector<UserSummary> users() const;
    [[nodiscard]] std::uint32_t free_number() const; // the lowest train number not in use, from 101
    // Lines worth logging since the last call (logins, couplings, refusals).
    [[nodiscard]] std::vector<std::string> take_log();

private:
    struct User;
    struct Train;
    struct Impl;
    // 0x4619 to everyone, kept in the train's record for the snapshot.
    void send_route_left(std::uint32_t train, float left, bool rear_first);
    std::unique_ptr<Impl> impl_;
    Transport& transport_;
    ServerOptions options_;
    double time_{};
    bool paused_ = false;
    std::function<std::string(std::string_view)> console_commands_;
    std::function<std::string(const net::ConsolePlace&)> console_place_;
};

} // namespace openrts::server
