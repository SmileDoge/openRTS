#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <utility>

#include <openrts/formats/route_world.hpp>
#include <openrts/net/connection.hpp>
#include <openrts/net/play.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/net/world.hpp>

namespace openrts::simulation {
class SignalSystem;
class TrackNetwork;
} // namespace openrts::simulation

namespace openrts::viewer {

class TrainView;

// openRTS watching a game on an RTS server (--mp-connect, docs/net.md,
// "Observer"): the server's world (time of day, junctions, signal aspects)
// and its trains, which other clients and the server drive. They are drawn
// one package interval behind and interpolated along the track, with the
// lights and pantographs their owners send. It sends the camera's place
// (player_pos, so the server relays the trains' looks to it) and asks each
// train's owner for its look (state_request 2); nothing else yet.
class NetSession {
public:
    // `early`: packages that came with the snapshot, after its end.
    // `console`: logged in as the dispatcher's console (--mp-console).
    NetSession(std::shared_ptr<net::RtsConnection> connection, net::Snapshot snapshot,
               std::vector<net::ReceivedPackage> early, bool console = false);

    // The first call puts the snapshot's trains on the track; every call
    // applies what the server sent and moves the trains.
    // `camera`: where the player looks from, for player_pos.
    void update(simulation::TrackNetwork& network, simulation::SignalSystem* signals, TrainView& trains,
                const std::optional<formats::RoutePosition>& camera);
    // The server's time of day, once after it came.
    [[nodiscard]] std::optional<double> take_time_of_day() { return std::exchange(time_of_day_, std::nullopt); }
    [[nodiscard]] bool down() const noexcept { return down_; }
    // The dispatcher paused the game (0x4616): time and trains stand.
    [[nodiscard]] bool paused() const noexcept { return paused_; }

    // The dispatcher's console (openRTS's server, docs/server.md).
    [[nodiscard]] bool console() const noexcept { return console_; }
    // A command line for the server, as typed at its console.
    void send_command(std::string_view line);
    // A consist put on the track by its cars (console_place).
    void send_place(const net::ConsolePlace& place);
    // The station routes server train `train` can take next (console_next),
    // `back` the other way; the server's answer comes in next_routes().
    void ask_next_routes(std::uint32_t train, bool back);
    [[nodiscard]] const std::optional<net::ConsoleNext>& next_routes() const noexcept { return next_routes_; }
    // A line in the console's log (a problem found here).
    void note(std::string line) { console_log_.push_back(std::move(line)); }
    // The game as the server last told the console (every second).
    [[nodiscard]] const std::optional<net::ConsoleState>& console_state() const noexcept { return console_state_; }
    // The commands sent and what the server answered, the dispatchers'
    // messages, oldest first.
    [[nodiscard]] const std::vector<std::string>& console_log() const noexcept { return console_log_; }
    // A junction's new position for the server (0x460A, as RTS's console
    // sends its route's junctions); rts_server.exe sets it as sent, its lock
    // byte too (0x40C52C), and sends everyone the result.
    void request_junction(std::uint32_t node, std::uint32_t branch);
    // A hand switch thrown on foot (0x4642, rts1.exe 0x4D1EB8: the junction
    // and the branch): the server throws it unless it is locked or reserved
    // (rts_server.exe 0x40C5A4) and sends everyone the result.
    void ask_junction(std::uint32_t node, std::uint32_t branch);
    // RTS's hand route (0x4619, the console's "Задать маршрут"): `length` m
    // from the train's head along the junctions as they stand, from its rear
    // when `rear_first`; below 0 cancels it. The junctions go first
    // (request_junction), as RTS's console sends them.
    void send_route(std::uint32_t server_train, float length, bool rear_first);
    // The distance a train's route has left, as the server last sent it
    // (0x4619), and whether it is laid from the rear; nothing: no route.
    [[nodiscard]] std::optional<std::pair<float, bool>> route_left(std::uint32_t server_train) const;
    // A train's name and notes (0x461A, the console's train form's
    // "Название" and "Заметки"; RTS's server sends them to everyone, its
    // clients keep none): as the server last sent them, nothing none.
    struct TrainNames {
        std::u16string name, notes;
    };
    [[nodiscard]] const TrainNames* train_names(std::uint32_t server_train) const;
    // Its number as the console's state has it (0: not known).
    [[nodiscard]] std::uint32_t train_number(std::uint32_t server_train) const;
    // The console's: a train's name and notes for the server (0x461A), which
    // sends them to everyone.
    void send_train_names(std::uint32_t server_train, const std::u16string& name, const std::u16string& notes);
    // The TrainView train a server train is drawn as (0 none), and back.
    [[nodiscard]] std::uint64_t view_train(std::uint32_t server_train) const;
    [[nodiscard]] std::uint32_t server_train(std::uint64_t view_train) const;

    // --- Players and helpers (docs/net.md, "Players and helpers").
    // The server is openRTS's: it answered openrts_hello (helpers need it).
    [[nodiscard]] bool openrts_server() const noexcept { return openrts_server_; }
    // Its game is a thunderstorm (its answer to openrts_hello).
    [[nodiscard]] bool server_storm() const noexcept { return server_storm_; }
    // The server lets its game be joined from Discord: the address it gives
    // (empty: the one we connected to), its players and places (0 no
    // limit); nothing: not allowed.
    struct DiscordJoin {
        std::string address;
        int players = 0;
        int places = 0;
    };
    [[nodiscard]] const std::optional<DiscordJoin>& discord_join() const noexcept { return discord_join_; }
    // A server train as the players know it: its name or number.
    [[nodiscard]] std::string train_label(std::uint32_t server_train) const;
    // The other players as the server relays their places (player_pos),
    // heard within the last 20 s.
    struct Player {
        std::uint32_t user{};
        std::string nick;
        std::uint32_t train{};          // the server train he is in, 0 none
        formats::RoutePosition at;      // his camera's place
        double heard{};                 // when, s of this session
    };
    [[nodiscard]] std::vector<Player> players() const;
    // Our help: asked for, given, refused, ended (the server's last word).
    struct Help {
        std::uint32_t train{};          // the server train
        std::uint32_t engineer{};
        std::string nick;               // the engineer's
        net::HelperState state = net::HelperState::ended;
    };
    [[nodiscard]] const std::optional<Help>& help() const noexcept { return help_; }
    void ask_to_help(std::uint32_t server_train);
    void stop_helping();
    // The engineer's: players asking to help him, and his helpers.
    struct Helper {
        std::uint32_t user{};
        std::uint32_t train{};
        std::string nick;
    };
    [[nodiscard]] const std::vector<Helper>& asking() const noexcept { return asking_; }
    [[nodiscard]] const std::vector<Helper>& helpers() const noexcept { return helpers_; }
    void answer(std::uint32_t user, bool take);
    void drop_helper(std::uint32_t user);

private:
    struct Remote {
        net::SnapshotTrain record;
        std::uint64_t train = 0; // in TrainView; 0 while not on the track
        net::RemoteMotion motion;
        bool warned = false;
        std::vector<std::uint8_t> pantographs; // the masks last logged, by car
        // The camera was more than 3 km off: the server sent none of its
        // lights, looks and sounds meanwhile (audience 4 goes by the
        // player's place, which is the camera's).
        bool far = false;
    };

    void apply(const net::ReceivedPackage& package, simulation::TrackNetwork& network, simulation::SignalSystem* signals,
               TrainView& trains, double now);
    void position(Remote& remote, const net::TrainPos& pos, simulation::TrackNetwork& network, TrainView& trains,
                  double now);
    void move_trains(simulation::TrackNetwork& network, TrainView& trains, double now);
    // The trains the server gave us (docs/net.md, "Own trains"): simulated
    // here, one of them driven, all sent out.
    struct Owned {
        net::SnapshotTrain record;
        std::uint64_t train = 0;              // in TrainView; 0 while off the track
        std::optional<net::TrainPos> back_at; // the server's place for it while off the track
        bool moving = false;
        bool announced = false;     // stood still once since it was placed here
        bool waiting = false;       // a part cut off: no server id yet (0x4610 brings it)
        double next_position = 0.0; // train_pos due while it moves
        double next_look = 0.0;     // car_state at most every 250 ms
        double next_sound = 0.0;    // sound_values at most every 200 ms
        std::vector<std::array<float, 9>> sound_sent;          // by car: Var1..8, travelled
        std::vector<std::vector<std::uint8_t>> lights_sent;    // by car, by light
        std::vector<std::pair<std::uint8_t, bool>> looks_sent; // by car: pantographs, running
        std::size_t derailed_sent = 0; // derailed cars the last 0x4636 named
    };
    // Puts an own train on the track at record.pos; `drive`: the player
    // takes it (its DLLs start in the server's state).
    bool place_owned(std::uint32_t id, Owned& own, simulation::TrackNetwork& network, TrainView& trains, bool drive);
    // Off the track here (`keep`: it stays ours, to be put back).
    void take_off(std::uint32_t id, Owned& own, TrainView& trains);
    void assigned(const net::TrainAssignUpdate& a, simulation::TrackNetwork& network, TrainView& trains, double now);
    // Couplings and cuts: ours to the server, the others' from it.
    void send_couplings(TrainView& trains);
    // Another client's coupling, or the server's own into one of ours.
    void coupled(const net::Coupling& c, simulation::TrackNetwork& network, TrainView& trains);
    void uncoupled(const net::Uncoupling& u, TrainView& trains);
    void renamed(std::uint32_t from, std::uint32_t to);
    std::uint32_t next_local_ = 100000; // ids of parts we cut off, until the server's (rts1.exe: 100244, ...)
    [[nodiscard]] std::uint32_t driven_id(const TrainView& trains) const;
    void send_own(TrainView& trains, double now);
    void send_look(std::uint32_t id, Owned& own, TrainView& trains, bool all);
    void send_sound(std::uint32_t id, Owned& own, TrainView& trains, double now);
    [[nodiscard]] double now() const;

    std::shared_ptr<net::RtsConnection> connection_;
    net::Snapshot snapshot_;
    std::vector<net::ReceivedPackage> early_;
    bool started_ = false;
    bool down_ = false;
    bool paused_ = false;
    std::map<std::uint32_t, Remote> remotes_;
    std::optional<net::SignalHeads> heads_;
    std::vector<net::SignalHeadState> waiting_heads_; // until the signals have loaded
    std::optional<double> time_of_day_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::map<std::uint16_t, std::size_t> received_;    // ids since the last report, with counts
    std::map<std::uint16_t, std::size_t> not_handled_; // of them, the ones the observer leaves
    double report_at_ = 10.0;
    std::map<std::uint32_t, Owned> owned_;
    std::size_t triggers_sent_ = 0, values_sent_ = 0; // for the log
    double position_at_ = 0.0;                           // the next player_pos due
    double range_at_ = 0.0;                              // the next check of the remote trains' distance
    std::optional<formats::RoutePosition> sent_camera_; // the last one sent
    bool console_ = false;
    // Players and helpers.
    bool hello_sent_ = false;
    bool openrts_server_ = false;
    bool server_storm_ = false;
    std::optional<DiscordJoin> discord_join_;
    std::map<std::uint32_t, Player> players_;
    // Each player's places of the last seconds (heard time, place): one in
    // a train drawn behind by its delay is shown as far behind (players()).
    std::map<std::uint32_t, std::deque<std::pair<double, formats::RoutePosition>>> player_trails_;
    std::optional<Help> help_;
    std::vector<Helper> asking_, helpers_;
    void helper_note(const net::HelperNote& note, TrainView& trains);
    void helper_streams(TrainView& trains, double now);
    double cab_due_ = 0.0;       // the engineer's next cab stream
    double watch_due_ = 0.0;     // the helper's next "I am in car N"
    struct SentCab {
        net::Bytes packed;
        double at{};
    };
    std::map<std::uint32_t, SentCab> cab_sent_;        // by car: the last stream
    std::map<std::uint32_t, double> cab_watched_;      // by car: until when a helper is in it
    bool helping_view_ = false;                        // TrainView::help done for help_
    // Tests (environment): OPENRTS_MP_HELP=<nick> asks to help him,
    // OPENRTS_MP_TAKE_HELPERS takes every helper, OPENRTS_MP_HELP_SWITCH=
    // <id>:<state> works that switch of the engineer's cab once it is here.
    bool test_asked_ = false, test_switched_ = false;
    std::map<std::uint32_t, std::pair<std::size_t, std::uint32_t>> cab_seen_; // by car: packages, the test switch's state
    std::map<std::uint32_t, std::pair<float, bool>> routes_; // by server train: 0x4619
    std::map<std::uint32_t, TrainNames> names_;             // by server train: 0x461A
    std::optional<net::ConsoleState> console_state_;
    std::optional<net::ConsoleNext> next_routes_;
    std::vector<std::string> console_log_;
};

} // namespace openrts::viewer
