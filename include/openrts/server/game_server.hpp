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
    // A thunderstorm (openRTS's own; the weather is rain): told to openRTS
    // players in the answer to their openrts_hello.
    bool storm = false;
    RouteCounts counts;
    double time_of_day = 12.0 * 3600.0; // s
    // rts_server.exe sends the time every 300 s (live capture 2026-11-05).
    double time_period = 300.0;
    bool admit = true;             // new players taken (else 0x4702)
    // Dispatcher consoles (0x4500: openRTS's in the game; rts_server_DSP.exe
    // expects replies not read yet). Off: refused with 0x4702. Hidden from
    // 2026-11-09 until the server was done; on since 2026-11-21.
    bool consoles = false;
    // A console from this computer comes in with any nick. From another one
    // it needs this password (0x4707 when wrong); without one it is refused
    // (0x4702). Until 2026-11-21 a nick of `dispatchers` alone let it in.
    std::string dispatcher_password;
    // When not empty, a console from another computer also needs one of
    // these nicks.
    std::vector<std::string> dispatchers;
    // The game may be joined from the players' Discord profiles (their
    // "Join" button; told to openRTS players in the hello's answer), at this
    // address (empty: the one each player connected to).
    bool discord_join = false;
    std::string public_address;
    // Places: a new player beyond so many playing is refused (0x4702), as
    // when new players are not taken; one coming back under his nick is not.
    // 0: no limit.
    std::uint32_t max_players = 0;
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
    bool openrts = false;     // an openRTS player (said openrts_hello)
    std::uint32_t helping{};  // the train he helps on (помощник машиниста), 0 none
    std::uint32_t asking{};   // the train he asked to help on, its engineer not yet answering
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
    // The console's train form (testers, 2026-11-21): its number (0x4605 to
    // everyone, as rts_server.exe sends it when the console changes it: RTS
    // numbers the cars by it; not one another train has), and its name and
    // notes (0x461A to everyone; the console's free text "Название" and its
    // memo "Заметки"; empty both: none).
    bool set_number(std::uint32_t train, std::uint32_t number, std::string& error);
    bool set_names(std::uint32_t train, std::u16string name, std::u16string notes, std::string& error);
    // A train's name and notes, empty none.
    [[nodiscard]] std::pair<std::u16string, std::u16string> names(std::uint32_t train) const;
    // The server joins two trains standing end to end (the nearest ends
    // within 50 m), as rts_server.exe does with a LOOSE train run into
    // another (0x40DCDC -> 0x4501E0): `0x460F` to everyone, A's owner too.
    bool couple(std::uint32_t a, std::uint32_t b, std::string& error);
    // The server cuts a train before car `car` (from 0): the part with the
    // cars from it on is a train of its own, kind and owner as the train's;
    // `0x460E` to everyone, its owner too.
    bool cut(std::uint32_t train, std::uint32_t car, std::string& error);
    // To one player (by nick) or to everyone ("*").
    bool message(std::string_view nick, std::u16string_view text, std::string& error);
    void set_paused(bool paused);
    [[nodiscard]] bool paused() const noexcept { return paused_; }
    void set_time(double seconds);
    [[nodiscard]] double time_of_day() const noexcept { return time_; }
    // Throws junction `node` (TDB node number); `lock` set or cleared when given.
    bool set_junction(std::uint32_t node, std::optional<std::uint32_t> branch, std::optional<bool> lock, std::string& error);
    // Holds the signal of TDB signal item `item` at `aspect`, the dispatcher's
    // pick (the console's "set aspect": manual, a route leaves it so), or
    // with nothing puts it under automatic block. With the signals run as
    // rts_server.exe runs them (SignalSystem::dispatched; docs/server.md
    // "Signals") it is the signal unit's; else an override of its heads.
    bool set_signal(std::uint32_t item, std::optional<formats::SignalAspect> aspect, std::string& error);
    // The console's other signal actions (rts_server.exe's TfPult menu):
    // open (its script runs until a train passes it), automatic block,
    // close (manual at STOP_AND_PROCEED, as unticking "Открыт" /
    // "Автоблокировка", 0x40C648), back to its defaults from the route
    // database.
    enum class SignalAction : std::uint8_t { open, automatic, close, defaults };
    bool control_signal(std::uint32_t item, SignalAction action, std::string& error);
    // Every signal unit with a head of signal type `type` (sigcfg's name,
    // case ignored) in `mode` (the console's "Сигналы" by type, 0x40CAF0);
    // how many in `count`.
    bool set_signal_type_mode(std::string_view type, simulation::UnitMode mode, std::size_t& count, std::string& error);
    // Every signal back to its defaults (the console's "restore defaults").
    bool restore_signal_defaults(std::string& error);
    // "Задать как умолчания" (0x4499F0): every signal's defaults become its
    // mode and aspect now (ServerRoute::save_signal_defaults writes them).
    bool set_signal_defaults(std::string& error);
    // A signal type shown as normal or not (the console's "Отображать как
    // поездной" by type, 0x40CAF0; the route database's
    // SignalTypeShownAsNormal): its heads count for their unit's aspect.
    // How many units have such a head in `count`.
    bool set_signal_type_shown(std::string_view type, bool on, std::size_t& count, std::string& error);
    // A console's 0x4618 (rts_server.exe 0x40FB90): the unit takes the mode;
    // manual holds the aspect, the dispatcher's when `pinned` is set.
    bool control_signal_unit(const net::SignalControl& control, std::string& error);
    // One line on the signal of TDB item `item`: its mode, aspect, whether
    // the dispatcher picked it, its defaults.
    [[nodiscard]] std::string signal_report(std::uint32_t item) const;
    bool kick(std::string_view nick, std::string& error);
    // The dispatcher makes a player another's helper (помощник машиниста) on
    // `train`, as RTS's dispatcher assigns helpers; nothing: he helps no
    // more. Both are openRTS players (docs/server.md, "Helpers").
    bool set_helper(std::string_view nick, std::optional<std::uint32_t> train, std::string& error);
    // A train's route, as the console assembles one: from the train's front
    // (else its rear, when it runs rear first) through each stop in turn,
    // the shortest way; a stop is the TDB signal items it may be (one name
    // can stand at several stations: the nearest along the way is taken).
    // Every junction on it is set, facing and trailing alike, and held:
    // players cannot throw it until the train's rear has passed it. Its
    // manual signals facing the train open (0x40918C; not those whose aspect
    // the dispatcher picked) and close again as the train passes each, or
    // when the route is cancelled or ends. `result`: what was set, or why not.
    bool set_route(std::uint32_t train, const std::vector<std::vector<std::uint32_t>>& stops, std::string& result);
    bool cancel_route(std::uint32_t train, std::string& error);
    // RTS's hand route (the console's "Задать маршрут", 0x4619): `length` m
    // from the train's head along the junctions as they stand (from its rear
    // when `rear_first`, the console's direction flag). Its junctions are held
    // as set_route's; the distance left goes to everyone as 0x4619 every 0.5
    // s while it changes, -1 when it ends.
    bool lay_route(std::uint32_t train, double length, bool rear_first, std::string& result);
    // The train's route along the junctions as they stand from its head (its
    // rear when `rear_first`) to `to` (the console's map); false when `to` is
    // not ahead of it.
    bool lay_route_to(std::uint32_t train, const simulation::TrackLocation& to, bool rear_first, std::string& result);
    // The junctions a train's route holds (TDB nodes), in its order.
    [[nodiscard]] std::vector<std::uint32_t> route_junctions(std::uint32_t train) const;
    // For a map: each car's ends where the server has the train (empty when
    // it is not on its track), and the way its route was laid.
    [[nodiscard]] std::vector<simulation::CarPlacement> train_cars(std::uint32_t train) const;
    // Where the train's head stands facing the way it goes (its rear facing
    // back when `rear`: running rear first); nothing when not on the track.
    [[nodiscard]] std::optional<simulation::TrackLocation> train_end(std::uint32_t train, bool rear) const;
    [[nodiscard]] std::optional<simulation::RouteLeg> route_leg(std::uint32_t train) const;

    // The station routes segments are made of (ServerRoute::station_routes;
    // it must outlive the server). Without them there are no segments.
    void set_station_routes(const StationRoutes* routes) noexcept;
    // A segment of a train's route, a station route of the route database
    // (rts_server.exe's "Сборка маршрута поезда", 0x450B; docs/server.md,
    // "Station routes").
    struct SegmentRequest {
        std::uint32_t train{};
        std::uint32_t route{};  // the station route's ID
        bool reverse = false;   // run from its end to its start
        bool hold = false;      // the train's next segment waits
        bool reversed = false;  // the train runs it rear first
        std::uint32_t priority{};
    };
    struct SegmentSummary {
        std::uint32_t id{};
        std::uint32_t train{};
        std::uint32_t route{};
        std::string name;    // the station route's
        std::string station;
        bool reverse = false, hold = false, reversed = false;
        std::uint32_t priority{};
        std::uint8_t state{}; // 0 waiting, 1 set, 2 set part of the way, 3 the train on it, 4 left
        std::uint16_t thrown{};
        std::size_t junctions{};
        float length{};
    };
    // Checked as rts_server.exe checks a segment (0x469804: the train, an
    // oncoming route on the stretch it leads to, a way from the train's
    // previous segment) and queued by its priority (0x41F520); each step
    // sets those whose turn it is (0x46A348). Its id, or why not.
    std::optional<std::uint32_t> add_segment(const SegmentRequest& request, std::string& error);
    // Cancelled (0x469C68): the train's route goes (to its previous
    // segment's end when it has one).
    bool cancel_segment(std::uint32_t id, std::string& error);
    [[nodiscard]] std::vector<SegmentSummary> segments() const;
    // Where a train's next segment is looked for from (rts_server.exe's
    // "Собрать маршрут", StationRoutes::available): the end of its last
    // segment, else its head; `back` the other way ("Направление": it
    // reverses there, or starts rear first).
    struct NextFrom {
        simulation::TrackLocation at;
        bool rear_first = false;               // the segment run so
        double behind = 0.0;                   // m back to look for the point a train stands at
        std::optional<std::uint32_t> after;    // the segment it follows, nothing from the train
    };
    [[nodiscard]] std::optional<NextFrom> next_from(std::uint32_t train, bool back) const;
    // The station routes the train can take next from there (as the
    // window's "Станционные маршруты" lists them: main tracks first, onto a
    // wrong track last, the nearest first between): a console's
    // console_next answered.
    [[nodiscard]] net::ConsoleNext next_routes(std::uint32_t train, bool back) const;
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
    // The segments' step (0x46A348).
    void step_segments();
    // A segment's packages to the consoles: 0x450B, 0x450E, 0x450D.
    void send_segment(std::size_t index, std::uint16_t package);
    [[nodiscard]] net::RouteSegment segment_record(std::size_t index) const;
    // A coupling or a cut, a player's (`sender`, not sent back to them) or
    // the server's own (sender 0: to everyone). The cut's part is named to
    // its sender as `local_part` (0x4610).
    bool couple_trains(std::uint32_t a, std::uint32_t b, std::uint8_t end_a, std::uint8_t end_b, std::uint32_t sender);
    std::optional<std::uint32_t> cut_train(std::uint32_t train, std::uint32_t car, std::uint8_t side, std::uint32_t local_part,
                                           std::uint32_t sender);
    // Helpers (openRTS's own packages, to openRTS players only).
    void send_helper(const User& user, const net::HelperNote& note);
    // `helper` asks or helps no more: he and the engineer are told.
    void end_helping(User& helper, net::HelperState why);
    // The players connected (not the consoles).
    [[nodiscard]] std::uint32_t players_connected() const;
    // The answer to an openRTS player's hello: the storm, the Discord join
    // with the players and the places.
    [[nodiscard]] net::Bytes hello_answer() const;
    // He helps on `train` now: he and its engineer are told.
    void start_helping(User& helper, std::uint32_t train);
    // A helper's train gone or given to another: his help ends.
    void check_helpers();
    std::unique_ptr<Impl> impl_;
    Transport& transport_;
    ServerOptions options_;
    double time_{};
    bool paused_ = false;
    std::function<std::string(std::string_view)> console_commands_;
    std::function<std::string(const net::ConsolePlace&)> console_place_;
};

} // namespace openrts::server
