#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/audio/audio_engine.hpp>
#include <openrts/audio/sound_bank.hpp>
#include <openrts/audio/sound_instance.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/formats/terrain.hpp>
#include <openrts/simulation/auto_driver.hpp>
#include <openrts/simulation/dispatcher.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/speed_limits.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/simulation/train_route.hpp>

#include "camera.hpp"
#include "viewer_scene.hpp"

namespace openrts::viewer {

// A consist from the content on the route: simulated (simulation::Train on a
// fixed step), driven with the keyboard, drawn with posed bogies and wheels,
// and heard through its cars' sound scripts. Uncoupled cuts stay on the
// track as further trains (not driven); trains that meet couple, push or
// collide as RTS decides (simulation::resolve_contacts).
class TrainView {
public:
    // Who drives a train: the keyboard, the test driver along its path, or
    // nobody (it stands with its brakes as they are).
    enum class Role : std::uint8_t { driven, ai, standing };
    struct Summary {
        std::uint64_t id{};
        std::string name;
        Role role = Role::standing;
        double speed{};      // m/s
        bool placed = false;
        std::size_t cars{};
        double length{};
        float target = -1.0f; // the test driver's target speed, m/s
        bool derailed = false;
        bool asleep = false;  // STATIC: not simulated until something moves it
    };

    TrainView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, formats::RoutePosition origin,
              simulation::TrackNetwork& network);
    ~TrainView();

    // Loads the consist (RTS lookup: CONSISTS_RTS, then CONSISTS; or a
    // virtual path) and its stock; returns an error text on failure.
    [[nodiscard]] std::string load(std::string_view consist);
    // Puts the loaded train with its front at (node, offset).
    [[nodiscard]] std::string place(std::uint32_t node, double offset, bool forward);
    // Puts the train on a route path as explore mode does: the rear at the
    // path start, the train along the path's first leg.
    [[nodiscard]] std::string place_on_path(const formats::TrainPath& path);
    // A path of the route by file name, id or name (case-insensitive); the
    // first path for an empty name.
    [[nodiscard]] const formats::TrainPath* find_path(std::string_view name) const;
    // Puts the train on this path as soon as its shapes are loaded.
    void select_path(const formats::TrainPath& path);
    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] bool placed() const noexcept;
    [[nodiscard]] const std::string& consist_name() const noexcept { return consist_; }

    void update(const SceneInput& input, ViewerCamera& camera, float elapsed, renderer::ModelLibrary& library,
                audio::AudioEngine* sound);
    void submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library);
    void draw_overlay();
    void set_origin(const formats::RoutePosition& origin) { origin_ = origin; }
    // The streamed world objects near the camera: obstacles for derailed
    // cars sliding along (null: none).
    void set_obstacles(const std::vector<renderer::StreamedInstance>* obstacles) { obstacles_ = obstacles; }
    void stop_sound(audio::AudioEngine& sound);
    [[nodiscard]] bool following() const noexcept { return follow_; }
    // The driven train.
    [[nodiscard]] const simulation::Train* train() const noexcept;
    // Train `id` is the one driven from the keyboard (the player's).
    [[nodiscard]] bool is_player(std::uint64_t id) const noexcept;
    // Every train on the track (the driven one first).
    [[nodiscard]] std::vector<const simulation::Train*> trains() const;
    // A traveller at the end of the train that leads (by the motion, else
    // the reverser), facing away from the train.
    [[nodiscard]] std::optional<simulation::TrackTraveller> ahead() const;
    [[nodiscard]] glm::vec3 head_position() const;

    // Track control (docs/paths.md): every train, paths, roles.
    [[nodiscard]] std::vector<Summary> summaries() const;
    [[nodiscard]] const simulation::Train* train_by_id(std::uint64_t id) const;
    [[nodiscard]] simulation::Dispatcher& dispatcher() noexcept { return *dispatcher_; }
    [[nodiscard]] const simulation::Dispatcher& dispatcher() const noexcept { return *dispatcher_; }
    [[nodiscard]] const simulation::SpeedLimits& speed_limits() const noexcept { return *limits_; }
    // The route's paths, then the ones saved from Track control.
    [[nodiscard]] const std::vector<formats::TrainPath>& route_paths() const noexcept { return paths_; }
    [[nodiscard]] const std::vector<std::string>& consist_list();
    // Where the content's consists lie on disk (for a file dialog).
    [[nodiscard]] std::filesystem::path default_consist_folder();
    // A consist's cars without putting it anywhere (Track control's preview):
    // each car's length and where its front is behind the front of car 0.
    struct ConsistLayout {
        std::string name;
        std::vector<float> lengths;  // car 0 (the front) first
        std::vector<double> fronts;  // metres from the front of car 0
        std::vector<bool> engines;
        double length{};             // the whole train, as Train::length()
    };
    [[nodiscard]] std::string consist_layout(std::string_view consist, ConsistLayout& layout) const;
    // Gives train `id` this path (built on the network); an error text on failure.
    [[nodiscard]] std::string assign_path(std::uint64_t id, simulation::PathPlan plan);
    [[nodiscard]] std::string assign_route_path(std::uint64_t id, const formats::TrainPath& path);
    void clear_path(std::uint64_t id);
    [[nodiscard]] std::string set_role(std::uint64_t id, Role role);
    [[nodiscard]] std::string remove(std::uint64_t id);
    // Uncouples train `id` before car `index` (end cocks closed, couplers left open).
    [[nodiscard]] std::string uncouple_at(std::uint64_t id, std::size_t index);
    // A consist put on the track with its front at `where`, facing along it.
    [[nodiscard]] std::string spawn(std::string_view consist, const simulation::TrackLocation& where, Role role);
    // Saves train `id`'s path as <user_path_folder>/<name>.pat (never into the route).
    [[nodiscard]] std::string save_path(std::uint64_t id, const std::string& name);
    // The camera follows train `id` (0: the driven train).
    void follow_train(std::uint64_t id);
    // The route's signals, for the test drivers (next signal and its aspect).
    void set_signals(const simulation::SignalSystem* signals) noexcept { signals_ = signals; }
    // Text about train `id`'s path: leg, reserved way, what it waits for.
    [[nodiscard]] std::string path_status(std::uint64_t id) const;
    // RTS's information keys (docs/reverse/dispatcher.md): F5 a page of the
    // train's cars (again: the next page; with Shift: the train as a whole),
    // Ctrl+F5 the speed limits and the kilometre, F6 the stations ahead.
    void car_info_key(bool shift);
    void toggle_limits_panel() noexcept { limits_panel_ = !limits_panel_; }
    void toggle_stations_panel() noexcept { stations_panel_ = !stations_panel_; }
    void draw_hud();
    // Where Track control keeps saved paths: user_paths/<route> in the
    // working directory.
    [[nodiscard]] std::filesystem::path user_path_folder() const;

private:
    struct Car;
    struct Unit;
    struct Pose {
        glm::vec3 front{}, rear{};
        float front_heading{}, rear_heading{};
    };
    void request_models(renderer::ModelLibrary& library);
    void fit_shapes(renderer::ModelLibrary& library);
    void apply_handles(const SceneInput& input);
    // Train::turn_around plus the viewer's per-car state; the driver moves to
    // the new front engine with the handles as they were, reverser neutral.
    void turn_around();
    void step_trains();
    // Uncouples unit `unit` behind car `index - 1`; the driver stays with
    // the part that has the master engine.
    void split(std::size_t unit, std::size_t index, bool close_cocks, bool keep_open);
    // Puts the keyboard driver on the driven train again (new master after
    // the cars changed), keeping the handles.
    void reattach_driver();
    [[nodiscard]] simulation::Train& driven();
    [[nodiscard]] const simulation::Train& driven() const;
    void update_sound(float elapsed, const ViewerCamera& camera, audio::AudioEngine& sound);
    void list_consists();
    [[nodiscard]] float alpha() const noexcept;
    [[nodiscard]] Pose pose(const Unit& unit, std::size_t car) const;
    [[nodiscard]] float cars_lift(const Unit& unit, std::size_t car) const;
    [[nodiscard]] std::optional<float> ground_height(const glm::vec3& point) const;
    [[nodiscard]] std::optional<Pose> slid_pose(const Unit& unit, std::size_t car) const;
    void collide_slides(renderer::ModelLibrary& library);
    [[nodiscard]] glm::vec3 local(const simulation::TrackPoint& point) const;
    [[nodiscard]] glm::vec3 local(const formats::RoutePosition& position) const;
    // A consist's cars and train, its stock appended to `stock`.
    [[nodiscard]] std::string make_unit(std::string_view consist, Unit& unit,
                                        std::vector<std::unique_ptr<formats::WagonDefinition>>& stock) const;
    // Controllers as the unit's role wants them.
    void attach_role(Unit& unit);
    [[nodiscard]] Unit* find_unit(std::uint64_t id);
    [[nodiscard]] const Unit* find_unit(std::uint64_t id) const;
    // What the test driver of `unit` must do this step.
    [[nodiscard]] simulation::DriveOrders drive_orders(Unit& unit);
    void load_user_paths();
    // The unit the information panels describe: the followed train, else the driven one.
    [[nodiscard]] const Unit* shown_unit() const;
    // Platforms ahead of the unit's leading end: (station and platform, metres).
    [[nodiscard]] std::vector<std::pair<std::string, double>> stations_ahead(const Unit& unit) const;

    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    formats::RoutePosition origin_;
    mutable std::unordered_map<std::int64_t, std::unique_ptr<formats::TerrainTile>> terrain_;
    const std::vector<renderer::StreamedInstance>* obstacles_ = nullptr;
    simulation::TrackNetwork* network_ = nullptr;
    std::string consist_;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock_;
    std::vector<Unit> units_; // [0]: the driven train; the rest: cuts and trains from Track control
    std::unique_ptr<simulation::Dispatcher> dispatcher_;
    std::unique_ptr<simulation::SpeedLimits> limits_;
    const simulation::SignalSystem* signals_ = nullptr;
    std::uint64_t follow_id_ = 0;
    std::size_t spawned_ = 0;
    float overspeed_time_ = 0.0f;
    bool car_info_ = false;
    bool car_info_overall_ = false;
    std::size_t car_page_ = 0;
    static constexpr std::size_t car_page_size = 15;
    bool limits_panel_ = false;
    bool stations_panel_ = false;
    // A HUD panel the user can drag: where its pivot corner sits, as a
    // fraction of the display (it keeps its place when the window is
    // resized), remembered in viewer_state.ini.
    struct HudPanel {
        const char* key;
        float x = 0.0f;
        float y = 0.0f;
        float pivot_x = 0.0f; // 0: anchored by the left edge, 1: by the right one
        bool loaded = false;
        bool dragging = false;
    };
    HudPanel car_info_place_{.key = "hud_car_info"};
    HudPanel limits_place_{.key = "hud_limits", .pivot_x = 1.0f};
    HudPanel stations_place_{.key = "hud_stations", .pivot_x = 1.0f};
    // Starts a panel window at its place; false when it is not drawn.
    bool begin_panel(const char* id, HudPanel& place, float default_x, float default_y);
    void end_panel(HudPanel& place);
    std::shared_ptr<simulation::DriverHandles> driver_;
    double accumulator_{};
    bool follow_ = true;
    std::size_t joints_ = 0, junctions_ = 0, coupler_breaks_ = 0, derailments_ = 0;
    bool end_of_track_ = false;
    std::unique_ptr<audio::SoundBank> bank_;
    int substeps_ = 1;
    float sway_scale_ = 1.0f; // body sway shown on the cars: 1 = the RTS model's size, 0 off
    bool random_loop_phase_ = true; // SoundInstance::set_random_loop_phase for every car

    // Panel: route paths and the consist picker.
    std::vector<formats::TrainPath> paths_;
    int path_choice_ = 0;
    std::vector<std::string> consists_; // virtual paths
    bool consists_listed_ = false;
    char filter_[96]{};
    int consist_choice_ = -1;
    std::string pending_load_;
    bool pending_path_ = false;
    bool pending_turn_ = false;
    std::optional<std::size_t> pending_uncouple_; // uncouple the driven train before this car
    std::string message_;
};

} // namespace openrts::viewer
#endif
