#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
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
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    // Draws the window (ImGui builds) and acts on the trains and the network.
    void draw(TrainView& trains, simulation::SignalSystem* signals);

private:
    struct Line {
        std::uint32_t node{};
        std::vector<double> x, z, offset; // samples along the node, route metres
        double min_x{}, max_x{}, min_z{}, max_z{};
    };
    enum class PickKind : std::uint8_t { none, train, junction, signal, track, start_point, plan_point };
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
    void draw_map(TrainView& trains, const simulation::SignalSystem* signals);
    void draw_panel(TrainView& trains, simulation::SignalSystem* signals);
    void draw_menu(TrainView& trains, simulation::SignalSystem* signals);
    void apply_plan(TrainView& trains);
    void start_editing(TrainView& trains, std::uint64_t id);

    simulation::TrackNetwork* network_;
    formats::RoutePosition origin_;
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
    bool spawn_turned_ = false;
    bool spawn_ai_ = true;
    bool show_signals_ = true;
    bool show_platforms_ = true;
    bool show_junction_ids_ = false;
    bool show_all_paths_ = false;
};

} // namespace openrts::viewer
#endif
