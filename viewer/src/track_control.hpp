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

#include "train_view.hpp"

namespace openrts::viewer {

// The Track control window (RTS's in-game "RTS track control", 0x549A60,
// docs/reverse/dispatcher.md): the route's real track from above with the
// trains, junctions and signals; context menus on a train (jump to it, its
// path, role, uncouple, remove), on a junction (throw it, or switch it on
// the edited path), on a signal (its heads), on the track (spawn a consist,
// place path points). F7 opens it.
class TrackControl {
public:
    TrackControl(simulation::TrackNetwork& network, formats::RoutePosition origin);

    void toggle() noexcept { open_ = !open_; }
    // Signal names (route database, Ctrl+F7) by signal object.
    void set_signal_names(std::function<std::string(std::uint32_t)> names) { signal_names_ = std::move(names); }
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
        std::function<void(const std::string& consist, const simulation::TrackLocation& front)> place_consist;
        std::function<void(std::uint64_t train)> remove_train; // the train as drawn here
        // RTS's hand route from the console: the selected train to a place
        // of the track (its junctions thrown, then 0x4619), cancelled; and
        // the route a train has (the distance left, laid from its rear).
        std::function<void(std::uint64_t train, const simulation::TrackLocation& to)> route_to;
        std::function<void(std::uint64_t train)> cancel_route;
        std::function<std::optional<std::pair<float, bool>>(std::uint64_t train)> route_left;
    };
    // The train picked on the map (as drawn here, 0 none).
    [[nodiscard]] std::uint64_t selected() const noexcept { return selected_; }
    void set_network_actions(NetworkActions actions) { network_actions_ = std::move(actions); }
    void open() noexcept { open_ = true; }
    // Selects a train and centres the map on it.
    void show_train(TrainView& trains, std::uint64_t train);

private:
    struct Line {
        std::uint32_t node{};
        std::vector<double> x, z, offset; // samples along the node, route metres
        double min_x{}, max_x{}, min_z{}, max_z{};
    };
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

    void build_lines();
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
    std::vector<Line> lines_;
    bool open_ = false;
    bool centred_ = false;
    // View: the route point at the canvas centre and pixels per metre.
    double centre_x_ = 0.0, centre_z_ = 0.0, scale_ = 0.25;
    float canvas_x_ = 0.0f, canvas_y_ = 0.0f, canvas_w_ = 1.0f, canvas_h_ = 1.0f;
    float drag_ = 0.0f;
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
    };
    Placement placement_;
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
    bool show_signals_ = true;
    bool show_platforms_ = true;
    bool show_junction_ids_ = false;
    bool show_all_paths_ = false;
    bool show_signal_names_ = true;
    bool show_sidings_ = true;
    bool show_station_names_ = true;
    bool show_electrification_ = false;
    // The electrification layer: each node's stretches, built when first drawn.
    std::vector<std::vector<simulation::Electrification::Stretch>> electro_stretches_;
    std::vector<bool> electro_built_;
    void draw_electrification_panel(TrainView& trains);
    // Station names (RTS 0x555624): every platform item counted under its
    // station's name, the label at the middle of them.
    struct Station {
        std::string name;
        double x{}, z{}; // route metres
        std::uint32_t items{};
    };
    std::vector<Station> stations_;
    // Platforms and sidings: the stretch of track between their two ends.
    struct Place {
        bool platform = true;
        std::string name;
        std::vector<double> x, z; // route metres along the track
        double length{};          // m between its two ends
    };
    std::vector<Place> places_;
    void build_places();
    std::function<std::string(std::uint32_t)> signal_names_;
    NetworkActions network_actions_;
    // What is under the mouse: a tooltip with a signal's name and aspects,
    // or a platform's or siding's name and length.
    void hover_tooltip(float mouse_x, float mouse_y, const simulation::SignalSystem* signals) const;
};

} // namespace openrts::viewer
#endif
