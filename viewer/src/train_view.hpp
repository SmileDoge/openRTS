#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <array>
#include <filesystem>
#include <future>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/audio/audio_engine.hpp>
#include <openrts/audio/interior_acoustics.hpp>
#include <openrts/formats/route_database.hpp>
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
#include <openrts/simulation/train_lights.hpp>
#include <openrts/simulation/train_route.hpp>
#include <openrts/trainplugin/native_plugin.hpp>

#include "camera.hpp"
#include "cab_view.hpp"
#include "engineer.hpp"
#include "legacy_cab.hpp"
#include "particle_effects.hpp"
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
    // For the start's loading screen: `settled` once the train is on the
    // track and the start view applied (or it cannot be put on the track);
    // `pending` its shapes still loading, and for a start in the cab
    // (`cab`) the cab.
    struct Loading {
        bool settled = true;
        std::size_t pending = 0;
        bool cab = false;
    };
    [[nodiscard]] Loading loading() const;
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
    // Walking is the player's, not a train's (the main panel's "On foot"):
    // on, he stands on the ground under `eye` and walks, into a cab of his
    // train by its door; off, the free camera.
    void set_walking(bool on, const glm::vec3& eye);
    [[nodiscard]] bool walking() const noexcept { return engineer_.active(); }
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
    // The share of a sound outside that reaches the engineer's room through
    // its doors and windows (1 when he is in no room). RTS multiplies every
    // sound object by it: trains, the world, the weather (0x44AB7C,
    // docs/sound.md "Rooms and sound proofing").
    [[nodiscard]] float outside_gain() const noexcept { return outside_gain_; }
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
    // Every diesel exhaust of every train this frame (SmokeView).
    void collect_exhausts(std::vector<ExhaustSource>& out) const;
    // A traveller at the end of the train that leads (by the motion, else
    // the reverser), facing away from the train.
    [[nodiscard]] std::optional<simulation::TrackTraveller> ahead() const;
    [[nodiscard]] glm::vec3 head_position() const;
    // RTS's config.ini (engineer.Height, engineer.velocity, ...): it is not in the VFS.
    void set_rts_config(std::string text) { rts_config_ = std::move(text); }
    // Switch `id` of the player's cab to `state` (--capture-switch).
    void set_cab_switch(std::uint32_t id, std::uint32_t state) {
        if (legacy_) legacy_->set_switch(id, state);
        if (native_) (void)native_->operate(static_cast<OrtsHandle>(id), static_cast<float>(state));
    }
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> cab_switch_state(std::uint32_t id) const {
        const auto* out = native_ ? &native_output_ : legacy_ ? legacy_->output(legacy_->player()) : nullptr;
        if (!out || id >= out->switches.size()) return std::nullopt;
        return std::pair{out->switches[id].state, out->switches[id].set_state};
    }
    // Car `car` of the driven train: shape space to world (no body sway).
    [[nodiscard]] std::optional<glm::mat4> car_transform(std::size_t car) const;

    // Track control (docs/paths.md): every train, paths, roles.
    [[nodiscard]] std::vector<Summary> summaries() const;
    // A train's cars for the inspector: what they are and where drawn.
    struct InspectedCar {
        std::size_t index{};
        std::string name;    // the definition's Name
        std::string file;    // TRAINS/TRAINSET/<folder>/<file>.eng or .wag
        core::VirtualPath shape;
        bool engine = false;
        float mass{}, length{}, width{}, height{};
        std::string library; // the .eng's LibraryModule
        std::optional<glm::mat4> transform; // shape space to the scene, when drawn
    };
    [[nodiscard]] std::vector<InspectedCar> inspected_cars(std::uint64_t train) const;
    [[nodiscard]] const simulation::Train* train_by_id(std::uint64_t id) const;
    [[nodiscard]] simulation::Dispatcher& dispatcher() noexcept { return *dispatcher_; }
    [[nodiscard]] const simulation::Dispatcher& dispatcher() const noexcept { return *dispatcher_; }
    [[nodiscard]] const simulation::SpeedLimits& speed_limits() const noexcept { return *limits_; }
    // The DLLs' start state for the next loaded consist: -1 running (the
    // default), 0 cold, 1 on (an activity's TrainState, docs/train-dll.md).
    void set_start_state(int state) noexcept { start_state_ = state; }
    // Where the player starts once the driven train stands on the track
    // (--spawn): in its cab; on foot beside the cab car, as RTS starts an
    // activity by default (0x42F4A4: 2 m off the car's left side at its
    // front end); or the free camera above the train.
    enum class StartView : std::uint8_t { none, cab, car, above };
    void set_start_view(StartView view) noexcept {
        start_view_ = view;
        starts_in_cab_ = view == StartView::cab;
    }
    // Situations: the trains on the route as they stand (roles, where each
    // front is, the cars in order), <situation_folder>/<name>.situation.
    [[nodiscard]] std::filesystem::path situation_folder() const;
    [[nodiscard]] std::vector<std::string> situations() const;
    [[nodiscard]] std::string save_situation(const std::string& name) const;
    // Puts a saved situation back at the next update (the trains there now
    // and their sounds go first).
    void request_situation(std::filesystem::path file) { pending_situation_ = std::move(file); }
    // A train to put on the track: a situation's, or one from an RTS
    // snapshot (--mp-snapshot): its name, role, where car 0's front stands
    // and its cars front to back.
    struct PlacedTrain {
        std::string name;
        Role role = Role::standing;
        std::optional<simulation::TrackLocation> front;
        std::vector<formats::ConsistEntry> cars;
        // The cars' keys (0 or none: new ones). A car keeps its key through
        // couplings and cuts, and the DLL host its DLL by it: a car that
        // comes back finds its DLL as it left it (RTS binds a DLL to its car
        // once, 0x5026FC).
        std::vector<std::uint32_t> keys;
    };
    // A new car key (unique in this program).
    [[nodiscard]] static std::uint32_t new_car_key();
    // Puts these trains back at the next update, as a situation does;
    // `source` names them in the messages.
    void request_trains(std::vector<PlacedTrain> trains, std::string source) {
        pending_trains_ = std::move(trains);
        pending_trains_source_ = std::move(source);
    }
    // A train another client drives (the network, docs/net.md): put on the
    // track now, never simulated and never woken; the network moves it
    // (find_train()->shift / place). Its id, 0 when it does not fit.
    std::uint64_t add_remote(const PlacedTrain& train, std::string* error = nullptr);
    // One of this client's own trains beside the ones there (the network: a
    // second train of the player, one assigned or put back during play):
    // simulated, standing until driven (set_role). Its id, 0 when it does
    // not fit.
    std::uint64_t add_local(const PlacedTrain& train, std::string* error = nullptr);
    // Takes train `id` off the track, the driven one too: the player is let
    // out where he was (no train, the free camera). False: no such train.
    bool drop(std::uint64_t id);
    // A train of this client becomes another's (the network: taken away,
    // RTS kind 2): drawn by the network from now, its DLLs bound but not run
    // (RTS 0x424488); the player leaves it if he drove it. take_back: ours
    // again, its DLLs going on where they stopped.
    bool give_away(std::uint64_t id);
    bool take_back(std::uint64_t id);
    // Couplings and cuts since the last call, for the network (0x460F,
    // 0x460E). A coupling: `survivor` took `absorbed` in, its cars in front
    // or behind, turned or not. A cut: train `from` was cut before car
    // `index`; `kept` holds the part the player stays with (it may be a new
    // train object), `part` the other; `front_part`: the part is the cars
    // before `index`.
    struct Coupling {
        std::uint64_t survivor{}, absorbed{};
        bool absorbed_ahead = false, absorbed_turned = false;
    };
    struct Cut {
        std::uint64_t from{}, kept{}, part{};
        std::size_t index{};
        bool front_part = false;
    };
    [[nodiscard]] std::vector<Coupling> take_couplings() { return std::exchange(couplings_, {}); }
    [[nodiscard]] std::vector<Cut> take_cuts() { return std::exchange(cuts_, {}); }
    // Trains the player may take (the network: his own). On foot, Enter at
    // an exterior door of one of their cabs makes it the driven train; the
    // Train panel lists them with "Drive".
    // A network game (--mp-connect): the panel leaves out what puts trains
    // on the track (paths, consists, situations).
    void set_network(bool on) noexcept { network_game_ = on; }
    // A train's category for speed posts as the server says (network).
    void set_category(std::uint64_t id, simulation::TrainCategory category);
    void set_boardable(std::uint64_t id, bool on) {
        if (on) {
            boardable_.insert(id);
        } else {
            boardable_.erase(id);
        }
    }
    // Train `id` becomes the driven one, its cab that of `car` (the engine
    // he walked up to; SIZE_MAX the DLL's own). `seat`: the player is put in
    // its chair (the reserve "Drive"); else he stays where he stands, to
    // get in by the door as into any cab of his. An error, or empty.
    [[nodiscard]] std::string board(std::uint64_t id, std::size_t car = SIZE_MAX, bool seat = true);
    [[nodiscard]] simulation::Train* find_train(std::uint64_t id);
    bool remove_train(std::uint64_t id);
    // A remote train's look as its owner sends it (car_light, car_state):
    // RTS draws another player's train only by these, its own lights'
    // conditions and pantograph rules are not worked out (0x4F07D4).
    void set_remote_light(std::uint64_t id, std::size_t car, std::size_t light, bool on);
    void set_remote_look(std::uint64_t id, std::size_t car, std::uint8_t pantographs, bool running);
    // The train the server gives this client (docs/net.md, "Own train"):
    // put on the track now as the driven train, every train there now gone;
    // its DLLs, cab and keyboard as with --situation. An error, or empty.
    [[nodiscard]] std::string place_driven(PlacedTrain train);
    // A train's look as the network sends it (a train simulated here): per
    // car its kind (0 wagon, 1 electric, 2 diesel), raised pantographs,
    // whether it runs, and each light's wanted state with the State it shows.
    struct LightLook {
        bool on = false;
        const formats::LightState* state = nullptr;
    };
    struct CarLook {
        std::uint8_t kind{};
        std::uint8_t pantographs{};
        bool running = false;
        std::vector<LightLook> lights;
    };
    [[nodiscard]] std::vector<CarLook> look_of(std::uint64_t id) const;
    // A train's derailed cars as the network sends them (0x4636): a flag per
    // car and each derailed car's place, heading and roll. Empty while none is.
    struct CarDerail {
        std::size_t car{};
        glm::vec3 heading{0.0f, 0.0f, 1.0f};
        formats::RoutePosition position;
        float tilt{}, tilt_target{};
        float weight{}; // m * g, N
    };
    struct DerailLook {
        std::vector<std::uint8_t> derailed; // by car
        std::vector<CarDerail> cars;
    };
    [[nodiscard]] DerailLook derail_of(std::uint64_t id) const;
    // A line for the player (the message strip).
    void say(std::string text) { message_ = std::move(text); }
    // A train's exterior sound for the network (0x4639, 0x463D): per car the
    // discrete triggers it played since the last call (the group's
    // NonDistributableTriggers left out) and its Var1..Var8 and distance
    // travelled. Collected only for the trains given to share_sound.
    struct CarSound {
        std::vector<std::uint32_t> triggers;
        std::array<float, 8> vars{};
        float travelled{};
    };
    void share_sound(std::uint64_t id, bool on) {
        if (on) {
            shared_sound_.insert(id);
        } else {
            shared_sound_.erase(id);
        }
    }
    [[nodiscard]] std::vector<CarSound> take_sound(std::uint64_t id);
    // A remote train's sound as its owner sends it: a discrete trigger, and
    // the variables its sound curves read.
    void remote_sound_trigger(std::uint64_t id, std::size_t car, std::uint32_t code);
    void set_remote_sound(std::uint64_t id, std::size_t car, const std::array<float, 8>& vars);
    // The whole exterior sound of a train simulated here, per car (the
    // answer to state_request 2, 0x463A), and another client's applied.
    struct CarSoundState {
        std::array<float, 8> vars{};
        float travelled{};
        std::vector<audio::SoundInstance::StreamState> streams;
    };
    [[nodiscard]] std::vector<CarSoundState> sound_state_of(std::uint64_t id) const;
    void restore_remote_sound(std::uint64_t id, std::size_t car, const std::vector<std::uint32_t>& current);
    // Saves the situation as `name` once the driven train stands on the
    // track (--save-situation, for scripted checks).
    void save_situation_when_placed(std::string name) { save_when_placed_ = std::move(name); }
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
    // How lit the world is (SkyState::world_lit) and the weather (0 clear, 1
    // snow, 2 rain): the light conditions of trains without a DLL, the cab
    // lamps' NIGHT_AUTO.
    void set_world_light(const float world_lit, const std::uint8_t weather) noexcept {
        world_lit_ = world_lit;
        weather_ = weather;
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
    // --capture-uncouple: the Train window's "uncouple here" before car `index`
    // of the driven train (done in update(), the train standing).
    void uncouple_driven(std::size_t index) { pending_uncouple_ = index; }
    // --capture-drive: train `index` (in the trains' order) becomes the driven one.
    void drive_unit(std::size_t index);
    // A test (--capture-board): the engineer on the ground at a cab door of
    // car `car` of the driven train.
    void stand_at_cab_door(std::size_t car);
    [[nodiscard]] std::string car_menu_report() const;
    // For the capture report: lights, pantographs, the line, cables, the
    // native plugin, of every train.
    [[nodiscard]] std::string systems_report() const;
    void pointer(const glm::vec3& eye, const std::optional<glm::vec3>& direction, bool click, bool shift, float scroll,
                 bool release = false, const glm::vec2& cursor = {}, bool held = false);
    [[nodiscard]] bool engineer_inside() const noexcept { return engineer_.inside(); }
    // The view from inside his cab or salon: not while the car view has the
    // camera (F3).
    [[nodiscard]] bool viewing_inside() const noexcept { return engineer_.inside() && car_view_ == CarView::off; }
    // F3: the camera orbits the train's first car, then its last, then back.
    void car_view_key(ViewerCamera& camera);
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
    void update_car_view(ViewerCamera& camera);
    void toggle_limits_panel() noexcept { limits_panel_ = !limits_panel_; }
    void toggle_stations_panel() noexcept { stations_panel_ = !stations_panel_; }
    void draw_hud();
    // Where Track control keeps saved paths: user_paths/<route> in
    // user_data_directory().
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
    void step_trains();
    // Uncouples unit `unit` behind car `index - 1`; the driver stays with
    // the part he drives from (the cab's car, else the master engine).
    void split(std::size_t unit, std::size_t index, bool close_cocks, bool keep_open);
    // The cars the cab and the engineer are in (Car::key), taken before the
    // driven train's cars change.
    struct DrivenKeys {
        std::uint32_t cab{}, engineer{};
    };
    [[nodiscard]] DrivenKeys driven_keys() const;
    // Puts the keyboard driver on the driven train again (new master after
    // the cars changed), keeping the handles. With `keep` (cars coupled on
    // or cut off): the DLLs, the cab and the engineer stay with their cars,
    // as RTS keeps them (0x469ED8 / 0x469858); else they start again.
    void reattach_driver(const DrivenKeys* keep = nullptr);
    bool keep_attached(const DrivenKeys& keep);
    // The DLL host's trains (RTS: every train the player has been in keeps
    // its DLLs running, 0x424488): the driven unit first, then the parts cut
    // off from it (Unit::host), in units_ order. Made again after every
    // change; the step checks it is still so. Their DLL engines get their
    // controllers (the standing ones: an AI unit keeps its driver).
    bool reconsist_host();
    [[nodiscard]] bool host_current() const;
    std::vector<std::vector<std::uint32_t>> host_keys_; // each host train's car keys at the last reconsist
    // What car `i` of unit `u` gets from its DLL (none: not in the host).
    [[nodiscard]] const dllhost::CarOutput* dll_output(std::size_t u, std::size_t i) const;
    [[nodiscard]] bool dll_car(std::size_t u, std::size_t i) const;
    [[nodiscard]] const std::array<float, 8>* dll_sound_variables(std::size_t u, std::size_t i) const;
    // The driven unit is one the host already runs (a part cut off): the
    // player takes it over without a new Init (RTS 0x468C74 binds once).
    bool take_over_host_unit();
    // The 3-D cab of car `player` of the driven unit (none: the first engine).
    void attach_cab(std::size_t player);
    [[nodiscard]] simulation::Train& driven();
    [[nodiscard]] const simulation::Train& driven() const;
    void update_listener(const ViewerCamera& camera);
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
    // The same from its cars, front to back.
    [[nodiscard]] std::string make_unit_from(std::string name, const std::vector<formats::ConsistEntry>& entries, Unit& unit,
                                             std::vector<std::unique_ptr<formats::WagonDefinition>>& stock,
                                             const std::vector<std::uint32_t>& keys) const;
    [[nodiscard]] std::string make_unit_from(std::string name, const std::vector<formats::ConsistEntry>& entries, Unit& unit,
                                             std::vector<std::unique_ptr<formats::WagonDefinition>>& stock) const;
    [[nodiscard]] std::string load_situation(const std::filesystem::path& file);
    [[nodiscard]] std::string put_back(std::vector<PlacedTrain> saved, const std::string& source);
    void apply_start_view(ViewerCamera& camera);
    StartView start_view_ = StartView::none;
    bool starts_in_cab_ = false; // the start view asked for (start_view_ is cleared once applied)
    // The driven train was just made (load, put_back): its DLLs' Init gives
    // it its air (LegacyCab::attach `fresh`).
    bool fresh_train_ = false;
    std::filesystem::path pending_situation_;
    std::vector<PlacedTrain> pending_trains_;
    std::string pending_trains_source_;
    std::set<std::uint64_t> shared_sound_; // take_sound
    std::set<std::uint64_t> boardable_;    // set_boardable
    bool network_game_ = false;            // set_network
    std::vector<Coupling> couplings_;      // take_couplings
    std::vector<Cut> cuts_;                // take_cuts
    std::string board_prompt_;             // Enter at another cab's door (his other trains, the far end of his own)
    // The cabs of engines he is not in, by car key: the ones he stood at
    // (their doors for boarding) and the ones he left, kept with their
    // doors as he left them (RTS keeps each car's cabin, wagon +556); a car's
    // outer doors follow its cab's.
    std::map<std::uint32_t, std::unique_ptr<CabView>> board_cabs_;
    struct BoardTarget {
        std::uint64_t train{};
        std::size_t car{};
        std::size_t door{};
    };
    // The cab door nearest to his feet: of another car of the driven train
    // (the far end of a multiple unit), or of another train of his.
    [[nodiscard]] std::optional<BoardTarget> board_target(const glm::vec3& feet);
    // On the ground at the door of car `car` of the driven train: that car is
    // his, its cab driven from as when he walks into it; the same Enter then
    // climbs in.
    void walk_up_to_cab(std::size_t car, const glm::vec3& feet);
    // Car `car` of the driven train's cab, as he found it at its door (the
    // one board_target read, taken), else a new one.
    [[nodiscard]] std::unique_ptr<CabView> take_cab(std::size_t car);
    // The cab of car `car` of the driven train, which he leaves, kept.
    void keep_cab(std::size_t car, std::unique_ptr<CabView> cab);
    // Sounds of trains taken away: stopped at the next sound update (a
    // sound object dropped while it plays leaves its loops running).
    std::vector<std::unique_ptr<audio::SoundInstance>> retired_sounds_;
    void retire_sounds(Unit& unit);
    std::uint64_t add_unit(const PlacedTrain& train, bool remote, std::string* error);
    // The cab's switch frames and display values one step back (CabView::Blend).
    std::vector<float> cab_switch_frames_before_, cab_display_values_before_;
    std::size_t cab_before_car_ = SIZE_MAX;
    std::string save_when_placed_;
    std::array<char, 64> situation_name_{}; // the Train window's name for a situation to save
    int situation_choice_ = -1;
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
    // The car view (F3) and the camera the engineer keeps meanwhile.
    enum class CarView : std::uint8_t { off, head, tail };
    CarView car_view_ = CarView::off;
    ViewerCamera person_camera_{};
    // Each car's view as it was left (by Car::key): the angle from the
    // car's heading, so the same side of it is seen again.
    struct CarViewMemory {
        float yaw_from_heading{};
        float pitch{};
        float distance{};
        float zoom = 1.0f;
    };
    std::unordered_map<std::uint32_t, CarViewMemory> car_view_memory_;
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
    // The driven train's engine with a native plugin (EngExtended
    // ORTSTrainPlugin): its controller and its cab state.
    void attach_native();
    // The engineer walked into another engine's cab (RTS 0x435654 ->
    // 0x436734): it becomes the player's (the DLLs' ChangeLoco, the keyboard
    // driver's master).
    void change_cab(std::size_t car);
    // openRTS's default logic for the driven train without a DLL: horn,
    // bell, sander, pantographs, doors (SceneInput keys).
    void apply_default_systems(const SceneInput& input);
    void update_pantographs(float elapsed);
    void update_exterior_doors(float elapsed);
    // A car of the driven train turned against the cab the engineer drives
    // from: its GlobalID 1 / 2 doors swapped for the Y / U keys' sides.
    [[nodiscard]] bool door_side_turned(const Car& car) const;
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
    void post_cab_sounds();
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
    // `eye` (scene) in the coordinates of the cab the engineer is in, if he is.
    [[nodiscard]] std::optional<glm::vec3> eye_in_cab(const glm::vec3& eye) const;
    // The body's offset on its springs in the scene (times the sway slider).
    // A car's body on its springs, between the last two steps (render time).
    [[nodiscard]] glm::vec3 body_sway(const Unit& unit, std::size_t i) const;
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
    // Where the listener is in the engineer's cab or salon (its shape
    // coordinates) and which room that is, for the sounds this frame.
    // The route database's signal units on the TDB signal items as RTS
    // hangs them (RTSSignalData for the DLLs' ALSN), read aside once.
    struct SignalUnits {
        formats::RouteDatabase database;
        std::vector<std::int32_t> item_unit; // by TDB item, -1 none
    };
    std::shared_future<std::shared_ptr<const SignalUnits>> signal_units_;
    [[nodiscard]] const SignalUnits* signal_units();
    double items_searched_[2]{}; // how far the item list's walks went (ahead, behind), m
    const CabView* listener_cab_ = nullptr;
    glm::vec3 listener_local_{};
    int listener_room_ = audio::InteriorAcoustics::outside;
    int traced_room_ = -2; // OPENRTS_SOUND_TRACE: the room last logged
    float outside_gain_ = 1.0f;
    std::vector<audio::SoundSourceLevel> source_levels_; // the cab script's SoundSources as heard
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
    float world_lit_ = 1.0f;
    std::uint8_t weather_ = 0;
    float frame_elapsed_{};
    // The driven train without a DLL: openRTS's default systems.
    bool horn_ = false;
    bool bell_ = false;
    bool pantographs_down_ = false;      // P: the driver lowered them
    std::array<bool, 2> doors_open_{};    // [0] right (GlobalID 1), [1] left (GlobalID 2)
    // The native plugin of the driven train (attach_native).
    std::shared_ptr<trainplugin::NativeLocomotive> native_;
    std::size_t native_car_ = SIZE_MAX;
    dllhost::CarOutput native_output_;    // its cab as the 3-D cab draws it
    struct NativeBinding {
        std::uint32_t id{};
        int key{};
        bool shift = false, ctrl = false;
        int direction{};
    };
    std::vector<NativeBinding> native_bindings_;
    std::map<std::uint32_t, bool> native_going_down_;
    void native_operate(std::uint32_t id, int direction);
    // The cab state the 3-D cab of car `car` shows: a DLL's or the native plugin's.
    [[nodiscard]] const dllhost::CarOutput* cab_state(std::size_t car) const;
    // A coupling being fixed (the car menu's "Fix coupling", RTS 0x463F84
    // id 17: 120 s, 0x424488 counts it down).
    struct Repair {
        std::uint64_t train{};
        std::size_t car{};
        bool front = true;
        float left{};
    };
    std::optional<Repair> repair_;
    // The car menu's "Cargo information" (RTS 0x502680: a message for 30 s).
    std::string cargo_text_;
    float cargo_time_{};
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
