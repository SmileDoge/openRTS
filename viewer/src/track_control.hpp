#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <openrts/formats/route_world.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train_route.hpp>
#include <openrts/trackmap/track_map.hpp>

#include "train_view.hpp"

namespace openrts::viewer {

// The Track control window (RTS's in-game "RTS track control", 0x549A60,
// docs/reverse/dispatcher.md): the route's map (trackmap::TrackMap) with
// the trains, their paths and routes; context menus on a train (jump to it,
// its path, role, uncouple, remove), on a junction (throw it, or switch it
// on the edited path), on a signal (its heads), on the track (spawn a
// consist, place path points). F7 opens it.
class TrackControl {
public:
    TrackControl(simulation::TrackNetwork& network, formats::RoutePosition origin);

    void toggle() noexcept { open_ = !open_; }
    // Signal names (route database, Ctrl+F7) by signal object.
    void set_signal_names(std::function<std::string(std::uint32_t)> names) { map_.signal_name = std::move(names); }
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    // A consist is being placed (Esc drops it).
    [[nodiscard]] bool placing() const noexcept { return placement_.active; }
    // The camera this frame, in scene coordinates against the scene's
    // current (floating) origin: the player's figure on the map.
    void set_viewer(const glm::vec3& eye, const glm::vec3& forward, const formats::RoutePosition& scene_origin) noexcept {
        eye_ = eye;
        forward_ = forward;
        scene_origin_ = scene_origin;
    }
    // "Teleport here": where to put the camera, on the route (absolute), the
    // track's height when known (else the ground there) and the yaw.
    struct Teleport {
        formats::RoutePosition at;
        bool on_track = false; // at.y is the track's height
        float yaw{};
    };
    [[nodiscard]] std::optional<Teleport> take_teleport() {
        auto request = teleport_;
        teleport_.reset();
        return request;
    }
    // Draws the window (ImGui builds) and acts on the trains and the network.
    void draw(TrainView& trains, simulation::SignalSystem* signals);

    // In a network game the server decides junctions and signals: a throw
    // and a signal's aspect go to it as requests (NetSession) instead of
    // acting here. `set_signal` empty: no signal control (a player).
    struct NetworkActions {
        std::function<void(std::uint32_t node, std::uint32_t branch)> throw_junction;
        std::function<void(std::uint32_t item, std::optional<formats::SignalAspect> aspect)> set_signal;
        // The console: a consist placed on the map goes to the server (it
        // stands there, nobody's, until given to a player); a train removed.
        // Empty: a player, who cannot.
        struct PlaceOptions {
            std::uint32_t number{}; // 0: the server's lowest free
            std::string name;
            std::uint16_t kind{};   // RTS's: 0 STATIC, 1 PLAYER, 2 TRAFFIC, 3 LOOSE
            std::uint8_t state{};   // 0 cold, 1 on, 2 running
            std::uint8_t chocks{};  // under its head 1, its tail 2 (RTS's PlaceDrag)
        };
        std::function<void(const std::string& consist, const simulation::TrackLocation& front, const PlaceOptions& options)> place_consist;
        std::function<void(std::uint64_t train)> remove_train; // the train as drawn here
        // RTS's hand route from the console: the selected train to a place
        // of the track (its junctions thrown, then 0x4619), cancelled; and
        // the route a train has (the distance left, laid from its rear).
        // Through the points to pass in order (Track control's "Точка
        // проезда"), not through another train; what was wrong, or empty.
        std::function<std::string(std::uint64_t train, const std::vector<simulation::TrackLocation>& via, const simulation::TrackLocation& to)> route_to;
        std::function<void(std::uint64_t train)> cancel_route;
        std::function<std::optional<std::pair<float, bool>>(std::uint64_t train)> route_left;
        // The train's number and name as the server keeps them ("№101
        // name", empty: its own label) and its notes (0x461A).
        std::function<std::pair<std::string, std::string>(std::uint64_t train)> train_names;
        // Its number and name as the server keeps them, and set (the
        // train's menu; 2026-11-21, the organiser).
        std::function<std::pair<std::uint32_t, std::string>(std::uint64_t train)> train_form;
        std::function<void(std::uint64_t train, std::uint32_t number, const std::string& name)> set_train_form;
        // A standing train put again by the server (2026-11-21): elsewhere
        // (the front of its first car) or with chocks under its ends (1 the
        // head, 2 the tail). Its chocks as the train here has them.
        std::function<void(std::uint64_t train, const simulation::TrackLocation& head)> move_train;
        std::function<void(std::uint64_t train, std::uint8_t chocks)> set_chocks;
    };
    // The train picked on the map (as drawn here, 0 none).
    [[nodiscard]] std::uint64_t selected() const noexcept { return selected_; }
    void set_network_actions(NetworkActions actions) { network_actions_ = std::move(actions); }
    void open() noexcept { open_ = true; }
    // Selects a train and centres the map on it.
    void show_train(TrainView& trains, std::uint64_t train);

private:
    enum class PickKind : std::uint8_t { none, train, junction, signal, track, start_point, plan_point, placement };
    struct Pick {
        PickKind kind = PickKind::none;
        std::uint64_t train{};
        std::uint32_t junction{};
        std::uint32_t signal{};     // signal object
        std::size_t point{};        // plan point
        simulation::TrackLocation location; // track
        double x{}, z{};            // route metres of the click
    };

    [[nodiscard]] double world_x(const simulation::TrackPoint& point) const;
    [[nodiscard]] double world_z(const simulation::TrackPoint& point) const;
    [[nodiscard]] Pick pick(float mouse_x, float mouse_y, TrainView& trains, const simulation::SignalSystem* signals) const;
    // The track point nearest to the cursor within `reach` pixels.
    [[nodiscard]] std::optional<simulation::TrackLocation> nearest_track(float mouse_x, float mouse_y, double reach) const;
    void draw_map(TrainView& trains, const simulation::SignalSystem* signals);
    void draw_panel(TrainView& trains, simulation::SignalSystem* signals);
    // "Signal info": a window with everything about one signal object.
    void draw_signal_window(simulation::SignalSystem* signals);
    std::optional<std::uint32_t> signal_window_;
    void draw_menu(TrainView& trains, simulation::SignalSystem* signals);
    void apply_plan(TrainView& trains);
    void start_editing(TrainView& trains, std::uint64_t id);

    simulation::TrackNetwork* network_;
    formats::RoutePosition origin_;
    glm::vec3 eye_{};
    glm::vec3 forward_{0.0f, 0.0f, -1.0f};
    formats::RoutePosition scene_origin_;
    std::optional<Teleport> teleport_;
    // Map metres (against origin_) of a scene point.
    [[nodiscard]] double map_x(double scene_x) const;
    [[nodiscard]] double map_z(double scene_z) const;
    // A teleport to map point (x, z); `track` gives the height and heading.
    void request_teleport(double x, double z, const simulation::TrackPoint* track);
    trackmap::TrackMap map_;
    bool open_ = false;
    bool centred_ = false;
    std::uint64_t selected_ = 0;
    std::optional<std::uint32_t> selected_signal_;
    bool editing_ = false;
    simulation::PathPlan plan_;
    Pick menu_;
    std::string message_;
    char path_name_[64]{};
    float wait_seconds_ = 60.0f;
    char consist_filter_[64]{};
    int consist_choice_ = -1;
    bool spawn_ai_ = true;
    // The consist put down (as the ТЧД console's train form): its type, 0
    // by its cars, 1 passenger, 2 freight, 3 other; its state, 0 running, 1
    // on and standing, 2 cold (no air, pantographs down).
    int spawn_category_ = 0;
    int spawn_state_ = 0;
    void spawn_options();
    // A network game's placement: the number, name, kind and state the
    // server puts it with (STATIC and cold by default).
    char net_number_[16]{};
    char net_name_[128]{};
    int net_kind_ = 0;
    int net_state_ = 0;
    void net_spawn_options();
    // The train menu's number and name, for that train.
    std::uint64_t form_train_ = 0;
    char form_number_[16]{};
    char form_name_[128]{};
    bool focused_ = false; // the window has the keyboard focus

    // Putting a consist down: first a preview on the map, with the front car
    // and the way it faces marked; dragged along the track, turned, then
    // placed or dropped.
    struct Placement {
        bool active = false;
        std::string consist; // what TrainView::spawn takes
        TrainView::ConsistLayout layout;
        simulation::TrackLocation head; // the front of car 0, facing the way the train runs
        int grabbed = -1;               // the car being dragged
        // From the last layout: each car's front and rear where it fits.
        std::vector<std::pair<simulation::TrackPoint, simulation::TrackPoint>> cars;
        bool fits = false;
        std::string overlaps; // a train the preview runs into
        std::uint64_t moving = 0; // a train put again (its id), 0: a consist put on the track
    };
    Placement placement_;
    // The train menu's "Move it": the preview made from the train's cars,
    // its front where the train's is.
    void start_moving(TrainView& trains, std::uint64_t id);
    // The train menu's chocks (none, head, tail, both).
    void chocks_menu(TrainView& trains, std::uint64_t id);
    // The consist put down: its chocks (0 none, 1 head, 2 tail, 3 both).
    int spawn_chocks_ = 0;
    // The network's hand route: the points to pass, for train route_via_train_.
    std::vector<simulation::TrackLocation> route_via_;
    std::uint64_t route_via_train_ = 0;
    int net_chocks_ = 0;
    void start_placement(TrainView& trains, const std::string& consist, simulation::TrackLocation where);
    void lay_placement(TrainView& trains);
    // Moves the preview `metres` along the track (positive: the way it
    // faces); refused when the cars would leave the track.
    void slide_placement(TrainView& trains, double metres);
    // Moves the front to `where`, keeping the way it faces on the screen.
    void move_placement(TrainView& trains, simulation::TrackLocation where);
    // The same stretch of track, the front car at the other end.
    void turn_placement(TrainView& trains);
    void finish_placement(TrainView& trains);
    // The preview car under the cursor, -1 when none.
    [[nodiscard]] int placement_car_at(float mouse_x, float mouse_y) const;
    bool show_all_paths_ = false;
    bool show_electrification_ = false;
    // The electrification layer: each node's stretches, built when first drawn.
    std::vector<std::vector<simulation::Electrification::Stretch>> electro_stretches_;
    std::vector<bool> electro_built_;
    void draw_electrification_panel(TrainView& trains);
    NetworkActions network_actions_;
};

} // namespace openrts::viewer
#endif
