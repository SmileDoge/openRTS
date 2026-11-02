#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <filesystem>
#include <future>
#include <map>
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
#include <openrts/renderer/frustum.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/glow_sprites.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/renderer/scene_effects.hpp>
#include <openrts/formats/terrain.hpp>
#include <openrts/simulation/auto_driver.hpp>
#include <openrts/simulation/dispatcher.hpp>
#include <openrts/simulation/electrification.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/speed_limits.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/simulation/train_route.hpp>

#include "camera.hpp"
#include "cab_view.hpp"
#include "engineer.hpp"
#include "legacy_cab.hpp"
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
    // The trains' cars for this frame. A car out of `frustum` is left out,
    // or given as a shadow caster only (no pose) when it is within
    // `shadow_range` of `eye`; one farther than `draw_distance` (on the
    // ground plane) is left out. The car the engineer is in is always drawn.
    struct SubmitView {
        const renderer::Frustum* frustum = nullptr;
        glm::vec3 eye{};
        float draw_distance = 0.0f;  // <= 0: no limit
        float shadow_range = 0.0f;
    };
    void submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library, const SubmitView& view = {});
    // The engineer into the driving cab's seat (--capture-cab).
    void enter_cab() { set_engineer(true); }
    // The cab the engineer is in as the last submit() drew it: a sphere
    // around his place in it, fixed to the cab, and its forward, for the
    // cab's own shadow map.
    [[nodiscard]] std::optional<renderer::ShadowFocus> cab_shadow_focus() const { return cab_focus_; }
    void draw_overlay();
    void set_origin(const formats::RoutePosition& origin);
    // The streamed world objects near the camera: obstacles for derailed
    // cars sliding along (null: none).
    void set_obstacles(const std::vector<renderer::StreamedInstance>* obstacles) { obstacles_ = obstacles; }
    void stop_sound(audio::AudioEngine& sound);
    // The player's cab keys are on (a locomotive DLL's cab): the viewer's
    // camera and default driving keys are off.
    [[nodiscard]] bool keyboard_captured() const noexcept;
    // Whether the driving keys (the DLL's cab keys, openRTS's handles) reach
    // the driven cab: not while the engineer is on foot out of the driven car
    // (on the platform, in another car). With the engineer off (an outside
    // camera) they do, as RTS lets one drive from any view.
    [[nodiscard]] bool cab_keys_live() const noexcept;
    // Which of a sound script's camera groups play for the player's train.
    [[nodiscard]] audio::SoundCamera sound_camera() const noexcept;
    [[nodiscard]] bool following() const noexcept { return follow_; }
    void stop_following() noexcept { follow_ = false; }
    // Terrain height at a scene point (x, z), when its tile has heights.
    [[nodiscard]] std::optional<float> ground_at(const glm::vec3& point) const { return ground_height(point); }
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
    // RTS's config.ini (engineer.Height, engineer.velocity, ...): it is not in the VFS.
    void set_rts_config(std::string text) { rts_config_ = std::move(text); }
    // Switch `id` of the player's cab to `state` (--capture-switch).
    void set_cab_switch(std::uint32_t id, std::uint32_t state) {
        if (legacy_) legacy_->set_switch(id, state);
    }
    // Car `car` of the driven train: shape space to world (no body sway).
    [[nodiscard]] std::optional<glm::mat4> car_transform(std::size_t car) const;

    // Track control (docs/paths.md): every train, paths, roles.
    [[nodiscard]] std::vector<Summary> summaries() const;
    [[nodiscard]] const simulation::Train* train_by_id(std::uint64_t id) const;
    [[nodiscard]] simulation::Dispatcher& dispatcher() noexcept { return *dispatcher_; }
    [[nodiscard]] const simulation::Dispatcher& dispatcher() const noexcept { return *dispatcher_; }
    [[nodiscard]] const simulation::SpeedLimits& speed_limits() const noexcept { return *limits_; }
    // The DLLs' start state for the next loaded consist: -1 running (the
    // default), 0 cold, 1 on (an activity's TrainState, docs/train-dll.md).
    void set_start_state(int state) noexcept { start_state_ = state; }
    // The route's overhead line (docs/electrification.md).
    [[nodiscard]] simulation::Electrification& electrification() noexcept { return *electrification_; }
    // The player train's exterior lights that are on (DLL-switched): glow
    // sprites (type 0, by texture) and light sources for the shaders
    // (headlights, searchlight), made by update().
    struct LightGlow {
        renderer::GlowSprite sprite;
        core::VirtualPath texture;
    };
    [[nodiscard]] const std::vector<LightGlow>& light_glows() const noexcept { return light_glows_; }
    [[nodiscard]] const std::vector<renderer::ModelExtraLight>& light_beams() const noexcept { return light_beams_; }
    // The lamps of the cab or salon the engineer is in that are on.
    [[nodiscard]] const std::vector<renderer::ModelExtraLight>& interior_lights() const noexcept { return interior_lights_; }
    // The world clock the DLLs read (RTS_VAR_TIME, RTS_ISNIGHT): the scene's
    // time of day, s since midnight, and whether the sky is at night.
    void set_world_clock(const double day_time, const bool night) noexcept {
        day_time_ = day_time;
        night_ = night;
    }
    // The cab screens (КЛУБ, САВПЭ ...) of the cab the engineer is in, for
    // this frame, in scene coordinates (filled by submit()).
    [[nodiscard]] const std::vector<ScreenBatch>& screen_batches() const noexcept { return screen_batches_; }
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
    // The 3-D cab: the caption of the control under the mouse, and with Alt
    // (`all`) the captions of every control near `eye`.
    void collect_cab_labels(std::vector<WorldLabel>& labels, const glm::vec3& eye, bool all) const;
    // The mouse in the 3-D cab: the ray from `eye` (none: not over the view),
    // a left click, the wheel. A click or the wheel works the switch under it.
    // `release`: the left button was let go (a NONFIXED button comes back).
    // `cursor`: the cursor in RTS's window pixels (the scene scales ours by
    // the view angle), `held`: the left button down (a CONTROLLERX /
    // CONTROLLERY control taken hold of follows the cursor).
    // RTS's car menu (Num3, rts1.exe 0x4121BC): outside the cab, a coupling
    // (within 5 m) or bogie (within twice the car's width) on the screen gets
    // a menu of what a hand can do there (0x415A34 picks, 0x462FC8 builds,
    // 0x463F84 acts); the key again closes it. RTS takes the nearest one;
    // here the one nearest to `aim` (the cursor, else the screen's middle,
    // in clip x / y) is taken, so the one looked at opens, and the cab car's
    // own bogies are offered too.
    void car_menu_key(const glm::vec3& eye, const glm::mat4& view_projection, glm::vec2 aim = {});
    [[nodiscard]] bool car_menu_open() const noexcept { return car_menu_.has_value(); }
    // --capture-menu: pick item `id` of the open menu (done in update());
    // the menu (or the last one) and its car's state as text.
    void choose_car_menu_item(int id) {
        if (car_menu_) car_menu_choice_ = id;
    }
    [[nodiscard]] std::string car_menu_report() const;
    void pointer(const glm::vec3& eye, const std::optional<glm::vec3>& direction, bool click, bool shift, float scroll,
                 bool release = false, const glm::vec2& cursor = {}, bool held = false);
    [[nodiscard]] bool engineer_inside() const noexcept { return engineer_.inside(); }
    // F7: car numbers on the side of each car facing `eye`, within `range`.
    void collect_car_numbers(std::vector<WorldLabel>& labels, const glm::vec3& eye, float range) const;
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
    std::unique_ptr<simulation::Electrification> electrification_;
    int start_state_ = -1;
    simulation::LineSupply line_;   // the driven train's, last step
    bool line_engines_ = false;     // the driven train has electric engines
    const simulation::SignalSystem* signals_ = nullptr;
    std::uint64_t follow_id_ = 0;
    std::size_t spawned_ = 0;
    std::uint32_t numbered_trains_ = 0; // RTS's train counter for car numbers
    void number_cars(Unit& unit);
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
    // The player's train runs locomotive DLLs (legacy_cab.hpp).
    std::unique_ptr<LegacyCab> legacy_;
    std::string legacy_status_;
    void attach_legacy();
    void update_pantographs(float elapsed);
    void update_exterior_doors(float elapsed);
    // Each train's line voltage at its electric engines, before the step.
    void feed_line();
    // The signals and limits ahead for the DLLs, before their step.
    void feed_world(const Unit& unit);
    // The next signal's aspect (the cab's locomotive signal lights without a DLL).
    [[nodiscard]] std::uint32_t signal_aspect(const Unit& unit) const;
    void update_engineer(const SceneInput& input, ViewerCamera& camera, float elapsed, renderer::ModelLibrary& library);
    // The engineer outside: what is around him this frame.
    void gather_walk_world(const glm::vec3& around, renderer::ModelLibrary& library);
    [[nodiscard]] std::optional<float> walk_floor(const glm::vec3& at) const;
    [[nodiscard]] bool walk_blocked(const glm::vec3& from, const glm::vec3& to) const;
    void update_aux_sounds(Unit& unit, std::size_t u, std::size_t i, const audio::SoundInputs& exterior, float elapsed,
                           const ViewerCamera& camera, audio::AudioEngine& sound);
    void update_cab_sound(Unit& unit, std::size_t u, std::size_t i, const audio::SoundInputs& exterior, float elapsed,
                          const ViewerCamera& camera, audio::AudioEngine& sound);
    struct BodyPose {
        glm::mat4 transform{1.0f};
        Pose here;
        float heading{};
    };
    // Where car `i`'s body is drawn this frame (interpolated, swaying, tipped).
    // `sway` off: on its springs at rest (the cab the player is in: RTS sways
    // the camera instead, 0x424488).
    [[nodiscard]] std::optional<BodyPose> body_pose(const Unit& unit, std::size_t i, bool sway = true) const;
    // The body's offset on its springs in the scene (times the sway slider).
    [[nodiscard]] glm::vec3 sway_offset(const Unit& unit, std::size_t i) const;
    // The 3-D cab (cab_view.hpp) of car cab_car_ of the driven train.
    std::unique_ptr<CabView> cab_;
    std::size_t cab_car_ = SIZE_MAX;
    bool cab_requested_ = false;
    std::future<renderer::StreamedModelAsset> cab_loading_;
    // The player on foot: in a cab chair, walking about the cab, outside.
    Engineer engineer_;
    std::size_t engineer_car_ = SIZE_MAX;  // the car he is in (or got off)
    // That car's cab when it is not cab_car_ (he walked into the next section).
    std::unique_ptr<CabView> visit_cab_;
    bool visit_requested_ = false;
    std::future<renderer::StreamedModelAsset> visit_loading_;
    std::optional<CabView::Picked> hovered_; // the control under the mouse
    bool keys_were_live_ = true;             // cab_keys_live() the last frame
    glm::vec2 drag_cursor_{};                // where the cursor was when a dragged control last moved
    std::vector<ScreenBatch> screen_batches_; // this frame's cab screens, scene coordinates
    // The cab shape's placement in the last submitted frame: the mouse ray and
    // the captions use it with that frame's camera, so they keep still on the move.
    std::optional<glm::mat4> cab_to_scene_;
    std::optional<renderer::ShadowFocus> cab_focus_;
    // Where that sphere sits in the cab (cab coordinates) and for which cab:
    // it follows the eye only when he has moved a metre from it, so the
    // body's sway does not shake the map.
    std::optional<glm::vec3> cab_focus_anchor_;
    const void* cab_focus_cab_ = nullptr;
    double day_time_ = 12.0 * 3600.0;
    std::vector<LightGlow> light_glows_;
    std::vector<renderer::ModelExtraLight> light_beams_;
    std::vector<renderer::ModelExtraLight> interior_lights_;
    // Textures the cabs' TextureChanges draw, held while the train is here.
    std::map<std::string, std::shared_ptr<const renderer::GpuTexture>> cab_textures_;
    std::vector<float> lamp_level_; // the engineer's cab or salon, by lamp (LightFade)
    void update_lights(float elapsed, const glm::vec3& eye);
    bool night_ = false;
    float frame_elapsed_{};
    [[nodiscard]] CabView* engineer_cab() noexcept { return engineer_car_ == cab_car_ ? cab_.get() : visit_cab_.get(); }
    [[nodiscard]] const CabView* engineer_cab() const noexcept {
        return engineer_car_ == cab_car_ ? cab_.get() : visit_cab_.get();
    }
    void set_engineer(bool on);
    // Through door `door` of his cab into the car coupled at that end.
    void pass_to_next_car(std::size_t door);
    bool engineer_configured_ = false;
    std::string rts_config_; // RTS's config.ini text (set_rts_config)
    glm::vec3 look_{0.0f, 0.0f, 1.0f}; // the camera's direction, for the cab labels
    // Outside he collides with the world's objects and stands on them
    // (platforms, ballast, bridges), or only with the ground and the trains.
    bool walk_objects_ = true;
    struct WalkMesh {
        std::future<std::shared_ptr<CabCollision>> loading;
        std::shared_ptr<CabCollision> mesh; // null while loading or when it does not read
    };
    std::unordered_map<std::string, WalkMesh> walk_meshes_; // by shape path
    struct WalkObject {
        const CabCollision* mesh{};
        glm::mat4 transform{1.0f}, inverse{1.0f};
    };
    std::vector<WalkObject> walk_objects_near_;
    struct WalkBox { // a car body: |x| < half_width, |z| < half_length, 0 < y < height (car coordinates)
        glm::mat4 inverse{1.0f};
        float half_width{}, half_length{}, height{};
    };
    std::vector<WalkBox> walk_cars_;
    double accumulator_{};
    bool follow_ = true;
    std::size_t joints_ = 0, junctions_ = 0, coupler_breaks_ = 0, derailments_ = 0;
    bool end_of_track_ = false;
    std::unique_ptr<audio::SoundBank> bank_;
    int substeps_ = 1;
    bool realistic_air_ = false; // openRTS realistic air for every train (BrakeModel::realistic)
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
    // The open car menu: a coupling (the joint at a car's end) or a bogie.
    struct CarMenu {
        bool coupling = true;
        std::uint64_t train = 0; // simulation::Train::id()
        std::size_t car = 0;     // in that train
        bool front = true;       // the end / bogie towards the train front
        int highlight = 0;
    };
    struct CarMenuItem {
        int id;            // RTS's item id (0x463F84)
        const char* label; // English, translated when shown
    };
    std::optional<CarMenu> car_menu_;
    std::optional<CarMenu> last_car_menu_; // for car_menu_report()
    std::optional<int> car_menu_choice_; // picked in the window, done in update()
    [[nodiscard]] std::vector<CarMenuItem> car_menu_items(const CarMenu& menu) const;
    void update_car_menu(const glm::vec3& eye);
    void do_car_menu_item(const CarMenu& menu, int id);
    void draw_car_menu();
    // Joins `absorbed`'s unit into `survivor`'s after Train::couple (as
    // resolve_contacts reports it); returns the survivor's index.
    std::size_t merge_units(const simulation::Train* survivor, const simulation::Train* absorbed, bool absorbed_ahead,
                            bool absorbed_turned);
    std::string message_;
};

} // namespace openrts::viewer
#endif
