#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <openrts/formats/route_world.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train_route.hpp>

// The map of a route's track from above (RTS's track control, 0x549A60, and
// its server's console map; docs/reverse/dispatcher.md): the real track,
// junctions, signals with their names, platforms, sidings and stations,
// with the trains, lines and marks its owner gives each frame. It draws in
// an ImGui window (wheel zooms, right drag pans), shows tooltips on signals
// and platforms, and tells its owner what was clicked; the viewer's Track
// control and the server's window build their panels and menus on it.
namespace openrts::trackmap {

// A train: its cars, front and rear, and how it is drawn.
struct MapTrain {
    std::uint64_t id{};
    std::string label; // by its head
    ImU32 colour{};
    bool derailed = false;
    std::vector<std::pair<simulation::TrackPoint, simulation::TrackPoint>> cars; // front, rear
};

// An arrow just ahead of `front`, pointing away from `rear`: the way a
// train faces. The map's trains have one, a consist being placed too.
void draw_direction_arrow(ImDrawList* draw, ImVec2 front, ImVec2 rear, ImU32 fill, ImU32 outline, float size);

// A line over the track (a route, a path, the wire).
struct MapLine {
    std::vector<simulation::TrackPoint> points;
    ImU32 colour{};
    float width = 3.0f;
};

// A mark: a disc with a letter, or a ring.
struct MapMark {
    simulation::TrackPoint at;
    ImU32 colour{};
    std::string letter;
    ImU32 letter_colour{};
    float radius = 7.0f;
    bool ring = false;
};

enum class PickKind : std::uint8_t { none, train, junction, signal, track };

// What is under the cursor.
struct Pick {
    PickKind kind = PickKind::none;
    std::uint64_t train{};
    std::uint32_t junction{};
    std::uint32_t signal{}; // signal object
    simulation::TrackLocation location; // track
    double x{}, z{};                    // route metres at the cursor
};

// What the map shows of the route.
struct Layers {
    bool signals = true;
    bool platforms = true;
    bool sidings = true;
    bool station_names = true;
    bool signal_names = true;
    bool junction_ids = false;
};

class TrackMap {
public:
    TrackMap(const simulation::TrackNetwork& network, formats::RoutePosition origin);

    Layers layers;
    // What this frame shows besides the route, given before draw().
    std::vector<MapTrain> trains;
    std::vector<MapLine> lines; // over the track, under the junctions and signals
    std::vector<MapMark> marks; // under the trains
    std::uint64_t selected_train = 0;
    std::optional<std::uint32_t> selected_signal;
    // A signal object's name ("литера"), and more lines for its tooltip.
    std::function<std::string(std::uint32_t object)> signal_name;
    std::function<std::vector<std::string>(std::uint32_t object)> signal_notes;
    // The owner's own drawing, after the map's layers, in its clip.
    std::function<void(ImDrawList*)> overlay;

    struct Frame {
        bool hovered = false; // the cursor is on the map
        bool active = false;  // a button is held on it
        ImVec2 mouse;
        std::optional<Pick> left_click;  // pressed this frame
        std::optional<Pick> right_click; // released without a drag: a menu
    };
    // Draws the map in the current ImGui window's free region. `quiet`: no
    // tooltips (a menu of the owner's is open).
    Frame draw(const simulation::SignalSystem* signals, bool quiet = false);

    // The view: the route point at its centre and pixels per metre.
    [[nodiscard]] double centre_x() const noexcept { return centre_x_; }
    [[nodiscard]] double centre_z() const noexcept { return centre_z_; }
    [[nodiscard]] double scale() const noexcept { return scale_; }
    void centre(double x, double z) noexcept;
    // Centred on a point, zoomed in to at least `min_scale`.
    void centre_on(const simulation::TrackPoint& point, double min_scale = 0.0);
    // Every one of `points` on the map: unchanged when they all are,
    // else centred on them and zoomed out (or in, to `max_scale` at most)
    // until they fit with a margin.
    void fit(const std::vector<simulation::TrackPoint>& points, double max_scale = 1.5);

    // Route metres against the origin of a point.
    [[nodiscard]] double world_x(const simulation::TrackPoint& point) const;
    [[nodiscard]] double world_z(const simulation::TrackPoint& point) const;
    // Where a point is on the screen (as last drawn).
    [[nodiscard]] ImVec2 screen(const simulation::TrackPoint& point) const;
    [[nodiscard]] ImVec2 screen(double x, double z) const;
    // What is under a screen point: a train, a junction, a signal, else the
    // track within 12 px.
    [[nodiscard]] Pick pick(ImVec2 mouse, const simulation::SignalSystem* signals) const;
    // The track point nearest to a screen point within `reach` px.
    [[nodiscard]] std::optional<simulation::TrackLocation> nearest_track(ImVec2 mouse, double reach) const;
    // Points along a stretch of a vector node, every 10 m.
    [[nodiscard]] std::vector<simulation::TrackPoint> along(std::uint32_t node, double from, double to) const;
    // Points along a route leg between leg distances `from` and `to`.
    [[nodiscard]] std::vector<simulation::TrackPoint> along(const simulation::RouteLeg& leg, double from, double to) const;

    [[nodiscard]] const formats::RoutePosition& origin() const noexcept { return origin_; }
    [[nodiscard]] const simulation::TrackNetwork& network() const noexcept { return *network_; }

private:
    struct Line {
        std::uint32_t node{};
        std::vector<double> x, z, offset; // samples along the node, route metres
        double min_x{}, max_x{}, min_z{}, max_z{};
    };
    // Platforms and sidings: the stretch of track between their two ends.
    struct Place {
        bool platform = true;
        std::string name;
        std::vector<double> x, z; // route metres along the track
        double length{};          // m between its two ends
    };
    // Station names (RTS 0x555624): every platform item counted under its
    // station's name, the label at the middle of them.
    struct Station {
        std::string name;
        double x{}, z{};
        std::uint32_t items{};
    };
    void build_lines();
    void build_places();
    void hover_tooltip(ImVec2 mouse, const simulation::SignalSystem* signals) const;

    const simulation::TrackNetwork* network_;
    formats::RoutePosition origin_;
    std::vector<Line> lines_;
    std::vector<Place> places_;
    std::vector<Station> stations_;
    double centre_x_ = 0.0, centre_z_ = 0.0, scale_ = 0.25;
    float canvas_x_ = 0.0f, canvas_y_ = 0.0f, canvas_w_ = 1.0f, canvas_h_ = 1.0f;
    float drag_ = 0.0f;
};

} // namespace openrts::trackmap
