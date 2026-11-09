#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>
#include <openrts/simulation/train.hpp>

// The route an openRTS server plays (docs/server.md): its track network, its
// signals running their scripts, the paths and the rolling stock the
// dispatcher makes trains from, and the counts a client checks at the login.
namespace openrts::server {

// The four counts of the login reply. rts1.exe compares them with its own
// route's (0x47A1A0, 0x47AAEE..0x47AB42) and refuses a route that differs:
// "Route count of tdb items / track items / electric substations /
// electrosections doesn't match server's". CONFIRMED against the live
// capture of 2026-11-05 (Kuban: 1653 nodes = 921 vector + 555 junction + 177
// end, 4074 items, 3, 3).
struct RouteCounts {
    std::uint32_t nodes{};       // TDB TrackNodes
    std::uint32_t items{};       // TDB TrItemTable
    std::uint32_t substations{}; // .electrotype substations
    std::uint32_t sections{};    // .electrodb electrosections
};

// A consist made a train: the cars for the server's simulation and their
// records for the network.
struct ConsistTrain {
    std::vector<simulation::CarSetup> cars;
    std::vector<net::TrainCar> records;
    float top_speed{}; // m/s: the consist's MaxVelocity, else its first engine's
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty() && !cars.empty(); }
};

class ServerRoute {
public:
    // `root`: the RTS installation; `route`: its folder under ROUTES.
    [[nodiscard]] static std::unique_ptr<ServerRoute> load(const std::filesystem::path& root, const std::string& route,
                                                           std::string& error);
    ~ServerRoute();
    ServerRoute(const ServerRoute&) = delete;
    ServerRoute& operator=(const ServerRoute&) = delete;

    [[nodiscard]] const std::string& folder() const noexcept { return folder_; }
    [[nodiscard]] const std::string& route_id() const noexcept { return route_id_; } // the .trk RouteID
    [[nodiscard]] const RouteCounts& counts() const noexcept { return counts_; }
    [[nodiscard]] simulation::TrackNetwork& network() noexcept { return *network_; }
    // nullptr when the route's signals did not load (the server runs without).
    [[nodiscard]] simulation::SignalSystem* signals() noexcept { return signals_.get(); }
    [[nodiscard]] const std::vector<formats::TrainPath>& paths() const noexcept { return paths_; }
    // A path by file stem or TrPathName, case ignored.
    [[nodiscard]] const formats::TrainPath* path(std::string_view name) const;
    // The consists of the installation (TRAINS/CONSISTS_RTS, then CONSISTS), by name.
    [[nodiscard]] std::vector<std::string> consist_names() const;
    // A consist's cars; `number` goes into each car's uid (number << 18 |
    // index). The wagon files are read once and kept for the server's life.
    [[nodiscard]] ConsistTrain consist(std::string_view name, std::uint32_t number);
    // The same from the cars alone (a console's consist): `max_velocity` the
    // consist's (m/s, 0 none).
    [[nodiscard]] ConsistTrain cars(const std::vector<formats::ConsistEntry>& entries, float max_velocity, std::uint32_t number);
    // The front of a train `length` m long whose rear stands at the path's
    // start (`back` m behind it), facing along the path's first leg (as the
    // single player's explore start, TrainView::place_on_path).
    [[nodiscard]] std::optional<simulation::TrackTraveller> path_front(const formats::TrainPath& path, double length, double back = 0.0);
    // Problems met while loading (signals without scripts and the like).
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }
    // The TDB signal items named `name` in the route database (the
    // signals' "литеры", <RouteID>.rtsdb), case ignored.
    [[nodiscard]] std::vector<std::uint32_t> signal_items(std::string_view name) const;
    // Every signal name with its items, by name.
    [[nodiscard]] const std::vector<std::pair<std::string, std::uint32_t>>& signal_names() const noexcept { return signal_names_; }

private:
    ServerRoute() = default;

    core::Vfs vfs_;
    core::VirtualPath route_;
    std::string folder_;
    std::string route_id_;
    RouteCounts counts_;
    std::unique_ptr<simulation::TrackNetwork> network_;
    std::shared_ptr<const formats::SignalConfig> signal_config_;
    std::shared_ptr<const formats::SignalScriptFile> signal_scripts_;
    std::unique_ptr<simulation::SignalSystem> signals_;
    std::vector<formats::TrainPath> paths_;
    std::map<std::string, std::unique_ptr<formats::WagonDefinition>> wagons_; // by folder\name.ext, lower case
    std::vector<std::pair<std::string, std::uint32_t>> signal_names_;           // name, TDB item; by name
    std::vector<std::string> notes_;
};

} // namespace openrts::server
