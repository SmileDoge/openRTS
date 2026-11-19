#ifdef OPENRTS_HAS_WGPU
#include "train_view.hpp"
#include "cargo_system.hpp"

#include <openrts/core/profile.hpp>

#include <openrts/renderer/route_space.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <functional>
#include <future>
#include <iomanip>
#include <set>
#include <sstream>
#include <iostream>
#include <tuple>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <fstream>
#include <iterator>
#include <random>

#include <openrts/formats/shape_file.hpp>
#include <openrts/formats/sound.hpp>
#include <openrts/formats/terrain.hpp>
#include <openrts/simulation/distributor.hpp>
#include <openrts/simulation/track_traveller.hpp>
#include <openrts/simulation/train_lights.hpp>
#include <openrts/simulation/track_profile.hpp>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include "key_layout.hpp"
#include "mod_files.hpp"
#include "rts_config.hpp"
#include "ui_fonts.hpp"
#include "ui_text.hpp"
#include "profile_strip.hpp"
#include "viewer_state.hpp"

namespace openrts::viewer {
namespace {

// OPENRTS_SOUND_TRACE set: the player's cab sound logs what its streams do.
bool sound_trace() {
    static const bool on = [] {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t size = 0;
        const bool set = _dupenv_s(&value, &size, "OPENRTS_SOUND_TRACE") == 0 && value != nullptr;
        std::free(value);
        return set;
#else
        return std::getenv("OPENRTS_SOUND_TRACE") != nullptr;
#endif
    }();
    return on;
}

// OPENRTS_DOOR_PICK set: the cab's doors under the mouse, aimed at
// each from the eye, logged closed and again open.
bool door_pick_test() {
    static const bool on = [] {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t size = 0;
        const bool set = _dupenv_s(&value, &size, "OPENRTS_DOOR_PICK") == 0 && value != nullptr;
        std::free(value);
        return set;
#else
        return std::getenv("OPENRTS_DOOR_PICK") != nullptr;
#endif
    }();
    return on;
}

// OPENRTS_PLACEMENT_CHECK set: once the driven train stands, its cars but
// the second picked and saved as a placement, shown where they stand, and
// how far each shown car is from the one it came from, logged.
bool placement_check() {
    static const bool on = [] {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t size = 0;
        const bool set = _dupenv_s(&value, &size, "OPENRTS_PLACEMENT_CHECK") == 0 && value != nullptr;
        std::free(value);
        return set;
#else
        return std::getenv("OPENRTS_PLACEMENT_CHECK") != nullptr;
#endif
    }();
    return on;
}

constexpr float pi = 3.14159265358979f;
// Rail head above the track centre line (the TDB height): the top of the
// standard MSTS rail profile (GLOBAL/SHAPES/A1t10mStrt.s: 0.325 m).
constexpr float rail_top = 0.325f;
// Lift of a car whose shape has no WHEELS nodes: a shape origin at the tread.
constexpr float default_lift = 0.3f;
constexpr std::uint64_t placement_owner = 0;

[[nodiscard]] std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

[[nodiscard]] const char* handle_name(const simulation::DriverHandles::BrakeHandle handle) {
    switch (handle) {
    case simulation::DriverHandles::BrakeHandle::charge:
        return tr("I charge");
    case simulation::DriverHandles::BrakeHandle::running:
        return tr("II running");
    case simulation::DriverHandles::BrakeHandle::lap:
        return tr("III lap");
    case simulation::DriverHandles::BrakeHandle::lap_feed:
        return tr("IV lap+feed");
    case simulation::DriverHandles::BrakeHandle::ep_service:
        return tr("VE EP service");
    case simulation::DriverHandles::BrakeHandle::service:
        return tr("V service");
    case simulation::DriverHandles::BrakeHandle::emergency:
        return tr("VI emergency");
    }
    return "?";
}

[[nodiscard]] float wrap_angle(float angle) {
    while (angle > pi) angle -= 2.0f * pi;
    while (angle < -pi) angle += 2.0f * pi;
    return angle;
}

[[nodiscard]] float mix_angle(const float from, const float to, const float t) {
    return from + wrap_angle(to - from) * t;
}

[[nodiscard]] bool any_handle(const SceneInput& input) {
    return input.throttle_up || input.throttle_down || input.reverser_forward || input.reverser_back || input.brake_more
        || input.brake_less || input.independent_more || input.independent_less || input.toggle_ep;
}

} // namespace

struct TrainView::Car {
    const formats::WagonDefinition* definition = nullptr;
    std::uint32_t key{}; // this car in the game: follows it through couplings (the .con UiD repeats)
    core::VirtualPath shape;
    std::string folder;
    std::string file;    // its .eng / .wag stem in the folder (situations)
    bool engine = false; // an .eng
    bool flipped = false;
    std::future<renderer::StreamedModelAsset> loading;
    bool requested = false;
    bool fitted = false;
    bool script_tried = false;
    float lift = default_lift;
    std::vector<std::int32_t> wheels;
    // The wheels' and bogies' rest matrices in the shape (their chain from
    // the root), to keep them on the rails while the body moves on its
    // springs; a wheel under a bogie follows its bogie (not loose).
    std::vector<glm::mat4> wheel_rest;
    std::vector<bool> wheel_loose;
    // How far each wheel set has spun beyond the run while its axle slipped
    // (боксование, CarState::wheelslip), rad: the k-th "wheels" node is axle
    // k, as RTS numbers them (0x4F0BC8).
    std::vector<float> wheel_spin;
    std::vector<glm::mat4> bogie_rest; // by bogies
    // Pantograph nodes (PANTOGRAPH*<n>): node, pantograph number 1..; the
    // number's z along the shape (to tell the rear one).
    std::vector<std::pair<std::int32_t, std::uint8_t>> pantograph_nodes;
    std::array<float, 4> pantograph_z{};
    std::uint32_t pantograph_frames{};    // frames of the shape's animation
    std::array<float, 4> pantograph_frame{}; // current frame of pantographs 1..4
    std::uint8_t pantograph_raised{};     // bit n-1: pantograph n wanted up
    // The shape's ExteriorDoors: the passenger doors open by the DLL's door
    // states (GM_DOOR_OPEN by GlobalID), the cab doors as the cab's doors.
    struct ExteriorDoorAnim {
        std::int32_t node = -1;
        std::string name;
        float time = 1.0f;
        float open{};
        std::size_t index{};    // in the .sd's ExteriorDoors (the DLL host's door order)
        std::uint32_t group{};  // GlobalID: 1 the right doors, 2 the left ones, 0 none
        float last_frame{};     // the node's own last key frame (the shape's animation is longer)
        std::optional<bool> wanted; // the engineer opened or shut it from outside (no cab door of its name)
    };
    std::vector<ExteriorDoorAnim> exterior_doors;
    // Its shape .sd's ESD_Exterior_Details when the engineer can walk on it
    // or climb it (ExteriorPlanes, ExteriorLadders, ExteriorLedges: RTS mode 6).
    std::shared_ptr<const formats::ExteriorDetails> outside;
    std::vector<std::uint32_t> door_groups; // every ExteriorDoor's GlobalID, in .sd order
    std::vector<simulation::LightRunner> lights; // by LightID: fading and the States shown
    bool cold = false;      // an engine started cold (the consist's Cold; RTS LocoFlags bit 3)
    std::uint8_t pantographs_heard{}; // the raised pantographs the sounds last knew (non-DLL)
    // A remote train's car (Unit::remote): its lights and pantographs as the
    // owner sends them (set_remote_light / set_remote_look).
    std::vector<std::uint8_t> remote_lights;
    // A remote car's Var1..Var8 as its owner sends them (0x463D).
    std::optional<std::array<float, 8>> remote_sound_vars;
    // Its owner's whole sound state (0x463A) until its sound object is made.
    std::vector<std::uint32_t> remote_sound_restore;
    // The driven train's sound for the network (take_driven_sound).
    std::vector<std::uint32_t> net_triggers;
    std::array<float, 8> net_vars{};
    float net_travelled{};
    // Each light's wanted state the last update (update_lights), for the network.
    std::vector<std::uint8_t> lit_now;
    std::uint8_t remote_pantographs{};
    // FreightAnim of its consist entry: objects fastened to the car and the cargo.
    std::vector<formats::ConsistFreight> freight;
    std::vector<core::VirtualPath> freight_shapes; // resolved, empty when missing
    bool freight_requested = false; // its FreightAnim shapes asked for (TrainView::freight_loading_)
    // A scenario's car: the service and train it started in (its ID with its uid).
    std::optional<std::pair<std::uint32_t, std::uint32_t>> activity_origin;
    // The scenario's sounds on it (StaticObjects SOUND): SendSoundTrigger's codes.
    struct ActivitySound {
        std::int32_t object{};
        core::VirtualPath path;
        std::shared_ptr<formats::SoundScript> script;
        std::unique_ptr<audio::SoundInstance> instance;
        std::vector<std::uint32_t> pending;
        bool tried = false;
    };
    std::vector<ActivitySound> activity_sounds;
    std::vector<std::pair<std::int32_t, bool>> bogies; // matrix, is front bogie (in the shape)
    // The wheel chocks drawn on the car's shape (.sd WagonObjects DragA /
    // DragB with FirstStateInvisible: the BASH nodes of the MaDaDev cars):
    // a node (and those under it) and whether it is at the shape's front
    // end (its z), shown only while a chock is at that end.
    std::vector<std::pair<std::int32_t, bool>> chock_nodes;
    std::shared_ptr<const formats::SoundScript> script;
    core::VirtualPath script_path;
    std::unique_ptr<audio::SoundInstance> sound;
    std::optional<formats::SoundOwner> sound_owner; // what `sound` was built for (none: no group for it)
    std::uint32_t uid{};    // UiD of its consist entry
    std::uint32_t number{}; // RTS car number (car +16): train << 18 | record << 10 | UiD, stays with the car
    std::vector<formats::SoundEvent> pending_events;
    std::vector<std::uint32_t> pending_triggers; // discrete SMS triggers a DLL posted
    // The cab sound (RTSCabinSound) of the engine the player drives.
    bool cab_script_tried = false;
    std::shared_ptr<const formats::SoundScript> cab_script;
    core::VirtualPath cab_script_path;
    std::unique_ptr<audio::SoundInstance> cab_sound;
    std::vector<std::uint32_t> pending_cab_triggers;
    // Wheel flats struck this step: the bogie (its own end) and the depth, mm.
    std::vector<std::pair<std::uint8_t, float>> pending_flats;
    bool flat_told = false; // the player was told of its flat
    // The engine's AuxLibrarySound scripts (Brakes.sms, Usavp.sms, ...): a
    // sound object per library, played with that library's own triggers.
    struct AuxSound {
        std::size_t library{}; // in definition->engine->aux_libraries
        std::shared_ptr<const formats::SoundScript> script;
        core::VirtualPath path;
        std::unique_ptr<audio::SoundInstance> instance;
    };
    bool aux_sounds_tried = false;
    std::vector<AuxSound> aux_sounds;
    std::vector<std::vector<std::uint32_t>> pending_aux_triggers; // per library
};

struct TrainView::Unit {
    std::unique_ptr<simulation::Train> train;
    std::vector<Car> cars;
    Role role = Role::standing;
    std::string name;
    bool freight = true; // no passenger cars: freight speed limits
    // The server's category in a network game (0x4607 / 0x4605); without
    // one, from the cars (`freight`).
    std::optional<simulation::TrainCategory> category;
    // Put down from Track control as cold, on or running (TrainView::spawn):
    // its DLLs' start when the player takes it; none: the Train window's.
    std::optional<int> start_state;
    [[nodiscard]] simulation::TrainCategory speed_category() const noexcept {
        return category.value_or(simulation::category_from_cars(freight));
    }
    // Role::ai: RTS's traffic (docs/traffic.md), one body on its path; the
    // scenario's speeds and rates, else default_traffic_settings.
    std::unique_ptr<simulation::TrafficDriver> traffic;
    std::optional<simulation::TrafficSettings> traffic_settings;
    // A scenario's traffic train: removed at its path's end (not NO_REMOVE)
    // or at its remove time, and then only away from the player unless
    // FORCE_REMOVAL; WaitHere's time left (infinity: for good).
    bool scenario_traffic = false;
    bool remove_at_end = true;
    bool force_removal = false;
    std::optional<std::int32_t> remove_time;
    double wait_here = 0.0;
    float wait_left = -1.0f; // s still to wait at a waiting point
    float still = 0.0f;      // s standing still (RTS: a LOOSE train becomes STATIC after 30 s)
    std::vector<simulation::CarPlacement> previous, current;
    std::vector<double> previous_travelled, current_travelled;
    // Each car's bogies' wheels turned (CarState::rolled: they stand in a
    // slide), at the same two steps.
    std::vector<std::array<double, 2>> previous_rolled, current_rolled;
    void take_rolled() {
        current_rolled.resize(train->cars().size());
        for (std::size_t i = 0; i < current_rolled.size(); ++i) current_rolled[i] = train->cars()[i].rolled;
    }
    // The bodies on their springs (CarState::body) at the last two steps:
    // drawn between them like the placements, not stepping at 20 Hz.
    std::vector<BodyOffset> previous_body, current_body;
    // Its cars are in the DLL host (the driven train, or a part cut off from
    // it): host_train is its train there (0: the driven one).
    bool host = false;
    std::size_t host_train = SIZE_MAX;
    // Driven by another client: moved by the network, no physics (add_remote).
    bool remote = false;
    // A scenario's train: its service and number in it (start_activity).
    std::optional<std::pair<std::uint32_t, std::uint32_t>> activity_id;

    // Both interpolation ends at the train's present state.
    void settle() {
        previous = current = train->placements();
        current_travelled.resize(train->cars().size());
        for (std::size_t i = 0; i < current_travelled.size(); ++i) current_travelled[i] = train->cars()[i].travelled;
        previous_travelled = current_travelled;
        take_rolled();
        previous_rolled = current_rolled;
        take_body();
        previous_body = current_body;
    }
    void take_body() {
        current_body.resize(train->cars().size());
        for (std::size_t i = 0; i < current_body.size(); ++i) {
            const auto& car = train->cars()[i];
            current_body[i] = {glm::vec3{car.body[0], car.body[1], car.body[2]},
                               glm::vec3{car.body_turn[0], car.body_turn[1], car.body_turn[2]}};
        }
    }
};

TrainView::TrainView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const formats::RoutePosition origin,
                     simulation::TrackNetwork& network)
    : vfs_(std::move(vfs)), route_(std::move(route)), origin_(origin), network_(&network) {
    paths_ = formats::load_train_paths(*vfs_, route_);
    std::erase_if(paths_, [](const formats::TrainPath& path) { return !path.ok(); });
    load_user_paths();
    dispatcher_ = std::make_unique<simulation::Dispatcher>(network);
    const auto definition = formats::load_route_definition(*vfs_, route_);
    limits_ = std::make_unique<simulation::SpeedLimits>(network, definition.ok() ? definition.speed_limit : 0.0f);
    limits_without_tolerance_ = remembered("limits_without_tolerance").value_or("0") == "1";
    limits_->set_without_tolerance(limits_without_tolerance_);
    route_tolerance_ = definition.ok() ? definition.timetable_tolerance : 0.0f;
    electrification_ = simulation::Electrification::load(*vfs_, route_, network);
    if (!electrification_->data().error.empty()) std::cout << "electrification: " << electrification_->data().error << '\n';
    cargo_ = std::make_unique<CargoSystem>(vfs_, route_, origin_);
}

std::filesystem::path TrainView::user_path_folder() const {
    return user_data_directory() / "user_paths" / std::string(route_.filename());
}

void TrainView::load_user_paths() {
    std::error_code error;
    const auto folder = user_path_folder();
    if (!std::filesystem::is_directory(folder, error)) return;
    std::vector<formats::TrainPath> saved;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() != ".pat") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        auto path = formats::parse_train_path(bytes);
        if (!path.ok()) continue;
        const auto stem = entry.path().stem().u8string();
        path.file = "user/" + std::string(stem.begin(), stem.end());
        saved.push_back(std::move(path));
    }
    std::sort(saved.begin(), saved.end(), [](const auto& a, const auto& b) { return a.file < b.file; });
    std::erase_if(paths_, [](const formats::TrainPath& path) { return path.file.starts_with("user/"); });
    for (auto& path : saved) paths_.push_back(std::move(path));
}

TrainView::~TrainView() = default;

bool TrainView::loaded() const noexcept {
    return !units_.empty() && units_.front().train != nullptr;
}

bool TrainView::placed() const noexcept {
    return loaded() && units_.front().train->placed();
}

bool TrainView::activity_in_cab(const ActivityId train) const {
    return !units_.empty() && units_.front().role == Role::driven && units_.front().activity_id == train && engineer_.inside();
}

bool TrainView::activity_near(const ActivityId train, const formats::RoutePosition& at, const double radius,
                              const std::string_view reach) const {
    const auto unit = std::find_if(units_.begin(), units_.end(), [&](const Unit& u) { return u.activity_id == train; });
    if (unit == units_.end() || !unit->train) return false;
    // A car is in the radius when any of it is, coupler to coupler: the
    // scenarios put the point at a signal's place on the track (Pavlovsk's
    // exam: M20 at Radius 1), and a cab past it by its overhang has passed
    // it. Its bogies alone (placements) leave ~3 m at each end out.
    const auto ends = unit->train->car_ends();
    if (ends.empty()) return false;
    const glm::vec3 p = local(at);
    const glm::vec2 q{p.x, p.z};
    const auto within = [&](const simulation::CarPlacement& car) {
        // The car's line from its front to its rear, on the ground.
        const glm::vec3 a3 = local(car.front), b3 = local(car.rear);
        const glm::vec2 a{a3.x, a3.z}, b{b3.x, b3.z};
        const glm::vec2 ab = b - a;
        const float span = glm::dot(ab, ab);
        const float t = span > 1e-6f ? std::clamp(glm::dot(q - a, ab) / span, 0.0f, 1.0f) : 0.0f;
        return glm::length(q - (a + ab * t)) <= static_cast<float>(radius);
    };
    const auto mode = lower(std::string(reach));
    const auto& cars = ends;
    if (mode == "wagon_all") return std::all_of(cars.begin(), cars.end(), within);
    if (mode == "wagon_front") return within(cars.front());
    if (mode == "wagon_back") return within(cars.back());
    if (mode == "wagon_player" || mode == "player") {
        const std::size_t i = &*unit == &units_.front() && cab_car_ < cars.size() ? cab_car_ : 0;
        return within(cars[i]);
    }
    return std::any_of(cars.begin(), cars.end(), within);
}

std::optional<double> TrainView::activity_car_value(const ActivityId train, const std::int32_t number, const bool by_id,
                                                    const std::string_view area, const std::string_view name, const int idx,
                                                    const int sub) const {
    const Unit* unit = nullptr;
    std::size_t index = 0;
    if (by_id) {
        for (const auto& u : units_)
            for (std::size_t i = 0; i < u.cars.size() && !unit; ++i)
                if (u.cars[i].activity_origin == train && u.cars[i].uid == static_cast<std::uint32_t>(number)) {
                    unit = &u;
                    index = i;
                }
    } else {
        const auto found = std::find_if(units_.begin(), units_.end(), [&](const Unit& u) { return u.activity_id == train; });
        if (found != units_.end()) {
            unit = &*found;
            const auto count = static_cast<std::int32_t>(found->cars.size());
            const auto at = number >= 0 ? number : count + number;
            if (at < 0 || at >= count) return std::nullopt;
            index = static_cast<std::size_t>(at);
        }
    }
    if (!unit || index >= unit->train->cars().size() || index >= unit->cars.size()) return std::nullopt;
    const auto& state = unit->train->cars()[index];
    const auto key = lower(std::string(name));
    const auto signed_along = [&](const double value) { return idx == 0 ? std::abs(value) : idx == 1 ? value : -value; };
    if (key == "brakecylinder") return state.cylinder;
    if (key == "auxiliary") return state.auxiliary;
    if (key == "pipepressure") return sub == 1 || idx == 1 ? state.charging_pipe : state.pipe;
    if (key == "mainreservoir") return state.main_reservoir;
    if (key == "independentbrake") return state.independent;
    if (key == "handbrake") return state.handbrake * 100.0;
    if (key == "drag") {
        // The brake shoes (chocks): Idx 0 the bit mask (1 under the front
        // bogie, 2 the rear: the train's way, as PlaceDrag and the car menu
        // put them), 1 the front one, 2 the rear one.
        const auto mask = static_cast<std::uint32_t>(state.chocks & (simulation::chock_front | simulation::chock_rear));
        return idx == 1 ? static_cast<double>(mask & 1u) : idx == 2 ? static_cast<double>((mask >> 1) & 1u) : static_cast<double>(mask);
    }
    if (key == "velocity") return signed_along(state.velocity);
    if (key == "acceleration") return signed_along(state.acceleration);
    const auto where = lower(std::string(area));
    if (key == "smsstream") {
        const auto& car = unit->cars[index];
        const auto* sound = where == "cabin" ? car.cab_sound.get() : car.sound.get();
        return sound && sub >= 0 ? static_cast<double>(sound->stream_sounding(static_cast<std::size_t>(sub))) : 0.0;
    }
    const auto u = static_cast<std::size_t>(unit - units_.data());
    const auto* out = dll_car(u, index) ? dll_output(u, index) : nullptr;
    if (key == "panto") {
        // WAGON: the raised pantographs; CABIN: the ones the cab asks up
        // (the DLL's Engine Panto). Idx 0 the bit mask, 1..8 one of them.
        const std::uint32_t mask = where == "cabin" && out ? out->engine.panto : unit->cars[index].pantograph_raised;
        if (idx <= 0) return static_cast<double>(mask);
        return idx <= 8 ? static_cast<double>((mask >> (idx - 1)) & 1u) : 0.0;
    }
    if (key == "locostate") {
        // The locomotive's Engine, as its DLL set it: 0 the reverser,
        // 1 Power, 2 UR, 4 ALSNOn, 5 Wheelslip, 6 ThrottlePosition,
        // 7 MainSwitch (3, BatteryCharge, is not passed on by the host).
        if (!out || !out->has_engine) return std::nullopt;
        const auto& e = out->engine;
        switch (idx) {
        case 0: return static_cast<double>(e.reverse);
        case 1: return static_cast<double>(e.power);
        case 2: return static_cast<double>(e.equalising);
        case 4: return static_cast<double>(e.alsn_on);
        case 5: return static_cast<double>(e.wheelslip);
        case 6: return static_cast<double>(e.throttle);
        case 7: return static_cast<double>(e.main_switch);
        default: return std::nullopt;
        }
    }
    if (key == "switch") {
        // CABIN Switch Idx: SubIdx -1 its State, 0 its SetState, 1 its frame.
        if (where != "cabin" || !out || idx < 0 || static_cast<std::size_t>(idx) >= out->switches.size()) return std::nullopt;
        const auto& s = out->switches[static_cast<std::size_t>(idx)];
        return sub == 0 ? static_cast<double>(s.set_state) : sub == 1 ? static_cast<double>(s.current_frame) : static_cast<double>(s.state);
    }
    if (key == "playerinarea" || key == "playerinchair") {
        // activity_WagonStateVariables: -5 the player not in this car, -3 in
        // it but not in the object named (CABIN an engine's cab, SALON /
        // INTERIOR a passenger salon, WAGON its outside). PlayerInArea's
        // types: 2 the sound regions (InteriorArea), the others (-2
        // ExteriorPlane, 1 doors, 9, 10 Ledge) not told apart here; -2 in
        // another type, -1 in none. PlayerInChair's: 1 Chair (a cab's),
        // 2 Seat (a salon's); -4 not seated, -2 in the other type. Idx 0
        // (any type): type * 1000 + its index. The value: the index of the
        // area or seat of that type he is in; with a SubIdx, whether it is
        // that one (0 / 1).
        const bool here = !units_.empty() && unit == &units_.front() && index == engineer_car_
                          && (engineer_.inside() || engineer_.mode() == Engineer::Mode::on_car);
        if (!here) return -5.0;
        const auto* room = engineer_cab();
        const bool outside = engineer_.mode() == Engineer::Mode::on_car;
        const bool salon = room && room->salon();
        const bool object = where == "wagon" ? outside : !outside && room && (where == "cabin" ? !salon : salon);
        if (!object) return -3.0;
        const auto answer = [&](const int type, const std::optional<std::size_t> at, const double other) -> double {
            if (!at) return other;
            const double value = idx == 0 ? type * 1000.0 + static_cast<double>(*at) : static_cast<double>(*at);
            if (idx != 0 && idx != type) return -2.0;
            return sub >= 0 ? (value == static_cast<double>(sub) ? 1.0 : 0.0) : value;
        };
        if (key == "playerinchair") {
            if (outside || engineer_.mode() != Engineer::Mode::seated || engineer_.risen()) return -4.0;
            if (const auto seat = engineer_.seat()) return answer(2, seat, -4.0);
            return answer(1, engineer_.chair(), -4.0);
        }
        if (outside) return -1.0;
        // Seated: where the chair or seat puts him; on foot: his feet.
        glm::vec3 at = engineer_.feet();
        const auto& description = room->description();
        if (engineer_.mode() == Engineer::Mode::seated) {
            if (const auto seat = engineer_.seat(); seat && *seat < description.seats.size()) {
                const auto& p = description.seats[*seat].seated;
                at = {p[0], p[1], p[2]};
            } else if (engineer_.chair() < description.chairs.size()) {
                const auto& p = description.chairs[engineer_.chair()].seated;
                at = {p[0], p[1], p[2]};
            }
        }
        std::optional<std::size_t> in;
        for (std::size_t k = 0; k < description.areas.size() && !in; ++k) {
            const auto& a = description.areas[k];
            if (at.x >= (std::min)(a.x1, a.x2) && at.x <= (std::max)(a.x1, a.x2) && at.z >= (std::min)(a.z1, a.z2)
                && at.z <= (std::max)(a.z1, a.z2))
                in = k;
        }
        return answer(2, in, -1.0);
    }
    return std::nullopt;
}

std::size_t TrainView::activity_car_count(const ActivityId train) const {
    const auto unit = std::find_if(units_.begin(), units_.end(), [&](const Unit& u) { return u.activity_id == train; });
    return unit == units_.end() || !unit->train ? 0 : unit->train->cars().size();
}

void TrainView::activity_service_startup(const std::uint32_t service, const bool starts) {
    if (starts) {
        services_held_.erase(service);
    } else {
        services_held_.insert(service);
    }
}

void TrainView::activity_speed_limit(const ActivityId train, const float limit) {
    if (limit > 0.0f) {
        activity_speed_limits_[train] = limit;
    } else {
        activity_speed_limits_.erase(train);
    }
    std::cout << "activity: train " << train.first << ' ' << train.second << " speed limit "
              << (limit > 0.0f ? std::to_string(static_cast<int>(std::lround(limit * 3.6f))) + " km/h" : std::string("off")) << std::endl;
    // Traffic keeps under it; off, its own maximum again.
    for (auto& unit : units_) {
        if (unit.activity_id != train || !unit.traffic) continue;
        auto& settings = unit.traffic->settings();
        const auto* service = activity_ ? activity_->service(train.first) : nullptr;
        const auto* own = service ? service->train(train.second) : nullptr;
        const float written = own ? own->max_speed.value_or(0.0f) : settings.max_speed;
        settings.max_speed = limit > 0.0f ? (written > 0.0f ? (std::min)(written, limit) : limit) : written;
        unit.traffic_settings = settings;
    }
}

simulation::SpeedLimit TrainView::limit_of(const Unit& unit) const {
    auto limit = limits_->evaluate(*unit.train, unit.speed_category());
    if (unit.activity_id) {
        if (const auto own = activity_speed_limits_.find(*unit.activity_id); own != activity_speed_limits_.end()) {
            if (limit.current < 0.0f || own->second < limit.current) limit.current = own->second;
            if (limit.ahead >= 0.0f && own->second < limit.ahead) limit.ahead = -1.0f; // no lower post ahead under it
        }
    }
    return limit;
}

std::optional<std::size_t> TrainView::activity_reverse_points(const ActivityId train) const {
    const auto unit = std::find_if(units_.begin(), units_.end(), [&](const Unit& u) { return u.activity_id == train; });
    if (unit == units_.end() || !unit->train) return std::nullopt;
    const auto* progress = dispatcher_->progress(unit->train->id());
    if (!progress) return std::nullopt;
    return progress->leg;
}

void TrainView::activity_sound_trigger(const std::int32_t object, const std::uint32_t code, const ActivityId train, const std::int32_t number) {
    for (auto& unit : units_) {
        if (object < 0) {
            if (unit.activity_id != train || unit.cars.empty()) continue;
            const auto count = static_cast<std::int32_t>(unit.cars.size());
            const auto at = std::clamp(number >= 0 ? number : count + number, 0, count - 1);
            unit.cars[static_cast<std::size_t>(at)].pending_triggers.push_back(code);
            unit.cars[static_cast<std::size_t>(at)].pending_cab_triggers.push_back(code);
            return;
        }
        for (auto& car : unit.cars)
            for (auto& sound : car.activity_sounds)
                if (sound.object == object) {
                    sound.pending.push_back(code);
                    return;
                }
    }
}

void TrainView::activity_modify_wagons(const ActivityId train, const formats::ActivityWagonGroup& group) {
    const auto unit = std::find_if(units_.begin(), units_.end(), [&](const Unit& u) { return u.activity_id == train; });
    if (unit != units_.end()) apply_wagon_group(*unit, group);
}

void TrainView::apply_wagon_group(Unit& unit, const formats::ActivityWagonGroup& group) {
    auto& train = *unit.train;
    const auto count = static_cast<std::int32_t>(train.cars().size());
    if (count == 0) return;
    const auto a = static_cast<std::size_t>(std::clamp(group.first, 0, count - 1));
    const auto b = static_cast<std::size_t>(group.last < 0 || group.last >= count ? count - 1 : group.last);
    using Flags = simulation::Train::CarFlags;
    // Only what is set: a flag touched wakes the train.
    const auto flags = [&](const std::size_t car, const Flags which, const std::uint8_t mask, const bool on) {
        if (mask != 0) train.set_car_flags(car, which, mask, on);
    };
    for (std::size_t i = a; i <= b; ++i) {
        if (group.handbrake) train.set_handbrake(i, *group.handbrake);
        if (group.cold && i < unit.cars.size() && unit.cars[i].engine) unit.cars[i].cold = *group.cold;
    }
    // The ends: pipe_ends' *_front bits (0x55) at the first car, *_rear
    // (0xAA) at the last (RTS: car flags & 0x55555500 / 0xAAAAAA00).
    flags(a, Flags::pipe_ends, group.ends_set & 0x55u, true);
    flags(a, Flags::pipe_ends, group.ends_clear & 0x55u, false);
    flags(b, Flags::pipe_ends, group.ends_set & 0xAAu, true);
    flags(b, Flags::pipe_ends, group.ends_clear & 0xAAu, false);
    flags(a, Flags::chocks, group.place_drag & simulation::chock_front, true);
    flags(b, Flags::chocks, group.place_drag & simulation::chock_rear, true);
}

void TrainView::retag_activity(const ActivityId from, const ActivityId to) {
    // Its SetSpeedLimit and SetDefaultUncouple go with it.
    if (const auto limit = activity_speed_limits_.find(from); limit != activity_speed_limits_.end() && from != to) {
        activity_speed_limits_[to] = limit->second;
        activity_speed_limits_.erase(from);
    }
    if (const auto rule = uncouple_rules_.find(from); rule != uncouple_rules_.end() && from != to) {
        uncouple_rules_[to] = rule->second;
        uncouple_rules_.erase(from);
    }
    for (auto& unit : units_) {
        if (unit.activity_id != from) continue;
        unit.activity_id = to;
        // Another service: its path (PROBABLE: AssignService with an empty
        // NewPathLoad takes the new service's own path).
        const auto* service = activity_ && to.first != from.first ? activity_->service(to.first) : nullptr;
        if (service && !service->path.empty() && unit.role != Role::standing) {
            const auto error = give_activity_path(unit, *service);
            std::cout << "activity: train " << to.first << ' ' << to.second << " on the path of " << service->name
                      << (error.empty() ? std::string() : ": " + error) << std::endl;
        }
    }
}

float TrainView::distance_to(const Unit& unit, const glm::vec3& point) const {
    if (unit.current.empty()) return 0.0f;
    float nearest = std::numeric_limits<float>::max();
    const auto consider = [&](const simulation::TrackPoint& p) {
        const glm::vec3 at = local(p);
        nearest = (std::min)(nearest, glm::length(glm::vec2{at.x - point.x, at.z - point.z}));
    };
    for (std::size_t i = 0; i < unit.current.size(); i += 4) consider(unit.current[i].front);
    consider(unit.current.back().rear);
    return nearest;
}

bool TrainView::train_near(const Unit& unit, const float range) const {
    if (units_.empty() || &unit == &units_.front()) return true;
    if (camera_eye_ && distance_to(unit, *camera_eye_) <= range) return true;
    return distance_to_player(unit) <= range;
}

void TrainView::let_go_far_shapes(renderer::ModelLibrary& library) {
    const auto now = std::chrono::steady_clock::now();
    if (now - shapes_checked_ < std::chrono::seconds(1)) return;
    shapes_checked_ = now;
    std::unordered_set<std::string> needed;
    for (const auto& unit : units_) {
        if (!train_near(unit, train_keep_range)) continue;
        for (const auto& car : unit.cars) {
            needed.insert(car.shape.key());
            for (const auto& path : car.freight_shapes) needed.insert(path.key());
        }
    }
    std::size_t dropped = 0;
    for (auto it = train_shapes_.begin(); it != train_shapes_.end();) {
        // Only a loaded one: one on its way is looked at again once it is in.
        if (needed.contains(it->first) || !library.find(it->second)) {
            ++it;
            continue;
        }
        // A shape the route places too (a static car) stays: the route lets
        // it go with its tiles.
        if (!shape_keeper_ || !shape_keeper_(it->first)) library.remove(it->second);
        shapes_asked_.erase(it->first);
        for (auto& unit : units_) {
            for (auto& car : unit.cars) {
                if (car.shape.key() == it->first) car.requested = false;
                for (const auto& path : car.freight_shapes)
                    if (path.key() == it->first) car.freight_requested = false;
            }
        }
        it = train_shapes_.erase(it);
        ++dropped;
    }
    if (dropped > 0) std::cout << "trains: " << dropped << " shapes of trains farther than " << train_keep_range << " m let go" << std::endl;
}

std::unordered_set<std::string> TrainView::shapes_in_use() const {
    std::unordered_set<std::string> used;
    if (cab_) used.insert(cab_->shape().key());
    if (visit_cab_) used.insert(visit_cab_->shape().key());
    for (const auto& unit : units_) {
        if (!train_near(unit, train_keep_range)) continue;
        for (const auto& car : unit.cars) {
            if (!car.shape.empty()) used.insert(car.shape.key());
            for (const auto& path : car.freight_shapes) used.insert(path.key());
        }
    }
    for (const auto& [_, cab] : board_cabs_)
        if (cab) used.insert(cab->shape().key());
    return used;
}

float TrainView::distance_to_player(const Unit& unit) const {
    const auto& player = units_.front();
    if (&unit == &player || unit.current.empty() || player.current.empty()) return 0.0f;
    const glm::vec3 centre = local(player.current.front().front);
    float nearest = std::numeric_limits<float>::max();
    for (const auto& end : {unit.current.front().front, unit.current.back().rear}) {
        const glm::vec3 at = local(end);
        nearest = (std::min)(nearest, glm::length(glm::vec2{at.x - centre.x, at.z - centre.z}));
    }
    return nearest;
}

TrainView::Loading TrainView::loading() const {
    Loading result;
    result.cab = starts_in_cab_;
    if (!pending_load_.empty() || !pending_situation_.empty() || !pending_trains_.empty() || pending_activity_) {
        result.settled = false;
        return result;
    }
    if (!loaded()) return result;
    if (pending_path_ || (placed() && start_view_ != StartView::none && units_.front().role == Role::driven)) result.settled = false;
    // The trains within 2 km of the player's (a scenario's far stations load
    // behind the game, out of sight).
    for (const auto& unit : units_) {
        if (distance_to_player(unit) >= 2000.0f) continue;
        for (const auto& car : unit.cars) {
            if (car.loading.valid()) ++result.pending;
        }
    }
    result.pending += freight_loading_.size();
    if (starts_in_cab_ && cab_ && !cab_->loaded()) ++result.pending;
    return result;
}

const simulation::Train* TrainView::train() const noexcept {
    return loaded() ? units_.front().train.get() : nullptr;
}

bool TrainView::is_player(const std::uint64_t id) const noexcept {
    return loaded() && units_.front().role == Role::driven && units_.front().train->id() == id;
}

simulation::Train& TrainView::driven() {
    return *units_.front().train;
}

const simulation::Train& TrainView::driven() const {
    return *units_.front().train;
}

glm::vec3 TrainView::local(const simulation::TrackPoint& point) const {
    return {
        static_cast<float>((point.tile_x - origin_.tile_x) * simulation::tile_size + point.x - origin_.x),
        static_cast<float>(point.y - origin_.y),
        static_cast<float>((point.tile_z - origin_.tile_z) * simulation::tile_size + point.z - origin_.z),
    };
}

glm::vec3 TrainView::local(const formats::RoutePosition& position) const {
    return local(simulation::TrackPoint{
        .tile_x = position.tile_x, .tile_z = position.tile_z, .x = position.x, .y = position.y, .z = position.z});
}

std::string TrainView::make_unit(const std::string_view consist_name, Unit& unit,
                                 std::vector<std::unique_ptr<formats::WagonDefinition>>& stock) const {
    // A .con file anywhere on disk (Track control's "Browse..."), else RTS's
    // lookup (CONSISTS_RTS, then CONSISTS) or a virtual path. The cars come
    // from the content's TRAINSET either way.
    formats::ConsistDefinition consist;
    const std::filesystem::path file(std::u8string(consist_name.begin(), consist_name.end()));
    std::error_code file_error;
    if (file.is_absolute() && std::filesystem::is_regular_file(file, file_error)) {
        std::ifstream input(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        consist = formats::parse_consist(bytes);
        if (consist.name.empty()) {
            const auto stem = file.stem().u8string();
            consist.name = std::string(stem.begin(), stem.end());
        }
    } else if (auto own = activity_consist(consist_name)) {
        consist = std::move(*own);
    } else {
        consist = formats::load_consist(*vfs_, consist_name);
    }
    if (!consist.ok()) return "consist '" + std::string(consist_name) + "': " + consist.error;
    return make_unit_from(consist.name, consist.entries, unit, stock);
}

namespace {
std::atomic<std::uint32_t> last_car_key{0};
} // namespace

std::uint32_t TrainView::new_car_key() {
    return ++last_car_key;
}

void TrainView::reserve_car_keys(const std::uint32_t up_to) {
    auto now = last_car_key.load();
    while (now < up_to && !last_car_key.compare_exchange_weak(now, up_to)) {
    }
}

std::string TrainView::make_unit_from(std::string name, const std::vector<formats::ConsistEntry>& entries, Unit& unit,
                                      std::vector<std::unique_ptr<formats::WagonDefinition>>& stock) const {
    return make_unit_from(std::move(name), entries, unit, stock, {});
}

std::string TrainView::make_unit_from(std::string name, const std::vector<formats::ConsistEntry>& entries, Unit& unit,
                                      std::vector<std::unique_ptr<formats::WagonDefinition>>& stock,
                                      const std::vector<std::uint32_t>& keys) const {
    std::vector<Car> cars;
    std::vector<simulation::CarSetup> setups;
    bool passenger = false;
    for (const auto& entry : entries) {
        auto wagon = std::make_unique<formats::WagonDefinition>(
            formats::load_wagon(*vfs_, entry.folder, entry.name, entry.engine, stock_rules()));
        if (!wagon->ok()) return entry.folder + "/" + entry.name + ": " + wagon->error;
        passenger = passenger || wagon->kind == formats::WagonKind::passenger;
        // The cargo's mass rides with the car (RTS counts it in the car's mass).
        for (const auto& f : entry.freight) wagon->mass += (std::max)(f.mass, 0.0f);
        Car car;
        car.definition = wagon.get();
        const auto index = cars.size();
        car.key = index < keys.size() && keys[index] != 0 ? keys[index] : new_car_key();
        car.uid = entry.uid;
        car.folder = entry.folder;
        car.file = entry.name;
        car.engine = entry.engine;
        car.flipped = entry.flipped;
        car.cold = entry.cold;
        car.freight = entry.freight;
        for (const auto& f : entry.freight) {
            // FreightAnimShape: in the car's folder, else TRAINS\OBJECTS, else GLOBAL\SHAPES.
            core::VirtualPath found;
            for (const std::string base : {"TRAINS/TRAINSET/" + entry.folder + "/", std::string("TRAINS/OBJECTS/"), std::string("GLOBAL/SHAPES/")}) {
                if (f.shape.empty()) break;
                if (auto path = core::VirtualPath::parse(base + f.shape); path && vfs_->exists(*path)) {
                    found = *path;
                    break;
                }
            }
            car.freight_shapes.push_back(found);
        }
        car.shape = core::VirtualPath::parse("TRAINS/TRAINSET/" + entry.folder + "/" + wagon->shape).value_or(core::VirtualPath{});
        setups.push_back({.definition = wagon.get(),
                          .flipped = entry.flipped,
                          .handbrake = entry.handbrake ? *entry.handbrake / 100.0f : 0.0f,
                          .shoes = entry.shoes});
        stock.push_back(std::move(wagon));
        cars.push_back(std::move(car));
    }
    unit.name = std::move(name);
    unit.freight = !passenger;
    unit.cars = std::move(cars);
    unit.train = std::make_unique<simulation::Train>(std::move(setups));
    unit.train->set_cant_reference(limits_->route_limit());
    // Its ЭПТ's own pace (a new train, a new one).
    unit.train->scatter_ep_fill(std::random_device{}());
    // The consist's PlaceDrag (RTS 0x477928): the cars' chocks from the start.
    for (std::size_t i = 0; i < entries.size() && i < unit.cars.size(); ++i)
        if (entries[i].place_drag != 0) unit.train->set_car_flags(i, simulation::Train::CarFlags::chocks, entries[i].place_drag, true);
    unit.train->set_coupler_substeps(static_cast<std::uint32_t>(substeps_));
    if (!distributor_stability_loaded_) {
        distributor_stability_loaded_ = true;
        distributor_stability_ = remembered("distributor_stability").value_or("0") == "1";
        ptr_resistance_ = remembered("ptr_resistance").value_or("0") == "1";
        axle_dynamics_ = remembered("axle_dynamics").value_or("0") == "1";
    }
    unit.train->set_resistance_model(ptr_resistance_ ? simulation::ResistanceModel::ptr : simulation::ResistanceModel::msts);
    unit.train->set_axle_dynamics(axle_dynamics_);
    unit.train->set_distributor_stability(distributor_stability_);
    return {};
}

std::string TrainView::load(const std::string_view consist_name) {
    Unit unit;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    if (auto error = make_unit(consist_name, unit, stock); !error.empty()) return error;
    for (const auto& old : units_) dispatcher_->clear(old.train->id());
    consist_ = unit.name;
    units_.clear();
    stock_ = std::move(stock);
    number_cars(unit);
    unit.role = Role::driven;
    driver_ = simulation::attach_keyboard_driver(*unit.train);
    units_.push_back(std::move(unit));
    fresh_train_ = true;
    attach_legacy();
    follow_id_ = 0;
    joints_ = junctions_ = coupler_breaks_ = derailments_ = 0;
    end_of_track_ = false;
    return {};
}

std::string TrainView::place(const std::uint32_t node, const double offset, const bool forward) {
    if (!loaded()) return "no consist loaded";
    std::string error;
    if (!driven().place(*network_, node, offset, forward, &error)) return error;
    units_.front().settle();
    accumulator_ = 0.0;
    return {};
}

const formats::TrainPath* TrainView::find_path(const std::string_view name) const {
    if (paths_.empty()) return nullptr;
    if (name.empty()) return &paths_.front();
    const auto wanted = lower(std::string(name));
    for (const auto& path : paths_) {
        if (lower(path.file) == wanted || lower(path.id) == wanted || lower(path.name) == wanted) return &path;
    }
    return nullptr;
}

std::optional<simulation::TrackLocation> TrainView::rear_towards(const std::uint32_t node, const double offset,
                                                                 const glm::vec3& direction) const {
    simulation::TrackTraveller rear(*network_, node, offset, true);
    auto ahead = rear;
    const auto here = local(rear.pose());
    ahead.move(1.0);
    const bool forward = glm::dot(local(ahead.pose()) - here, direction) >= 0.0f;
    return simulation::TrackLocation{.node = node, .offset = offset, .forward = forward};
}

std::optional<simulation::TrackLocation> TrainView::path_rear(const formats::TrainPath& path) const {
    if (path.nodes.empty() || path.nodes.front().point >= path.points.size()) return std::nullopt;
    const auto& first = path.nodes.front();
    const auto start = path.points[first.point].position;
    const auto location = network_->locate(
        simulation::TrackPoint{.tile_x = start.tile_x, .tile_z = start.tile_z, .x = start.x, .y = start.y, .z = start.z},
        20.0);
    if (!location) return std::nullopt;
    // The first leg: towards the next node's point.
    glm::vec3 direction{0.0f};
    if (first.next != formats::PathNode::invalid && first.next < path.nodes.size()) {
        direction = local(path.points[path.nodes[first.next].point].position) - local(start);
    }
    return rear_towards(location->node, location->offset, direction);
}

std::optional<simulation::TrackLocation> TrainView::placement_rear(const formats::RoutePosition& start,
                                                                   const formats::RoutePosition& heading) const {
    const auto location = network_->locate(
        simulation::TrackPoint{.tile_x = start.tile_x, .tile_z = start.tile_z, .x = start.x, .y = start.y, .z = start.z},
        20.0);
    if (!location) return std::nullopt;
    return rear_towards(location->node, location->offset, local(heading) - local(start));
}

std::string TrainView::place_on_path(const formats::TrainPath& path) {
    if (!loaded()) return "no consist loaded";
    const auto start = path_rear(path);
    if (!start) return "no track at the start of path '" + path.file + "'";
    const simulation::TrackTraveller rear(*network_, start->node, start->offset, start->forward);
    // The front is a train length further on: the rear stands at the path
    // start (MSTS explore mode; RTS UNKNOWN).
    auto front = rear;
    front.move(driven().length(), simulation::JunctionMode::plain, placement_owner);
    auto error = place(front.node(), front.offset(), front.forward());
    if (!error.empty()) return error;
    message_ = path.name.empty() ? trf("on path %s", path.file.c_str()) : trf("on path %s (%s)", path.file.c_str(), path.name.c_str());
    return {};
}

std::optional<simulation::TrackTraveller> TrainView::ahead() const {
    if (!placed()) return std::nullopt;
    const double speed = driven().speed();
    const bool backward = speed < -0.1 || (std::abs(speed) <= 0.1 && driver_ && driver_->reverser < 0);
    if (!backward) return driven().front_end();
    const auto rear = driven().rear_end();
    if (!rear) return std::nullopt;
    return simulation::TrackTraveller(*network_, rear->node(), rear->offset(), !rear->forward());
}

glm::vec3 TrainView::head_position() const {
    if (!loaded() || units_.front().current.empty()) return {};
    return pose(units_.front(), 0).front;
}

std::vector<const simulation::Train*> TrainView::trains() const {
    std::vector<const simulation::Train*> result;
    for (const auto& unit : units_) {
        if (unit.train && unit.train->placed()) result.push_back(unit.train.get());
    }
    return result;
}

void TrainView::select_path(const formats::TrainPath& path) {
    path_choice_ = static_cast<int>(&path - paths_.data());
    pending_path_ = true;
}

void TrainView::request_models(renderer::ModelLibrary& library) {
    const auto load_cab = [&](CabView* cab, bool& requested, std::future<renderer::StreamedModelAsset>& loading) {
        if (!cab) return;
        if (!requested && !library.find(cab->shape())) {
            requested = true;
            loading = std::async(std::launch::async, [vfs = vfs_, path = cab->shape()] {
                return renderer::load_model_asset(*vfs, path, {});
            });
        }
        if (loading.valid() && loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto asset = loading.get();
            if (asset.ok()) {
                library.add(std::move(asset));
            } else {
                std::cout << "cab shape '" << cab->shape().str() << "': " << asset.error << '\n';
            }
        }
        if (!cab->bound()) {
            if (auto* model = library.find(cab->shape()); model && model->rig()) cab->bind(*model->rig());
        }
    };
    load_cab(cab_.get(), cab_requested_, cab_loading_);
    load_cab(visit_cab_.get(), visit_requested_, visit_loading_);
    // The next cars' rooms he may see into (update_neighbour_rooms).
    for (const auto key : neighbour_room_keys_) {
        const auto at = find_car(key);
        if (!at || at->first != 0) continue;
        if (auto* room = car_cab(0, at->second, false); room && room != cab_.get() && room != visit_cab_.get())
            load_cab(room, room_loads_[key].requested, room_loads_[key].loading);
    }
    std::vector<Car*> all;
    for (auto& unit : units_) {
        for (auto& car : unit.cars) all.push_back(&car);
    }
    // A few loads at once, the nearest trains first: a scenario's fifty
    // trains asking for their 150 shapes at once kept the world around the
    // player from loading.
    constexpr std::size_t loads_at_once = 6;
    std::size_t on_the_way = freight_loading_.size();
    for (const auto* car : all) on_the_way += car->loading.valid() ? 1 : 0;
    std::vector<std::pair<float, Unit*>> nearest;
    for (auto& unit : units_) {
        // Far trains are not loaded (and let go of what they had).
        if (!train_near(unit, train_load_range)) continue;
        nearest.emplace_back(camera_eye_ ? (std::min)(distance_to(unit, *camera_eye_), distance_to_player(unit)) : distance_to_player(unit),
                             &unit);
    }
    std::stable_sort(nearest.begin(), nearest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [distance, unit] : nearest) {
        (void)distance;
        for (auto& car : unit->cars) {
            if (on_the_way >= loads_at_once) break;
            if (!car.requested && !car.shape.empty() && !library.find(car.shape)
                // One load per shape for all the trains: the others wait for
                // it till the library has made it. (Asked again while a loaded
                // one waited in the library's queue, a scenario's 1700 cars
                // loaded the same shapes over and over.)
                && shapes_asked_.insert(car.shape.key()).second) {
                car.requested = true;
                train_shapes_.emplace(car.shape.key(), car.shape);
                car.loading = std::async(std::launch::async, [vfs = vfs_, path = car.shape] {
                    return renderer::load_model_asset(*vfs, path, {});
                });
                ++on_the_way;
            }
            // Objects fastened to cars (FreightAnim): one load per shape for
            // all the cars (once a load per car: 500 coal cars, 500 loads of
            // the coal).
            if (car.freight_requested || on_the_way >= loads_at_once) continue;
            car.freight_requested = true;
            for (const auto& path : car.freight_shapes) {
                if (path.str().empty() || library.find(path) || !shapes_asked_.insert(path.key()).second) continue;
                train_shapes_.emplace(path.key(), path);
                freight_loading_.push_back(std::async(std::launch::async, [vfs = vfs_, path] {
                    return renderer::load_model_asset(*vfs, path, {});
                }));
                ++on_the_way;
            }
        }
    }
    for (auto it = freight_loading_.begin(); it != freight_loading_.end();) {
        if (!it->valid() || it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto asset = it->get();
        if (asset.ok() && !library.find(asset.source_path)) {
            library.add(std::move(asset));
        } else if (!asset.ok()) {
            std::cout << "freight shape '" << asset.source_path.str() << "': " << asset.error << '\n';
        }
        it = freight_loading_.erase(it);
    }
    for (auto* each : all) {
        auto& car = *each;
        if (!car.loading.valid()) continue;
        if (car.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        auto asset = car.loading.get();
        if (!asset.ok()) {
            std::cout << "train shape '" << car.shape.str() << "': " << asset.error << '\n';
            continue;
        }
        library.add(std::move(asset));
    }
    let_go_far_shapes(library);
}

// From the shape: bogie positions and wheel nodes (RTS 0x4F0BC8: BOGIE* and
// WHEELS* nodes; bogies sorted by z), and the lift that puts the wheels on
// the rail head.
void TrainView::fit_shapes(renderer::ModelLibrary& library) {
    for (auto& unit : units_) {
    for (std::size_t i = 0; i < unit.cars.size(); ++i) {
        auto& car = unit.cars[i];
        if (car.fitted) continue;
        auto* model = library.find(car.shape);
        if (!model || !model->rig()) continue;
        car.fitted = true;
        const auto& rig = *model->rig();
        const auto node_height = [&](std::size_t m) {
            float y = 0.0f;
            for (std::size_t guard = 0; m < rig.matrices.size() && guard <= rig.matrices.size(); ++guard) {
                y += rig.matrices[m].values[10];
                const auto parent = m < rig.hierarchy.size() ? rig.hierarchy[m] : -1;
                if (parent < 0) break;
                m = static_cast<std::size_t>(parent);
            }
            return y;
        };
        float front_z = 0.0f;
        float rear_z = 0.0f;
        int bogies = 0;
        std::optional<float> lowest_axle;
        for (std::size_t m = 0; m < rig.matrices.size(); ++m) {
            const auto name = lower(rig.matrices[m].name);
            if (name.starts_with("wheels")) {
                car.wheels.push_back(static_cast<std::int32_t>(m));
                const float y = node_height(m);
                lowest_axle = lowest_axle ? (std::min)(*lowest_axle, y) : y;
            }
            if (name.starts_with("pantograph")) {
                // RTS 0x4F0BC8: PANTOGRAPHTOP / MIDDLE / BOTTOM, the number,
                // then an optional A or B (the ED9M's PANTOGRAPHBOTTOM1A).
                std::size_t end = name.size();
                if (end > 0 && (name[end - 1] == 'a' || name[end - 1] == 'b')) --end;
                std::size_t start = end;
                while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9') --start;
                const int number = start < end ? std::atoi(name.substr(start, end - start).c_str()) : 1;
                const auto n = static_cast<std::uint8_t>(number >= 1 && number <= 4 ? number : 1);
                car.pantograph_nodes.emplace_back(static_cast<std::int32_t>(m), n);
                if (name.find("bottom") != std::string::npos || car.pantograph_z[n - 1] == 0.0f) {
                    car.pantograph_z[n - 1] = rig.matrices[m].values[11];
                }
            }
            if (name.starts_with("bogie")) {
                const float z = rig.matrices[m].values[11];
                if (bogies == 0) {
                    front_z = rear_z = z;
                } else {
                    front_z = (std::max)(front_z, z);
                    rear_z = (std::min)(rear_z, z);
                }
                ++bogies;
                car.bogies.emplace_back(static_cast<std::int32_t>(m), false);
            }
        }
        for (auto& [matrix, front] : car.bogies) {
            front = rig.matrices[static_cast<std::size_t>(matrix)].values[11] >= (front_z + rear_z) * 0.5f;
        }
        {
            // The rest matrices, as the renderer chains them (row vectors
            // stored: each row an axis's image).
            const auto rest_of = [&](std::size_t m) {
                glm::mat4 result{1.0f};
                for (std::size_t guard = 0; m < rig.matrices.size() && guard <= rig.matrices.size(); ++guard) {
                    const auto& v = rig.matrices[m].values;
                    glm::mat4 node{1.0f};
                    node[0] = glm::vec4{v[0], v[1], v[2], 0.0f};
                    node[1] = glm::vec4{v[3], v[4], v[5], 0.0f};
                    node[2] = glm::vec4{v[6], v[7], v[8], 0.0f};
                    node[3] = glm::vec4{v[9], v[10], v[11], 1.0f};
                    result = node * result;
                    const auto parent = m < rig.hierarchy.size() ? rig.hierarchy[m] : -1;
                    if (parent < 0) break;
                    m = static_cast<std::size_t>(parent);
                }
                return result;
            };
            const auto under_bogie = [&](std::size_t m) {
                for (std::size_t guard = 0; m < rig.hierarchy.size() && guard <= rig.matrices.size(); ++guard) {
                    const auto parent = rig.hierarchy[m];
                    if (parent < 0 || static_cast<std::size_t>(parent) >= rig.matrices.size()) return false;
                    m = static_cast<std::size_t>(parent);
                    if (lower(rig.matrices[m].name).starts_with("bogie")) return true;
                }
                return false;
            };
            car.bogie_rest.clear();
            for (const auto& [matrix, front] : car.bogies) car.bogie_rest.push_back(rest_of(static_cast<std::size_t>(matrix)));
            car.wheel_rest.clear();
            car.wheel_loose.clear();
            for (const auto wheel : car.wheels) {
                car.wheel_rest.push_back(rest_of(static_cast<std::size_t>(wheel)));
                car.wheel_loose.push_back(!under_bogie(static_cast<std::size_t>(wheel)));
            }
        }
        // RTS numbers pantographs 1 and 2 by where they stand, not by their
        // names (0x4F0BC8): towards the shape's rear (z < 0) is 1, towards its
        // front 2. So the ED9M's motor car, one pantograph named 1 in its
        // front half, has pantograph 2, and the same car turned (_MR) has 1;
        // its DLL raises them by these numbers. RTS takes each part's own z;
        // here the pantograph's base (its BOTTOM node) decides for all its
        // parts, so a middle arm with a z of its own stays with them.
        {
            std::array<std::uint8_t, 4> renumber{1, 2, 3, 4};
            std::array<float, 4> z = car.pantograph_z;
            for (std::uint8_t named = 1; named <= 2; ++named) {
                const bool used = std::any_of(car.pantograph_nodes.begin(), car.pantograph_nodes.end(),
                                              [named](const auto& node) { return node.second == named; });
                if (used) renumber[named - 1] = car.pantograph_z[named - 1] < 0.0f ? 1 : 2;
            }
            for (auto& node : car.pantograph_nodes) node.second = renumber[node.second - 1];
            if (renumber[0] != 1 || renumber[1] != 2) {
                std::cout << "pantographs of " << car.shape.str() << ": named 1 is " << int(renumber[0]) << ", named 2 is "
                          << int(renumber[1]) << " (by where they stand, as RTS)" << std::endl;
            }
            for (std::uint8_t named = 1; named <= 2; ++named) {
                if (renumber[named - 1] != named) car.pantograph_z[renumber[named - 1] - 1] = z[named - 1];
            }
        }
        if (!car.pantograph_nodes.empty()) {
            // The pantographs' own key frames, not the animation's header: the
            // ChS4z's keys run 0..1 in a shape whose animation says 1 frame
            // (frames - 1 gave 0 and they never moved); the ED9M's run in a
            // 250-frame one.
            float last_key = 0.0f;
            for (const auto& animation : rig.animations) {
                for (const auto& node : animation.nodes) {
                    const bool pantograph = std::any_of(car.pantograph_nodes.begin(), car.pantograph_nodes.end(),
                                                        [&](const auto& p) { return p.first == node.matrix_index; });
                    if (!pantograph) continue;
                    for (const auto& key : node.positions) last_key = (std::max)(last_key, static_cast<float>(key.frame));
                    for (const auto& key : node.rotations) last_key = (std::max)(last_key, static_cast<float>(key.frame));
                }
            }
            car.pantograph_frames = last_key > 0.0f ? static_cast<std::uint32_t>(std::lround(last_key)) + 1u
                                                    : model->animation().frame_count;
        }
        if (const auto bytes = vfs_->read(formats::shape_description(*vfs_, car.shape))) {
            const auto details = formats::parse_exterior_details(*bytes);
            if (details.walkable() || details.door_ways()) car.outside = std::make_shared<const formats::ExteriorDetails>(details);
            // A node's own key frames: the ED9M's doors move over frames
            // 0..5 of a 250-frame shape animation.
            const auto node_last_frame = [&](const std::size_t m) {
                float last = 0.0f;
                for (const auto& animation : rig.animations) {
                    for (const auto& node : animation.nodes) {
                        if (node.matrix_index != static_cast<std::int32_t>(m)) continue;
                        for (const auto& key : node.positions) last = (std::max)(last, static_cast<float>(key.frame));
                        for (const auto& key : node.rotations) last = (std::max)(last, static_cast<float>(key.frame));
                    }
                }
                return last;
            };
            // Wheel chocks (WagonObjects DragA / DragB): which end each node
            // is at by its place in the shape, z > 0 the front.
            const auto node_z = [&](std::size_t m) {
                float z = 0.0f;
                for (std::size_t guard = 0; m < rig.matrices.size() && guard <= rig.matrices.size(); ++guard) {
                    z += rig.matrices[m].values[11];
                    const auto parent = m < rig.hierarchy.size() ? rig.hierarchy[m] : -1;
                    if (parent < 0) break;
                    m = static_cast<std::size_t>(parent);
                }
                return z;
            };
            const auto under = [&](std::size_t m, const std::size_t top) {
                for (std::size_t guard = 0; m < rig.matrices.size() && guard <= rig.matrices.size(); ++guard) {
                    if (m == top) return true;
                    const auto parent = m < rig.hierarchy.size() ? rig.hierarchy[m] : -1;
                    if (parent < 0) return false;
                    m = static_cast<std::size_t>(parent);
                }
                return false;
            };
            for (const auto& object : details.wagon_objects) {
                if ((object.kind != "draga" && object.kind != "dragb") || !object.first_state_invisible) continue;
                for (const auto& name : object.nodes) {
                    for (std::size_t m = 0; m < rig.matrices.size(); ++m) {
                        if (lower(rig.matrices[m].name) != lower(name)) continue;
                        const float z = node_z(m);
                        const bool shape_front = z != 0.0f ? z > 0.0f : object.kind == "draga";
                        for (std::size_t c = 0; c < rig.matrices.size(); ++c) {
                            if (under(c, m)) car.chock_nodes.push_back({static_cast<std::int32_t>(c), shape_front});
                        }
                        break;
                    }
                }
            }
            for (std::size_t k = 0; k < details.door_details.size(); ++k) {
                const auto& door = details.door_details[k];
                car.door_groups.push_back(door.global_id);
                for (std::size_t m = 0; m < rig.matrices.size(); ++m) {
                    if (lower(rig.matrices[m].name) != lower(door.name)) continue;
                    car.exterior_doors.push_back({.node = static_cast<std::int32_t>(m),
                                                  .name = door.name,
                                                  .time = (std::max)(door.anim_time, 0.05f),
                                                  .index = k,
                                                  .group = door.global_id,
                                                  .last_frame = node_last_frame(m)});
                    break;
                }
            }
        }
        if (lowest_axle && car.definition->wheel_radius > 0.05f) {
            // The tread (axle height - WheelRadius) on the rail head.
            car.lift = std::clamp(rail_top - (*lowest_axle - car.definition->wheel_radius), 0.1f, 0.5f);
        }
        if (bogies >= 2 && front_z > rear_z) {
            // Distances along the train: a flipped car has its shape front towards the rear.
            if (car.flipped) {
                unit.train->set_bogies(i, -rear_z, front_z);
            } else {
                unit.train->set_bogies(i, front_z, -rear_z);
            }
        }
    }
    }
}

void TrainView::apply_handles(const SceneInput& input) {
    using Handle = simulation::DriverHandles::BrakeHandle;
    if (!driver_ || units_.empty() || units_.front().role != Role::driven) return;
    auto& d = *driver_;
    // Position I is held: the handle springs back to II when [ is let go.
    if (d.brake == Handle::charge && !input.brake_less_held) d.brake = Handle::running;
    if (!any_handle(input)) return;
    if (input.throttle_up && d.throttle < d.notches) ++d.throttle;
    if (input.throttle_down && d.throttle > 0) --d.throttle;
    // The reverser is kept towards the train front; the driver's "forward"
    // is his cab's (a cab facing the train rear: the other way).
    int forward = 1;
    if (cab_car_ < units_.front().cars.size() && cab_car_ < driven().cars().size()) {
        const bool turned = cab_ && cab_->turned();
        if (driven().cars()[cab_car_].flipped != turned) forward = -1;
    }
    if (input.reverser_forward && d.reverser * forward < 1) d.reverser += forward;
    if (input.reverser_back && d.reverser * forward > -1) d.reverser -= forward;
    if (input.brake_more && d.brake != Handle::emergency) d.brake = static_cast<Handle>(static_cast<int>(d.brake) + 1);
    if (input.brake_less && d.brake != Handle::charge) d.brake = static_cast<Handle>(static_cast<int>(d.brake) - 1);
    if (input.independent_more) d.independent = (std::min)(d.independent + 0.5f, 4.0f);
    if (input.independent_less) d.independent = (std::max)(d.independent - 0.5f, 0.0f);
    if (input.toggle_ep) d.ep_brake = !d.ep_brake;
}

TrainView::DrivenKeys TrainView::driven_keys() const {
    DrivenKeys keys;
    if (units_.empty() || units_.front().role != Role::driven) return keys;
    const auto& cars = units_.front().cars;
    if (cab_car_ < cars.size()) keys.cab = cars[cab_car_].key;
    if (engineer_car_ < cars.size()) keys.engineer = cars[engineer_car_].key;
    return keys;
}

void TrainView::reattach_driver(const DrivenKeys* keep) {
    if (!loaded()) return;
    if (units_.front().role != Role::driven) {
        attach_role(units_.front());
        return;
    }
    auto& train = driven();
    // The engine driven from stays master when it is still in the train.
    std::size_t master = SIZE_MAX;
    if (keep && keep->cab) {
        const auto& cars = units_.front().cars;
        for (std::size_t i = 0; i < cars.size(); ++i)
            if (cars[i].key == keep->cab) master = i;
    }
    for (std::size_t i = 0; i < train.cars().size(); ++i) train.set_controller(i, nullptr);
    const auto old = driver_ ? std::optional<simulation::DriverHandles>{*driver_} : std::nullopt;
    driver_ = simulation::attach_keyboard_driver(train, master);
    if (driver_ && old) {
        const auto notches = driver_->notches;
        *driver_ = *old;
        driver_->notches = notches;
        driver_->throttle = (std::min)(driver_->throttle, notches);
    }
    if (keep && keep_attached(*keep)) return;
    attach_legacy();
}

// Cars coupled on or cut off while the player drives: RTS rebinds nothing
// (a DLL is bound to its car once, 0x5026FC) and regroups (0x4F995C); the
// engineer stays where he is. False: start again (attach_legacy).
bool TrainView::keep_attached(const DrivenKeys& keep) {
    auto& unit = units_.front();
    const auto find = [&](const std::uint32_t key) {
        for (std::size_t i = 0; i < unit.cars.size() && key; ++i)
            if (unit.cars[i].key == key) return i;
        return SIZE_MAX;
    };
    const auto cab = find(keep.cab);
    if (cab == SIZE_MAX || !cab_) return false;
    if (legacy_) {
        unit.host = true;
        if (!reconsist_host()) {
            std::cerr << "locomotive DLLs: the consist changed, starting them again" << std::endl;
            return false;
        }
        // The cab's car is the player's engine (or one without a DLL).
        if (legacy_->has_dll(cab) && legacy_->player() != cab) return false;
    }
    cab_car_ = cab;
    const auto engineer = find(keep.engineer);
    if (engineer == SIZE_MAX) {
        // He was in a car that left: back to the driver's seat.
        const bool inside = engineer_.inside();
        visit_cab_.reset();
        engineer_car_ = cab_car_;
        if (inside) set_engineer(true);
    } else {
        engineer_car_ = engineer;
        if (engineer_car_ == cab_car_) visit_cab_.reset();
    }
    attach_native();
    if (legacy_) std::cerr << "locomotive DLLs: kept through the change, the player is in car " << legacy_->player() + 1 << std::endl;
    return true;
}

void TrainView::attach_legacy() {
    legacy_.reset();
    legacy_status_.clear();
    if (units_.empty() || units_.front().role != Role::driven) return;
    auto& unit = units_.front();
    std::vector<LegacyCarRef> cars;
    for (const auto& car : unit.cars) cars.push_back({car.definition, car.folder, car.uid, car.key});
    // A train put down cold, on or running starts so (Track control);
    // cold, its pantographs stay down.
    const int start = unit.start_state.value_or(start_state_);
    if (unit.start_state) pantographs_down_ = *unit.start_state == 0;
    // A train just made and started cold: its main reservoirs empty (a DLL's
    // Init may fill them; testers, 2026-11-16: the cold ЭД4М had 8.5 atm).
    if (fresh_train_ && start == 0) {
        auto& train = *unit.train;
        for (std::size_t i = 0; i < train.cars().size(); ++i) {
            const auto& c = train.cars()[i];
            train.set_air(i, {.pipe = c.pipe, .charging_pipe = c.charging_pipe, .cylinder = c.cylinder, .auxiliary = c.auxiliary,
                              .control = c.control, .main_reservoir = 0.0f});
        }
    }
    legacy_ = LegacyCab::attach(*vfs_, cars, *unit.train, legacy_status_, start, &route_, std::exchange(fresh_train_, false));
    // A new host: the driven train alone (the parts cut off before went with
    // the old one; their controllers turn inert).
    for (auto& each : units_) {
        each.host = false;
        each.host_train = SIZE_MAX;
    }
    host_keys_.clear();
    if (legacy_) {
        unit.host = true;
        unit.host_train = 0;
        host_keys_.emplace_back();
        for (const auto& car : unit.cars) host_keys_.back().push_back(car.key);
        // The player's other trains (the network's own) join the new host, to
        // be bound when he gets in and to go on when he leaves.
        bool more = false;
        for (std::size_t u = 1; u < units_.size(); ++u) {
            if (units_[u].remote || !boardable_.contains(units_[u].train->id())) continue;
            units_[u].host = true;
            more = true;
        }
        if (more && !reconsist_host()) std::cerr << "locomotive DLLs: the player's other trains not taken in" << std::endl;
    }
    attach_cab(legacy_ ? legacy_->player() : SIZE_MAX);
    if (!legacy_status_.empty()) {
        message_ = tr("locomotive DLLs: ") + legacy_status_;
        std::cerr << "locomotive DLLs: " << legacy_status_ << std::endl;
    }
    if (legacy_) std::fprintf(stderr, "locomotive DLLs: running, the player is in car %zu\n", legacy_->player() + 1);
    attach_native();
}

void TrainView::attach_cab(std::size_t player) {
    auto& unit = units_.front();
    // The 3-D cab of the engine the player drives: the DLL's one, else the first engine.
    engineer_.off();
    visit_cab_.reset();
    cab_.reset();
    cab_car_ = SIZE_MAX;
    engineer_car_ = SIZE_MAX;
    cab_requested_ = false;
    // A native plugin's engine next (EngExtended ORTSTrainPlugin), else the first engine.
    for (std::size_t i = 0; player == SIZE_MAX && i < unit.cars.size(); ++i)
        if (unit.cars[i].definition && unit.cars[i].definition->engine && !unit.cars[i].definition->engine->train_plugin.empty()) player = i;
    for (std::size_t i = 0; player == SIZE_MAX && i < unit.cars.size(); ++i)
        if (unit.cars[i].definition && unit.cars[i].definition->engine) player = i;
    if (player < unit.cars.size()) {
        const auto& car = unit.cars[player];
        cab_ = CabView::create(*vfs_, *car.definition,
                               formats::wagon_path(car.folder, car.definition->name, car.definition->engine.has_value()));
        if (cab_) cab_car_ = player;
    }
}

bool TrainView::reconsist_host() {
    if (!legacy_ || units_.empty()) return false;
    std::vector<std::vector<LegacyCarRef>> trains;
    std::vector<simulation::Train*> sims;
    std::vector<std::size_t> which;
    std::vector<std::uint32_t> types;
    units_.front().host = true;
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        unit.host_train = SIZE_MAX;
        if (!unit.host) continue;
        // Another client's now (a train taken away, the network): its DLLs
        // stay bound and keep their state, but do not run (RTS kind 2).
        types.push_back(unit.remote ? 2u : u == 0 ? 1u : 3u);
        trains.emplace_back();
        for (const auto& car : unit.cars) trains.back().push_back({car.definition, car.folder, car.uid, car.key});
        sims.push_back(unit.train.get());
        which.push_back(u);
    }
    legacy_->set_train_types(std::move(types));
    if (!legacy_->reconsist(*vfs_, trains, sims)) return false;
    host_keys_.clear();
    for (std::size_t t = 0; t < which.size(); ++t) {
        auto& unit = units_[which[t]];
        unit.host_train = t;
        host_keys_.emplace_back();
        for (const auto& car : unit.cars) host_keys_.back().push_back(car.key);
        // A part cut off moves by its own DLLs (RTS runs them, 0x424488);
        // one sent off with a path keeps its driver.
        if (t == 0 || unit.role != Role::standing || unit.remote) continue;
        for (std::size_t i = 0; i < unit.cars.size(); ++i)
            if (legacy_->has_dll(t, i)) unit.train->set_controller(i, legacy_->controller(t, i));
    }
    return true;
}

bool TrainView::host_current() const {
    if (!legacy_) return true;
    std::size_t t = 0;
    for (const auto& unit : units_) {
        if (!unit.host) continue;
        if (t >= host_keys_.size() || unit.host_train != t || unit.cars.size() != host_keys_[t].size()) return false;
        for (std::size_t i = 0; i < unit.cars.size(); ++i)
            if (unit.cars[i].key != host_keys_[t][i]) return false;
        ++t;
    }
    return t == host_keys_.size() && t == legacy_->trains();
}

const dllhost::CarOutput* TrainView::dll_output(const std::size_t u, const std::size_t i) const {
    if (!legacy_ || u >= units_.size() || units_[u].host_train == SIZE_MAX) return nullptr;
    return legacy_->output(units_[u].host_train, i);
}

bool TrainView::dll_car(const std::size_t u, const std::size_t i) const {
    return legacy_ && u < units_.size() && units_[u].host_train != SIZE_MAX && legacy_->has_dll(units_[u].host_train, i);
}

const std::array<float, 8>* TrainView::dll_sound_variables(const std::size_t u, const std::size_t i) const {
    if (!legacy_ || u >= units_.size() || units_[u].host_train == SIZE_MAX) return nullptr;
    return legacy_->sound_variables(units_[u].host_train, i);
}

bool TrainView::take_over_host_unit() {
    if (!legacy_ || units_.empty() || !units_.front().host) return false;
    auto& unit = units_.front();
    // The player's engine there: the first with a DLL and a driving cab (a
    // part cut off from a multiple unit may start with a motor car, whose
    // Cabin is its salon), else the first with a DLL.
    std::size_t player = SIZE_MAX, first = SIZE_MAX;
    for (std::size_t i = 0; i < unit.cars.size() && player == SIZE_MAX; ++i) {
        if (!legacy_->has_dll(unit.host_train, i)) continue;
        if (first == SIZE_MAX) first = i;
        const auto& engine = *unit.cars[i].definition->engine;
        if (engine.cab_view && !engine.cabin.empty()) player = i;
    }
    if (player == SIZE_MAX) player = first;
    if (player == SIZE_MAX) return false;
    auto& train = *unit.train;
    for (std::size_t i = 0; i < train.cars().size(); ++i) train.set_controller(i, nullptr);
    driver_ = simulation::attach_keyboard_driver(train, player);
    if (!reconsist_host()) return false;
    if (legacy_->player() != player && !legacy_->enter(player, train, unit.start_state.value_or(-1))) return false;
    attach_cab(legacy_->player());
    std::fprintf(stderr, "locomotive DLLs: the player takes car %zu of a train they had left\n", legacy_->player() + 1);
    attach_native();
    return true;
}

// A native openRTS plugin (include/openrts/plugin/train_plugin.h) on the
// driven engine: it gets the engine's controller, its cab (.sd) and keys.
void TrainView::attach_native() {
    native_car_ = SIZE_MAX;
    native_bindings_.clear();
    native_going_down_.clear();
    if (units_.empty() || units_.front().role != Role::driven || cab_car_ >= units_.front().cars.size()) {
        native_.reset();
        return;
    }
    auto& unit = units_.front();
    const auto& car = unit.cars[cab_car_];
    const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
    if (!engine || engine->train_plugin.empty() || (legacy_ && legacy_->has_dll(cab_car_))) {
        native_.reset();
        return;
    }
    // The same engine as before (a coupling, a cut): the plugin keeps its state.
    if (!native_ || native_->definition() != car.definition) {
        native_.reset();
        const auto file = formats::wagon_path(car.folder, car.definition->name, true);
        const auto dll = file.parent().resolve(engine->train_plugin);
        std::string error;
        const auto real = dll ? vfs_->real_path(*dll, std::filesystem::temp_directory_path() / "openrts", &error) : std::nullopt;
        if (!real) {
            message_ = trf("native plugin %s: not found", engine->train_plugin.c_str());
            std::fprintf(stderr, "native plugin %s: not found %s\n", engine->train_plugin.c_str(), error.c_str());
            return;
        }
        auto loaded = trainplugin::NativeLocomotive::load(*real, *car.definition, cab_ ? &cab_->description() : nullptr, &error);
        if (!loaded) {
            message_ = trf("native plugin: %s", error.c_str());
            std::fprintf(stderr, "native plugin: %s\n", error.c_str());
            return;
        }
        native_ = std::move(loaded);
        std::fprintf(stderr, "native plugin: %s on car %zu\n", native_->name().c_str(), cab_car_ + 1);
    }
    native_car_ = cab_car_;
    // The train's controller forwards to the plugin the viewer keeps.
    struct Forward final : simulation::LocomotiveController {
        std::shared_ptr<trainplugin::NativeLocomotive> plugin;
        void step(double dt, const simulation::EngineInputs& inputs, simulation::EngineDemands& demands) override {
            plugin->step(dt, inputs, demands);
        }
    };
    auto forward = std::make_unique<Forward>();
    forward->plugin = native_;
    unit.train->set_controller(native_car_, std::move(forward));
    native_->set_player(true);
    if (cab_) {
        for (const auto& s : cab_->description().switches) {
            for (std::size_t k = 0; k < s.keys.size() && k < 2; ++k) {
                const auto key = parse_cab_key(s.keys[k]);
                if (!key.key) continue;
                const int direction = s.keys.size() >= 2 ? (k == 0 ? 1 : -1) : 0;
                native_bindings_.push_back({s.id, key.key, key.shift, key.ctrl, direction});
            }
        }
    }
}

// RTS's click and step rules (0x437AA4) for a native cab: one key steps up
// and back down between the ends; two keys step one way each.
void TrainView::native_operate(const std::uint32_t id, const int direction) {
    if (!native_) return;
    auto& controls = native_->cab().controls();
    if (id >= controls.size()) return;
    const auto& c = controls[id];
    const int states = static_cast<int>((std::max)(c.states, 1u));
    int target = static_cast<int>(std::lround(c.target));
    if (direction != 0) {
        target = std::clamp(target + direction, 0, states - 1);
    } else {
        bool& down = native_going_down_[id];
        if (target >= states - 1) down = true;
        if (target <= 0) down = false;
        target = std::clamp(target + (down ? -1 : 1), 0, states - 1);
    }
    if (static_cast<float>(target) == c.target) return;
    if (!native_->operate(static_cast<OrtsHandle>(id), static_cast<float>(target))) message_ = tr("the locomotive refuses that");
}

std::optional<CabView::AirGauges> TrainView::air_gauges(const Unit& unit, const std::size_t car) const {
    if (!gauges_air_loaded_) {
        gauges_air_loaded_ = true;
        gauges_air_ = remembered("gauges_openrts_air").value_or("1") == "1";
    }
    if (!gauges_air_ || !unit.train) return std::nullopt;
    const auto& cars = unit.train->cars();
    if (car >= cars.size()) return std::nullopt;
    const auto& state = cars[car];
    return CabView::AirGauges{.pipe = state.pipe, .cylinder = state.cylinder, .main_reservoir = state.main_reservoir};
}

const trainplugin::BrakesParameters& TrainView::brakes_parameters_of(const formats::EngineDefinition& engine) const {
    auto [at, made] = brakes_parameters_.try_emplace(&engine);
    if (made) at->second = trainplugin::brakes_parameters(engine);
    return at->second;
}

const dllhost::CarOutput* TrainView::cab_state(const std::size_t car) const {
    if (helping_) return interior_output(car);
    if (native_ && car == native_car_) return &native_output_;
    const auto* out = legacy_ ? legacy_->output(car) : nullptr;
    // openRTS's cab brake devices: Brakes.dll's gauges and lamps show theirs.
    if (out && !units_.empty() && car < units_.front().cars.size()) {
        const auto* definition = units_.front().cars[car].definition;
        const auto* asked = units_.front().train->demands_of(car);
        if (definition && definition->engine && asked && asked->cab_brakes) {
            const auto& parameters = brakes_parameters_of(*definition->engine);
            patched_output_ = *out;
            trainplugin::show_cab_brakes(parameters, trainplugin::working_cab(definition->engine->brakes_library, out->switches),
                                         *asked->cab_brakes, patched_output_.displays);
            return &patched_output_;
        }
    }
    // openRTS's valve over Brakes.dll: its УР gauge (BR_MANU1) shows the
    // valve's УР, not the one the DLL keeps for itself.
    if (out && legacy_->air() == trainplugin::DllAir::own_valve && !units_.empty() && car < units_.front().cars.size()) {
        const auto* definition = units_.front().cars[car].definition;
        // The working cab's gauge (BR_MANU<k>; the ЭП20's second cab: 100).
        int gauge = -1;
        if (definition && definition->engine) {
            const auto& all = definition->engine->brakes_library;
            const auto cab = trainplugin::working_cab(all, out->switches);
            gauge = cab == 0 ? all.equalising_gauge : all.for_cab(cab).equalising_gauge;
        }
        const auto* asked = units_.front().train->demands_of(car);
        // Cut off the pipe (double traction, the 367) it still shows its own УР.
        if (gauge >= 0 && static_cast<std::size_t>(gauge) < out->displays.size() && asked && asked->equalising_kept >= 0.0f) {
            patched_output_ = *out;
            patched_output_.displays[static_cast<std::size_t>(gauge)].value = asked->equalising_kept;
            return &patched_output_;
        }
    }
    return out;
}

const dllhost::CarOutput* TrainView::interior_output(const std::size_t car) const {
    if (helping_) {
        const auto it = helper_outputs_.find(car);
        return it != helper_outputs_.end() ? &it->second : nullptr;
    }
    return legacy_ && legacy_->has_dll(car) ? legacy_->output(car) : nullptr;
}

bool TrainView::aboard(const Unit& unit, const std::size_t u) const noexcept {
    return u == 0 && (unit.role == Role::driven || (helping_ != 0 && unit.train && unit.train->id() == helping_));
}

std::string TrainView::help(const std::uint64_t id) {
    if (helping_ == id) return {};
    auto* unit = find_unit(id);
    if (!unit || !unit->remote) return "no such train of another player here";
    stop_helping();
    unit = find_unit(id);
    // His own driven train stands meanwhile (as when he takes another of his
    // trains: its DLLs wait in the host), to be driven again after.
    helper_own_ = 0;
    if (!units_.front().remote && units_.front().role == Role::driven) {
        helper_own_ = units_.front().train->id();
        if (legacy_) legacy_->release_keys();
        units_.front().role = Role::standing;
        attach_role(units_.front());
    }
    const auto index = static_cast<std::size_t>(unit - units_.data());
    if (index != 0) std::swap(units_.front(), units_[index]);
    helping_ = id;
    helper_outputs_.clear();
    helper_hands_.clear();
    set_engineer(false);
    native_.reset();
    driver_.reset();
    follow_ = false;
    // The cab of the engineer's first engine; the stream fills it.
    attach_cab(SIZE_MAX);
    LegacyCab::Remote remote;
    remote.output = [this](const std::size_t car) { return interior_output(car); };
    remote.cab = [this](const std::size_t car) -> const formats::CabDescription* {
        if (car == cab_car_ && cab_) return &cab_->description();
        if (car == engineer_car_ && visit_cab_) return &visit_cab_->description();
        return nullptr;
    };
    remote.operate = [this](const std::size_t car, const std::uint32_t switch_id, const std::uint32_t state) {
        helper_hands_.push_back({.kind = HelperHand::Kind::operate, .car = car, .id = switch_id, .state = state});
        // Shown at once: the engineer's answer comes with the next stream.
        if (const auto it = helper_outputs_.find(car); it != helper_outputs_.end() && switch_id < it->second.switches.size()) {
            it->second.switches[switch_id].set_state = state;
        }
    };
    remote.drag = [this](const std::size_t car, const std::uint32_t switch_id, const float delta, const bool release) {
        helper_hands_.push_back({.kind = HelperHand::Kind::drag, .car = car, .id = switch_id, .delta = delta, .release = release});
    };
    helper_cab_ = LegacyCab::remote(std::move(remote), cab_car_);
    if (cab_) set_engineer(true);
    follow_train(id);
    message_ = trf("You help the engineer of %s: its cab is his.", units_.front().name.c_str());
    std::cout << "net: the player helps in train " << units_.front().name << ", cab car " << cab_car_ + 1 << std::endl;
    return {};
}

void TrainView::stop_helping() {
    if (!helping_) return;
    const auto own = std::exchange(helper_own_, 0);
    if (helper_cab_) helper_cab_->release_keys();
    helper_cab_.reset();
    helper_outputs_.clear();
    helper_hands_.clear();
    helping_ = 0;
    set_engineer(false);
    cab_.reset();
    visit_cab_.reset();
    cab_requested_ = false;
    visit_requested_ = false;
    cab_car_ = SIZE_MAX;
    engineer_car_ = SIZE_MAX;
    std::cout << "net: the player helps no more" << std::endl;
    // His own train again, its DLLs where they waited.
    if (own && find_unit(own)) {
        if (const auto problem = set_role(own, Role::driven); !problem.empty()) {
            std::cout << "net: back to his train: " << problem << std::endl;
        } else if (cab_) {
            set_engineer(true);
        }
    }
}

void TrainView::set_helper_output(const std::size_t car, dllhost::CarOutput output) {
    if (!helping_ || units_.empty() || car >= units_.front().cars.size()) return;
    // Its cab triggers play once, in the cab sound (update_cab_sound).
    auto& pending = units_.front().cars[car].pending_cab_triggers;
    for (const auto code : output.sound_cab) pending.push_back(code);
    output.sound_cab.clear();
    // A switch the hands moved stays shown there until the engineer's answer.
    helper_outputs_[car] = std::move(output);
}

std::size_t TrainView::helper_car() const noexcept {
    if (!helping_ || !engineer_.inside()) return SIZE_MAX;
    return engineer_car_;
}

std::string TrainView::helper_hand(const HelperHand& hand) {
    if (!legacy_ || units_.empty() || units_.front().role != Role::driven || units_.front().remote) return "not driving a DLL train";
    if (hand.car >= units_.front().cars.size() || !legacy_->has_dll(hand.car)) return "no DLL in that car";
    if (hand.kind == HelperHand::Kind::operate) {
        return legacy_->operate_switch(hand.car, hand.id, hand.state) ? "done" : "refused by the DLL";
    }
    legacy_->drag_switch(hand.car, hand.id, hand.delta, hand.release);
    return "done";
}

std::optional<dllhost::CarOutput> TrainView::helper_output(const std::size_t car) {
    if (units_.empty() || units_.front().role != Role::driven || units_.front().remote || !legacy_) return std::nullopt;
    if (car >= units_.front().cars.size() || !legacy_->has_dll(car)) return std::nullopt;
    helper_watched_until_[car] = helper_clock_ + 3.0;
    const auto* out = cab_state(car);
    if (!out) return std::nullopt;
    // The cab only: the exterior's sounds, lights and doors reach the
    // helper as every other client's (the trains' own packages).
    dllhost::CarOutput copy = *out;
    copy.sound_ext.clear();
    copy.messages.clear();
    copy.aux_sound.clear();
    copy.exhausts.clear();
    copy.doors.clear();
    copy.light_flags.clear();
    copy.sound_cab = std::exchange(helper_triggers_[car], {});
    return copy;
}

void TrainView::change_cab(const std::size_t car) {
    if (units_.empty() || units_.front().role != Role::driven || car == cab_car_) return;
    auto& unit = units_.front();
    if (car >= unit.cars.size() || !unit.cars[car].definition || !unit.cars[car].definition->engine) return;
    // The cab he walked into is swapped in; one he is not in (boarding
    // another train at its door) is made here.
    std::unique_ptr<CabView> made;
    if (car != engineer_car_ || !visit_cab_) {
        made = take_cab(car);
        if (!made) return;
    }
    if (legacy_ && legacy_->has_dll(car)) {
        if (!legacy_->enter(car, *unit.train)) return;
    } else if (legacy_ && legacy_->has_dll(cab_car_)) {
        return; // a DLL train's other engines without a DLL are not driven from
    }
    if (made) {
        visit_cab_ = std::move(made);
        visit_requested_ = false;
    }
    // The cab he walked into is the one driven now.
    std::swap(cab_, visit_cab_);
    std::swap(cab_requested_, visit_requested_);
    std::swap(cab_loading_, visit_loading_);
    const auto old = cab_car_;
    cab_car_ = car;
    engineer_car_ = car;
    // The cab driven from before stays in its car with its doors.
    keep_cab(old, std::move(visit_cab_));
    if (!legacy_) {
        // The keyboard driver: the new engine is master; its handles start
        // where a cab not driven from stands.
        auto& train = *unit.train;
        const auto keep = driver_ ? std::optional<simulation::DriverHandles>{*driver_} : std::nullopt;
        for (std::size_t i = 0; i < train.cars().size(); ++i) train.set_controller(i, nullptr);
        driver_ = simulation::attach_keyboard_driver(train, car);
        if (driver_ && keep) {
            const auto notches = driver_->notches;
            *driver_ = *keep;
            driver_->notches = notches;
            driver_->throttle = 0;
            driver_->reverser = 0;
        }
        native_.reset();
        attach_native();
    }
    message_ = trf("the cab of car %zu is driven now", car + 1);
}

bool TrainView::keyboard_captured() const noexcept {
    if (!units_.empty() && units_.front().remote) return false; // not his train
    return ((legacy_ && legacy_->keyboard()) || native_) && cab_keys_live();
}

audio::SoundCamera TrainView::sound_camera() const noexcept {
    // RTS (0x44AB7C, docs/sound.md): outside - on foot, the free or an
    // outside camera, the head out of a window - the ExternalCam groups; in
    // the driven car the CabCam ones; in another car of the train the
    // PassengerCam ones.
    if (!viewing_inside() || engineer_.heading_out()) return audio::SoundCamera::external;
    return engineer_car_ == cab_car_ ? audio::SoundCamera::cab : audio::SoundCamera::passenger;
}

bool TrainView::cab_keys_live() const noexcept {
    if (engineer_.mode() == Engineer::Mode::off) return true;
    return engineer_.inside() && engineer_car_ == cab_car_;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> TrainView::cut_train(const std::uint64_t id, const std::size_t index) {
    auto* unit = find_unit(id);
    if (!unit || index == 0 || index >= unit->cars.size()) return std::nullopt;
    const auto unit_index = static_cast<std::size_t>(unit - units_.data());
    const auto before = units_.size();
    // The train keeps the cars before the cut (Train::uncouple gives the
    // rear part a train of its own); the cocks at the joint closed.
    split(unit_index, index, true, true, false);
    if (units_.size() != before + 1) return std::nullopt;
    const auto rear = units_[unit_index].train->id() == id ? units_.back().train->id() : units_[unit_index].train->id();
    return std::pair{id, rear};
}

void TrainView::split(const std::size_t unit_index, const std::size_t index, const bool close_cocks, const bool keep_open,
                      const bool announce) {
    if (unit_index >= units_.size()) return;
    auto& unit = units_[unit_index];
    const auto keys = driven_keys();
    // The driver stays in the cab he drives from; without one, with the
    // master engine, the first engine (attach_keyboard_driver).
    std::optional<std::size_t> master;
    if (unit_index == 0 && cab_car_ < unit.cars.size()) master = cab_car_;
    for (std::size_t i = 0; !master && i < unit.cars.size(); ++i) {
        if (unit.cars[i].definition && unit.cars[i].definition->engine) {
            master = i;
            break;
        }
    }
    const auto from = unit.train->id();
    const auto scenario_id = unit.activity_id;
    auto cut_train = unit.train->uncouple(index, close_cocks, keep_open);
    if (!cut_train) return;
    Unit cut;
    cut.train = std::move(cut_train);
    const auto split_at = unit.cars.begin() + static_cast<std::ptrdiff_t>(index);
    cut.cars.assign(std::make_move_iterator(split_at), std::make_move_iterator(unit.cars.end()));
    unit.cars.erase(split_at, unit.cars.end());
    // The driver goes with the master engine.
    const bool swapped = unit_index == 0 && master && *master >= index;
    if (swapped) std::swap(unit, cut);
    if (scenario_id) {
        // A scenario's train (activity.pdf 4.2 45, SetDefaultUncouple): the
        // part with MainWagon keeps its id (else the driver's, else the
        // head), the other is the rule's NewID; UncoupleCars hears of it.
        const auto rule = uncouple_rules_.find(*scenario_id);
        const auto has = [&](const Unit& part, const std::pair<ActivityId, std::uint32_t>& car) {
            return std::any_of(part.cars.begin(), part.cars.end(), [&](const Car& c) { return c.activity_origin == car.first && c.uid == car.second; });
        };
        // `unit` is the driver's part when he drives this train, else the head.
        bool unit_keeps = true;
        if (rule != uncouple_rules_.end() && rule->second.main) unit_keeps = !has(cut, *rule->second.main);
        auto& keeper = unit_keeps ? unit : cut;
        auto& other = unit_keeps ? cut : unit;
        keeper.activity_id = scenario_id;
        other.activity_id = rule != uncouple_rules_.end() && rule->second.new_id ? rule->second.new_id : std::nullopt;
        if (&other == &cut && rule != uncouple_rules_.end() && rule->second.standing) cut.role = Role::standing;
        ActivityCut report{.train = *scenario_id, .head = swapped ? cut.cars.size() : unit.cars.size(),
                           .tail = swapped ? unit.cars.size() : cut.cars.size(), .cut = {}};
        for (const auto& c : other.cars) if (c.activity_origin) report.cut.emplace_back(*c.activity_origin, c.uid);
        activity_cuts_.push_back(std::move(report));
        if (activity_cuts_.size() > 16) activity_cuts_.erase(activity_cuts_.begin());
        std::cout << "activity: train " << scenario_id->first << ' ' << scenario_id->second << " cut: " << keeper.cars.size()
                  << " cars keep it, " << other.cars.size() << " are "
                  << (other.activity_id ? std::to_string(other.activity_id->first) + ' ' + std::to_string(other.activity_id->second)
                                        : std::string("unnamed"))
                  << std::endl;
    }
    if (announce) {
        cuts_.push_back({from, unit.train->id(), cut.train->id(), index, swapped});
        if (cuts_.size() > 16) cuts_.erase(cuts_.begin());
    }
    for (std::size_t i = 0; i < cut.train->cars().size(); ++i) cut.train->set_controller(i, nullptr);
    cut.name = unit.name + " (cut)";
    cut.freight = unit.freight;
    cut.category = unit.category; // a part keeps its train's (RTS: the new train takes its kind)
    // RTS: the part cut off keeps the train's flag 4 and its DLLs run on
    // (0x4EEC50, 0x424488).
    cut.host = unit.host && legacy_ != nullptr;
    unit.settle();
    cut.settle();
    const bool host = unit.host && legacy_ != nullptr;
    units_.push_back(std::move(cut));
    if (unit_index == 0) {
        reattach_driver(&keys);
    } else {
        attach_role(units_[unit_index]);
        if (host) reconsist_host();
    }
}

bool TrainView::join_trains(const std::uint64_t survivor, const std::uint64_t absorbed, const bool at_front,
                            const bool other_front, const double gap) {
    auto* s = find_unit(survivor);
    auto* a = find_unit(absorbed);
    if (!s || !a || s == a || !s->train->placed() || !a->train->placed()) return false;
    s->train->set_asleep(false);
    if (!s->train->couple(*a->train, at_front, other_front, (std::max)(gap, 0.0))) return false;
    const auto merged = merge_units(s->train.get(), a->train.get(), at_front, at_front == other_front, false);
    if (merged >= units_.size()) return false;
    message_ = trf("coupled: %zu cars; the hoses and cocks by hand (the car menu at the joint)", units_[merged].cars.size());
    return true;
}

std::size_t TrainView::merge_units(const simulation::Train* survivor_train, const simulation::Train* absorbed_train,
                                   const bool absorbed_ahead, const bool absorbed_turned, const bool announce) {
    const auto keys = driven_keys();
    const auto find = [&](const simulation::Train* train) {
        return static_cast<std::size_t>(std::find_if(units_.begin(), units_.end(), [&](const Unit& unit) {
                                            return unit.train.get() == train;
                                        }) - units_.begin());
    };
    const auto s = find(survivor_train);
    const auto a = find(absorbed_train);
    if (s >= units_.size() || a >= units_.size() || s == a) return units_.size();
    if (announce) {
        couplings_.push_back({units_[s].train->id(), units_[a].train->id(), absorbed_ahead, absorbed_turned});
        if (couplings_.size() > 16) couplings_.erase(couplings_.begin());
    }
    // A scenario's trains (CoupleCars): the survivor keeps its id, or takes
    // the absorbed one's when it has none.
    if (units_[s].activity_id && units_[a].activity_id) activity_couplings_.emplace_back(*units_[s].activity_id, *units_[a].activity_id);
    if (!units_[s].activity_id) units_[s].activity_id = units_[a].activity_id;
    const bool host = units_[s].host || units_[a].host;
    auto absorbed_cars = std::move(units_[a].cars);
    if (absorbed_turned) {
        std::reverse(absorbed_cars.begin(), absorbed_cars.end());
        for (auto& car : absorbed_cars) car.flipped = !car.flipped;
    }
    auto& cars = units_[s].cars;
    if (absorbed_ahead) {
        absorbed_cars.insert(absorbed_cars.end(), std::make_move_iterator(cars.begin()), std::make_move_iterator(cars.end()));
        cars = std::move(absorbed_cars);
    } else {
        cars.insert(cars.end(), std::make_move_iterator(absorbed_cars.begin()), std::make_move_iterator(absorbed_cars.end()));
    }
    units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(a));
    const auto survivor = s > a ? s - 1 : s;
    units_[survivor].host = host && legacy_ != nullptr;
    if (survivor == 0) {
        reattach_driver(&keys);
    } else {
        attach_role(units_[survivor]);
        if (units_[survivor].host) reconsist_host();
    }
    units_[survivor].settle();
    return survivor;
}

void TrainView::step_trains() {
    OPENRTS_ZONE("trains: physics step");
    // Meetings first, on the positions the step starts from (the earlier
    // unit survives a coupling, so the driven train keeps its place).
    // Trains of this program first: in a meeting the earlier survives, and
    // our train takes another client's in (RTS: its own train always takes
    // the other); two remote trains never meet here.
    std::vector<simulation::Train*> trains;
    std::vector<std::uint8_t> remote;
    for (const bool pass : {false, true}) {
        for (auto& unit : units_) {
            if (unit.remote != pass) continue;
            trains.push_back(unit.train.get());
            remote.push_back(unit.remote ? 1 : 0);
        }
    }
    for (const auto& result : simulation::resolve_contacts(trains, remote)) {
        if (result.outcome != simulation::ContactOutcome::coupled) {
            message_ = result.outcome == simulation::ContactOutcome::collision
                ? "collision: " + std::to_string(static_cast<int>(result.force / 1000.0)) + " kN, over DerailBufferForce"
                : "buffers: the couplers are open or not automatic";
            continue;
        }
        const auto survivor = merge_units(result.survivor, result.absorbed, result.absorbed_ahead, result.absorbed_turned);
        if (survivor >= units_.size()) continue;
        message_ = trf("coupled: %zu cars; the hoses and cocks by hand (the car menu at the joint)", units_[survivor].cars.size());
        trains.clear();
        break; // the list changed; the rest meet on the next step
    }

    // STATIC consists (RTS train type 0): a standing train with nobody at
    // its handles goes to sleep after 30 s below 0.3 m/s, as RTS turns a
    // LOOSE train STATIC (0x424488); a meeting that moved it wakes it (RTS:
    // pushed without coupling it becomes LOOSE and moves on).
    for (auto& unit : units_) {
        auto& train = *unit.train;
        if (unit.remote) continue;
        if (train.asleep()) {
            if (unit.role != Role::standing || std::abs(train.speed()) > 0.01) {
                train.set_asleep(false);
                unit.still = 0.0f;
            }
            continue;
        }
        if (unit.role != Role::standing || !train.placed() || train.derailed()) {
            unit.still = 0.0f;
            continue;
        }
        unit.still = std::abs(train.speed()) < 0.3 ? unit.still + static_cast<float>(simulation::train_step) : 0.0f;
        if (unit.still > 30.0f) train.set_asleep(true);
    }
    // Traffic (RTS's TRAFFIC trains, docs/traffic.md): its controller sets
    // the acceleration of the one body; the others' ends ahead stop it.
    std::vector<simulation::Train*> placed_trains;
    for (auto& unit : units_)
        if (unit.train->placed()) placed_trains.push_back(unit.train.get());
    for (auto& unit : units_) {
        if (unit.role != Role::ai || !unit.traffic || !unit.train->placed() || unit.remote) continue;
        auto orders = simulation::traffic_orders(*unit.train, *dispatcher_, signals_, *limits_, unit.speed_category(),
                                                 unit.traffic->settings(), unit.wait_left, placed_trains);
        if (unit.wait_here > 0.0) {
            orders.hold = true;
            if (std::abs(unit.train->speed()) < 0.05) unit.wait_here -= simulation::train_step;
        }
        unit.traffic->step(simulation::train_step, *unit.train, orders);
    }
    std::vector<std::pair<std::size_t, std::size_t>> breaks;
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        if (unit.remote) continue;
        unit.train->step();
        if (cargo_ && unit.train->placed()) {
            const auto placements = unit.train->placements();
            cargo_centres_.clear();
            for (const auto& placement : placements) {
                const glm::vec3 centre = (local(placement.front) + local(placement.rear)) * 0.5f;
                cargo_centres_.push_back({centre.x, centre.y, centre.z});
            }
            cargo_->update(static_cast<float>(simulation::train_step), *unit.train, cargo_centres_);
        }
        for (const auto& event : unit.train->take_events()) {
            auto* car = event.car < unit.cars.size() ? &unit.cars[event.car] : nullptr;
            switch (event.kind) {
            case simulation::TrainEventKind::joint:
                if (u == 0) ++joints_;
                if (car) {
                    car->pending_events.push_back(formats::SoundEvent::wagon_on_joint);
                    car->pending_events.push_back(formats::SoundEvent::wagon_on_any_joint);
                }
                break;
            case simulation::TrainEventKind::junction:
                if (u == 0) ++junctions_;
                if (car) car->pending_events.push_back(formats::SoundEvent::wagon_on_junction);
                break;
            case simulation::TrainEventKind::end_of_track:
                if (u == 0) end_of_track_ = true;
                break;
            case simulation::TrainEventKind::wheel_flat:
                if (car && car->pending_flats.size() < 16) car->pending_flats.emplace_back(event.bogie, event.amount);
                break;
            case simulation::TrainEventKind::coupler_break:
                ++coupler_breaks_;
                if (std::none_of(breaks.begin(), breaks.end(), [&](const auto& b) { return b.first == u; })) {
                    breaks.emplace_back(u, event.car + 1);
                }
                break;
            case simulation::TrainEventKind::derailment:
                ++derailments_;
                if (u == 0 && network_game_) {
                    // RTS: only the dispatcher puts a train back on the rails.
                    message_ = trf("derailed: car %u; the dispatcher puts the train back (removes it from the world and places it again)",
                                   event.car + 1);
                } else {
                    message_ = u == 0 ? trf("derailed: car %u; Esc, the Train tab, \"Put on the path start\" puts the train back", event.car + 1)
                                      : trf("derailed: car %u of a cut", event.car + 1);
                }
                break;
            }
        }
    }
    // A broken coupler uncouples there with the cocks open (RTS: "Train
    // damaged"); both parts lose their air and brake.
    for (auto it = breaks.rbegin(); it != breaks.rend(); ++it) {
        split(it->first, it->second, false, false);
        message_ = trf("train damaged: the coupler behind car %zu broke", static_cast<std::size_t>(it->second));
    }
    // Paths: progress and the junctions reserved ahead.
    std::vector<const simulation::Train*> all;
    for (const auto& unit : units_) all.push_back(unit.train.get());
    dispatcher_->update(all);
}

void TrainView::attach_role(Unit& unit) {
    auto& train = *unit.train;
    for (std::size_t i = 0; i < train.cars().size(); ++i) train.set_controller(i, nullptr);
    unit.traffic.reset();
    train.set_kinematic(false);
    if (unit.role == Role::ai) {
        // RTS's traffic: no handles, the train one body its controller moves
        // (docs/traffic.md). Track control's AI trains need an engine and
        // take the default speeds; a scenario's have their own.
        const bool engine = std::any_of(train.cars().begin(), train.cars().end(),
                                        [](const simulation::CarState& car) { return car.definition && car.definition->engine; });
        auto settings = unit.traffic_settings ? *unit.traffic_settings : simulation::default_traffic_settings(train, limits_->route_limit());
        const auto problem = !unit.traffic_settings && !engine ? std::string("no engine") : settings.problem();
        if (!problem.empty()) {
            std::cout << unit.name << ": not traffic: " << problem << std::endl;
            unit.role = Role::standing;
            return;
        }
        train.set_asleep(false);
        train.set_kinematic(true);
        unit.traffic = std::make_unique<simulation::TrafficDriver>(std::move(settings));
    } else if (unit.role == Role::driven && &unit == &units_.front()) {
        reattach_driver();
    }
}

TrainView::Unit* TrainView::find_unit(const std::uint64_t id) {
    for (auto& unit : units_) {
        if (unit.train && unit.train->id() == id) return &unit;
    }
    return nullptr;
}

const TrainView::Unit* TrainView::find_unit(const std::uint64_t id) const {
    for (const auto& unit : units_) {
        if (unit.train && unit.train->id() == id) return &unit;
    }
    return nullptr;
}

std::vector<TrainView::InspectedCar> TrainView::inspected_cars(const std::uint64_t train) const {
    std::vector<InspectedCar> result;
    for (const auto& unit : units_) {
        if (!unit.train || unit.train->id() != train) continue;
        for (std::size_t i = 0; i < unit.cars.size(); ++i) {
            const auto& car = unit.cars[i];
            InspectedCar c{.index = i, .shape = car.shape, .engine = car.engine};
            if (car.definition) {
                c.name = car.definition->name;
                c.mass = car.definition->mass;
                c.length = car.definition->length;
                c.width = car.definition->width;
                c.height = car.definition->height;
                if (car.definition->engine) c.library = car.definition->engine->library_module;
            }
            if (c.name.empty()) c.name = car.file;
            c.file = "TRAINS/TRAINSET/" + car.folder + "/" + car.file + (car.engine ? ".eng" : ".wag");
            if (i < unit.current.size()) {
                if (const auto body = body_pose(unit, i, false)) c.transform = body->transform;
            }
            result.push_back(std::move(c));
        }
        break;
    }
    return result;
}

std::vector<TrainView::Summary> TrainView::summaries() const {
    std::vector<Summary> result;
    for (const auto& unit : units_) {
        if (!unit.train) continue;
        result.push_back({.id = unit.train->id(),
                          .name = unit.name,
                          .role = unit.role,
                          .speed = unit.train->speed(),
                          .placed = unit.train->placed(),
                          .cars = unit.cars.size(),
                          .length = unit.train->length(),
                          .target = unit.traffic ? unit.traffic->target_speed() : -1.0f,
                          .derailed = unit.train->derailed(),
                          .asleep = unit.train->asleep()});
    }
    return result;
}

const simulation::Train* TrainView::train_by_id(const std::uint64_t id) const {
    const auto* unit = find_unit(id);
    return unit ? unit->train.get() : nullptr;
}

std::filesystem::path TrainView::default_consist_folder() {
    // The folder of the first consist the content lists, on disk.
    for (const auto& path : consist_list()) {
        if (const auto virtual_path = core::VirtualPath::parse(path)) {
            // Folder files come back as they are (a consist in a pack is
            // copied to the temporary folder first).
            if (const auto file = vfs_->real_path(*virtual_path, std::filesystem::temp_directory_path() / "openrts")) {
                return file->parent_path();
            }
        }
    }
    return {};
}

const std::vector<std::string>& TrainView::consist_list() {
    if (!consists_listed_) list_consists();
    return consists_;
}

std::string TrainView::assign_path(const std::uint64_t id, simulation::PathPlan plan) {
    if (!find_unit(id)) return "no such train";
    auto route = simulation::build_route(*network_, plan);
    if (!route.ok()) return route.error;
    dispatcher_->assign(id, std::move(plan), std::move(route));
    return {};
}

std::string TrainView::assign_route_path(const std::uint64_t id, const formats::TrainPath& path) {
    std::string error;
    auto plan = simulation::plan_from_path(*network_, path, &error);
    if (!plan) return path.file + ": " + error;
    auto result = assign_path(id, std::move(*plan));
    if (result.empty()) route_path_of_[id] = path.file;
    return result;
}

void TrainView::clear_path(const std::uint64_t id) {
    dispatcher_->clear(id);
}

std::string TrainView::set_role(const std::uint64_t id, const Role role) {
    // Driving a train of his own ends his help.
    if (helping_ && role == Role::driven && id != helping_) stop_helping();
    auto* unit = find_unit(id);
    if (!unit) return "no such train";
    // Another client's train (the network) moves by its owner's packages.
    if (unit->remote) return "another player drives this train";
    if (role == Role::driven) {
        const auto index = static_cast<std::size_t>(unit - units_.data());
        const bool host = legacy_ && index != 0 && unit->host;
        if (index != 0) {
            units_.front().role = Role::standing;
            attach_role(units_.front());
            std::swap(units_.front(), units_[index]);
        }
        units_.front().role = Role::driven;
        units_.front().traffic.reset();
        units_.front().train->set_kinematic(false);
        units_.front().train->set_asleep(false);
        if (driver_) {
            driver_->throttle = 0;
            driver_->reverser = 0;
        }
        // A part the host runs: its DLLs go on as they are.
        if (!(host && take_over_host_unit())) reattach_driver();
        follow_id_ = 0;
        consist_ = units_.front().name;
        return {};
    }
    unit->role = role;
    attach_role(*unit);
    if (role == Role::ai && unit->role != Role::ai) return "the train has no engine to drive";
    return {};
}

void TrainView::drive_unit(const std::size_t index) {
    if (index < units_.size()) {
        const auto error = set_role(units_[index].train->id(), Role::driven);
        if (!error.empty()) std::fprintf(stderr, "drive train %zu: %s\n", index + 1, error.c_str());
    }
}

std::string TrainView::remove(const std::uint64_t id) {
    auto* unit = find_unit(id);
    if (!unit) return "no such train";
    if (unit == &units_.front()) return "make another train the driven one first";
    dispatcher_->clear(id);
    unit->train->vacate();
    const bool host = unit->host;
    retire_sounds(*unit);
    units_.erase(units_.begin() + (unit - units_.data()));
    if (host) reconsist_host();
    if (follow_id_ == id) follow_id_ = 0;
    return {};
}

std::string TrainView::uncouple_at(const std::uint64_t id, const std::size_t index) {
    auto* unit = find_unit(id);
    if (!unit) return "no such train";
    unit->train->set_asleep(false);
    unit->still = 0.0f;
    if (std::abs(unit->train->speed()) > 0.1) return "stop the train to uncouple";
    if (index == 0 || index >= unit->cars.size()) return "no coupling there";
    split(static_cast<std::size_t>(unit - units_.data()), index, true, true);
    return {};
}

void TrainView::number_cars(Unit& unit) {
    // RTS 0x4EF740: a train put on the track takes the next number of a
    // global counter (bits 18 and up), the record it belongs to (bits 10..17;
    // none in single player: 0, PROBABLE) and each car keeps the UiD of its
    // consist entry (bits 0..9, PROBABLE).
    const auto train = ++numbered_trains_;
    for (auto& car : unit.cars) car.number = (train << 18) | (car.uid & 0x3FFu);
}

void TrainView::collect_car_numbers(std::vector<WorldLabel>& labels, const glm::vec3& eye, const float range) const {
    // F7 (RTS 0x408574): the number on both sides of every car body, as
    // 0x45C54C writes it ("AAAA-BBBB-CCCC"), 2.64-2.8 m from the end of the
    // body that is the shape's rear, at half its height. RTS paints it on the
    // body; here a label on the side that faces the camera.
    if (!placed()) return;
    for (const auto& unit : units_) {
        if (unit.current.empty()) continue;
        for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size(); ++i) {
            const auto& car = unit.cars[i];
            if (!car.definition || car.number == 0) continue;
            const auto here = pose(unit, i);
            auto along = here.front - here.rear;
            const float span = glm::length(along);
            if (span < 0.01f) continue;
            along /= span;
            if (car.flipped) along = -along; // the shape's forward
            const glm::vec3 centre = (here.front + here.rear) * 0.5f;
            if (glm::distance(centre, eye) > range + car.definition->length) continue;
            const glm::vec3 side = glm::normalize(glm::vec3{along.z, 0.0f, -along.x});
            const float half_width = (std::max)(car.definition->width, 2.0f) * 0.5f;
            const float half_height = (std::max)(car.definition->height, 2.0f) * 0.5f;
            const glm::vec3 spot = centre - along * (car.definition->length * 0.5f - 2.72f) + glm::vec3{0.0f, half_height, 0.0f};
            char text[32];
            std::snprintf(text, sizeof text, "%04u-%04u-%04u", car.number >> 18, (car.number >> 10) & 0xFFu, car.number & 0x3FFu);
            for (const float s : {1.0f, -1.0f}) {
                const auto position = spot + side * (half_width * s);
                const auto towards = eye - position;
                if (glm::dot(side * s, towards) <= 0.0f) continue; // the far side
                const float distance = glm::length(towards);
                if (distance > range) continue;
                // Full up to half the range, then fading out.
                const float fade = std::clamp(2.0f - 2.0f * distance / range, 0.0f, 1.0f);
                const auto alpha = static_cast<std::uint32_t>(fade * 255.0f);
                labels.push_back({.position = position, .text = text, .colour = (alpha << 24) | 0x00f0f0f0u});
            }
        }
    }
}

std::string TrainView::consist_layout(const std::string_view consist, ConsistLayout& layout) const {
    Unit unit;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    if (auto error = make_unit(consist, unit, stock); !error.empty()) return error;
    layout = {.name = unit.name, .length = unit.train->length()};
    // Car centres as the train lays them (Train::placements): offsets from
    // the last car's centre.
    const auto& cars = unit.train->cars();
    if (cars.empty()) return "the consist has no cars";
    const double head = cars.front().offset + cars.front().definition->length * 0.5;
    for (std::size_t i = 0; i < cars.size(); ++i) {
        const float length = cars[i].definition->length;
        layout.lengths.push_back(length);
        layout.fronts.push_back(head - (cars[i].offset + length * 0.5));
        layout.engines.push_back(cars[i].definition->engine.has_value());
    }
    return {};
}

std::string TrainView::spawn(const std::string_view consist, const simulation::TrackLocation& where, const Role role,
                             const std::optional<simulation::TrainCategory> category, const std::optional<int> start_state) {
    const bool first = units_.empty();
    Unit unit;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    if (auto error = make_unit(consist, unit, stock); !error.empty()) return error;
    std::string error;
    if (!unit.train->place(*network_, where.node, where.offset, where.forward, &error)) return error;
    unit.category = category;
    unit.start_state = start_state;
    // Cold: no air anywhere, the main reservoirs too (the compressors and
    // the valve fill it once the engine is brought to life).
    if (start_state && *start_state == 0) {
        for (std::size_t i = 0; i < unit.train->cars().size(); ++i) unit.train->set_air(i, {});
    }
    for (auto& definition : stock) stock_.push_back(std::move(definition));
    number_cars(unit);
    unit.name += " #" + std::to_string(++spawned_);
    unit.role = role == Role::driven ? Role::standing : role;
    unit.settle();
    // A consist put down to stand is STATIC from the start (RTS's static
    // consists do not roll away).
    if (unit.role == Role::standing) unit.train->set_asleep(true);
    units_.push_back(std::move(unit));
    attach_role(units_.back());
    // Without a train of the player's own (the menu's "no train"), the first
    // one put on the route becomes theirs. The camera stays the free one it
    // was put down from: following it from behind (the default with no start
    // view) left it unable to move, only to turn (testers, 2026-11-18).
    if (first) {
        follow_ = false;
        return set_role(units_.front().train->id(), Role::driven);
    }
    return {};
}

// rts1.exe 0x50D3B4's inputs per diesel exhaust: what a DLL sets in its
// ExhaustContr, else the .eng's values at the engine's load: the throttle of
// the player's train without a DLL, idling (0.1) for the others (0x50B430,
// 0x424488: AI trains run it by their pull, here kept at idle).
void TrainView::collect_exhausts(std::vector<ExhaustSource>& out) const {
    for (std::size_t u = 0; u < units_.size(); ++u) {
        const auto& unit = units_[u];
        if (!unit.train || unit.current.empty()) continue;
        const auto& states = unit.train->cars();
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size() && i < unit.current.size(); ++i) {
            const auto& car = unit.cars[i];
            const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
            if (!engine || engine->kind != formats::EngineKind::diesel || engine->exhausts.empty()) continue;
            const auto body = body_pose(unit, i);
            if (!body) continue;
            const auto* out_state = dll_output(u, i);
            float load = 0.1f;
            if (u == 0 && unit.role == Role::driven && !legacy_ && driver_ && driver_->notches > 0) {
                load = static_cast<float>(driver_->throttle) / static_cast<float>(driver_->notches);
            } else if (unit.traffic) {
                load = unit.traffic->throttle(); // RTS: traffic diesels by its seeming throttle
            }
            const auto& state = states[i];
            const glm::vec3 along = glm::normalize(glm::vec3{body->transform[2]});
            for (std::size_t k = 0; k < engine->exhausts.size(); ++k) {
                const auto& exhaust = engine->exhausts[k];
                ExhaustSource source;
                source.key = (static_cast<std::uint64_t>(unit.train->id()) << 32) | (static_cast<std::uint64_t>(i) << 8) | k;
                source.position = glm::vec3{body->transform * glm::vec4{exhaust.position[0], exhaust.position[1], exhaust.position[2], 1.0f}};
                source.direction = glm::vec3{body->transform * glm::vec4{exhaust.direction[0], exhaust.direction[1], exhaust.direction[2], 0.0f}};
                // CarState::velocity is towards the train front; the body's +z the car's own front.
                source.velocity = along * (state.velocity * (state.flipped ? -1.0f : 1.0f));
                source.acceleration = state.acceleration;
                source.nozzle = exhaust.size;
                source.colour = exhaust.colour;
                source.mass = exhaust.mass;
                if (out_state && k < out_state->exhausts.size()) {
                    const auto& dll = out_state->exhausts[k];
                    source.rate = dll.rate;
                    source.magnitude = dll.magnitude;
                    source.dispersion = dll.dispersion;
                    source.mass = dll.mass;
                    source.colour = dll.colour;
                } else {
                    const auto mix = [load](const std::array<float, 2>& range) {
                        return load <= 0.11f ? range[0] * load * 10.0f : range[0] + (range[1] - range[0]) * load;
                    };
                    source.rate = mix(exhaust.rate);
                    source.magnitude = mix(exhaust.magnitude);
                    source.dispersion = exhaust.dispersion;
                }
                if (!exhaust.texture.empty()) {
                    if (const auto folder = core::VirtualPath::parse("TRAINS/TRAINSET/" + car.folder)) {
                        if (auto file = folder->resolve(exhaust.texture)) source.texture = *file;
                    }
                }
                if (source.texture.empty()) source.texture = *core::VirtualPath::parse("GLOBAL/TEXTURES/t_diesel_smoke.ace");
                out.push_back(std::move(source));
            }
        }
        // Wooden shoes smouldering or alight smoke at their bogie (about
        // 0.3 of the car's length from its middle, at the wheels' height).
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size() && i < unit.current.size(); ++i) {
            const auto& state = states[i];
            for (std::size_t b = 0; b < 2; ++b) {
                const auto& shoe = state.shoe_state[b];
                const bool smoulders = state.shoes[b] == formats::BrakeShoe::wood && shoe.heat >= simulation::wood_smoulders && shoe.wear < 1.0f;
                if (!shoe.burning && !smoulders) continue;
                const auto body = body_pose(unit, i);
                if (!body || !state.definition) continue;
                const float z = (b == 0 ? 1.0f : -1.0f) * 0.3f * state.definition->length;
                const glm::vec3 along = glm::normalize(glm::vec3{body->transform[2]});
                ExhaustSource source;
                source.key = (static_cast<std::uint64_t>(unit.train->id()) << 32) | (static_cast<std::uint64_t>(i) << 8) | (0xF0u + b);
                source.position = glm::vec3{body->transform * glm::vec4{0.0f, 0.7f, z, 1.0f}};
                source.direction = glm::vec3{body->transform * glm::vec4{0.0f, 1.0f, 0.0f, 0.0f}};
                source.velocity = along * (state.velocity * (state.flipped ? -1.0f : 1.0f));
                source.acceleration = state.acceleration;
                source.nozzle = 0.3f;
                source.rate = shoe.burning ? 12.0f : 4.0f;
                source.magnitude = shoe.burning ? 2.0f : 1.0f;
                source.dispersion = 0.6f;
                source.mass = 0.5f;
                source.colour = shoe.burning ? 0xD0302820u : 0x90B0B0B0u;
                source.texture = *core::VirtualPath::parse("GLOBAL/TEXTURES/t_diesel_smoke.ace");
                out.push_back(std::move(source));
            }
        }
    }
}

namespace {

// The brakes' presets (TrainView::BrakeSet): RTS's model as RTS has it; the
// realistic air with openRTS's valve over Brakes.dll, the model's gauges,
// the weather's rails, the brake force at most real stock's and the
// distributors' stability.
// The realistic set has brakes in service (condition 6, testers'
// proposal); RTS's has no wear.
constexpr TrainView::BrakeSet rts_brakes{.realistic = false, .dll_air = 0, .gauges = false, .rails = 0, .limit = false, .stability = false,
                                         .condition = 0, .pressing = 100};
constexpr TrainView::BrakeSet real_brakes{.realistic = true, .dll_air = 2, .gauges = true, .rails = 1, .limit = true, .stability = true,
                                          .condition = 6, .pressing = 100};

[[nodiscard]] std::string brake_set_code(const TrainView::BrakeSet& s) {
    return std::to_string(s.realistic ? 1 : 0) + ',' + std::to_string(s.dll_air) + ',' + std::to_string(s.gauges ? 1 : 0) + ',' +
           std::to_string(s.rails) + ',' + std::to_string(s.limit ? 1 : 0) + ',' + std::to_string(s.stability ? 1 : 0) + ',' +
           std::to_string(s.condition) + ',' + std::to_string(s.pressing);
}

// Six figures (before the condition and pressing, 2026-11-18) or eight.
[[nodiscard]] std::optional<TrainView::BrakeSet> brake_set_from(const std::string& code) {
    int v[8]{0, 0, 0, 0, 0, 0, 0, 100};
    std::istringstream fields(code);
    char comma = ',';
    int read = 0;
    for (int k = 0; k < 8; ++k) {
        if (k > 0 && !(fields >> comma)) break;
        if (comma != ',' || !(fields >> v[k])) return std::nullopt;
        ++read;
    }
    if (read != 6 && read != 8) return std::nullopt;
    return TrainView::BrakeSet{.realistic = v[0] != 0, .dll_air = std::clamp(v[1], 0, 2), .gauges = v[2] != 0,
                               .rails = std::clamp(v[3], 0, 6), .limit = v[4] != 0, .stability = v[5] != 0,
                               .condition = std::clamp(v[6], 0, 10), .pressing = std::clamp(v[7], 50, 150)};
}

} // namespace

TrainView::BrakeSet TrainView::brake_set() const noexcept {
    return {.realistic = realistic_air_, .dll_air = dll_air_, .gauges = gauges_air_, .rails = rails_, .limit = limit_brake_force_,
            .stability = distributor_stability_, .condition = brake_condition_, .pressing = brake_pressing_};
}

void TrainView::apply_brake_set(const BrakeSet& set) {
    // The realistic air is the only model: the preset's flag only marks it.
    realistic_air_ = true;
    dll_air_ = set.dll_air;
    dll_air_loaded_ = true;
    remember("dll_air", std::to_string(dll_air_));
    if (legacy_) legacy_->set_air(static_cast<trainplugin::DllAir>(dll_air_));
    gauges_air_ = set.gauges;
    gauges_air_loaded_ = true;
    remember("gauges_openrts_air", gauges_air_ ? "1" : "0");
    rails_ = set.rails;
    limit_brake_force_ = set.limit;
    rails_loaded_ = true;
    remember("rails", std::to_string(rails_));
    remember("limit_brake_force", limit_brake_force_ ? "1" : "0");
    distributor_stability_ = set.stability;
    distributor_stability_loaded_ = true;
    remember("distributor_stability", distributor_stability_ ? "1" : "0");
    for (auto& unit : units_)
        if (unit.train) unit.train->set_distributor_stability(distributor_stability_);
    brake_condition_ = set.condition;
    brake_pressing_ = set.pressing;
    remember("brake_condition", std::to_string(brake_condition_));
    remember("brake_pressing", std::to_string(brake_pressing_));
}

std::filesystem::path TrainView::situation_folder() const {
    return user_data_directory() / "situations" / std::string(route_.filename());
}

std::vector<std::string> TrainView::situations() const {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(situation_folder(), error)) {
        if (entry.path().extension() != ".situation") continue;
        const auto stem = entry.path().stem().u8string();
        names.emplace_back(stem.begin(), stem.end());
    }
    std::sort(names.begin(), names.end());
    return names;
}

namespace {

[[nodiscard]] const char* role_key(const TrainView::Role role) {
    return role == TrainView::Role::driven ? "driven" : role == TrainView::Role::ai ? "ai" : "standing";
}

[[nodiscard]] std::string clean_file_name(const std::string& name) {
    std::string clean;
    for (const char c : name) clean += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
    return clean;
}

} // namespace

// A text file: a [train] section per train, front first; `front` is its
// front end on the track network (node, offset, facing), `car` a car as a
// consist entry (folder, file, engine, flipped, UiD), front to back.
std::string TrainView::save_situation(const std::string& name) const {
    const auto clean = clean_file_name(name);
    if (clean.empty()) return tr("give the situation a name");
    if (units_.empty()) return tr("there are no trains to save");
    std::error_code fs_error;
    std::filesystem::create_directories(situation_folder(), fs_error);
    const auto file = situation_folder() / std::filesystem::path(std::u8string(clean.begin(), clean.end())).replace_extension(".situation");
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) return "cannot write " + file.string();
    output << "# openRTS situation: the trains on the route as they stood\n";
    output << "version=1\nroute=" << route_.filename() << '\n';
    std::size_t saved = 0;
    for (const auto& unit : units_) {
        const auto front = unit.train ? unit.train->front_end() : std::nullopt;
        if (!front) continue;
        output << "\n[train]\nname=" << unit.name << "\nrole=" << role_key(unit.role) << "\nfront=" << front->node() << ' '
               << std::setprecision(17) << front->offset() << ' ' << (front->forward() ? 1 : 0) << '\n';
        const auto& states = unit.train->cars();
        for (std::size_t k = 0; k < unit.cars.size(); ++k) {
            const auto& car = unit.cars[k];
            output << "car=" << car.folder << '\t' << car.file << '\t' << (car.engine ? 1 : 0) << '\t' << (car.flipped ? 1 : 0) << '\t'
                   << car.uid;
            // The bogies' brake shoes (the consist's ORTSBrakeShoes).
            if (k < states.size()) {
                output << '\t' << formats::brake_shoe_name(states[k].shoes[0]) << ',' << formats::brake_shoe_name(states[k].shoes[1]);
            }
            output << '\n';
        }
        ++saved;
    }
    if (!output) return "cannot write " + file.string();
    if (saved == 0) return tr("there are no trains on the track to save");
    return {};
}

namespace {

[[nodiscard]] std::string base64_encode(const std::vector<std::uint8_t>& data) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0)
                                | (i + 2 < data.size() ? data[i + 2] : 0);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < data.size() ? table[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? table[n & 63] : '=';
    }
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> base64_decode(const std::string_view text) {
    const auto value = [](const char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) continue;
        bits = (bits << 6) | static_cast<std::uint32_t>(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<std::uint8_t>((bits >> count) & 0xFF));
        }
    }
    return out;
}

} // namespace

namespace {
// Defined with the scenario's traffic below.
[[nodiscard]] std::uint32_t block_key(std::uint32_t service, std::int32_t point);
} // namespace

// The [trains] part of a saved game: per train its situation lines (name,
// role, front, the cars with their keys), its scenario and path lines, and
// its running state as "state=" lines; then the DLLs' states and the
// engineer.
std::string TrainView::save_game() const {
    std::ostringstream out;
    out << std::setprecision(17);
    for (const auto& unit : units_) {
        if (unit.remote || !unit.train) continue;
        const auto front = unit.train->front_end();
        if (!front) continue;
        out << "[train]\nname=" << unit.name << "\nrole=" << role_key(unit.role) << "\nfront=" << front->node() << ' ' << front->offset()
            << ' ' << (front->forward() ? 1 : 0) << '\n';
        const auto& states = unit.train->cars();
        for (std::size_t k = 0; k < unit.cars.size(); ++k) {
            const auto& car = unit.cars[k];
            out << "car=" << car.folder << '\t' << car.file << '\t' << (car.engine ? 1 : 0) << '\t' << (car.flipped ? 1 : 0) << '\t'
                << car.uid << '\t';
            if (k < states.size()) out << formats::brake_shoe_name(states[k].shoes[0]) << ',' << formats::brake_shoe_name(states[k].shoes[1]);
            out << '\t' << car.key << '\n';
        }
        if (unit.activity_id) out << "service=" << unit.activity_id->first << ' ' << unit.activity_id->second << '\n';
        out << "traffic=" << (unit.scenario_traffic ? 1 : 0) << ' ' << (unit.remove_at_end ? 1 : 0) << ' ' << (unit.force_removal ? 1 : 0)
            << ' ' << (unit.remove_time ? std::to_string(*unit.remove_time) : std::string("-")) << ' ' << unit.wait_here << ' '
            << unit.wait_left << '\n';
        const auto id = unit.train->id();
        if (const auto* progress = dispatcher_->progress(id); progress && dispatcher_->route(id)) {
            const auto path = route_path_of_.find(id);
            out << "route=" << progress->leg << ' ' << progress->next_mark << '\n';
            if (path != route_path_of_.end() && !unit.activity_id) out << "path=" << path->second << '\n';
        }
        simulation::StateWriter state;
        unit.train->save_state(state);
        std::istringstream lines(state.text());
        for (std::string line; std::getline(lines, line);) out << "state=" << line << '\n';
    }
    if (legacy_) {
        std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> dlls;
        if (legacy_->save_state(dlls)) {
            out << "[dll]\n";
            for (const auto& [key, bytes] : dlls) out << "car=" << key << ' ' << base64_encode(bytes) << '\n';
        }
    }
    out << "[engineer]\nin_cab=" << (engineer_.inside() && engineer_car_ == cab_car_ ? 1 : 0) << '\n';
    if (activity_) {
        // The scenario's traffic: which services started (or were given
        // up), which are held, and the block points' states.
        out << "[scenario]\n";
        for (std::size_t i = 0; i < traffic_plans_.size(); ++i)
            out << "plan=" << i << ' ' << (traffic_plans_[i].started ? 1 : 0) << ' ' << (traffic_plans_[i].waiting ? 1 : 0) << '\n';
        for (const auto service : services_held_) out << "held=" << service << '\n';
        for (std::size_t i = 0; i < activity_block_list_.size(); ++i) {
            const auto& block = activity_block_list_[i];
            const auto key = block_key(block.service, block.point->id);
            out << "block=" << i << ' ' << (block.released ? 1 : 0) << ' ' << (dispatcher_->block_enabled(key) ? 1 : 0) << '\n';
        }
    }
    return out.str();
}

std::string TrainView::load_game(const std::string& text) {
    std::vector<PlacedTrain> trains;
    std::vector<SavedTrain> extras;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> dlls;
    std::set<std::uint32_t> held;
    bool in_cab = false;
    std::string section;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with('[')) {
            section = line;
            if (section == "[train]") {
                trains.emplace_back();
                extras.emplace_back();
            }
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (section == "[dll]" && key == "car") {
            const auto space = value.find(' ');
            if (space == std::string::npos) continue;
            dlls.emplace_back(static_cast<std::uint32_t>(std::strtoul(value.substr(0, space).c_str(), nullptr, 10)),
                              base64_decode(std::string_view(value).substr(space + 1)));
            continue;
        }
        if (section == "[engineer]" && key == "in_cab") {
            in_cab = value == "1";
            continue;
        }
        if (section == "[scenario]") {
            std::istringstream fields(value);
            if (key == "plan") {
                std::size_t i{};
                int started = 0, waiting = 0;
                if (fields >> i >> started >> waiting && i < traffic_plans_.size()) {
                    traffic_plans_[i].started = started != 0;
                    traffic_plans_[i].waiting = waiting != 0;
                }
            } else if (key == "held") {
                std::uint32_t service{};
                if (fields >> service) held.insert(service);
            } else if (key == "block") {
                std::size_t i{};
                int released = 0, enabled = 1;
                if (fields >> i >> released >> enabled && i < activity_block_list_.size()) {
                    auto& block = activity_block_list_[i];
                    block.released = released != 0;
                    dispatcher_->enable_block(block_key(block.service, block.point->id), enabled != 0);
                }
            }
            continue;
        }
        if (section != "[train]" || trains.empty()) continue;
        auto& train = trains.back();
        auto& extra = extras.back();
        if (key == "name") {
            train.name = value;
        } else if (key == "role") {
            train.role = value == "driven" ? Role::driven : value == "ai" ? Role::ai : Role::standing;
        } else if (key == "front") {
            std::istringstream fields(value);
            simulation::TrackLocation where;
            int forward = 1;
            if (fields >> where.node >> where.offset >> forward) {
                where.forward = forward != 0;
                train.front = where;
            }
        } else if (key == "car") {
            std::vector<std::string> fields;
            std::size_t start = 0;
            for (;;) {
                const auto tab = value.find('\t', start);
                fields.push_back(value.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
            if (fields.size() < 5) continue;
            formats::ConsistEntry entry;
            entry.folder = fields[0];
            entry.name = fields[1];
            entry.engine = fields[2] == "1";
            entry.flipped = fields[3] == "1";
            entry.uid = static_cast<std::uint32_t>(std::strtoul(fields[4].c_str(), nullptr, 10));
            if (fields.size() > 5 && !fields[5].empty()) {
                const auto comma = fields[5].find(',');
                entry.shoes[0] = formats::brake_shoe_from(fields[5].substr(0, comma));
                entry.shoes[1] = comma == std::string::npos ? entry.shoes[0] : formats::brake_shoe_from(fields[5].substr(comma + 1));
            }
            const auto car_key = fields.size() > 6 ? static_cast<std::uint32_t>(std::strtoul(fields[6].c_str(), nullptr, 10)) : 0u;
            train.cars.push_back(std::move(entry));
            train.keys.push_back(car_key);
            if (train.keys.size() == 1) extra.first_key = car_key;
        } else if (key == "service") {
            std::istringstream fields(value);
            std::uint32_t service{}, number{};
            if (fields >> service >> number) train.activity_id = std::pair{service, number};
        } else if (key == "traffic") {
            std::istringstream fields(value);
            int scenario = 0, remove = 1, force = 0;
            std::string remove_time;
            fields >> scenario >> remove >> force >> remove_time >> extra.wait_here >> extra.wait_left;
            extra.scenario_traffic = scenario != 0;
            extra.remove_at_end = remove != 0;
            extra.force_removal = force != 0;
            if (!remove_time.empty() && remove_time != "-") extra.remove_time = std::atoi(remove_time.c_str());
        } else if (key == "route") {
            std::istringstream fields(value);
            if (fields >> extra.leg >> extra.mark) extra.has_route = true;
        } else if (key == "path") {
            extra.path = value;
        } else if (key == "state") {
            extra.state += value + '\n';
        }
    }
    std::erase_if(trains, [](const PlacedTrain& t) { return t.cars.empty(); });
    std::erase_if(extras, [](const SavedTrain& e) { return e.first_key == 0 && e.state.empty(); });
    if (trains.empty()) return tr("the save has no trains");
    // The saved keys come back; the cars made from now on take keys above them.
    for (const auto& train : trains)
        for (const auto k : train.keys) reserve_car_keys(k);
    pending_game_ = std::move(extras);
    pending_dlls_ = std::move(dlls);
    pending_dll_tries_ = 0;
    pending_engineer_in_cab_ = in_cab;
    if (activity_) services_held_ = std::move(held);
    request_trains(std::move(trains), tr("saved game"));
    return {};
}

// ---------------------------------------------------------------------------
// Placements (docs/placements.md)

std::filesystem::path TrainView::placement_folder() const {
    return user_data_directory() / "placements" / std::string(route_.filename());
}

std::vector<std::string> TrainView::placements() const {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(placement_folder(), error)) {
        if (entry.path().extension() != ".placement") continue;
        const auto stem = entry.path().stem().u8string();
        names.emplace_back(stem.begin(), stem.end());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void TrainView::select_at(const glm::vec3& eye, const glm::mat4& view_projection, const glm::vec2 aim, const bool ctrl, const bool shift) {
    // The car whose length on the screen passes nearest the click (a car
    // 1.5 m above its rails), the nearer of two alike.
    const auto screen = [&](const glm::vec3& p) -> std::optional<glm::vec2> {
        const glm::vec4 clip = view_projection * glm::vec4{p, 1.0f};
        if (clip.w <= 0.1f) return std::nullopt;
        return glm::vec2{clip.x, clip.y} / clip.w;
    };
    std::optional<std::pair<std::size_t, std::size_t>> best;
    float best_score = 0.03f;
    for (std::size_t u = 0; u < units_.size(); ++u) {
        const auto& unit = units_[u];
        if (!unit.train || unit.remote || unit.current.empty()) continue;
        for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size(); ++i) {
            const auto here = pose(unit, i);
            const glm::vec3 up{0.0f, 1.5f, 0.0f};
            const auto a = screen(here.front + up);
            const auto b = screen(here.rear + up);
            if (!a || !b) continue;
            const glm::vec2 ab = *b - *a;
            const float t = glm::dot(ab, ab) > 1e-12f ? std::clamp(glm::dot(aim - *a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
            const float off = glm::distance(aim, *a + ab * t);
            const float score = off + 0.00002f * glm::distance(eye, (here.front + here.rear) * 0.5f);
            if (score < best_score) {
                best_score = score;
                best = std::pair{u, i};
            }
        }
    }
    if (!best) {
        if (!ctrl && !shift) selection_.clear();
        return;
    }
    const auto& unit = units_[best->first];
    const auto id = unit.train->id();
    std::set<std::uint32_t> all;
    for (const auto& car : unit.cars) all.insert(car.key);
    if (shift) {
        // One car of a train picked already (coupled in it).
        const auto it = selection_.find(id);
        if (it == selection_.end()) return;
        const auto key = unit.cars[best->second].key;
        if (!it->second.erase(key)) it->second.insert(key);
        if (it->second.empty()) selection_.erase(it);
    } else if (ctrl) {
        if (!selection_.erase(id)) selection_[id] = std::move(all);
    } else {
        selection_.clear();
        selection_[id] = std::move(all);
    }
}

namespace {

// A car's box: its ends on the rails, 3.2 m wide, 0.5 .. 4.3 m up.
void add_car_box(const glm::vec3& front, const glm::vec3& rear, const std::uint32_t colour, std::vector<renderer::WideLine>& lines) {
    glm::vec3 along = front - rear;
    glm::vec3 side = glm::cross(along, glm::vec3{0.0f, 1.0f, 0.0f});
    if (glm::length(side) < 1e-4f) return;
    side = glm::normalize(side) * 1.6f;
    const glm::vec3 low{0.0f, 0.5f, 0.0f}, high{0.0f, 4.3f, 0.0f};
    const glm::vec3 c[8] = {front + side + low, front - side + low, rear - side + low, rear + side + low,
                            front + side + high, front - side + high, rear - side + high, rear + side + high};
    static constexpr int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) lines.push_back({c[e[0]], c[e[1]], colour, 2.0f});
}

// The least distance between segments ab and cd.
[[nodiscard]] float segment_distance(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d) {
    const glm::vec3 u = b - a, v = d - c, w = a - c;
    const float uu = glm::dot(u, u), uv = glm::dot(u, v), vv = glm::dot(v, v), uw = glm::dot(u, w), vw = glm::dot(v, w);
    const float den = uu * vv - uv * uv;
    float s = den > 1e-9f ? std::clamp((uv * vw - vv * uw) / den, 0.0f, 1.0f) : 0.0f;
    float t = vv > 1e-9f ? std::clamp((uv * s + vw) / vv, 0.0f, 1.0f) : 0.0f;
    s = uu > 1e-9f ? std::clamp((uv * t - uw) / uu, 0.0f, 1.0f) : 0.0f;
    return glm::distance(a + u * s, c + v * t);
}

} // namespace

void TrainView::add_placement_lines(std::vector<renderer::WideLine>& lines) const {
    for (const auto& unit : units_) {
        if (!unit.train) continue;
        const auto it = selection_.find(unit.train->id());
        if (it == selection_.end()) continue;
        for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size(); ++i) {
            if (!it->second.contains(unit.cars[i].key)) continue;
            const auto here = pose(unit, i);
            add_car_box(here.front, here.rear, 0xff00e0ffu, lines);
        }
    }
    const std::uint32_t colour = preview_blocked_ ? 0xff3030ffu : 0xff40ff40u;
    for (const auto& unit : preview_units_) {
        for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size(); ++i) {
            const auto here = pose(unit, i);
            add_car_box(here.front, here.rear, colour, lines);
        }
    }
}

// Each picked train's runs of picked cars as trains of their own, standing:
// a run's front where its first car's front stands, the cars as a
// situation writes them (no keys: they are new each time it is put down).
std::string TrainView::save_placement(const std::string& name) const {
    const auto clean = clean_file_name(name);
    if (clean.empty()) return tr("give the placement a name");
    if (selection_.empty()) return tr("pick trains first (a click in the free flight)");
    std::ostringstream out;
    out << std::setprecision(17) << "# openRTS placement: trains to put down beside the others\nversion=1\nroute=" << route_.filename()
        << '\n';
    std::size_t trains = 0, cars = 0;
    for (const auto& unit : units_) {
        if (!unit.train) continue;
        const auto picked = selection_.find(unit.train->id());
        if (picked == selection_.end()) continue;
        const auto front = unit.train->front_end();
        const auto& states = unit.train->cars();
        if (!front || states.size() != unit.cars.size()) continue;
        const auto length_of = [&](const std::size_t i) { return unit.cars[i].definition ? unit.cars[i].definition->length : 0.0f; };
        const double head = states[0].offset + length_of(0) * 0.5;
        for (std::size_t i = 0; i < unit.cars.size();) {
            if (!picked->second.contains(unit.cars[i].key)) {
                ++i;
                continue;
            }
            // From the train's front back to this run's first car's front.
            auto t = *front;
            const double back = head - (states[i].offset + length_of(i) * 0.5);
            if (back > 1e-3) {
                t.reverse();
                t.move(back);
                t.reverse();
            }
            out << "\n[train]\nname=" << unit.name << "\nrole=standing\nfront=" << t.node() << ' ' << t.offset() << ' '
                << (t.forward() ? 1 : 0) << '\n';
            for (; i < unit.cars.size() && picked->second.contains(unit.cars[i].key); ++i) {
                const auto& car = unit.cars[i];
                out << "car=" << car.folder << '\t' << car.file << '\t' << (car.engine ? 1 : 0) << '\t' << (car.flipped ? 1 : 0) << '\t'
                    << car.uid << '\t' << formats::brake_shoe_name(states[i].shoes[0]) << ',' << formats::brake_shoe_name(states[i].shoes[1])
                    << '\n';
                ++cars;
            }
            ++trains;
        }
    }
    if (trains == 0) return tr("pick trains first (a click in the free flight)");
    std::error_code fs_error;
    std::filesystem::create_directories(placement_folder(), fs_error);
    const auto file = placement_folder() / std::filesystem::path(std::u8string(clean.begin(), clean.end())).replace_extension(".placement");
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << out.str();
    if (!output) return "cannot write " + file.string();
    return trf("placement %s saved: %zu trains, %zu cars", clean.c_str(), trains, cars);
}

std::string TrainView::preview_placement(const std::filesystem::path& file) {
    preview_units_.clear();
    preview_stock_.clear();
    preview_blocked_ = false;
    std::ifstream input(file, std::ios::binary);
    if (!input) return "cannot read " + file.string();
    struct Read {
        std::string name;
        std::optional<simulation::TrackLocation> front;
        std::vector<formats::ConsistEntry> cars;
    };
    std::vector<Read> trains;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[train]") {
            trains.emplace_back();
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos || trains.empty() || line.starts_with('#')) continue;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        auto& train = trains.back();
        if (key == "name") {
            train.name = value;
        } else if (key == "front") {
            std::istringstream fields(value);
            simulation::TrackLocation where;
            int forward = 1;
            if (fields >> where.node >> where.offset >> forward) {
                where.forward = forward != 0;
                train.front = where;
            }
        } else if (key == "car") {
            std::vector<std::string> fields;
            std::size_t start = 0;
            for (;;) {
                const auto tab = value.find('\t', start);
                fields.push_back(value.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
            if (fields.size() < 5) continue;
            formats::ConsistEntry entry;
            entry.folder = fields[0];
            entry.name = fields[1];
            entry.engine = fields[2] == "1";
            entry.flipped = fields[3] == "1";
            entry.uid = static_cast<std::uint32_t>(std::strtoul(fields[4].c_str(), nullptr, 10));
            if (fields.size() > 5 && !fields[5].empty()) {
                const auto comma = fields[5].find(',');
                entry.shoes[0] = formats::brake_shoe_from(fields[5].substr(0, comma));
                entry.shoes[1] = comma == std::string::npos ? entry.shoes[0] : formats::brake_shoe_from(fields[5].substr(comma + 1));
            }
            train.cars.push_back(std::move(entry));
        }
    }
    std::size_t missing = 0;
    for (auto& read : trains) {
        if (!read.front || read.cars.empty()) continue;
        Unit unit;
        if (const auto error = make_unit_from(read.name, read.cars, unit, preview_stock_); !error.empty()) {
            std::cout << "placement: " << read.name << ": " << error << std::endl;
            ++missing;
            continue;
        }
        std::string error;
        if (!unit.train->place(*network_, read.front->node, read.front->offset, read.front->forward, &error)) {
            std::cout << "placement: " << read.name << ": " << error << std::endl;
            ++missing;
            continue;
        }
        unit.settle();
        preview_units_.push_back(std::move(unit));
    }
    if (preview_units_.empty()) return tr("none of the placement's trains could be put down (see openrts.log)");
    // The track taken: a car of the placement within 2.5 m of a car there.
    for (const auto& unit : preview_units_) {
        for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size() && !preview_blocked_; ++i) {
            const auto p = pose(unit, i);
            for (const auto& other : units_) {
                for (std::size_t k = 0; k < other.cars.size() && k < other.current.size(); ++k) {
                    const auto q = pose(other, k);
                    if (segment_distance(p.front, p.rear, q.front, q.rear) < 2.5f) {
                        preview_blocked_ = true;
                        break;
                    }
                }
                if (preview_blocked_) break;
            }
        }
    }
    const auto stem = file.stem().u8string();
    preview_name_.assign(stem.begin(), stem.end());
    std::string text = trf("%zu trains shown where they will stand", preview_units_.size());
    if (missing > 0) text += "; " + trf("%zu could not be made", missing);
    if (preview_blocked_) text += "; " + std::string(tr("the track there is taken"));
    return text;
}

std::string TrainView::place_preview() {
    if (preview_units_.empty()) return tr("show a placement first");
    if (preview_blocked_) return tr("the track there is taken: move the trains standing there first");
    const bool first = units_.empty();
    const auto count = preview_units_.size();
    for (auto& definition : preview_stock_) stock_.push_back(std::move(definition));
    preview_stock_.clear();
    for (auto& unit : preview_units_) {
        number_cars(unit);
        unit.name += " #" + std::to_string(++spawned_);
        unit.role = Role::standing;
        unit.settle();
        unit.train->set_asleep(true);
        units_.push_back(std::move(unit));
        attach_role(units_.back());
    }
    preview_units_.clear();
    if (first) {
        if (const auto error = set_role(units_.front().train->id(), Role::driven); !error.empty()) return error;
    }
    return trf("placement %s put down: %zu trains", preview_name_.c_str(), count);
}

void TrainView::draw_placements() {
    if (!ImGui::TreeNode(tr("Placements"))) return;
    ImGui::Checkbox(tr("Pick trains with the mouse (free flight)"), &selecting_);
    ImGui::TextDisabled("%s", tr("(a click: a train; Ctrl+click: one more or one less; Shift+click: a car of a picked train, out or back)"));
    std::size_t cars = 0;
    for (const auto& [_, keys] : selection_) cars += keys.size();
    ImGui::Text(tr("Picked: %zu trains, %zu cars"), selection_.size(), cars);
    if (!selection_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear##placement"))) selection_.clear();
    }
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##placement_name", tr("name"), placement_name_.data(), placement_name_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(selection_.empty());
    if (ImGui::Button(tr("Save the picked trains"))) message_ = save_placement(placement_name_.data());
    ImGui::EndDisabled();
    const auto saved = placements();
    if (saved.empty()) {
        ImGui::TextDisabled("%s", tr("No placements on this route yet."));
    } else {
        placement_choice_ = std::clamp(placement_choice_, 0, static_cast<int>(saved.size()) - 1);
        if (ImGui::BeginListBox("##placements", ImVec2(-1.0f, 110.0f))) {
            for (int i = 0; i < static_cast<int>(saved.size()); ++i)
                if (ImGui::Selectable(saved[static_cast<std::size_t>(i)].c_str(), i == placement_choice_)) placement_choice_ = i;
            ImGui::EndListBox();
        }
        const auto& chosen = saved[static_cast<std::size_t>(placement_choice_)];
        const auto file = placement_folder() / std::filesystem::path(std::u8string(chosen.begin(), chosen.end())).replace_extension(".placement");
        if (ImGui::Button(tr("Show where they stand"))) message_ = preview_placement(file);
        ImGui::SameLine();
        ImGui::BeginDisabled(preview_units_.empty() || preview_blocked_);
        if (ImGui::Button(tr("Put them down"))) message_ = place_preview();
        ImGui::EndDisabled();
        if (!preview_units_.empty()) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel##placement"))) {
                preview_units_.clear();
                preview_stock_.clear();
                message_.clear();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Delete##placement"))) {
            std::error_code fs_error;
            std::filesystem::remove(file, fs_error);
            message_ = trf("placement %s deleted", chosen.c_str());
        }
    }
    ImGui::TreePop();
}

// The saved trains stand: each one's state, scenario fields and path back;
// then the engineer. The DLLs' states wait for the host (update).
void TrainView::apply_saved_game() {
    for (auto& unit : units_) {
        if (unit.cars.empty() || !unit.train) continue;
        const auto key = unit.cars.front().key;
        const auto it = std::find_if(pending_game_.begin(), pending_game_.end(), [&](const SavedTrain& s) { return s.first_key == key; });
        if (it == pending_game_.end()) continue;
        const auto& saved = *it;
        unit.scenario_traffic = saved.scenario_traffic;
        unit.remove_at_end = saved.remove_at_end;
        unit.force_removal = saved.force_removal;
        unit.remove_time = saved.remove_time;
        unit.wait_here = saved.wait_here;
        unit.wait_left = saved.wait_left;
        const auto id = unit.train->id();
        if (saved.has_route) {
            std::string error;
            if (unit.activity_id && activity_) {
                const auto service = std::find_if(activity_->services.begin(), activity_->services.end(),
                                                  [&](const formats::ActivityService& s) { return s.id == unit.activity_id->first; });
                if (service != activity_->services.end()) error = give_activity_path(unit, *service);
            } else if (!saved.path.empty()) {
                const auto path = std::find_if(paths_.begin(), paths_.end(), [&](const formats::TrainPath& p) { return p.file == saved.path; });
                if (path != paths_.end()) error = assign_route_path(id, *path);
            }
            if (!error.empty()) std::cout << "saved game: " << unit.name << ": " << error << std::endl;
            dispatcher_->restore_progress(id, saved.leg, saved.mark);
        }
        if (!saved.state.empty()) unit.train->restore_state(simulation::StateReader(saved.state));
        unit.settle();
    }
    pending_game_.clear();
    if (pending_engineer_in_cab_ && cab_) set_engineer(true);
    pending_engineer_in_cab_ = false;
}

std::string TrainView::load_situation(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return "cannot read " + file.string();
    std::vector<PlacedTrain> saved;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[train]") {
            saved.emplace_back();
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos || saved.empty() || line.starts_with('#')) continue;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        auto& train = saved.back();
        if (key == "name") {
            train.name = value;
        } else if (key == "role") {
            train.role = value == "driven" ? Role::driven : value == "ai" ? Role::ai : Role::standing;
        } else if (key == "front") {
            std::istringstream fields(value);
            simulation::TrackLocation where;
            int forward = 1;
            if (fields >> where.node >> where.offset >> forward) {
                where.forward = forward != 0;
                train.front = where;
            }
        } else if (key == "car") {
            std::vector<std::string> fields;
            std::size_t start = 0;
            for (;;) {
                const auto tab = value.find('\t', start);
                fields.push_back(value.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
            if (fields.size() < 5) continue;
            formats::ConsistEntry entry;
            entry.folder = fields[0];
            entry.name = fields[1];
            entry.engine = fields[2] == "1";
            entry.flipped = fields[3] == "1";
            entry.uid = static_cast<std::uint32_t>(std::strtoul(fields[4].c_str(), nullptr, 10));
            if (fields.size() > 5) {
                const auto comma = fields[5].find(',');
                entry.shoes[0] = formats::brake_shoe_from(fields[5].substr(0, comma));
                entry.shoes[1] = comma == std::string::npos ? entry.shoes[0] : formats::brake_shoe_from(fields[5].substr(comma + 1));
            }
            train.cars.push_back(std::move(entry));
        }
    }
    std::erase_if(saved, [](const PlacedTrain& train) { return !train.front || train.cars.empty(); });
    if (saved.empty()) return trf("no trains in %s", file.filename().string().c_str());
    return put_back(std::move(saved), "situation");
}

std::uint64_t TrainView::add_remote(const PlacedTrain& placed, std::string* error) {
    return add_unit(placed, true, error);
}

std::uint64_t TrainView::add_local(const PlacedTrain& placed, std::string* error) {
    const auto id = add_unit(placed, false, error);
    // With the player's DLLs running it joins their host, as a part cut off
    // does: its DLLs are bound when the player gets in, and go on when he
    // leaves (RTS binds a DLL to its car once, 0x5026FC).
    if (auto* unit = id != 0 && legacy_ ? find_unit(id) : nullptr) {
        unit->host = true;
        if (!reconsist_host()) {
            std::fprintf(stderr, "locomotive DLLs: train %s not taken in\n", unit->name.c_str());
            if (auto* again = find_unit(id)) again->host = false;
            reconsist_host();
        }
    }
    return id;
}

std::uint64_t TrainView::add_unit(const PlacedTrain& placed, const bool remote, std::string* error) {
    if (!placed.front || placed.cars.empty()) {
        if (error) *error = "no place or no cars";
        return 0;
    }
    Unit unit;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    if (auto problem = make_unit_from(placed.name, placed.cars, unit, stock, placed.keys); !problem.empty()) {
        if (error) *error = std::move(problem);
        return 0;
    }
    const auto& where = *placed.front;
    std::string problem;
    if (!unit.train->place(*network_, where.node, where.offset, where.forward, &problem)) {
        if (error) *error = std::move(problem);
        return 0;
    }
    for (auto& definition : stock) stock_.push_back(std::move(definition));
    unit.role = Role::standing;
    unit.remote = remote;
    unit.train->set_asleep(true);
    number_cars(unit);
    unit.settle();
    const auto id = unit.train->id();
    units_.push_back(std::move(unit));
    return id;
}

void TrainView::set_remote_light(const std::uint64_t id, const std::size_t car, const std::size_t light, const bool on) {
    for (auto& unit : units_) {
        if (unit.train->id() != id || !unit.remote || car >= unit.cars.size()) continue;
        auto& lights = unit.cars[car].remote_lights;
        if (light >= lights.size()) lights.resize(light + 1, 0);
        lights[light] = on ? 1 : 0;
    }
}

void TrainView::set_remote_look(const std::uint64_t id, const std::size_t car, const std::uint8_t pantographs,
                                const bool running) {
    for (auto& unit : units_) {
        if (unit.train->id() != id || !unit.remote || car >= unit.cars.size()) continue;
        auto& c = unit.cars[car];
        c.remote_pantographs = pantographs;
        // A diesel runs or stands cold (RTS: LocoFlags bit 3, 0x4C3DE0).
        if (c.definition && c.definition->engine && c.definition->engine->kind == formats::EngineKind::diesel) c.cold = !running;
    }
}

std::vector<TrainView::CarSound> TrainView::take_sound(const std::uint64_t id) {
    std::vector<CarSound> out;
    auto* unit = find_unit(id);
    if (!unit || unit->remote) return out;
    for (auto& car : unit->cars) {
        CarSound s;
        s.triggers = std::move(car.net_triggers);
        car.net_triggers.clear();
        s.vars = car.net_vars;
        s.travelled = car.net_travelled;
        out.push_back(std::move(s));
    }
    return out;
}

void TrainView::remote_sound_trigger(const std::uint64_t id, const std::size_t car, const std::uint32_t code) {
    for (auto& unit : units_) {
        if (unit.train->id() != id || !unit.remote || car >= unit.cars.size()) continue;
        auto& pending = unit.cars[car].pending_triggers;
        if (pending.size() < 96) pending.push_back(code);
    }
}

std::vector<TrainView::CarSoundState> TrainView::sound_state_of(const std::uint64_t id) const {
    std::vector<CarSoundState> out;
    const auto* unit = find_unit(id);
    if (!unit || unit->remote) return out;
    for (const auto& car : unit->cars) {
        CarSoundState s;
        s.vars = car.net_vars;
        s.travelled = car.net_travelled;
        if (car.sound) s.streams = car.sound->stream_states();
        out.push_back(std::move(s));
    }
    return out;
}

void TrainView::restore_remote_sound(const std::uint64_t id, const std::size_t car, const std::vector<std::uint32_t>& current) {
    for (auto& unit : units_) {
        if (unit.train->id() != id || !unit.remote || car >= unit.cars.size()) continue;
        auto& c = unit.cars[car];
        if (c.sound) {
            c.sound->restore(current);
        } else {
            c.remote_sound_restore = current;
        }
    }
}

void TrainView::set_remote_sound(const std::uint64_t id, const std::size_t car, const std::array<float, 8>& vars) {
    for (auto& unit : units_) {
        if (unit.train->id() != id || !unit.remote || car >= unit.cars.size()) continue;
        unit.cars[car].remote_sound_vars = vars;
    }
}

void TrainView::set_category(const std::uint64_t id, const simulation::TrainCategory category) {
    if (auto* unit = find_unit(id)) unit->category = category;
}

std::string TrainView::place_driven(PlacedTrain train) {
    train.role = Role::driven;
    std::vector<PlacedTrain> trains;
    trains.push_back(std::move(train));
    return put_back(std::move(trains), "server");
}

TrainView::DerailLook TrainView::derail_of(const std::uint64_t id) const {
    DerailLook out;
    const auto* unit = find_unit(id);
    if (!unit || unit->remote || !unit->train->derailed()) return out;
    const auto& states = unit->train->cars();
    out.derailed.assign(states.size(), 0);
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (!states[i].derailed) continue;
        out.derailed[i] = 1;
        const auto body = body_pose(*unit, i, false);
        if (!body) continue;
        CarDerail d;
        d.car = i;
        glm::vec3 along{body->transform[2]};
        along.y = 0.0f;
        if (glm::length(along) > 1e-4f) d.heading = glm::normalize(along);
        d.position = renderer::route_position(glm::vec3{body->transform[3]}, origin_);
        d.tilt = states[i].tilt;
        d.tilt_target = states[i].tilt_target;
        d.weight = (states[i].definition ? states[i].definition->mass : 0.0f) * 9.81f;
        out.cars.push_back(d);
    }
    return out;
}

std::vector<TrainView::CarLook> TrainView::look_of(const std::uint64_t id) const {
    std::vector<CarLook> out;
    const auto* unit = find_unit(id);
    if (!unit || unit->remote) return out;
    for (const auto& car : unit->cars) {
        CarLook look;
        if (car.definition && car.definition->engine) {
            look.kind = car.definition->engine->kind == formats::EngineKind::electric ? 1
                      : car.definition->engine->kind == formats::EngineKind::diesel   ? 2
                                                                                       : 0;
        }
        look.pantographs = car.pantograph_raised;
        look.running = !car.cold;
        if (car.definition) {
            const auto& lights = car.definition->lights;
            for (std::size_t k = 0; k < lights.size(); ++k) {
                LightLook l;
                l.on = k < car.lit_now.size() && car.lit_now[k] != 0;
                if (!lights[k].states.empty()) {
                    const auto shown = k < car.lights.size() ? car.lights[k].state : 0;
                    l.state = &lights[k].states[(std::min)(shown, lights[k].states.size() - 1)];
                }
                look.lights.push_back(l);
            }
        }
        out.push_back(std::move(look));
    }
    return out;
}

simulation::Train* TrainView::find_train(const std::uint64_t id) {
    for (auto& unit : units_) {
        if (unit.train->id() == id) return unit.train.get();
    }
    return nullptr;
}

bool TrainView::remove_train(const std::uint64_t id) {
    const auto it = std::find_if(units_.begin(), units_.end(), [id](const Unit& unit) { return unit.train->id() == id; });
    if (it == units_.end() || it == units_.begin() && it->role == Role::driven) return false;
    dispatcher_->clear(id);
    it->train->vacate();
    if (follow_id_ == id) follow_id_ = 0;
    retire_sounds(*it);
    const bool host = it->host && legacy_ != nullptr;
    units_.erase(it);
    // Its cars leave the DLL host, which keeps them by their keys with their
    // DLLs' state, for when they come back (another train's cut, given back).
    if (host && !units_.empty()) reconsist_host();
    return true;
}

bool TrainView::drop(const std::uint64_t id) {
    if (helping_ && id == helping_) stop_helping();
    auto* unit = find_unit(id);
    if (!unit) return false;
    if (unit != &units_.front() || unit->role != Role::driven) {
        if (unit != &units_.front()) return remove(id).empty();
        // The front without a driver (another client's, or one standing).
        dispatcher_->clear(id);
        unit->train->vacate();
        retire_sounds(*unit);
        units_.erase(units_.begin());
        if (follow_id_ == id) follow_id_ = 0;
        return true;
    }
    // The driven train: the player leaves it where he is; its DLLs, cab and
    // keyboard go with it (RTS: the train out of play, 0x4C3DE0).
    set_engineer(false);
    native_.reset();
    legacy_.reset();
    legacy_status_.clear();
    for (auto& each : units_) {
        each.host = false;
        each.host_train = SIZE_MAX;
    }
    host_keys_.clear();
    driver_.reset();
    cab_.reset();
    visit_cab_.reset();
    cab_requested_ = false;
    visit_requested_ = false;
    cab_car_ = SIZE_MAX;
    engineer_car_ = SIZE_MAX;
    dispatcher_->clear(id);
    unit->train->vacate();
    retire_sounds(*unit);
    units_.erase(units_.begin());
    if (follow_id_ == id) follow_id_ = 0;
    follow_ = false;
    consist_.clear();
    std::cout << "the driven train is gone: the player is out where he was" << std::endl;
    return true;
}

bool TrainView::give_away(const std::uint64_t id) {
    auto* unit = find_unit(id);
    if (!unit || unit->remote) return false;
    if (unit == &units_.front() && unit->role == Role::driven) {
        // The player leaves it where he is; its DLLs stay with the host.
        set_engineer(false);
        native_.reset();
        driver_.reset();
        cab_.reset();
        visit_cab_.reset();
        cab_requested_ = false;
        visit_requested_ = false;
        cab_car_ = SIZE_MAX;
        engineer_car_ = SIZE_MAX;
        follow_ = false;
        consist_.clear();
        std::cout << "the driven train is another player's now: the player is out where he was" << std::endl;
    }
    unit->role = Role::standing;
    unit->remote = true;
    unit->traffic.reset();
    unit->train->set_kinematic(false);
    for (std::size_t i = 0; i < unit->train->cars().size(); ++i) unit->train->set_controller(i, nullptr);
    unit->train->set_asleep(true);
    dispatcher_->clear(id);
    shared_sound_.erase(id);
    boardable_.erase(id);
    if (legacy_ && unit->host) reconsist_host();
    return true;
}

bool TrainView::take_back(const std::uint64_t id) {
    auto* unit = find_unit(id);
    if (!unit || !unit->remote) return false;
    unit->remote = false;
    unit->role = Role::standing;
    for (auto& car : unit->cars) {
        car.remote_lights.clear();
        car.remote_sound_vars.reset();
    }
    // Its DLLs, bound while it was ours, go on where they stopped; a train
    // never ours joins the player's host, to be bound when he gets in.
    if (legacy_) {
        unit->host = true;
        reconsist_host();
        if (const auto back = legacy_->take_restored()) {
            std::cout << "locomotive DLLs: " << back << " cars of " << unit->name << " back with their DLLs as they left them"
                      << std::endl;
        }
    }
    return true;
}

void TrainView::retire_sounds(Unit& unit) {
    shared_sound_.erase(unit.train->id());
    boardable_.erase(unit.train->id());
    for (auto& car : unit.cars) {
        board_cabs_.erase(car.key);
        if (car.sound) retired_sounds_.push_back(std::move(car.sound));
        if (car.cab_sound) retired_sounds_.push_back(std::move(car.cab_sound));
        for (auto& aux : car.aux_sounds)
            if (aux.instance) retired_sounds_.push_back(std::move(aux.instance));
    }
}

std::string TrainView::put_back(std::vector<PlacedTrain> saved, const std::string& source, const bool skip_broken) {
    std::erase_if(saved, [](const PlacedTrain& train) { return (!train.front && !train.rear) || train.cars.empty(); });
    if (saved.empty()) return tr("no trains to put on the track");
    // The driven train first; one at most.
    std::stable_partition(saved.begin(), saved.end(), [](const PlacedTrain& train) { return train.role == Role::driven; });
    for (std::size_t i = 1; i < saved.size(); ++i) {
        if (saved[i].role == Role::driven) saved[i].role = Role::standing;
    }
    // Every train built before the ones there now go: a broken file leaves them.
    std::vector<Unit> built;
    std::vector<std::size_t> built_from; // the saved train each was made from
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    std::size_t broken = 0;
    for (std::size_t i = 0; i < saved.size(); ++i) {
        const auto& train = saved[i];
        Unit unit;
        if (auto error = make_unit_from(train.name, train.cars, unit, stock, train.keys); !error.empty()) {
            if (!skip_broken) return error;
            std::cout << source << ": " << train.name << " left out: " << error << '\n';
            ++broken;
            continue;
        }
        unit.role = train.role;
        unit.activity_id = train.activity_id;
        for (auto& car : unit.cars) car.activity_origin = train.activity_id;
        built.push_back(std::move(unit));
        built_from.push_back(i);
    }
    if (built.empty()) return trf("none of the %zu trains could be made (see openrts.log)", saved.size());
    set_engineer(false);
    for (auto& old : units_) {
        dispatcher_->clear(old.train->id());
        old.train->vacate();
    }
    units_.clear();
    driver_.reset();
    stock_ = std::move(stock);
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < built.size(); ++i) {
        auto& unit = built[i];
        const auto& placed = saved[built_from[i]];
        simulation::TrackLocation where;
        if (placed.front) {
            where = *placed.front;
        } else {
            // The rear given: the front a train length ahead of it.
            simulation::TrackTraveller front(*network_, placed.rear->node, placed.rear->offset, placed.rear->forward);
            front.move(unit.train->length(), simulation::JunctionMode::plain, placement_owner);
            where = {.node = front.node(), .offset = front.offset(), .forward = front.forward()};
        }
        std::string error;
        if (!unit.train->place(*network_, where.node, where.offset, where.forward, &error)) {
            std::cout << source << ": " << unit.name << ": " << error << '\n';
            ++skipped;
            continue;
        }
        number_cars(unit);
        unit.settle();
        if (unit.role == Role::standing) unit.train->set_asleep(true);
        units_.push_back(std::move(unit));
    }
    if (units_.empty()) return tr("none of the saved trains fits on the track");
    for (std::size_t i = 1; i < units_.size(); ++i) attach_role(units_[i]);
    if (units_.front().role == Role::driven) {
        driver_ = simulation::attach_keyboard_driver(*units_.front().train);
        consist_ = units_.front().name;
    }
    fresh_train_ = true;
    attach_legacy();
    if (units_.front().role != Role::driven) attach_role(units_.front());
    pending_path_ = false;
    follow_id_ = 0;
    accumulator_ = 0.0;
    joints_ = junctions_ = coupler_breaks_ = derailments_ = 0;
    end_of_track_ = false;
    std::cout << source << ": " << units_.size() << " trains put back, the first " << role_key(units_.front().role) << " ("
              << units_.front().name << ")" << std::endl;
    if (skipped > 0 || broken > 0) {
        std::string problem;
        if (broken > 0) problem = trf("%zu trains could not be made: their stock is missing (see openrts.log)", broken);
        if (skipped > 0) problem += (problem.empty() ? "" : "; ") + trf("%zu of the saved trains did not fit on the track", skipped);
        return problem;
    }
    return {};
}

// ---------------------------------------------------------------------------
// A scenario's traffic and block points (docs/traffic.md)

namespace {

// The dispatcher's key of a scenario block point: its service (0: global) and number.
[[nodiscard]] std::uint32_t block_key(const std::uint32_t service, const std::int32_t point) {
    return (service << 16) | (static_cast<std::uint32_t>(point) & 0xFFFFu);
}

// `start` (s into the day) has come, the clock at `now`: within the 12
// hours after it (a scenario running over midnight).
[[nodiscard]] bool due(const double now, const double start) {
    return std::fmod(now - start + 86400.0 * 1.5, 86400.0) - 43200.0 >= 0.0;
}

} // namespace

std::vector<simulation::RouteBlock> TrainView::activity_blocks(const formats::ActivityService& service, const ActivityId train,
                                                                const simulation::TrainRoute& route) const {
    std::vector<simulation::RouteBlock> blocks;
    if (!activity_) return blocks;
    // On the NumPassed-th place where the path comes within 10 m of it.
    const auto place = [&](const formats::ActivityBlockPoint& point, const std::uint32_t owner, const bool quiet) {
        if (!point.location) return;
        const auto& p = *point.location;
        const simulation::TrackPoint target{.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
        const auto crossings = simulation::route_crossings(*network_, route, target, 10.0);
        const auto index = static_cast<std::size_t>((std::max)(point.passed, 0));
        if (index >= crossings.size()) {
            if (!quiet && !point.suppress_errors)
                std::cout << "activity: block point " << owner << ' ' << point.id << " is not on the path of " << service.name << '\n';
            return;
        }
        blocks.push_back({.key = block_key(owner, point.id), .leg = crossings[index].leg, .distance = crossings[index].distance});
    };
    // PointAffects: a service's point the trains of it listed, a global one
    // the services listed; nothing listed: all.
    const auto affects = [](const std::vector<std::int32_t>& list, const std::uint32_t id) {
        return list.empty() || std::find(list.begin(), list.end(), static_cast<std::int32_t>(id)) != list.end();
    };
    for (const auto& point : service.block_points)
        if (affects(point.affects, train.second)) place(point, service.id, false);
    for (const auto& point : activity_->global_block_points)
        if (affects(point.affects, service.id)) place(point, 0, true);
    return blocks;
}

std::string TrainView::give_activity_path(Unit& unit, const formats::ActivityService& service) {
    const auto* path = find_activity_path(service.path);
    if (!path) return "no path " + service.path;
    std::string error;
    auto plan = simulation::plan_from_path(*network_, *path, &error);
    if (!plan) return path->file + ": " + error;
    auto route = simulation::build_route(*network_, *plan);
    if (!route.ok()) return path->file + ": " + route.error;
    auto blocks = unit.activity_id ? activity_blocks(service, *unit.activity_id, route) : std::vector<simulation::RouteBlock>{};
    const auto id = unit.train->id();
    const auto count = blocks.size();
    dispatcher_->assign(id, std::move(*plan), std::move(route));
    dispatcher_->set_blocks(id, std::move(blocks));
    if (count > 0) std::cout << "activity: " << service.name << ": " << count << " block points on its path" << std::endl;
    if (!units_.empty() && &unit == &units_.front() && unit.role == Role::driven) set_timetable(unit, service);
    return {};
}

namespace {

// s into the day as RTS writes it (0x45CA30), wrapped into the day.
[[nodiscard]] std::string clock_text(std::int64_t seconds) {
    seconds %= 86400;
    if (seconds < 0) seconds += 86400;
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d:%02d", static_cast<int>(seconds / 3600), static_cast<int>(seconds / 60 % 60),
                  static_cast<int>(seconds % 60));
    return text;
}

} // namespace

// The TrainSchedule of the unit's train in `service`, on its path (RTS
// 0x471B9C reads it with the path; a station not on it: message 0x4B unless
// SuppressErrors).
void TrainView::set_timetable(const Unit& unit, const formats::ActivityService& service) {
    timetable_ = {};
    timetable_hint_.reset();
    const auto* record = unit.activity_id ? service.train(unit.activity_id->second) : nullptr;
    const auto* route = dispatcher_->route(unit.train->id());
    if (!record || record->schedule.empty() || !route) return;
    std::vector<simulation::TimetableStop> stops;
    for (const auto& s : record->schedule) {
        stops.push_back({.station = s.station, .skipped = s.skipped, .passengers = s.passengers, .arrival = s.arrival, .departure = s.departure});
    }
    simulation::locate_stops(*network_, *route, stops);
    std::size_t found = 0;
    for (std::size_t k = 0; k < stops.size(); ++k) {
        const auto& stop = stops[k];
        if (stop.found) {
            ++found;
            const double boarding = stop.waiting > 0 ? stop.min_wait * stop.passengers / stop.waiting : stop.passengers;
            std::cout << "activity: timetable: " << stop.station << " " << clock_text(stop.arrival) << " - " << clock_text(stop.departure)
                      << ", leg " << stop.leg + 1 << " at " << static_cast<long>(stop.distance) << " m, platform "
                      << static_cast<long>(stop.length) << " m, boarding " << static_cast<long>(boarding) << " s" << std::endl;
        } else if (!record->schedule[k].suppress_errors) {
            std::cout << "activity: timetable: station " << stop.station << " not found on the path" << std::endl;
        }
    }
    timetable_ = simulation::Timetable(std::move(stops), *route, record->timetable_tolerance.value_or(route_tolerance_), record->check_doors);
    timetable_service_ = service.name;
    timetable_train_ = record->id;
    timetable_owner_ = unit.train->id();
    std::cout << "activity: timetable of " << service.name << ": " << found << " of " << record->schedule.size()
              << " stops on its path" << std::endl;
}

std::optional<simulation::TimetableTrain> TrainView::timetable_train(const Unit& unit) const {
    const auto* progress = dispatcher_->progress(unit.train->id());
    if (!progress || !progress->on_route) return std::nullopt;
    simulation::TimetableTrain result;
    result.leg = progress->leg;
    result.head = progress->head;
    result.length = unit.train->length();
    result.speed = static_cast<float>(std::abs(unit.train->speed()));
    // The cars from the leading end back.
    const auto& cars = unit.train->cars();
    double behind = 0.0;
    for (std::size_t k = 0; k < cars.size(); ++k) {
        const auto& car = cars[progress->front_leads ? k : cars.size() - 1 - k];
        const double length = car.definition ? car.definition->length : 0.0;
        if (k == 0) result.lead_car = length;
        const float room = car.definition ? car.definition->passenger_capacity : 0.0f;
        if (room > 0.0f) result.cars.emplace_back(behind + length * 0.5, room);
        behind += length;
    }
    // openRTS knows the doors of a train without DLLs (Y / U); a DLL's own
    // doors are not known here: open.
    result.doors_open = legacy_ || native_ || doors_open_[0] || doors_open_[1];
    return result;
}

// Ctrl+Num7 and the boarding's course (RTS 0x424488, 0x4FB1B4); the key's
// hint once the train stands at a stop ready for it.
void TrainView::update_timetable(const SceneInput& input, const float elapsed) {
    if (!timetable_.empty() && (units_.empty() || units_.front().train->id() != timetable_owner_)) timetable_ = {};
    if (timetable_.empty() || units_.empty() || units_.front().role != Role::driven) return;
    const auto train = timetable_train(units_.front());
    const auto now = static_cast<std::int32_t>(std::floor(day_time_));
    bool started = false;
    if (input.board) {
        const auto result = train ? timetable_.start_boarding(*train, now) : simulation::BoardingStart::no_stop;
        switch (result) {
        case simulation::BoardingStart::started: started = true; break;
        case simulation::BoardingStart::already: message_ = tr("Passengers are boarding train"); break;
        case simulation::BoardingStart::moving: message_ = tr("stop the train to start the boarding"); break;
        case simulation::BoardingStart::no_stop: message_ = tr("no stop of the timetable here (F6: the timetable)"); break;
        case simulation::BoardingStart::doors_closed: message_ = tr("Can't start boarding passengers. All the doors are closed."); break;
        }
    }
    if (train) timetable_.step(*train, elapsed, now);
    for (const auto& event : timetable_.take_events()) {
        const auto& stop = timetable_.stops()[event.stop];
        switch (event.kind) {
        case simulation::TimetableEventKind::boarding_started:
            message_ = trf("%s: passengers are boarding, %s", stop.station.c_str(),
                           clock_text(static_cast<std::int64_t>(std::ceil(timetable_.boarding_left()))).c_str());
            std::cout << "activity: timetable: boarding at " << stop.station << " from " << clock_text(now) << ", "
                      << static_cast<long>(std::ceil(timetable_.boarding_left())) << " s" << std::endl;
            break;
        case simulation::TimetableEventKind::boarding_over:
            message_ = trf("%s: the boarding is over, the train may leave", stop.station.c_str());
            std::cout << "activity: timetable: boarding over at " << stop.station << ' ' << clock_text(now) << std::endl;
            break;
        case simulation::TimetableEventKind::left:
            // Ended at once: no passenger room at the platform.
            if (started && !event.done) {
                message_ = trf("%s: no passenger car at the platform", stop.station.c_str());
            } else if (!event.done) {
                message_ = trf("%s: the boarding stopped before it was over", stop.station.c_str());
            }
            std::cout << "activity: timetable: left " << stop.station << ' ' << clock_text(now) << (event.done ? "" : " (boarding not over)")
                      << std::endl;
            break;
        }
    }
    // Standing where the boarding can start, not started yet: the key, once a stop.
    if (train && !timetable_.boarding() && train->speed < 0.01f) {
        if (const auto at = timetable_.stop_at(*train); at && !timetable_.stops()[*at].arrived && timetable_hint_ != at) {
            const auto place = timetable_.place(*at, *train);
            if (place == simulation::StopPlace::in_place || place == simulation::StopPlace::covering) {
                timetable_hint_ = at;
                message_ = trf("%s: %s starts the boarding (F6: the timetable)", timetable_.stops()[*at].station.c_str(), board_key_name());
            }
        }
    }
}

std::string TrainView::timetable_report() const {
    if (timetable_.empty()) return "none";
    const auto train = units_.empty() ? std::nullopt : timetable_train(units_.front());
    std::string text = timetable_service_ + (train ? "" : " (off its path)");
    for (std::size_t k = 0; k < timetable_.stops().size(); ++k) {
        const auto& stop = timetable_.stops()[k];
        text += "\n  " + stop.station + " " + clock_text(stop.arrival) + " [" + (stop.arrived_at ? clock_text(*stop.arrived_at) : "-") + "] "
            + clock_text(stop.departure) + " [" + (stop.departed_at ? clock_text(*stop.departed_at) : "-") + "]";
        if (!stop.found) {
            text += " not on the path";
            continue;
        }
        if (train) text += " " + std::to_string(static_cast<long>(timetable_.distance(k, *train))) + " m";
        if (train) {
            const char* places[] = {"ahead", "partly", "in place", "covering", "passed"};
            text += std::string(" ") + places[static_cast<int>(timetable_.place(k, *train))];
        }
        if (timetable_.boarding() == k) text += ", boarding " + std::to_string(static_cast<long>(std::ceil(timetable_.boarding_left()))) + " s left";
        if (stop.departed) text += ", done";
    }
    if (train) {
        float room = 0.0f;
        for (const auto& car : train->cars) room += car.second;
        text += "\n  train " + std::to_string(static_cast<long>(train->length)) + " m, leg " + std::to_string(train->leg + 1) + " at "
            + std::to_string(static_cast<long>(train->head)) + " m, " + std::to_string(train->cars.size()) + " passenger cars, "
            + std::to_string(static_cast<long>(room)) + " seats";
    }
    return text;
}

// F6 in a scenario whose train has a timetable (RTS 0x466F70): the stops,
// their times as planned and as they were, the distance to each one not left
// yet, coloured as RTS colours them.
void TrainView::draw_timetable(const Unit& unit) {
#ifdef OPENRTS_HAS_IMGUI
    const auto now = static_cast<std::int32_t>(std::floor(day_time_));
    ImGui::TextUnformatted(trf("Train schedule. Service \"%s\", train %u  %s", timetable_service_.c_str(), timetable_train_,
                               clock_text(now).c_str()).c_str());
    ImGui::Separator();
    const auto train = timetable_train(unit);
    const auto& stops = timetable_.stops();
    const ImVec4 white{1.0f, 1.0f, 1.0f, 1.0f}, green{0.2f, 1.0f, 0.2f, 1.0f}, red{1.0f, 0.2f, 0.2f, 1.0f},
        yellow{1.0f, 0.9f, 0.1f, 1.0f}, lime{0.5f, 0.9f, 0.1f, 1.0f}, grey{0.6f, 0.6f, 0.6f, 1.0f};
    bool ready = false; // standing where the boarding of a stop not boarded yet can start
    if (ImGui::BeginTable("##timetable", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Station"));
        ImGui::TableSetupColumn(tr("Arrival"));
        ImGui::TableSetupColumn(tr("Actual"));
        ImGui::TableSetupColumn(tr("Departure"));
        ImGui::TableSetupColumn((std::string(tr("Actual")) + "##departure").c_str());
        ImGui::TableSetupColumn(tr("Distance"));
        ImGui::TableHeadersRow();
        for (std::size_t k = 0; k < stops.size(); ++k) {
            const auto& stop = stops[k];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (stop.found) {
                ImGui::TextUnformatted(stop.station.c_str());
            } else {
                ImGui::TextColored(grey, "%s", stop.station.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(clock_text(stop.arrival).c_str());
            ImGui::TableNextColumn();
            if (stop.arrived_at) ImGui::TextColored(timetable_.arrived_on_time(stop) ? green : red, "%s", clock_text(*stop.arrived_at).c_str());
            ImGui::TableNextColumn();
            // The last stop's 00:00:00: the run ends there.
            const bool end = k + 1 == stops.size() && stop.departure == 0;
            ImGui::TextUnformatted(end ? "-" : clock_text(stop.departure).c_str());
            ImGui::TableNextColumn();
            if (stop.departed && stop.departed_at) {
                ImGui::TextColored(simulation::Timetable::left_on_time(stop) || end ? green : red, "%s", clock_text(*stop.departed_at).c_str());
            } else if (timetable_.boarding() == k) {
                const auto left = clock_text(static_cast<std::int64_t>(std::ceil(timetable_.boarding_left())));
                ImGui::TextColored(yellow, "%s", trf("in %s", left.c_str()).c_str());
            } else if (stop.arrived && stop.departed_at) {
                // Left before the boarding was over (RTS keeps such a stop open).
                ImGui::TextColored(red, "%s", clock_text(*stop.departed_at).c_str());
            }
            ImGui::TableNextColumn();
            if (!stop.found) {
                ImGui::TextColored(grey, "%s", tr("not on the path"));
            } else if (!stop.departed && train) {
                const double d = timetable_.distance(k, *train);
                char text[32];
                std::snprintf(text, sizeof text, std::abs(d) >= 1000.0 ? tr("%.2f km") : tr("%.0f m"), std::abs(d) >= 1000.0 ? d / 1000.0 : d);
                const auto place = timetable_.place(k, *train);
                const auto colour = place == simulation::StopPlace::passed ? red
                    : place == simulation::StopPlace::ahead                ? white
                    : place == simulation::StopPlace::in_place             ? green
                    : place == simulation::StopPlace::covering             ? lime
                                                                           : yellow;
                ImGui::TextColored(colour, "%s", text);
                if (!stop.arrived && train->speed < 0.01f
                    && (place == simulation::StopPlace::in_place || place == simulation::StopPlace::covering))
                    ready = true;
            }
        }
        ImGui::EndTable();
    }
    if (!train) ImGui::TextColored(grey, "%s", tr("the train is off its path"));
    if (timetable_.boarding() && timetable_.boarding_left() > 0.0f) {
        ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "%s", tr("Passengers are boarding train"));
    } else if (ready) {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.9f, 1.0f), "%s", trf("[Press \"%s\" to start boarding passengers]", board_key_name()).c_str());
    }
#else
    (void)unit;
#endif
}

bool TrainView::traffic_place_taken(const simulation::Train& train) const {
    const auto mine = train.car_centres();
    for (const auto& unit : units_) {
        if (!unit.train->placed() || unit.train.get() == &train) continue;
        const auto theirs = unit.train->car_centres();
        for (std::size_t i = 0; i < mine.size(); ++i) {
            for (std::size_t j = 0; j < theirs.size(); ++j) {
                if (mine[i].node() != theirs[j].node()) continue;
                const double reach = (train.cars()[i].definition->length + unit.train->cars()[j].definition->length) * 0.5;
                if (std::abs(mine[i].offset() - theirs[j].offset()) < reach) return true;
            }
        }
    }
    return false;
}

// RTS 0x4EF740 for a scenario's traffic train: its consist at its path's
// start (TrainStartOffset further on), its speeds and rates from the
// scenario (without a positive maximum speed and acceleration RTS does not
// place it, message 0x88), its path reserved at once.
void TrainView::start_traffic(TrafficPlan& plan) {
    plan.started = true; // tried: a failure is not retried
    const auto* service = activity_ ? activity_->service(plan.id.first) : nullptr;
    const auto* train = service ? service->train(plan.id.second) : nullptr;
    if (!train) return;
    const auto fail = [&](const std::string& why) {
        std::cout << "activity: traffic " << service->name << " (" << train->consist << "): " << why << std::endl;
    };
    simulation::TrafficSettings settings;
    settings.max_speed = train->max_speed.value_or(0.0f);
    settings.acceleration = train->acceleration.value_or(0.0f);
    settings.deceleration = train->deceleration.value_or(0.0f);
    settings.efficiency = train->efficiency;
    settings.finish_speed = train->finish_speed;
    settings.route_limit = limits_->route_limit();
    settings.ignore_signals = train->flag("IGNORE_SIGNALS");
    settings.ignore_speed_limits = train->flag("IGNORE_SPEED_LIMITS");
    if (activity_->traffic_limits) {
        settings.curve_limits.clear();
        for (const auto& limit : *activity_->traffic_limits) settings.curve_limits.push_back({.radius = limit.radius, .speed = limit.speed});
    }
    if (const auto problem = settings.problem(); !problem.empty()) return fail(problem);
    const auto* path = find_activity_path(service->path);
    if (!path) return fail("no path " + service->path);
    const auto rear = path_rear(*path);
    if (!rear) return fail("no track at its path's start");
    auto consist = activity_consist(train->consist).value_or(formats::ConsistDefinition{});
    if (consist.entries.empty() && consist.error.empty()) consist = formats::load_consist(*vfs_, train->consist);
    if (!consist.ok() || consist.entries.empty()) return fail("consist: " + (consist.error.empty() ? std::string("no cars") : consist.error));
    Unit unit;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    if (auto error = make_unit_from(train->consist, consist.entries, unit, stock); !error.empty()) return fail(error);
    simulation::TrackTraveller back(*network_, rear->node, rear->offset, rear->forward);
    if (train->start_offset > 0.0f) back.move(train->start_offset, simulation::JunctionMode::plain, placement_owner);
    auto front = back;
    front.move(unit.train->length(), simulation::JunctionMode::plain, placement_owner);
    std::string error;
    if (!unit.train->place(*network_, front.node(), front.offset(), front.forward(), &error)) return fail(error);
    // Its place taken: CHECK_IF_BUSY waits for it to clear (till the remove
    // time), else the start fails (activity.pdf).
    if (traffic_place_taken(*unit.train)) {
        unit.train->vacate();
        if (service->flag("CHECK_IF_BUSY")) {
            plan.started = false;
            if (!plan.waiting) fail("its place is taken; waiting for it to clear");
            plan.waiting = true;
            return;
        }
        return fail("its place is taken");
    }
    for (auto& definition : stock) stock_.push_back(std::move(definition));
    number_cars(unit);
    unit.role = Role::ai;
    unit.activity_id = plan.id;
    for (auto& car : unit.cars) car.activity_origin = plan.id;
    for (const auto& group : train->wagon_groups) apply_wagon_group(unit, group); // RTS 0x4EF740 -> 0x4FA374
    if (!train->service_type.empty()) unit.category = simulation::category_from_cars(lower(train->service_type) == "freight");
    unit.traffic_settings = settings;
    unit.scenario_traffic = true;
    // FinishSpeed: removed at the end whatever the flags say.
    unit.remove_at_end = !service->flag("NO_REMOVE") || train->finish_speed > 0.0f;
    unit.force_removal = service->flag("FORCE_REMOVAL") || train->finish_speed > 0.0f;
    unit.remove_time = train->remove_time;
    unit.settle();
    units_.push_back(std::move(unit));
    auto& placed = units_.back();
    attach_role(placed);
    if (const auto problem = give_activity_path(placed, *service); !problem.empty()) fail(problem);
    if (train->startup_speed > 0.0f) placed.train->add_velocity(train->startup_speed);
    traffic_started_.push_back(plan.id);
    std::cout << "activity: traffic " << service->name << " (" << train->consist << ", " << placed.cars.size() << " cars) on its path"
              << std::endl;
}

void TrainView::update_traffic() {
    if (!activity_ || units_.empty()) return;
    const double now = day_time_;
    for (auto& plan : traffic_plans_) {
        if (plan.started || !due(now, plan.start)) continue;
        // ChangeServiceStartupType NONE: its trains wait till it starts again.
        if (services_held_.contains(plan.id.first)) continue;
        if (plan.remove && due(now, *plan.remove)) {
            plan.started = true; // its time is over before it could start
            continue;
        }
        start_traffic(plan);
    }
    // Block points releasing themselves: at their time, when such a train
    // reaches them (it stands at its way's end before them), or when its
    // way is reserved up to them.
    for (auto& block : activity_block_list_) {
        if (block.released) continue;
        const auto key = block_key(block.service, block.point->id);
        if (!dispatcher_->block_enabled(key)) continue;
        const auto& point = *block.point;
        bool release = point.release_time && due(now, *point.release_time);
        const auto matches = [&](const std::pair<std::int32_t, std::int32_t>& who, const Unit& unit) {
            if (!unit.activity_id) return false;
            const std::int32_t service = who.first == -2 ? static_cast<std::int32_t>(block.service) : who.first;
            return (service < 0 || unit.activity_id->first == static_cast<std::uint32_t>(service))
                && (who.second < 0 || unit.activity_id->second == static_cast<std::uint32_t>(who.second));
        };
        if (!release && (point.release_on_reach || point.release_on_approach)) {
            for (const auto& unit : units_) {
                const auto id = unit.train->id();
                const auto* blocks = dispatcher_->blocks(id);
                const auto* progress = dispatcher_->progress(id);
                if (!blocks || !progress || !progress->on_route) continue;
                for (const auto& b : *blocks) {
                    if (b.key != key || b.leg != progress->leg) continue;
                    if (point.release_on_reach && matches(*point.release_on_reach, unit)
                        && progress->head >= b.distance - simulation::traffic_stop_margin - 5.0)
                        release = true;
                    if (point.release_on_approach && matches(*point.release_on_approach, unit) && progress->blocked_point == key)
                        release = true;
                }
            }
        }
        if (release) {
            dispatcher_->enable_block(key, false);
            block.released = true;
            std::cout << "activity: block point " << block.service << ' ' << point.id << " released by its condition" << std::endl;
        }
    }
    // The log: where the traffic is, once a minute of game time.
    if (std::abs(now - traffic_logged_) >= 60.0) {
        traffic_logged_ = now;
        for (const auto& unit : units_) {
            if (!unit.scenario_traffic || !unit.activity_id) continue;
            const auto* progress = dispatcher_->progress(unit.train->id());
            const auto* route = dispatcher_->route(unit.train->id());
            std::cout << "traffic " << unit.activity_id->first << ' ' << unit.activity_id->second << ": " << std::fixed << std::setprecision(1)
                      << std::abs(unit.train->speed()) << " m/s";
            if (progress && route && progress->on_route) {
                std::cout << ", leg " << progress->leg + 1 << '/' << route->legs.size() << " at " << progress->head << " of "
                          << route->legs[progress->leg].length() << " m, way to " << progress->authority;
                if (progress->blocked_point)
                    std::cout << ", held by block point " << (*progress->blocked_point >> 16) << ' ' << (*progress->blocked_point & 0xFFFFu);
                else if (progress->blocked_at)
                    std::cout << ", junction " << *progress->blocked_at << " taken";
                if (progress->finished) std::cout << ", at its end";
            } else {
                std::cout << ", off its path";
            }
            std::cout << std::defaultfloat << std::endl;
        }
    }
    // Traffic at its path's end or its remove time goes: one a frame, and
    // without FORCE_REMOVAL only 1 km or more from the player's train (RTS:
    // "not near the player", the distance UNKNOWN).
    for (std::size_t u = 1; u < units_.size(); ++u) {
        auto& unit = units_[u];
        if (!unit.scenario_traffic || !unit.activity_id) continue;
        const auto id = unit.train->id();
        const auto* progress = dispatcher_->progress(id);
        const auto* route = dispatcher_->route(id);
        bool at_end = progress && progress->finished;
        if (!at_end && progress && route && progress->on_route && unit.traffic && unit.traffic->settings().finish_speed > 0.0f
            && progress->leg + 1 >= route->legs.size() && progress->head >= route->legs[progress->leg].length() - 1.0)
            at_end = true;
        const bool over = unit.remove_time && due(now, *unit.remove_time);
        if (!(at_end && unit.remove_at_end) && !over) continue;
        if (!unit.force_removal && distance_to_player(unit) < 1000.0f) continue;
        std::cout << "activity: traffic " << unit.activity_id->first << ' ' << unit.activity_id->second << " (" << unit.name << ") removed"
                  << (over ? " at its remove time" : " at its path's end") << std::endl;
        (void)remove(id);
        break;
    }
}

void TrainView::activity_block_point(const std::uint32_t service, const std::int32_t point, const bool enabled, const double distance) {
    std::size_t count = 0;
    for (const auto& block : activity_block_list_) {
        if (block.service != service || (point >= 0 && block.point->id != point)) continue;
        const double no_return = enabled && activity_ ? (std::max)(distance, static_cast<double>(activity_->no_return)) : 0.0;
        dispatcher_->enable_block(block_key(block.service, block.point->id), enabled, no_return);
        ++count;
    }
    if (count == 0) std::cout << "activity: no block point " << service << ' ' << point << std::endl;
}

void TrainView::activity_modify_velocity(const ActivityId train, const float speed, const float acceleration, const float deceleration) {
    for (auto& unit : units_) {
        if (unit.activity_id != train || !unit.traffic) continue;
        auto& settings = unit.traffic->settings();
        if (speed > 0.0f) settings.max_speed = speed;
        if (acceleration > 0.0f) settings.acceleration = acceleration;
        if (deceleration > 0.0f) settings.deceleration = deceleration;
        unit.traffic_settings = settings;
    }
}

void TrainView::activity_wait_here(const ActivityId train, const double time) {
    for (auto& unit : units_) {
        if (unit.activity_id == train && unit.traffic) unit.wait_here = time > 0.0 ? time : std::numeric_limits<double>::infinity();
    }
}

std::optional<formats::ConsistDefinition> TrainView::activity_consist(const std::string_view name) const {
    if (!activity_folder_ || name.empty() || name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos)
        return std::nullopt;
    auto stem = std::string(name);
    if (stem.size() > 4 && core::fold_case(stem.substr(stem.size() - 4)) == ".con") stem.resize(stem.size() - 4);
    const auto path = activity_folder_->resolve(stem + ".con");
    if (!path || !vfs_->exists(*path)) return std::nullopt;
    std::string error;
    const auto bytes = vfs_->read(*path, &error);
    if (!bytes) return formats::ConsistDefinition{.name = stem, .error = error};
    auto consist = formats::parse_consist(*bytes);
    if (consist.name.empty()) consist.name = stem;
    return consist;
}

const formats::TrainPath* TrainView::find_activity_path(const std::string_view name) const {
    if (name.empty()) return nullptr;
    const auto wanted = lower(std::string(name));
    for (const auto& path : activity_paths_) {
        if (lower(path.file) == wanted || lower(path.id) == wanted || lower(path.name) == wanted) return &path;
    }
    return find_path(name);
}

// RTS scenarios (docs/activities.md): the trains of the player's service
// and of the standing services; traffic services are not run yet.
std::string TrainView::start_activity(const formats::Activity& activity, const core::VirtualPath& folder) {
    activity_folder_ = folder;
    activity_paths_ = formats::load_train_paths_in(*vfs_, folder);
    std::erase_if(activity_paths_, [](const formats::TrainPath& path) { return !path.ok(); });
    const auto to_route = [](const formats::ActivityPoint& p) {
        return formats::RoutePosition{.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
    };
    std::vector<PlacedTrain> trains;
    std::size_t traffic = 0, later = 0, unplaced = 0;
    traffic_plans_.clear();
    traffic_started_.clear();
    activity_block_list_.clear();
    const auto add = [&](const formats::ActivityService& service, const formats::ActivityTrain& train, const Role role,
                         const std::optional<simulation::TrackLocation>& rear) {
        auto consist = activity_consist(train.consist).value_or(formats::ConsistDefinition{});
        if (consist.entries.empty() && consist.error.empty()) consist = formats::load_consist(*vfs_, train.consist);
        if (!consist.ok() || consist.entries.empty()) {
            std::cout << "activity: " << service.name << ": consist " << train.consist << ": "
                      << (consist.error.empty() ? std::string("no cars") : consist.error) << '\n';
            ++unplaced;
            return;
        }
        if (!rear) {
            std::cout << "activity: " << service.name << " (" << train.consist << "): no track at its place\n";
            ++unplaced;
            return;
        }
        PlacedTrain placed;
        placed.name = train.consist;
        placed.role = role;
        placed.rear = rear;
        placed.activity_id = std::pair{service.id, train.id};
        placed.cars = std::move(consist.entries);
        trains.push_back(std::move(placed));
    };
    const auto* player = activity.player_service();
    for (const auto& service : activity.services) {
        if (&service == player) {
            const auto* train = activity.player_train ? service.train(activity.player_train->train) : nullptr;
            if (!train && !service.trains.empty()) train = &service.trains.front();
            if (!train) continue;
            std::optional<simulation::TrackLocation> rear;
            if (train->start && train->heading) {
                rear = placement_rear(to_route(*train->start), to_route(*train->heading));
            } else if (const auto* path = find_activity_path(service.path)) {
                rear = path_rear(*path);
            } else {
                std::cout << "activity: " << service.name << ": no path " << service.path << '\n';
            }
            add(service, *train, Role::driven, rear);
        } else if (service.type == formats::ActivityServiceType::standing) {
            for (const auto& train : service.trains) {
                add(service, train, Role::standing,
                    train.start && train.heading ? placement_rear(to_route(*train.start), to_route(*train.heading)) : std::nullopt);
            }
        } else if (service.type == formats::ActivityServiceType::traffic) {
            // ServiceStarts NONE: started by hand or by the events (not run
            // yet); SINGLE (and for now SERIAL): each train at its time, the
            // scenario's start without one.
            if (lower(service.starts) == "none") {
                ++later;
                continue;
            }
            if (lower(service.starts) == "serial") std::cout << "activity: " << service.name << ": SERIAL starts as SINGLE\n";
            for (const auto& train : service.trains) {
                traffic_plans_.push_back({.id = {service.id, train.id},
                                          .start = train.start_time.value_or(activity.start_time),
                                          .remove = train.remove_time});
                ++traffic;
            }
        } else {
            ++later;
        }
    }
    std::cout << "activity: \"" << activity.name << "\": " << trains.size() << " trains to put on the track, " << unplaced
              << " without a consist or a place; " << traffic << " traffic trains to start at their times, " << later
              << " other services (the player's later ones)" << std::endl;
    // The block points, at their first states.
    for (const auto& service : activity.services)
        for (const auto& point : service.block_points) activity_block_list_.push_back({.service = service.id, .point = &point});
    for (const auto& point : activity.global_block_points) activity_block_list_.push_back({.service = 0, .point = &point});
    for (const auto& block : activity_block_list_)
        dispatcher_->enable_block((block.service << 16) | (static_cast<std::uint32_t>(block.point->id) & 0xFFFFu), block.point->enabled);
    if (trains.empty()) return tr("none of the scenario's trains could be put on the track (see openrts.log)");
    // The player's train starts as its TrainState says (the DLLs' Init),
    // unless the launch chose (--start): without one RTS leaves 0, cold, for
    // a PLAYER service (0x471B9C; 0x4EF740 raises no pantograph then).
    const auto record = [&](const std::pair<std::uint32_t, std::uint32_t>& id) -> const formats::ActivityTrain* {
        for (const auto& service : activity.services)
            if (service.id == id.first) return service.train(id.second);
        return nullptr;
    };
    if (start_state_ < 0 && player) {
        const auto* train = activity.player_train ? player->train(activity.player_train->train) : nullptr;
        if (!train && !player->trains.empty()) train = &player->trains.front();
        if (train) {
            start_state_ = train->state.value_or(player->type == formats::ActivityServiceType::traffic ? 2 : 0);
            pantographs_down_ = start_state_ == 0;
            std::cout << "activity: the player's train starts " << (start_state_ == 0 ? "cold" : start_state_ == 1 ? "on" : "running")
                      << " (TrainState " << (train->state ? "" : "not given, ") << start_state_ << ")" << std::endl;
        }
    }
    auto error = put_back(std::move(trains), "activity", true);
    // Each train's WagonParameters (RTS 0x4EF740 -> 0x4FA374): chocks,
    // hoses, end cocks, handbrakes as the scenario sets them.
    std::size_t groups = 0;
    std::set<std::string> other;
    for (auto& unit : units_) {
        if (!unit.activity_id) continue;
        const auto* train = record(*unit.activity_id);
        if (!train) continue;
        for (const auto& group : train->wagon_groups) {
            apply_wagon_group(unit, group);
            ++groups;
            other.insert(group.other.begin(), group.other.end());
        }
    }
    if (groups > 0) std::cout << "activity: " << groups << " wagon groups set on the trains" << std::endl;
    for (const auto& key : other) std::cout << "activity: WagonParameters " << key << " not run yet" << std::endl;
    if (units_.empty() || units_.front().role != Role::driven) {
        const auto problem = std::string(tr("the player's train could not be made (see openrts.log)"));
        return error.empty() ? problem : problem + "; " + error;
    }
    // The player's train on its service's path: the dispatcher reserves it
    // and sets its junctions, its block points hold it (RTS).
    if (player && !player->path.empty()) {
        if (const auto problem = give_activity_path(units_.front(), *player); !problem.empty())
            std::cout << "activity: the player's path: " << problem << std::endl;
    }
    // The scenario's sounds (StaticObjects SOUND, SoundType WAGON) on their
    // cars, found by their IDs (the train they start in, the consist UiD).
    for (std::size_t k = 0; k < activity.objects.size(); ++k) {
        const auto& object = activity.objects[k];
        if (lower(object.type) != "sound" || object.file.empty()) continue;
        unsigned s = 0, t = 0, n = 0;
        {
            char* end = nullptr;
            s = static_cast<unsigned>(std::strtoul(object.wagon_id.c_str(), &end, 10));
            if (end && *end == '-') t = static_cast<unsigned>(std::strtoul(end + 1, &end, 10));
            if (end && *end == '-') n = static_cast<unsigned>(std::strtoul(end + 1, &end, 10));
        }
        const auto path = object.location == 1 ? folder.resolve(object.file) : route_.resolve("SOUND/" + object.file);
        bool hung = false;
        for (auto& unit : units_) {
            for (auto& car : unit.cars) {
                if (hung || car.activity_origin != std::pair<std::uint32_t, std::uint32_t>{s, t} || car.uid != n || !path) continue;
                car.activity_sounds.push_back({.object = static_cast<std::int32_t>(k), .path = *path});
                hung = true;
            }
        }
        if (!hung) std::cout << "activity: sound object " << k << " (" << object.file << "): no car " << object.wagon_id << '\n';
    }
    // Where the player starts: in the train the header names (its cab for
    // the area CABIN), else on foot where it says, else beside the cab car
    // (RTS's default, 0x42F4A4).
    if (activity.player_train) {
        set_start_view(core::fold_case(activity.player_train->area) == "cabin" ? StartView::cab : StartView::car);
    } else if (activity.player_position) {
        start_point_ = to_route(*activity.player_position);
        set_start_view(StartView::position);
    } else {
        set_start_view(StartView::car);
    }
    return error;
}

void TrainView::apply_start_view(ViewerCamera& camera) {
    const auto view = std::exchange(start_view_, StartView::none);
    const auto& unit = units_.front();
    const auto report = [&] {
        if (engineer_.inside()) {
            std::cout << "start: cab, engineer " << (engineer_.mode() == Engineer::Mode::seated ? "seated in" : "on foot by")
                      << " chair " << engineer_.chair() << " of car " << engineer_car_ + 1 << std::endl;
            return;
        }
        const auto head = local(unit.current.front().front);
        const auto eye = camera.eye();
        std::cout << "start: " << (view == StartView::cab ? "cab" : view == StartView::car ? "car" : view == StartView::position ? "position" : "above") << ", engineer "
                  << (engineer_.mode() == Engineer::Mode::seated ? "seated" : engineer_.mode() == Engineer::Mode::outside ? "outside" : engineer_.active() ? "standing" : "off")
                  << ", eye " << glm::length(eye - head) << " m from the train's head, " << (eye.y - head.y) << " m above it" << std::endl;
    };
    if (unit.current.empty()) return;
    // The car the player starts at: the cab's, else the first.
    const std::size_t index = cab_car_ < unit.cars.size() ? cab_car_ : 0;
    const glm::vec3 front = local(unit.current[index].front);
    const glm::vec3 rear = local(unit.current[index].rear);
    glm::vec3 forward{front.x - rear.x, 0.0f, front.z - rear.z};
    forward = glm::length(forward) > 1e-3f ? glm::normalize(forward) : glm::vec3{0.0f, 0.0f, 1.0f};
    // Scene coordinates are MSTS's (left-handed): the car's left as its driver sees it.
    const glm::vec3 left = glm::normalize(glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f}));
    const auto look = [&camera](const glm::vec3& direction) {
        const auto d = glm::normalize(direction);
        camera.yaw = std::atan2(-d.x, -d.z);
        camera.pitch = std::asin(std::clamp(-d.y, -1.0f, 1.0f));
    };
    follow_ = false;
    if (view == StartView::cab && cab_) {
        set_engineer(true);
        report();
        return;
    }
    if (view == StartView::position && start_point_) {
        // A scenario's start outside the trains (PlayerStartPosition): on
        // the ground there, facing the cab car.
        glm::vec3 feet = local(*start_point_);
        if (const auto ground = ground_height(feet)) feet.y = *ground;
        const glm::vec3 toward{front.x - feet.x, 0.0f, front.z - feet.z};
        look(glm::length(toward) > 1e-3f ? toward : forward);
        camera.fly = true;
        if (cab_) {
            set_engineer(true);
            engineer_.stand_outside(feet);
            camera.position = feet + glm::vec3{0.0f, engineer_.height(), 0.0f};
        } else {
            camera.position = feet + glm::vec3{0.0f, 1.7f, 0.0f};
        }
        camera.target = camera.position + camera.forward() * camera.distance;
        report();
        return;
    }
    if (view == StartView::car || view == StartView::cab) {
        // RTS (0x42F4A4, an activity's default start): 2 m off the car's
        // left side at its front end, on the ground.
        const auto* definition = unit.cars[index].definition;
        const float half_width = definition && definition->width > 0.0f ? definition->width * 0.5f : 1.6f;
        glm::vec3 feet = front + left * (half_width + 2.0f);
        if (const auto ground = ground_height(feet)) feet.y = *ground;
        look(forward);
        camera.fly = true;
        if (cab_) {
            set_engineer(true);
            engineer_.stand_outside(feet);
            camera.position = feet + glm::vec3{0.0f, engineer_.height(), 0.0f};
        } else {
            camera.position = feet + glm::vec3{0.0f, 1.7f, 0.0f};
        }
        camera.target = camera.position + camera.forward() * camera.distance;
        report();
        return;
    }
    if (view == StartView::above) {
        // Behind and above the train's head, looking down onto it.
        const glm::vec3 head = local(unit.current.front().front);
        camera.fly = true;
        camera.position = head - forward * 35.0f + left * 12.0f + glm::vec3{0.0f, 22.0f, 0.0f};
        look(head - forward * 12.0f - camera.position);
        camera.target = camera.position + camera.forward() * camera.distance;
        report();
    }
}

std::string TrainView::save_path(const std::uint64_t id, const std::string& name) {
    const auto* plan = dispatcher_->plan(id);
    const auto* route = dispatcher_->route(id);
    if (!plan || !route) return "the train has no path";
    std::string clean;
    for (const char c : name) clean += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
    if (clean.empty()) return "give the path a name";
    auto path = simulation::path_from_route(*network_, *plan, *route);
    path.id = path.name = clean;
    const auto folder = user_path_folder();
    std::error_code fs_error;
    std::filesystem::create_directories(folder, fs_error);
    const auto file = folder / std::filesystem::path(std::u8string(clean.begin(), clean.end())).replace_extension(".pat");
    std::ofstream output(file, std::ios::binary);
    if (!output) return "cannot write " + file.string();
    const auto bytes = formats::write_train_path(path);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    load_user_paths();
    return {};
}

// The car view (openRTS, not RTS: RTS's camera is the engineer's): F3
// orbits the shown train's first car, again its last, again back to the
// camera as it was. The wheel brings the camera nearer, Ctrl and the wheel
// zoom; the train goes on being driven from the cab's keys.
void TrainView::car_view_key(ViewerCamera& camera) {
    const Unit* unit = follow_id_ != 0 ? find_unit(follow_id_) : nullptr;
    if (!unit && !units_.empty()) unit = &units_.front();
    if (!unit || unit->current.empty() || unit->cars.empty()) return;
    const auto heading_of = [&](const std::size_t i) {
        const auto here = pose(*unit, i);
        const glm::vec3 along = here.front - here.rear;
        return std::atan2(along.x, along.z);
    };
    const auto before = car_view_;
    if (before != CarView::off) {
        // The car left: its distance, zoom and angle kept for next time.
        const std::size_t i = before == CarView::head ? 0 : unit->cars.size() - 1;
        car_view_memory_[unit->cars[i].key] = {.yaw_from_heading = camera.yaw - heading_of(i),
                                               .pitch = camera.pitch,
                                               .distance = camera.distance,
                                               .zoom = camera.view_zoom};
    }
    // From the end the player sits nearer (his car in the train's list, F5):
    // in the rear cab the last car first, then car 0 (testers 2026-11-18).
    if (before == CarView::off) {
        const std::size_t at = unit == &units_.front() ? engineer_car_ : 0;
        car_view_tail_first_ = unit->cars.size() > 1 && at * 2 + 1 > unit->cars.size();
    }
    const auto first = car_view_tail_first_ ? CarView::tail : CarView::head;
    const auto second = car_view_tail_first_ ? CarView::head : CarView::tail;
    car_view_ = before == CarView::off ? first : before == first ? second : CarView::off;
    if (car_view_ == second && unit->cars.size() < 2) car_view_ = CarView::off;
    if (before == CarView::off) {
        person_camera_ = camera;
        follow_ = false;
    }
    if (car_view_ == CarView::off) {
        camera = person_camera_;
        camera.view_zoom = 1.0f;
        say(tr("normal view"));
        return;
    }
    const std::size_t i = car_view_ == CarView::head ? 0 : unit->cars.size() - 1;
    const auto here = pose(*unit, i);
    glm::vec3 forward = here.front - here.rear;
    forward.y = 0.0f;
    forward = glm::length(forward) > 0.01f ? glm::normalize(forward) : glm::vec3{0.0f, 0.0f, 1.0f};
    const glm::vec3 side{forward.z, 0.0f, -forward.x};
    // The first car seen from ahead and aside, the last from behind.
    const glm::vec3 towards_eye = glm::normalize((car_view_ == CarView::head ? forward : -forward) + side * 0.6f);
    const float length = unit->cars[i].definition ? unit->cars[i].definition->length : 20.0f;
    camera.yaw = std::atan2(towards_eye.x, towards_eye.z);
    camera.pitch = 0.2f;
    camera.distance = std::clamp(length * 1.2f, 15.0f, 50.0f);
    camera.view_zoom = 1.0f;
    if (const auto kept = car_view_memory_.find(unit->cars[i].key); kept != car_view_memory_.end()) {
        camera.yaw = heading_of(i) + kept->second.yaw_from_heading;
        camera.pitch = kept->second.pitch;
        camera.distance = kept->second.distance;
        camera.view_zoom = kept->second.zoom;
    }
    update_car_view(camera);
    say(car_view_ == CarView::head ? tr("view from the first car") : tr("view from the last car"));
}

void TrainView::update_car_view(ViewerCamera& camera) {
    const Unit* unit = follow_id_ != 0 ? find_unit(follow_id_) : nullptr;
    if (!unit && !units_.empty()) unit = &units_.front();
    if (!unit || unit->current.empty() || unit->cars.empty()) return;
    const std::size_t i = car_view_ == CarView::head ? 0 : unit->cars.size() - 1;
    const auto body = body_pose(*unit, i, true);
    if (!body) return;
    camera.fly = false;
    camera.min_far = 20000.0f;
    // About the middle of the body's height.
    camera.target = glm::vec3{body->transform[3]} + glm::vec3{0.0f, 2.2f, 0.0f};
}

void TrainView::car_info_key(const bool shift) {
    // As RTS (0x402C88): the first press shows the page (the train as a
    // whole with Shift); then F5 turns the page, Shift+F5 switches to the
    // whole train, and past the last page (or from the whole train) it hides.
    const auto* unit = shown_unit();
    if (!car_info_) {
        car_info_ = true;
        car_info_overall_ = shift;
        car_page_ = 0;
        return;
    }
    if (car_info_overall_ || !unit) {
        car_info_ = false;
        return;
    }
    if (shift) {
        car_info_overall_ = true;
        return;
    }
    car_page_ += car_page_size;
    if (car_page_ >= unit->cars.size()) car_info_ = false;
}

const TrainView::Unit* TrainView::shown_unit() const {
    const Unit* unit = follow_id_ != 0 ? find_unit(follow_id_) : nullptr;
    if (!unit && !units_.empty()) unit = &units_.front();
    return unit && unit->train && unit->train->placed() ? unit : nullptr;
}

namespace {

// "%3 мост", "%A ...": a siding or platform named as a mark for the
// announcer (САВПЭ), not a place (Kuban: %1 %3 %7 %8 %A, 136 of its 690
// siding ends; Chernyshevsk 180 of 1752, some with a Cyrillic letter).
[[nodiscard]] bool service_mark(const std::string_view name) {
    return !name.empty() && name[0] == '%';
}

} // namespace

void TrainView::toggle_profile_strip() {
    if (!profile_strip_on_) profile_strip_on_ = remembered("profile_strip").value_or("0") == "1";
    profile_strip_on_ = !*profile_strip_on_;
    remember("profile_strip", *profile_strip_on_ ? "1" : "0");
    profile_built_at_ = -1e9;
}

void TrainView::draw_profile_strip(const Unit& unit) {
#ifdef OPENRTS_HAS_IMGUI
    if (!profile_strip_) profile_strip_ = std::make_unique<ProfileStrip>();
    auto& strip = *profile_strip_;
    const auto& train = *unit.train;
    const double now = ImGui::GetTime();
    (void)strip.take_changed(); // a new span or shift: built again below when it goes past the built way
    const double left = strip.left(), right = strip.right();
    const bool outside = left < profile_left_ || right > profile_right_;
    if (now - profile_built_at_ >= 1.0 || outside || profile_train_ != train.id()) {
        profile_built_at_ = now;
        profile_train_ = train.id();
        profile_marks_.clear();
        const auto lead = train.leading_end();
        if (!lead) {
            profile_ = {};
        } else {
            // A little more than shown, each way; a point every 10 m up to 5
            // km shown, every 20 m beyond.
            const double margin = 0.25 * strip.span();
            const double ahead = (std::max)(right + margin, 100.0);
            const double behind = (std::max)(-left + margin, (std::max)(train.length(), 100.0) + 100.0);
            profile_ = simulation::track_profile(*lead, ahead, behind, strip.span() > 5000.0 ? 20.0 : 10.0);
            profile_left_ = -profile_.behind;
            profile_right_ = profile_.ahead;
            for (const auto& [km, distance] : limits_->mileposts_along(*lead, ahead, behind)) {
                char text[16];
                std::snprintf(text, sizeof text, "%.0f", km);
                profile_marks_.push_back({ProfileMark::Kind::kilometre, distance, text, 0});
            }
            for (const auto& [speed, distance] : limits_->limits_ahead(train, unit.speed_category(), ahead, 64)) {
                char text[16];
                std::snprintf(text, sizeof text, "%.0f", speed * 3.6f);
                profile_marks_.push_back({ProfileMark::Kind::limit, distance, text, 0});
            }
            if (signals_) {
                // The chain ahead of the end that leads.
                const auto front = train.front_end();
                const bool front_leads = !front || (front->node() == lead->node() && std::abs(front->offset() - lead->offset()) < 1e-3);
                for (const auto& at : signals_->positions(train, front_leads)) {
                    if (!(at.flags & 1u) || at.distance > ahead) continue; // a normal head
                    // The lamp of its normal heads' strongest aspect.
                    const auto aspect = at.aspect[0];
                    const std::uint32_t colour = aspect <= 1 ? IM_COL32(230, 50, 40, 255)
                                                 : aspect == 2 ? IM_COL32(240, 240, 240, 255)
                                                 : aspect <= 5 ? IM_COL32(250, 210, 40, 255)
                                                 : aspect <= 7 ? IM_COL32(60, 210, 90, 255)
                                                               : IM_COL32(120, 120, 120, 255);
                    profile_marks_.push_back({ProfileMark::Kind::signal, at.distance, signal_names_ ? signal_names_(at.object) : std::string(), colour});
                }
            }
            for (const auto& place : stations_ahead(unit)) {
                if (place.distance > ahead) continue;
                profile_marks_.push_back({place.siding ? ProfileMark::Kind::siding : ProfileMark::Kind::station, place.distance, place.name, 0});
            }
        }
    }
    ProfileView view;
    view.profile = &profile_;
    view.marks = profile_marks_;
    view.train_length = train.length();
    view.mass_tonnes = train.mass() / 1000.0;
    view.cars = unit.cars.size();
    strip.draw(view);
#else
    (void)unit;
#endif
}

std::vector<TrainView::PlaceAhead> TrainView::stations_ahead(const Unit& unit) const {
    std::vector<PlaceAhead> result;
    const auto& items = network_->database().items;
    std::set<std::string> seen;
    const auto add = [&](const std::uint32_t item, const double distance) {
        if (item >= items.size()) return;
        if (items[item].kind == formats::TrackItemKind::siding) {
            // A siding's two ends carry its name: the nearer one.
            const auto& name = items[item].name;
            if (name.empty() || (!service_marks_ && service_mark(name)) || !seen.insert("\x01" + name).second) return;
            result.push_back({name, distance, true});
            return;
        }
        if (items[item].kind != formats::TrackItemKind::platform) return;
        const auto& platform = items[item];
        // "station, platform"; a platform named after its station adds only
        // the rest ("о.п. Фабрика кожизделий п.I" -> "о.п. Фабрика кожизделий, п.I").
        std::string label = platform.station.empty() ? platform.name : platform.station;
        if (!platform.station.empty() && !platform.name.empty() && platform.name != platform.station) {
            std::string_view rest = platform.name;
            if (rest.starts_with(platform.station)) rest.remove_prefix(platform.station.size());
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == ',' || rest.front() == '-')) rest.remove_prefix(1);
            if (!rest.empty()) label += ", " + std::string(rest);
        }
        if (label.empty() || (!service_marks_ && service_mark(label)) || !seen.insert(label).second) return;
        result.push_back({label, distance, false});
    };
    // Along the path when the train has one, else along the junctions as set.
    const auto id = unit.train->id();
    const auto* route = dispatcher_->route(id);
    const auto* progress = dispatcher_->progress(id);
    if (route && progress && progress->on_route) {
        for (std::size_t l = progress->leg; l < route->legs.size() && result.size() < 16; ++l) {
            const auto& leg = route->legs[l];
            const double base = l == progress->leg ? progress->head : 0.0;
            for (const auto& span : leg.spans) {
                if (span.start + span.length() < base) continue;
                const auto* node = network_->node(span.node);
                if (!node) continue;
                std::vector<std::pair<double, std::uint32_t>> here;
                for (const auto& entry : node->items) {
                    const double low = (std::min)(span.from, span.to), high = (std::max)(span.from, span.to);
                    if (entry.offset < low || entry.offset > high) continue;
                    const double along = span.start + (span.forward ? entry.offset - span.from : span.from - entry.offset) - base;
                    if (along >= 0.0) here.emplace_back(along, entry.item);
                }
                std::sort(here.begin(), here.end());
                for (const auto& [along, item] : here) add(item, along);
            }
            if (l != progress->leg) break; // the next leg runs back: only the current one
        }
        return result;
    }
    const auto lead = unit.train->leading_end();
    if (!lead) return result;
    const auto ahead = simulation::look_ahead(*network_, lead->node(), lead->offset(), lead->forward(), 40000.0,
                                              [](const formats::TrackItem& item) {
                                                  return item.kind == formats::TrackItemKind::platform || item.kind == formats::TrackItemKind::siding;
                                              },
                                              80);
    for (const auto& found : ahead.items) add(found.item, found.distance);
    return result;
}

bool TrainView::begin_panel(const char* id, HudPanel& place, const float default_x, const float default_y) {
#ifdef OPENRTS_HAS_IMGUI
    const auto display = ImGui::GetIO().DisplaySize;
    if (!place.loaded) {
        place.loaded = true;
        place.x = default_x;
        place.y = default_y;
        if (const auto saved = remembered(place.key)) {
            // "x y", fractions of the display.
            char* end = nullptr;
            const float x = std::strtof(saved->c_str(), &end);
            const char* rest = end;
            const float y = std::strtof(rest, &end);
            if (end != rest) {
                place.x = std::clamp(x, 0.0f, 1.0f);
                place.y = std::clamp(y, 0.0f, 1.0f);
            }
        }
    }
    // Moved by hand (NoMove): dragging anywhere on the panel shifts its
    // anchor, so a right-anchored panel keeps its right edge while its text
    // gets wider or narrower.
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    // Kept below the profile strip while it is shown.
    ImGui::SetNextWindowPos(ImVec2(place.x * display.x, (std::max)(place.y * display.y, hud_top_)), ImGuiCond_Always, ImVec2(place.pivot_x, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    const bool open = ImGui::Begin(id, nullptr, flags);
    if (open && display.x > 0.0f && display.y > 0.0f) {
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) place.dragging = true;
        if (place.dragging) {
            const auto delta = ImGui::GetIO().MouseDelta;
            const auto size = ImGui::GetWindowSize();
            // The whole panel stays on the screen.
            const float low_x = place.pivot_x * size.x / display.x;
            const float high_x = 1.0f - (1.0f - place.pivot_x) * size.x / display.x;
            place.x = std::clamp(place.x + delta.x / display.x, (std::min)(low_x, high_x), (std::max)(low_x, high_x));
            place.y = std::clamp(place.y + delta.y / display.y, 0.0f, (std::max)(1.0f - size.y / display.y, 0.0f));
        }
    }
    return open;
#else
    (void)id;
    (void)place;
    (void)default_x;
    (void)default_y;
    return false;
#endif
}

void TrainView::end_panel(HudPanel& place) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui::End();
    if (place.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        place.dragging = false;
        char text[64];
        std::snprintf(text, sizeof text, "%.4f %.4f", place.x, place.y);
        remember(place.key, text);
    }
#else
    (void)place;
#endif
}

void TrainView::draw_hud() {
    draw_car_menu();
#ifdef OPENRTS_HAS_IMGUI
    if (cargo_time_ > 0.0f && !cargo_text_.empty()) {
        // The car menu's "Cargo information" (RTS shows it as a message for 30 s).
        const auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2{size.x * 0.5f, size.y * 0.25f}, ImGuiCond_Always, ImVec2{0.5f, 0.0f});
        ImGui::SetNextWindowBgAlpha(0.8f);
        if (ImGui::Begin("###cargo_info", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                             | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            ImGui::TextUnformatted(cargo_text_.c_str());
        }
        ImGui::End();
    }
    const auto* unit = shown_unit();
    if (!unit) return;
    const auto& train = *unit->train;
    const auto display = ImGui::GetIO().DisplaySize;
    if (!profile_strip_on_) profile_strip_on_ = remembered("profile_strip").value_or("0") == "1";
    hud_top_ = *profile_strip_on_ ? ProfileStrip::height() + 4.0f : 0.0f;
    // Default places: 10 px from the side, 60 px from the top.
    const float margin_x = display.x > 0.0f ? 10.0f / display.x : 0.0f;
    const float margin_y = display.y > 0.0f ? 60.0f / display.y : 0.0f;
    const auto limit = limit_of(*unit);
    const auto ordinate = limits_->ordinate(train);
    if (car_info_) {
        if (begin_panel("##car_info", car_info_place_, margin_x, margin_y)) {
            if (car_info_overall_) {
                std::size_t passenger = 0, freight = 0, engines = 0;
                for (const auto& car : unit->cars) {
                    if (!car.definition) continue;
                    if (car.definition->engine) ++engines;
                    else if (car.definition->kind == formats::WagonKind::passenger) ++passenger;
                    else ++freight;
                }
                ImGui::Text(tr("Overall: mass %.1f t, length %.0f m, units %zu (passenger %zu, freight %zu, engines %zu)"),
                            train.mass() / 1000.0, train.length(), unit->cars.size(), passenger, freight, engines);
                // The driven car's pressures, so a small window shows them
                // without the car pages (testers, 2026-11-12).
                if (unit == &units_.front()) {
                    const auto i = driven_car();
                    if (i < train.cars().size()) {
                        const auto& state = train.cars()[i];
                        const auto ur = equalising_of(0, i);
                        ImGui::Text(tr("Driven car %zu: pipe %.2f  equalising %s  cylinder %.2f atm"), i + 1, state.pipe,
                                    ur ? trf("%.2f", *ur).c_str() : "-", state.cylinder);
                    }
                }
                ImGui::Text(tr("Brake pressing %.0f tf per 100 tf of weight (as cast-iron shoes), brakes' condition %s"),
                            train.brake_pressing(), train.brake_condition() == 0 ? tr("none (as the files)") : std::to_string(train.brake_condition()).c_str());
                std::size_t chocked = 0;
                for (const auto& state : train.cars())
                    if (state.chocks != 0) ++chocked;
                if (chocked > 0) ImGui::Text(tr("Chocks under %zu cars"), chocked);
            } else {
                char next[64];
                std::snprintf(next, sizeof next, "%s", tr("none"));
                if (limit.ahead > 0.0f) std::snprintf(next, sizeof next, tr("%.0f km/h in %.0f m"), limit.ahead * 3.6f, limit.ahead_distance);
                ImGui::Text(tr("Overall: speed %.2f km/h, acceleration %.3f m/s2, limit %.0f km/h, next limit %s, km %s"),
                            train.speed() * 3.6, train.cars().front().acceleration, limit.current * 3.6f, next,
                            ordinate ? (std::to_string(static_cast<int>(*ordinate)) + "." + std::to_string(static_cast<int>(std::abs(*ordinate) * 100.0) % 100)).c_str() : "-");
                const auto& states = train.cars();
                const auto last = (std::min)(car_page_ + car_page_size, unit->cars.size());
                // A car a row (RTS's F5 takes two lines a car, which filled
                // the screen). The speed and acceleration are the train's,
                // above. The УР: only the driven car's valve keeps one.
                const auto driven = unit == &units_.front() ? driven_car() : SIZE_MAX;
                // The coupler: the force the joint passes (Train::joint_forces:
                // on the move by Newton, RTS's CF stopping at the springs' full
                // stretch, 100 kN on a flat car, 20 on a gondola).
                const auto joint_force = train.joint_forces();
                // The РК: a 483's control chamber under the realistic air,
                // the level it releases at in the flat mode (docs/train.md).
                // A right click on a car's name opens the pick below, outside
                // the table: a table pushes its own ID, so a popup opened in
                // a cell would not be the one begun after it.
                bool pick_car = false;
                if (ImGui::BeginTable("##car_pages", 12, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                    for (const char* title :
                         {"No.", "Car", "t", "Pipe", "Equalising", "Aux", "Control ch.", "Cyl.", "Force, kN", "Brake, kN", "Coupler, kN (- squeezed)", ""})
                        ImGui::TableSetupColumn(*title ? tr(title) : "");
                    ImGui::TableHeadersRow();
                    for (std::size_t i = car_page_; i < last && i < states.size(); ++i) {
                        const auto& car = unit->cars[i];
                        const auto& state = states[i];
                        // Marks: the handbrake, the chocks (by the car's own
                        // ends, as the car menu puts them), a derailment.
                        std::string marks;
                        if (tape_.recording() && car.key == tape_car_key_) marks += tr(" [TAPE]");
                        if (state.handbrake > 0.0f) marks += trf(" [HANDBRAKE %d%%]", static_cast<int>(state.handbrake * 100.0f));
                        if (state.chocks != 0) {
                            const bool front = (state.chocks & simulation::chock_front) != 0, rear = (state.chocks & simulation::chock_rear) != 0;
                            marks += front && rear ? tr(" [CHOCKS front and rear]") : front ? tr(" [CHOCK front]") : tr(" [CHOCK rear]");
                        }
                        if (state.derailed) marks += tr(" [DERAILED]");
                        // Wheel slide and the shoes (the realistic air, adhesion.hpp).
                        if (state.wheelslip != 0) {
                            std::string slipping;
                            for (std::uint32_t a = 0; a < 32; ++a)
                                if (state.wheelslip & (1u << a)) slipping += (slipping.empty() ? "" : ",") + std::to_string(a + 1);
                            marks += state.wheelslip == 15 && state.axles == 0 ? tr(" [WHEEL SLIP]") : trf(" [WHEEL SLIP: axles %s]", slipping.c_str());
                        }
                        // openRTS's limit: each axle's force as a share of its grip (the first of a bogie the most).
                        if (state.axles > 0 && std::abs(state.force) > 1.0f) {
                            std::string shares;
                            for (std::size_t a = 0; a < state.axles; ++a)
                                shares += (a == 0 ? "" : " ") + std::to_string(static_cast<int>(std::lround(std::abs(state.slip_ratio[a]) * 100.0f)))
                                          + ((state.wheelslip & (1u << a)) ? "*" : "");
                            marks += trf(" [grip by axle %s %%]", shares.c_str());
                        }
                        if (const float flat = (std::max)(state.flat[0], state.flat[1]); flat >= 0.1f) marks += trf(" [wheel flat %.1f mm]", flat);
                        if (state.sliding()) marks += tr(" [WHEEL SLIDE]");
                        else if (state.slid >= 0.5f) marks += trf(" [slid %.0f m]", state.slid);
                        if ((std::max)(state.shoe_state[0].wet, state.shoe_state[1].wet) >= 0.5f) marks += tr(" [shoes wet]");
                        // Wooden shoes: alight, smouldering, burnt away.
                        if (state.shoe_state[0].burning || state.shoe_state[1].burning) {
                            marks += tr(" [SHOES ON FIRE]");
                        } else if ((state.shoes[0] == formats::BrakeShoe::wood && state.shoe_state[0].heat >= simulation::wood_smoulders) ||
                                   (state.shoes[1] == formats::BrakeShoe::wood && state.shoe_state[1].heat >= simulation::wood_smoulders)) {
                            marks += tr(" [shoes smouldering]");
                        }
                        if (state.shoe_state[0].wear >= 1.0f || state.shoe_state[1].wear >= 1.0f) marks += tr(" [shoes burnt away]");
                        if ((std::max)(state.shoe_state[0].heat, state.shoe_state[1].heat) >= 100.0f) {
                            marks += trf(" [shoes +%.0f K]", (std::max)(state.shoe_state[0].heat, state.shoe_state[1].heat));
                        }
                        // Valves a hand opened (the car menu): they bleed the cylinder
                        // or vent the pipe, so they are said here.
                        if (state.valves & simulation::valve_release) marks += tr(" [RELEASE VALVE OPEN]");
                        if (state.valves & simulation::valve_emergency) marks += tr(" [EMERGENCY VALVE OPEN]");
                        // A DLL's own brake force (Engine::BrakeForce: the
                        // dynamic brake, or an EP brake it works by itself):
                        // the brake column has it, the cylinder does not.
                        const auto u = static_cast<std::size_t>(unit - units_.data());
                        if (const auto* out = dll_car(u, i) ? dll_output(u, i) : nullptr;
                            out && out->has_engine && std::abs(out->engine.brake_force) >= 500.0f) {
                            marks += trf(" [DLL brake %.0f kN]", out->engine.brake_force / 1000.0f);
                        }
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%02zu", i + 1);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(car.definition ? car.definition->name.c_str() : "?");
                        // A right click on the car: its bogie or coupling menu.
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                            f5_car_pick_ = std::pair{unit->train->id(), i};
                            pick_car = true;
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", car.definition ? car.definition->mass / 1000.0f : 0.0f);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", state.pipe);
                        ImGui::TableNextColumn();
                        if (const auto ur = i == driven ? equalising_of(0, i) : std::nullopt) {
                            ImGui::Text("%.2f", *ur);
                        } else {
                            ImGui::TextDisabled("-");
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", state.auxiliary);
                        ImGui::TableNextColumn();
                        if (car.definition && (car.definition->brakes.system & 5) &&
                            simulation::distributor_kind(*car.definition) == simulation::DistributorKind::vr483) {
                            ImGui::Text("%.2f", state.control);
                        } else {
                            ImGui::TextDisabled("-");
                        }
                        // The cylinder the brake force takes: the train brake's
                        // or the independent brake's, the higher (RTS's
                        // BrakeForce; an EMU's DLL may brake its own car with
                        // the independent one), marked when it is that one.
                        ImGui::TableNextColumn();
                        if (state.independent > state.cylinder + 0.005f) {
                            ImGui::Text("%.2f %s", state.independent, tr("(ind.)"));
                        } else {
                            ImGui::Text("%.2f", state.cylinder);
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", state.force / 1000.0f);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", state.brake_force / 1000.0f);
                        ImGui::TableNextColumn();
                        if (i + 1 < states.size()) {
                            ImGui::Text("%.0f", joint_force[i] / 1000.0f);
                        } else {
                            ImGui::TextDisabled("-");
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(marks.c_str());
                    }
                    // The whole train (every page): the traction and brake
                    // forces summed; the couplers' forces are inner and cancel
                    // in a sum, so the strongest pull and squeeze, the joint's
                    // number counted from the front (as RTS numbers them).
                    double mass = 0.0, force = 0.0, brake = 0.0;
                    float pull = 0.0f, squeeze = 0.0f;
                    std::size_t pull_at = 0, squeeze_at = 0;
                    for (std::size_t k = 0; k < states.size(); ++k) {
                        mass += k < unit->cars.size() && unit->cars[k].definition ? unit->cars[k].definition->mass : 0.0f;
                        force += states[k].force;
                        brake += states[k].brake_force;
                        if (k + 1 < states.size()) {
                            const float c = joint_force[k];
                            if (c > pull) pull = c, pull_at = k + 1;
                            if (-c > squeeze) squeeze = -c, squeeze_at = k + 1;
                        }
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted("="); // the font has no Greek: no Σ
                    ImGui::TableNextColumn();
                    ImGui::Text(tr("whole train, %zu cars"), states.size());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", mass / 1000.0);
                    for (int k = 0; k < 5; ++k) ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", force / 1000.0);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", brake / 1000.0);
                    ImGui::TableNextColumn();
                    if (pull_at || squeeze_at) {
                        ImGui::Text(tr("most pulled %.0f (No. %zu), squeezed %.0f (No. %zu)"), pull / 1000.0f, pull_at, squeeze / 1000.0f, squeeze_at);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                    ImGui::EndTable();
                }
                // F5's right click: which of the car's bogies or couplings.
                if (pick_car) ImGui::OpenPopup("##f5_car_pick");
                if (ImGui::BeginPopup("##f5_car_pick")) {
                    if (f5_car_pick_) {
                        const auto [train_id, index] = *f5_car_pick_;
                        ImGui::TextDisabled("%s", trf("car %zu", index + 1).c_str());
                        const auto open = [&](const bool coupling, const bool front) {
                            car_menu_ = CarMenu{.coupling = coupling, .train = train_id, .car = index, .front = front, .remote = true};
                            last_car_menu_ = car_menu_;
                            car_menu_choice_.reset();
                            ImGui::CloseCurrentPopup();
                        };
                        if (ImGui::Selectable(tr("Bogie towards the train's head"))) open(false, true);
                        if (ImGui::Selectable(tr("Bogie towards the train's tail"))) open(false, false);
                        if (ImGui::Selectable(tr("Coupling towards the head"))) open(true, true);
                        if (ImGui::Selectable(tr("Coupling towards the tail"))) open(true, false);
                    }
                    ImGui::EndPopup();
                }
                // What else acts on the whole train: the grade (+ pulls it
                // towards its rear) and the rolling resistance.
                double grade = 0.0, resistance = 0.0;
                for (const auto& s : states) {
                    grade += s.grade;
                    resistance += s.resistance;
                }
                ImGui::Text(tr("Whole train: grade %.1f kN (+ pulls to the rear), resistance %.1f kN"), grade / 1000.0, resistance / 1000.0);
                ImGui::TextDisabled(tr("F5: next page (%zu-%zu of %zu)  Shift+F5: the whole train"), car_page_ + 1, last, unit->cars.size());
            }
        }
        end_panel(car_info_place_);
    }
    if (limits_panel_) {
        if (begin_panel("##limits", limits_place_, 1.0f - margin_x, margin_y)) {
            if (ordinate) {
                const double km = std::floor(*ordinate);
                const int picket = static_cast<int>((*ordinate - km) * 10.0) + 1;
                ImGui::Text(tr("km %.0f  pk %d  (%.3f)"), km, picket, *ordinate);
            } else {
                ImGui::TextDisabled(tr("km -"));
            }
            ImGui::Text(tr("Limit %.0f km/h"), limit.current * 3.6f);
            ImGui::Separator();
            const auto ahead = limits_->limits_ahead(train, unit->speed_category(), 10000.0, 6);
            if (!ahead.empty() && ImGui::BeginTable("##limits_ahead", 2, ImGuiTableFlags_SizingFixedFit)) {
                for (const auto& [speed, distance] : ahead) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%4.0f km/h", speed * 3.6f);
                    ImGui::TableNextColumn();
                    ImGui::Text(distance >= 1000.0 ? "%.2f km" : "%.0f m", distance >= 1000.0 ? distance / 1000.0 : distance);
                }
                ImGui::EndTable();
            }
            if (signals_) {
                const auto next = signals_->ahead(train);
                if (next.valid) {
                    ImGui::Separator();
                    ImGui::Text(tr("Signal %s in %.0f m%s"), formats::to_string(next.aspect), next.distance,
                                next.speed > 0.0f ? (", " + std::to_string(static_cast<int>(next.speed * 3.6f)) + " km/h").c_str() : "");
                }
                if (alsn_) ImGui::Text(tr("ALSN %s"), simulation::alsn_name(alsn_->code(train)));
            }
        }
        end_panel(limits_place_);
    }
    if (track_profile_panel_ && limits_ && network_) {
        if (begin_panel("##profile", profile_place_, 1.0f - margin_x, 0.30f)) {
            const auto profile = simulation::build_track_window(train, *network_, limits_.get(), unit->speed_category(), signals_, {});
            ImGui::Text(tr("Grade %+.1f per mille, rail %.2f m"), profile.grade_at_head, profile.elevation_at_head);
            if (profile.current_limit > 0.0f) ImGui::Text(tr("Limit %.0f km/h"), profile.current_limit * 3.6f);
            else ImGui::TextDisabled(tr("Limit -"));
            const ImVec2 size(520.0f, 130.0f);
            ImGui::Dummy(size);
            const ImVec2 origin = ImGui::GetItemRectMin();
            auto* draw = ImGui::GetWindowDrawList();
            const auto& vertices = profile.vertices;
            if (vertices.size() >= 2) {
                const double first = vertices.front().distance;
                const double last = vertices.back().distance;
                double low = vertices.front().elevation;
                double high = low;
                for (const auto& vertex : vertices) {
                    low = std::min(low, vertex.elevation);
                    high = std::max(high, vertex.elevation);
                }
                if (high - low < 2.0) {
                    const double middle = 0.5 * (low + high);
                    low = middle - 1.0;
                    high = middle + 1.0;
                }
                const double span = std::max(1.0, last - first);
                const auto x_of = [&](const double distance) {
                    return origin.x + static_cast<float>((distance - first) / span) * size.x;
                };
                const auto y_of = [&](const double elevation) {
                    return origin.y + size.y * (1.0f - static_cast<float>((elevation - low) / (high - low)));
                };
                draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(120, 120, 120, 160));
                for (const auto& occupied : profile.occupied) {
                    draw->AddRectFilled(ImVec2(x_of(occupied.from), origin.y), ImVec2(x_of(occupied.to), origin.y + size.y),
                                        IM_COL32(150, 150, 150, 50));
                }
                for (std::size_t i = 1; i < vertices.size(); ++i) {
                    const auto& from = vertices[i - 1];
                    const auto& to = vertices[i];
                    draw->AddLine(ImVec2(x_of(from.distance), y_of(from.elevation)), ImVec2(x_of(to.distance), y_of(to.elevation)),
                                  IM_COL32(240, 240, 240, 220), 1.5f);
                }
                for (const auto& mark : profile.marks) {
                    if (mark.distance < first || mark.distance > last) continue;
                    ImU32 colour = IM_COL32(160, 160, 160, 180);
                    switch (mark.kind) {
                    case simulation::ProfileMarkKind::signal:
                        colour = mark.at_danger ? IM_COL32(230, 70, 70, 230) : IM_COL32(90, 210, 110, 230);
                        break;
                    case simulation::ProfileMarkKind::speed_post: colour = IM_COL32(230, 200, 80, 230); break;
                    case simulation::ProfileMarkKind::station:
                    case simulation::ProfileMarkKind::platform: colour = IM_COL32(110, 180, 230, 230); break;
                    case simulation::ProfileMarkKind::siding: colour = IM_COL32(160, 160, 160, 180); break;
                    }
                    const float x = x_of(mark.distance);
                    draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), colour, 1.2f);
                    if (!mark.name.empty() && mark.kind != simulation::ProfileMarkKind::siding && std::abs(mark.distance) < 400.0) {
                        draw->AddText(ImVec2(x + 2.0f, origin.y + 2.0f), colour, mark.name.c_str());
                    }
                }
                const float head = x_of(0.0);
                draw->AddLine(ImVec2(head, origin.y - 4.0f), ImVec2(head, origin.y + size.y + 4.0f), IM_COL32(255, 255, 255, 245),
                              2.0f);
            } else {
                ImGui::TextDisabled(tr("no track"));
            }
            ImGui::TextDisabled(tr("%.0f m behind, %.0f m ahead%s"), profile.behind, profile.ahead,
                                profile.cut_behind || profile.cut_ahead ? tr(" (cut)") : "");
        }
        end_panel(profile_place_);
    }
    if (!drive_panel_) drive_panel_ = remembered("drive_panel").value_or("1") != "0";
    if (*drive_panel_) draw_drive_panel(*unit);
    if (!profile_strip_on_) profile_strip_on_ = remembered("profile_strip").value_or("0") == "1";
    if (*profile_strip_on_) draw_profile_strip(*unit);
    if (stations_panel_) {
        if (begin_panel("##stations", stations_place_, 1.0f - margin_x, 0.45f)) {
            if (!timetable_.empty() && unit == &units_.front()) {
                // A scenario's timetable: RTS shows it here when the train has one.
                draw_timetable(*unit);
            } else {
                ImGui::TextUnformatted(tr("Stations ahead"));
                ImGui::Separator();
                if (!service_marks_loaded_) {
                    service_marks_loaded_ = true;
                    service_marks_ = remembered("f6_service_marks").value_or("0") == "1";
                }
                const auto stations = stations_ahead(*unit);
                if (stations.empty()) ImGui::TextDisabled(tr("none within 40 km"));
                // Two columns sized to the longest name: names never run into
                // the distances, which are right-aligned. Sidings in yellow, as
                // F7 and Track control draw them.
                if (!stations.empty() && ImGui::BeginTable("##stations_ahead", 2, ImGuiTableFlags_SizingFixedFit)) {
                    for (std::size_t k = 0; k < stations.size() && k < 12; ++k) {
                        const auto& [name, distance, siding] = stations[k];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (siding) {
                            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.1f, 1.0f), "%s", name.c_str());
                        } else {
                            ImGui::TextUnformatted(name.c_str());
                        }
                        ImGui::TableNextColumn();
                        char text[32];
                        std::snprintf(text, sizeof text, distance >= 1000.0 ? tr("in %.2f km") : tr("in %.0f m"),
                                      distance >= 1000.0 ? distance / 1000.0 : distance);
                        ImGui::TextUnformatted(text);
                    }
                    ImGui::EndTable();
                }
                ImGui::Separator();
                if (ImGui::Checkbox(tr("marks (%n ...)"), &service_marks_)) {
                    remember("f6_service_marks", service_marks_ ? "1" : "0");
                }
            }
        }
        end_panel(stations_place_);
    }
#endif
}

void TrainView::toggle_drive_panel() {
    drive_panel_ = !drive_panel_.value_or(true);
    remember("drive_panel", *drive_panel_ ? "1" : "0");
}

void TrainView::draw_drive_panel(const Unit& unit) {
#ifdef OPENRTS_HAS_IMGUI
    const auto& train = *unit.train;
    const auto display = ImGui::GetIO().DisplaySize;
    const float margin_x = display.x > 0.0f ? 10.0f / display.x : 0.0f;
    const float margin_y = display.y > 0.0f ? 60.0f / display.y : 0.0f;
    if (begin_panel("##drive", drive_place_, 1.0f - margin_x, margin_y)) {
        const ImVec4 dim{0.62f, 0.66f, 0.70f, 1.0f};
        const auto caption = [&](const char* text) { ImGui::TextColored(dim, "%s", text); };
        const auto big = [&](ImFont* font, const ImVec4& colour, const char* text) {
            if (font) ImGui::PushFont(font);
            else ImGui::SetWindowFontScale(1.8f);
            ImGui::TextColored(colour, "%s", text);
            if (font) ImGui::PopFont();
            else ImGui::SetWindowFontScale(1.0f);
        };
        const float speed = std::abs(static_cast<float>(train.speed())) * 3.6f;
        const auto limit = limit_of(unit);
        const float allowed = limit.current * 3.6f;
        ImGui::TextUnformatted(speed > 0.3f ? tr("MOVING") : tr("STANDING"));
        ImGui::Separator();
        // The speed and its limit: green within it, amber close, red over.
        if (ImGui::BeginTable("##drive_speed", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            caption(tr("speed"));
            char text[32];
            std::snprintf(text, sizeof text, "%.0f", speed);
            const ImVec4 colour = allowed > 0.0f && speed > allowed + 0.5f ? ImVec4{1.0f, 0.3f, 0.25f, 1.0f}
                                  : allowed > 0.0f && speed > allowed - 3.0f ? ImVec4{1.0f, 0.75f, 0.2f, 1.0f}
                                                                             : ImVec4{0.55f, 0.95f, 0.35f, 1.0f};
            big(title_font(), colour, text);
            const auto i = &unit == &units_.front() ? driven_car() : 0;
            const float accel = i < train.cars().size() ? train.cars()[i].acceleration : 0.0f;
            ImGui::TextColored(dim, tr("%+.2f m/s2"), accel);
            ImGui::TableNextColumn();
            ImGui::Dummy({16.0f, 0.0f});
            ImGui::SameLine();
            ImGui::BeginGroup();
            caption(tr("limit"));
            std::snprintf(text, sizeof text, allowed > 0.0f ? "%.0f" : "-", allowed);
            big(title_font(), ImVec4{0.95f, 0.95f, 0.95f, 1.0f}, text);
            if (limit.ahead > 0.0f && limit.ahead * 3.6f < allowed - 0.5f) {
                ImGui::TextColored(dim, tr("%.0f in %.0f m"), limit.ahead * 3.6f, limit.ahead_distance);
            }
            ImGui::EndGroup();
            ImGui::EndTable();
        }
        ImGui::Separator();
        // The driven car's pressures, kgf/cm2 (atm).
        if (const auto i = &unit == &units_.front() ? driven_car() : SIZE_MAX; i < train.cars().size()) {
            const auto& state = train.cars()[i];
            const auto ur = equalising_of(0, i);
            if (ImGui::BeginTable("##drive_air", 4, ImGuiTableFlags_SizingStretchSame)) {
                for (const char* name : {tr("TM"), tr("TC"), tr("UR"), tr("GR")}) {
                    ImGui::TableNextColumn();
                    caption(name);
                }
                const float values[4] = {state.pipe, (std::max)(state.cylinder, state.independent), ur.value_or(-1.0f), state.main_reservoir};
                for (const float v : values) {
                    ImGui::TableNextColumn();
                    char text[16];
                    std::snprintf(text, sizeof text, v >= 0.0f ? "%.1f" : "-", v);
                    big(large_font(), ImVec4{0.95f, 0.95f, 0.95f, 1.0f}, text);
                }
                ImGui::EndTable();
            }
            caption(tr("pressure, kgf/cm2"));
            ImGui::Separator();
        }
        // The next signal: its colour, its name (литера) or aspect, how far.
        caption(tr("next signal"));
        if (const auto next = signals_ ? signals_->ahead(train) : simulation::SignalAhead{}; next.valid) {
            using formats::SignalAspect;
            const ImVec4 colour = next.aspect == SignalAspect::stop ? ImVec4{1.0f, 0.25f, 0.2f, 1.0f}
                                  : next.aspect == SignalAspect::stop_and_proceed || next.aspect == SignalAspect::restricting
                                      ? ImVec4{0.9f, 0.9f, 0.95f, 1.0f}
                                  : next.aspect == SignalAspect::clear_1 || next.aspect == SignalAspect::clear_2 ? ImVec4{0.3f, 0.9f, 0.35f, 1.0f}
                                                                                                                  : ImVec4{1.0f, 0.82f, 0.2f, 1.0f};
            auto* list = ImGui::GetWindowDrawList();
            const auto at = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight();
            list->AddCircleFilled({at.x + h * 0.4f, at.y + h * 0.5f}, h * 0.32f, ImGui::GetColorU32(colour));
            ImGui::Dummy({h * 0.9f, h});
            ImGui::SameLine();
            std::string name = signal_names_ ? signal_names_(next.object) : std::string{};
            if (name.empty()) name = formats::to_string(next.aspect);
            ImGui::TextUnformatted(name.c_str());
            ImGui::SameLine();
            char text[32];
            std::snprintf(text, sizeof text, next.distance >= 1000.0 ? tr("%.2f km") : tr("%.0f m"),
                          next.distance >= 1000.0 ? next.distance / 1000.0 : next.distance);
            ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(text).x));
            ImGui::TextUnformatted(text);
        } else {
            ImGui::TextColored(dim, "%s", tr("none ahead"));
        }
        ImGui::Separator();
        // The next platform.
        caption(tr("platform"));
        const auto places = stations_ahead(unit);
        const auto platform = std::find_if(places.begin(), places.end(), [](const PlaceAhead& p) { return !p.siding; });
        if (platform != places.end()) {
            ImGui::TextUnformatted(platform->name.c_str());
            ImGui::TextColored(dim, platform->distance >= 1000.0 ? tr("%.2f km") : tr("%.0f m"),
                               platform->distance >= 1000.0 ? platform->distance / 1000.0 : platform->distance);
        } else {
            ImGui::TextColored(dim, "%s", tr("none within 40 km"));
        }
        ImGui::Separator();
        // The time of day and the frame rate.
        const auto seconds = static_cast<long long>(day_time_) % 86400;
        ImGui::Text("%lld:%02lld:%02lld", seconds / 3600, seconds % 3600 / 60, seconds % 60);
        ImGui::SameLine();
        char fps[24];
        std::snprintf(fps, sizeof fps, "%.0f FPS", ImGui::GetIO().Framerate);
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(fps).x));
        ImGui::TextColored(dim, "%s", fps);
        ImGui::TextColored(dim, "%s", tr("F4: hide"));
    }
    end_panel(drive_place_);
#else
    (void)unit;
#endif
}

void TrainView::follow_train(const std::uint64_t id) {
    follow_id_ = id;
    follow_ = true;
}

std::string TrainView::path_status(const std::uint64_t id) const {
    const auto* route = dispatcher_->route(id);
    const auto* progress = dispatcher_->progress(id);
    if (!route || !progress) return "no path";
    const auto& leg = route->legs[progress->leg];
    char text[256];
    if (!progress->on_route) return route->name + ": not on its path";
    if (progress->finished) return route->name + ": arrived";
    std::snprintf(text, sizeof text, tr("%s: leg %zu/%zu, %.0f of %.0f m, way reserved for %.0f m"), route->name.c_str(),
                  progress->leg + 1, route->legs.size(), progress->head, leg.length(),
                  (std::max)(0.0, progress->authority - progress->head));
    std::string result = text;
    if (progress->blocked_at) {
        result += ", waits at junction " + std::to_string(*progress->blocked_at);
        if (progress->blocked_by != 0) {
            const auto* other = find_unit(progress->blocked_by);
            result += " for " + (other ? other->name : "train " + std::to_string(progress->blocked_by));
        }
    }
    return result;
}

float TrainView::alpha() const noexcept {
    return static_cast<float>(std::clamp(accumulator_ / simulation::train_step, 0.0, 1.0));
}

// The car's bogie points between the last two steps, at the render time.
// Terrain height under a local point (bilinear between the tile samples),
// local metres; nothing where the tile is missing.
std::optional<float> TrainView::ground_height(const glm::vec3& point) const {
    const double absolute_x = static_cast<double>(origin_.tile_x) * simulation::tile_size + origin_.x + point.x;
    const double absolute_z = static_cast<double>(origin_.tile_z) * simulation::tile_size + origin_.z + point.z;
    const auto tile_x = static_cast<std::int32_t>(std::floor((absolute_x + simulation::tile_size * 0.5) / simulation::tile_size));
    const auto tile_z = static_cast<std::int32_t>(std::floor((absolute_z + simulation::tile_size * 0.5) / simulation::tile_size));
    const auto key = (static_cast<std::int64_t>(tile_x) << 32) ^ static_cast<std::uint32_t>(tile_z);
    auto found = terrain_.find(key);
    if (found == terrain_.end()) {
        auto tile = std::make_unique<formats::TerrainTile>(formats::load_terrain_tile(*vfs_, route_.resolve("TILES").value_or(route_), tile_x, tile_z));
        if (!tile->ok() || tile->sample_count < 2) tile.reset();
        found = terrain_.emplace(key, std::move(tile)).first;
    }
    const auto* tile = found->second.get();
    if (!tile) return std::nullopt;
    const double x = absolute_x - static_cast<double>(tile_x) * simulation::tile_size;
    const double z = absolute_z - static_cast<double>(tile_z) * simulation::tile_size;
    const double half = static_cast<double>(tile->sample_count) * tile->sample_size * 0.5;
    const double column = (x + half) / tile->sample_size;
    const double row = (half - z) / tile->sample_size;
    const auto last = static_cast<double>(tile->sample_count - 1);
    const double c = std::clamp(column, 0.0, last);
    const double r = std::clamp(row, 0.0, last);
    const auto c0 = static_cast<std::uint32_t>((std::min)(std::floor(c), last - 1.0));
    const auto r0 = static_cast<std::uint32_t>((std::min)(std::floor(r), last - 1.0));
    const auto fc = static_cast<float>(c - c0);
    const auto fr = static_cast<float>(r - r0);
    const float top = tile->height(r0, c0) * (1.0f - fc) + tile->height(r0, c0 + 1) * fc;
    const float bottom = tile->height(r0 + 1, c0) * (1.0f - fc) + tile->height(r0 + 1, c0 + 1) * fc;
    return top * (1.0f - fr) + bottom * fr - static_cast<float>(origin_.y);
}

// A car slid off the track: on along the line it left by, folded about its
// end towards the slide by its yaw, down on the ground (RTS 0x58A390:
// the body rests on what is under it; on the ballast 0.3 m below the rails
// where there is no terrain).
std::optional<TrainView::Pose> TrainView::slid_pose(const Unit& unit, const std::size_t i) const {
    const auto& states = unit.train->cars();
    if (i >= states.size()) return std::nullopt;
    const auto& state = states[i];
    if (state.derail != simulation::DerailKind::sliding || !state.slide_origin_set) return std::nullopt;
    const float lift = cars_lift(unit, i);
    const glm::vec3 up{0.0f, lift, 0.0f};
    auto front = local(state.slide_from.front) + up;
    auto rear = local(state.slide_from.rear) + up;
    auto along = front - rear;
    along.y = 0.0f;
    if (glm::length(along) > 0.01f) along = glm::normalize(along);
    front += along * state.slide;
    rear += along * state.slide;
    if (state.yaw != 0.0f) {
        const bool about_front = state.slide >= 0.0f;
        const auto pivot = about_front ? front : rear;
        const auto turn = glm::rotate(glm::mat4{1.0f}, state.yaw, glm::vec3{0.0f, 1.0f, 0.0f});
        const auto moved = glm::vec3{turn * glm::vec4{(about_front ? rear : front) - pivot, 0.0f}} + pivot;
        (about_front ? rear : front) = moved;
    }
    const auto settle = [&](glm::vec3& point, const float rail) {
        float y = rail - 0.3f;
        if (const auto ground = ground_height(point)) y = std::clamp(*ground + lift, rail - 6.0f, rail - 0.3f);
        point.y = y;
    };
    settle(front, local(state.slide_from.front).y + lift);
    settle(rear, local(state.slide_from.rear).y + lift);
    Pose pose;
    pose.front = front;
    pose.rear = rear;
    const float heading = std::atan2(front.x - rear.x, front.z - rear.z);
    pose.front_heading = pose.rear_heading = heading;
    return pose;
}

// A sliding car against the world's obstacles (RTS 0x58A630: objects with
// CollideFlags, bit 2 clear, their bounding sphere then their triangles;
// openRTS: half the sphere, ahead of the car): it stops and takes the hit.
void TrainView::collide_slides(renderer::ModelLibrary& library) {
    if (!obstacles_) return;
    for (auto& unit : units_) {
        const auto& states = unit.train->cars();
        for (std::size_t i = 0; i < states.size() && i < unit.cars.size(); ++i) {
            const auto& state = states[i];
            if (state.derail != simulation::DerailKind::sliding || state.slide_speed == 0.0f) continue;
            const auto pose = slid_pose(unit, i);
            if (!pose) continue;
            const auto centre = (pose->front + pose->rear) * 0.5f;
            auto heading = pose->front - pose->rear;
            heading.y = 0.0f;
            if (glm::length(heading) < 0.01f) continue;
            heading = glm::normalize(heading) * (state.slide_speed > 0.0f ? 1.0f : -1.0f);
            const float half_length = glm::length(pose->front - pose->rear) * 0.5f + 2.0f;
            for (const auto& object : *obstacles_) {
                if (object.collide_flags == 0 || (object.collide_flags & 2u) != 0) continue;
                if (object.kind != formats::WorldObjectKind::static_object && object.kind != formats::WorldObjectKind::collide_object) continue;
                auto* model = library.find(object.source_path);
                if (!model) continue;
                const auto bounds = model->bounds();
                if (bounds.radius <= 0.0f) continue;
                const auto at = glm::vec3{object.transform * glm::vec4{bounds.center, 1.0f}};
                auto to = at - centre;
                to.y = 0.0f;
                const float reach = bounds.radius * 0.5f + half_length;
                if (glm::dot(to, to) > reach * reach || glm::dot(to, heading) <= 0.0f) continue;
                const float m = unit.cars[i].definition->mass > 0.0f ? unit.cars[i].definition->mass : 50000.0f;
                const float force = m * std::abs(state.slide_speed) / 0.2f;
                const glm::vec3 turning{heading.z, 0.0f, -heading.x};
                const float side = glm::dot(to, turning) > 0.0f ? -1.0f : 1.0f;
                const float direction[3] = {side, 0.0f, state.slide_speed > 0.0f ? -1.0f : 1.0f};
                unit.train->stop_slide(i);
                unit.train->hit(i, force, direction, static_cast<float>(simulation::train_step));
                message_ = trf("a derailed car hit %s", std::string(object.source_path.filename()).c_str());
                break;
            }
        }
    }
}

float TrainView::cars_lift(const Unit& unit, const std::size_t car) const {
    return car < unit.cars.size() ? unit.cars[car].lift : default_lift;
}

TrainView::Pose TrainView::pose(const Unit& unit, const std::size_t car) const {
    const auto& now = unit.current[car];
    const auto& before = car < unit.previous.size() ? unit.previous[car] : now;
    const float t = alpha();
    const float lift = unit.cars[car].lift;
    const glm::vec3 up{0.0f, lift, 0.0f};
    return {
        .front = glm::mix(local(before.front), local(now.front), t) + up,
        .rear = glm::mix(local(before.rear), local(now.rear), t) + up,
        .front_heading = mix_angle(before.front.heading, now.front.heading, t),
        .rear_heading = mix_angle(before.rear.heading, now.rear.heading, t),
    };
}

simulation::RailCondition TrainView::rails_now() const noexcept {
    using simulation::RailCondition;
    switch (rails_) {
    case 0: return RailCondition::rts;
    case 1: return simulation::rails_for_weather(weather_);
    case 2: return RailCondition::dry;
    case 3: return RailCondition::wet;
    case 4: return RailCondition::snow;
    case 5: return RailCondition::ice;
    default: return RailCondition::leaves;
    }
}

std::optional<glm::vec3> TrainView::eye_in_cab(const glm::vec3& eye) const {
    const auto* cab = engineer_cab();
    if (!viewing_inside() || !cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) return std::nullopt;
    const auto body = body_pose(units_.front(), engineer_car_, false);
    if (!body) return std::nullopt;
    const glm::mat4 to_cab = glm::inverse(cab_to_scene_ ? *cab_to_scene_ : body->transform * cab->placement());
    return glm::vec3{to_cab * glm::vec4{eye, 1.0f}};
}

void TrainView::update(const SceneInput& given_input, ViewerCamera& camera, const float elapsed,
                       renderer::ModelLibrary& library, audio::AudioEngine* sound) {
    OPENRTS_ZONE("trains: update");
    camera_eye_ = camera.eye();
    // The frame the window will draw (the wheel on a cab's screen fits it in).
    frame_aspect_ = camera.aspect;
    frame_fov_ = camera.frame_fov;
    // While the car menu is open its window has the arrows and Enter.
    SceneInput menu_input;
    if (car_menu_) {
        menu_input = given_input;
        menu_input.walk_forward = menu_input.walk_back = menu_input.walk_left = menu_input.walk_right = false;
        menu_input.engineer_action = menu_input.engineer_action_ctrl = false;
    }
    const SceneInput& input = car_menu_ ? menu_input : given_input;
    frame_elapsed_ = elapsed;
    // The DLL trains' air (the Train window's choice).
    if (!dll_air_loaded_) {
        dll_air_loaded_ = true;
        dll_air_ = std::clamp(std::atoi(remembered("dll_air").value_or("0").c_str()), 0, 2);
    }
    if (legacy_) legacy_->set_air(static_cast<trainplugin::DllAir>(dll_air_));
    if (!own_brakes_loaded_) {
        own_brakes_loaded_ = true;
        own_brakes_ = std::clamp(std::atoi(remembered("own_brakes").value_or("0").c_str()), 0, 1);
        brake_numbers_ = std::clamp(std::atoi(remembered("brake_numbers").value_or("0").c_str()), 0, 1);
        own_klub_ = std::clamp(std::atoi(remembered("own_klub").value_or("0").c_str()), 0, 1);
    }
    if (legacy_) legacy_->set_own_brakes(own_brakes_ == 1, brake_numbers_ == 1);
    if (legacy_) legacy_->set_own_klub(own_klub_ == 1);
    if (network_ && network_->rail_joints().layout() != rail_joint_layout()) network_->set_rail_joint_layout(rail_joint_layout());
    // The rails' state (the Train window's choice, or the weather's).
    if (!rails_loaded_) {
        rails_loaded_ = true;
        rails_ = std::clamp(std::atoi(remembered("rails").value_or("1").c_str()), 0, 6);
        // On unless turned off (2026-11-18): MaDaDev's cars brake 98 kN each
        // whatever their weight, and the cast iron doubles it at a stop: the
        // light cars of an ЭД9М slid and wore flats, the heavy motor cars not
        // ("будто якорь скинул", testers).
        limit_brake_force_ = remembered("limit_brake_force").value_or("1") == "1";
        // The brakes' condition: in service (6) with the realistic air,
        // none with RTS's, until chosen (2026-11-18).
        const bool realistic = remembered("realistic_air").value_or("0") == "1";
        brake_condition_ = std::clamp(std::atoi(remembered("brake_condition").value_or(realistic ? "6" : "0").c_str()), 0, 10);
        brake_pressing_ = std::clamp(std::atoi(remembered("brake_pressing").value_or("100").c_str()), 50, 150);
    }
    const auto rails = rails_now();
    for (auto& unit : units_) {
        if (unit.train && unit.train->settings().body_motion != body_motion_model()) unit.train->set_body_motion(body_motion_model());
        if (unit.train) unit.train->set_switch_throws(switch_throws());
        if (unit.train) {
            unit.train->set_rails(rails);
            unit.train->set_limit_brake_force(limit_brake_force_);
            unit.train->set_brake_force_factor(static_cast<float>(brake_pressing_) / 100.0f);
            // Each train its own cars' wear (a seed of its own, kept when
            // the condition changes).
            if (unit.train->brake_condition() != brake_condition_ || (brake_condition_ != 0 && unit.train->brake_seed() == 0)) {
                const auto seed = unit.train->brake_seed();
                unit.train->set_brake_condition(brake_condition_, seed != 0 ? seed : std::random_device{}() | 1u);
            }
        }
    }
    if (!pending_load_.empty()) {
        if (sound) stop_sound(*sound);
        const auto name = std::exchange(pending_load_, {});
        const auto error = load(name);
        message_ = error.empty() ? "loaded " + consist_ : error;
        if (error.empty()) pending_path_ = true;
    }
    if (!pending_situation_.empty()) {
        if (sound) stop_sound(*sound);
        const auto file = std::exchange(pending_situation_, {});
        const auto error = load_situation(file);
        const auto stem = file.stem().u8string();
        message_ = error.empty() ? trf("situation %s put back", std::string(stem.begin(), stem.end()).c_str()) : error;
        if (!error.empty()) std::cout << "situation: " << error << std::endl;
    }
    if (pending_activity_) {
        if (sound) stop_sound(*sound);
        const auto activity = std::exchange(pending_activity_, nullptr);
        activity_ = activity;
        const auto error = start_activity(*activity, pending_activity_folder_);
        message_ = error.empty() ? trf("scenario %s", activity->name.c_str()) : error;
        if (!error.empty()) std::cout << "activity: " << error << std::endl;
        activity_started_ = true;
        update_traffic(); // the traffic due at the start stands before the loading ends
    }
    if (!pending_trains_.empty()) {
        if (sound) stop_sound(*sound);
        auto trains = std::exchange(pending_trains_, {});
        const auto source = std::exchange(pending_trains_source_, {});
        const auto error = put_back(std::move(trains), source);
        message_ = error.empty() ? trf("%s: trains put on the track", source.c_str()) : error;
        if (!error.empty()) std::cout << source << ": " << error << std::endl;
    }
    if (!loaded()) {
        update_engineer(input, camera, elapsed, library); // on foot with no trains at all
        return;
    }
    update_traffic();
    request_models(library);
    fit_shapes(library);
    if (placement_check() && !placement_checked_ && placed() && units_.front().cars.size() >= 3
        && std::all_of(units_.front().cars.begin(), units_.front().cars.end(), [](const Car& car) { return car.fitted || car.shape.empty(); })) {
        placement_checked_ = true;
        const auto& unit = units_.front();
        auto& keys = selection_[unit.train->id()];
        for (const auto& car : unit.cars) keys.insert(car.key);
        keys.erase(unit.cars[1].key);
        std::cout << "placement check: " << save_placement("zz_placement_check") << std::endl;
        const auto file = placement_folder() / "zz_placement_check.placement";
        std::cout << "placement check: " << preview_placement(file) << ", blocked " << (preview_blocked_ ? 1 : 0) << std::endl;
        // The preview's cars, in order, against the picked ones (car 0, then 2..).
        std::vector<std::size_t> picked;
        for (std::size_t i = 0; i < unit.cars.size(); ++i)
            if (i != 1) picked.push_back(i);
        float worst = 0.0f;
        std::size_t k = 0, shown = 0;
        for (const auto& p : preview_units_) {
            for (std::size_t i = 0; i < p.cars.size() && i < p.current.size() && k < picked.size(); ++i, ++k) {
                const auto a = pose(p, i);
                const auto b = pose(unit, picked[k]);
                worst = (std::max)({worst, glm::distance(a.front, b.front), glm::distance(a.rear, b.rear)});
                ++shown;
            }
        }
        std::cout << "placement check: " << preview_units_.size() << " trains, " << shown << " cars shown, the worst " << worst
                  << " m from where they came from" << std::endl;
        preview_units_.clear();
        preview_stock_.clear();
        selection_.clear();
        std::error_code fs_error;
        std::filesystem::remove(file, fs_error);
    }
    // A saved game: the trains' state once every car is fitted (the fitting
    // sets the car spacing), then the DLLs' once the host has the cars
    // (RTS: Init, then the saved state written over it).
    if (!pending_game_.empty() && pending_trains_.empty()) {
        // The trains whose shapes load (the far ones' never come: they wait
        // for no one); half a minute at most.
        const bool fitted = std::all_of(units_.begin(), units_.end(), [&](const Unit& u) {
            if (!train_near(u, train_load_range)) return true;
            return std::all_of(u.cars.begin(), u.cars.end(), [](const Car& car) {
                return car.fitted || car.shape.empty() || (car.requested && !car.loading.valid());
            });
        });
        pending_game_wait_ += elapsed;
        if (fitted || pending_game_wait_ > 30.0f) {
            apply_saved_game();
            pending_game_wait_ = 0.0f;
        }
    }
    if (pending_game_.empty() && !pending_dlls_.empty()) {
        if (legacy_ && legacy_->alive()) {
            if (legacy_->restore_state(pending_dlls_)) {
                std::cout << "saved game: " << pending_dlls_.size() << " DLL states put back" << std::endl;
                pending_dlls_.clear();
            } else if (++pending_dll_tries_ > 100) {
                std::cout << "saved game: the DLLs' states could not be put back" << std::endl;
                pending_dlls_.clear();
            }
        } else if (++pending_dll_tries_ > 600) {
            std::cout << "saved game: no DLLs came up for the saved states" << std::endl;
            pending_dlls_.clear();
        }
    }
    if (pending_path_) {
        // Bogies of every car first: they set the car spacing on the track.
        const bool shapes_ready = std::all_of(units_.front().cars.begin(), units_.front().cars.end(), [](const Car& car) {
            return car.fitted || car.shape.empty() || (car.requested && !car.loading.valid());
        });
        if (shapes_ready) {
            pending_path_ = false;
            if (paths_.empty()) {
                message_ = tr("the route has no paths: put the train with Track control (Ctrl+Alt+F7)");
            } else {
                const auto index = static_cast<std::size_t>(std::clamp(path_choice_, 0, static_cast<int>(paths_.size()) - 1));
                if (const auto error = place_on_path(paths_[index]); !error.empty()) message_ = error;
            }
        }
    }
    if (!placed()) return;
    if (start_view_ != StartView::none && !pending_path_ && units_.front().role == Role::driven) apply_start_view(camera);
    if (!save_when_placed_.empty() && !pending_path_) {
        const auto name = std::exchange(save_when_placed_, {});
        const auto error = save_situation(name);
        std::cout << "situation " << name << (error.empty() ? ": saved, " + std::to_string(units_.size()) + " trains" : ": " + error) << std::endl;
    }
    update_car_menu(camera.eye());
    if (input.toggle_follow) follow_ = !follow_;
    if (pending_uncouple_) {
        const auto index = *std::exchange(pending_uncouple_, std::nullopt);
        if (std::abs(driven().speed()) > 0.1) {
            message_ = tr("stop the train to uncouple");
        } else {
            split(0, index, true, true);
            message_ = trf("uncoupled before car %zu (end cocks closed)", static_cast<std::size_t>(index + 1));
        }
    }
    if (pending_couple_ && !units_.empty()) {
        const bool front = *std::exchange(pending_couple_, std::nullopt);
        const auto& cars = units_.front().cars;
        const bool done = !cars.empty() && couple_standing(0, front ? 0 : cars.size() - 1, front);
        std::cout << "test: couple the driven train at its " << (front ? "front" : "rear") << ": "
                  << (done ? std::to_string(units_.front().cars.size()) + " cars" : message_) << std::endl;
        for (const auto& unit : units_) {
            const auto f = unit.train->front_end();
            const auto r = unit.train->rear_end();
            std::cout << "test:   " << unit.name << (unit.remote ? " (remote)" : "") << ": " << unit.cars.size() << " cars, front "
                      << (f ? std::to_string(f->node()) + " " + std::to_string(f->offset()) + (f->forward() ? " along" : " against") : "-")
                      << ", rear "
                      << (r ? std::to_string(r->node()) + " " + std::to_string(r->offset()) + (r->forward() ? " along" : " against") : "-")
                      << std::endl;
        }
    }
    // On foot the DLL's cab keys are on at once (the arrows walk him).
    if (legacy_) legacy_->set_on_foot(engineer_.active());
    const bool keys_live = cab_keys_live();
    if (keys_live) {
        apply_handles(input);
    } else {
        apply_handles(SceneInput{}); // a held handle position still springs back
    }
    // Where the engineer's eye is in his cab: a key works a CHECKDISTANCEONPRESS
    // switch within its Radius only (key_reaches: the desk he is at).
    const auto key_eye = eye_in_cab(camera.eye());
    // A key held works its switch again once per key delay (RTS's
    // config.ini KeyDelay, ms; 600 without it).
    key_clock_ += elapsed;
    std::vector<KeyEvent> keys;
    if (keys_live) {
        const float delay = rts_config_value(rts_config_, "keydelay").value_or(600.0f) * 0.001f;
        keys = cab_key_repeat_.step(input.keys, key_clock_, delay);
    } else {
        cab_key_repeat_.clear();
    }
    helper_clock_ += elapsed;
    if (helper_cab_) {
        // The keys of the cab he is at (the other section's, walked into).
        helper_cab_->set_remote_player(engineer_car_ != SIZE_MAX && helper_outputs_.contains(engineer_car_) ? engineer_car_ : cab_car_);
        helper_cab_->set_on_foot(engineer_.active());
        if (keys_live) {
            for (const auto& key : keys) helper_cab_->key(key, key_eye);
        } else if (keys_were_live_) {
            helper_cab_->release_keys();
        }
    } else if (legacy_) {
        if (keys_live) {
            for (const auto& key : keys) legacy_->key(key, key_eye);
        } else if (keys_were_live_) {
            legacy_->release_keys();
        }
    }
    if (native_ && keys_live) {
        // The native cab's AssignKey keys (presses, and again while held).
        for (const auto& key : keys) {
            if (!key.down) continue;
            const auto* cab = engineer_cab();
            for (const auto& b : native_bindings_) {
                if (b.key != key.key || b.shift != key.shift || b.ctrl != key.ctrl) continue;
                const auto* spec = cab ? cab->description().find_switch(b.id) : nullptr;
                if (spec && !key_reaches(*spec, key_eye)) continue;
                native_operate(b.id, b.direction);
            }
        }
    }
    keys_were_live_ = keys_live;
    if (keys_live && !keyboard_captured()) apply_default_systems(input);
    // A coupling being fixed (0x424488 counts the 120 s down).
    if (repair_) {
        repair_->left -= elapsed;
        if (repair_->left <= 0.0f) {
            if (auto* unit = find_unit(repair_->train); unit && repair_->car < unit->train->cars().size()) {
                unit->train->set_car_flags(repair_->car, simulation::Train::CarFlags::coupler_open,
                                           simulation::coupler_broken_front | simulation::coupler_broken_rear, false);
                message_ = trf("the coupling of car %zu is fixed", repair_->car + 1);
            }
            repair_.reset();
        }
    }
    if (cargo_time_ > 0.0f) cargo_time_ = (std::max)(cargo_time_ - elapsed, 0.0f);

    accumulator_ += (std::min)(elapsed, 0.25f);
    while (accumulator_ >= simulation::train_step) {
        accumulator_ -= simulation::train_step;
        // The cab as it stands before the step, to draw between (CabView::Blend).
        cab_before_car_ = SIZE_MAX;
        if (const auto* before = cab_car_ != SIZE_MAX ? cab_state(cab_car_) : nullptr) {
            cab_switch_frames_before_.resize(before->switches.size());
            for (std::size_t s = 0; s < before->switches.size(); ++s) cab_switch_frames_before_[s] = before->switches[s].current_frame;
            cab_display_values_before_.resize(before->displays.size());
            for (std::size_t d = 0; d < before->displays.size(); ++d) cab_display_values_before_[d] = before->displays[d].value;
            cab_before_car_ = cab_car_;
        }
        for (auto& unit : units_) {
            unit.previous = std::move(unit.current);
            unit.previous_travelled = std::move(unit.current_travelled);
            unit.previous_rolled = std::move(unit.current_rolled);
            unit.previous_body = std::move(unit.current_body);
        }
        feed_line();
        if (legacy_ && !units_.empty() && units_.front().role == Role::driven) {
            // The host's trains as the units are now (a change missed: again).
            if (!host_current()) reconsist_host();
            {
                OPENRTS_ZONE("trains: DLL world feed");
                feed_world(units_.front());
            }
            std::vector<const simulation::Train*> trains;
            std::vector<Unit*> host_units;
            for (auto& each : units_) {
                if (each.host_train == SIZE_MAX) continue;
                if (trains.size() <= each.host_train) {
                    trains.resize(each.host_train + 1, nullptr);
                    host_units.resize(each.host_train + 1, nullptr);
                }
                trains[each.host_train] = each.train.get();
                host_units[each.host_train] = &each;
            }
            std::vector<LegacyCab::Triggers> posted;
            {
                OPENRTS_ZONE("trains: DLL host step");
                legacy_->step(trains, posted);
            }
            take_dll_tape();
            for (std::size_t t = 0; t < posted.size() && t < host_units.size(); ++t) {
                if (!host_units[t]) continue;
                auto& unit = *host_units[t];
                const auto& p = posted[t];
                for (std::size_t i = 0; i < p.aux.size() && i < unit.cars.size(); ++i) {
                    auto& pending = unit.cars[i].pending_aux_triggers;
                    if (pending.size() < p.aux[i].size()) pending.resize(p.aux[i].size());
                    for (std::size_t k = 0; k < p.aux[i].size(); ++k)
                        pending[k].insert(pending[k].end(), p.aux[i][k].begin(), p.aux[i][k].end());
                }
                for (std::size_t i = 0; i < p.exterior.size() && i < unit.cars.size(); ++i) {
                    auto& pending = unit.cars[i].pending_triggers;
                    pending.insert(pending.end(), p.exterior[i].begin(), p.exterior[i].end());
                }
                for (std::size_t i = 0; i < p.cab.size() && i < unit.cars.size(); ++i) {
                    auto& pending = unit.cars[i].pending_cab_triggers;
                    pending.insert(pending.end(), p.cab[i].begin(), p.cab[i].end());
                    // A helper watching this cab hears them too (helper_output).
                    if (t != 0 || p.cab[i].empty()) continue;
                    if (const auto watched = helper_watched_until_.find(i);
                        watched != helper_watched_until_.end() && watched->second >= helper_clock_) {
                        auto& sent = helper_triggers_[i];
                        for (const auto code : p.cab[i]) sent.push_back(static_cast<std::uint16_t>(code));
                        if (sent.size() > 256) sent.erase(sent.begin(), sent.end() - 256);
                    }
                }
            }
        }
        step_trains();
        // The native plugin's sounds and messages.
        if (native_ && native_car_ < units_.front().cars.size()) {
            auto& car = units_.front().cars[native_car_];
            for (const auto t : native_->take_sounds(false)) car.pending_triggers.push_back(t);
            for (const auto t : native_->take_sounds(true)) car.pending_cab_triggers.push_back(t);
            for (auto& m : native_->take_messages()) message_ = std::move(m);
        }
        collide_slides(library);
        for (auto& unit : units_) {
            unit.current = unit.train->placements();
            unit.current_travelled.resize(unit.train->cars().size());
            for (std::size_t i = 0; i < unit.current_travelled.size(); ++i) {
                unit.current_travelled[i] = unit.train->cars()[i].travelled;
            }
            if (unit.previous.size() != unit.current.size()) unit.previous = unit.current;
            if (unit.previous_travelled.size() != unit.current_travelled.size()) unit.previous_travelled = unit.current_travelled;
            unit.take_rolled();
            if (unit.previous_rolled.size() != unit.current_rolled.size()) unit.previous_rolled = unit.current_rolled;
            unit.take_body();
            if (unit.previous_body.size() != unit.current_body.size()) unit.previous_body = unit.current_body;
        }
        update_tape();
    }
    update_timetable(input, elapsed);

    const Unit* followed = follow_id_ != 0 ? find_unit(follow_id_) : nullptr;
    if (!followed) followed = &units_.front();
    if (follow_ && car_view_ == CarView::off && !followed->current.empty()) {
        // From the interpolated head car, as it is drawn.
        const auto head = pose(*followed, 0);
        glm::vec3 forward = head.front - head.rear;
        forward.y = 0.0f;
        if (glm::length(forward) > 0.01f) forward = glm::normalize(forward);
        camera.fly = true;
        camera.position = head.front - forward * 22.0f + glm::vec3{0.0f, 6.0f, 0.0f};
        camera.yaw = std::atan2(forward.x, forward.z) + pi;
        camera.pitch = 0.12f;
    }
    if (car_view_ != CarView::off) {
        // The car view (F3): the engineer stays where he is, with his own
        // camera, not walking or opening anything; the camera orbits the car.
        SceneInput still = input;
        still.walk_forward = still.walk_back = still.walk_left = still.walk_right = false;
        still.engineer_action = still.engineer_action_ctrl = false;
        still.cab_window = still.head_out = false;
        update_engineer(still, person_camera_, elapsed, library);
        update_car_view(camera);
    } else {
        update_engineer(input, camera, elapsed, library);
    }
    if (native_) {
        // The native cab as the 3-D cab reads a DLL's: switches by .sd ID,
        // displays' values and states.
        const auto& controls = native_->cab().controls();
        native_output_.switches.resize(controls.size());
        for (std::size_t k = 0; k < controls.size(); ++k) {
            auto& s = native_output_.switches[k];
            s.state = static_cast<std::uint32_t>(std::lround((std::max)(controls[k].state, 0.0f)));
            s.set_state = static_cast<std::uint32_t>(std::lround((std::max)(controls[k].target, 0.0f)));
            s.current_frame = controls[k].frame;
        }
        const auto& indicators = native_->cab().indicators();
        native_output_.displays.resize(indicators.size());
        for (std::size_t k = 0; k < indicators.size(); ++k) {
            native_output_.displays[k].value = indicators[k].value;
            native_output_.displays[k].state = indicators[k].state;
        }
        native_output_.panto_raised = static_cast<std::uint8_t>(native_->outputs().pantographs);
        native_output_.cab_aspect = signal_aspect(units_.front());
    }
    if (auto* cab = engineer_cab()) cab->step_displays(elapsed);
    update_pantographs(elapsed);
    update_exterior_doors(elapsed);
    spin_slipping_wheels(elapsed);
    // A flat on the player's train: told once it is 1 mm deep.
    if (!units_.empty() && units_.front().role == Role::driven) {
        auto& unit = units_.front();
        const auto& states = unit.train->cars();
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            const float flat = (std::max)(states[i].flat[0], states[i].flat[1]);
            auto& car = unit.cars[i];
            if (flat >= 1.0f && !car.flat_told) {
                car.flat_told = true;
                message_ = trf("car %zu: a wheel flat of %.1f mm: it knocks each turn (the car menu changes the wheel sets)", i + 1, flat);
                std::cout << "car " << i + 1 << ": a wheel flat of " << flat << " mm" << std::endl;
            } else if (flat < 1.0f) {
                car.flat_told = false;
            }
        }
    }
    update_lights(elapsed, input.light_eye.value_or(camera.eye()));
    update_listener(camera);
    post_cab_sounds();
    if (sound) {
        update_sound(elapsed, camera, *sound);
    } else {
        // Silent (--no-sound): what the DLLs post must not pile up.
        retired_sounds_.clear();
        for (auto& unit : units_) {
            for (auto& car : unit.cars) {
                car.pending_events.clear();
                car.pending_triggers.clear();
                car.pending_cab_triggers.clear();
                car.pending_aux_triggers.clear();
                car.pending_flats.clear();
            }
        }
    }
}

// The engineer (engineer.hpp): seated in the cab, walking about it, or
// outside; the camera is his eye. Arrows walk, Enter stands up, sits down,
// opens a door or climbs in.
void TrainView::set_origin(const formats::RoutePosition& origin) {
    // A point fixed in the world moves in scene coordinates by the old origin minus the new one.
    const glm::vec3 delta{
        static_cast<float>((origin_.tile_x - origin.tile_x) * simulation::tile_size + origin_.x - origin.x),
        static_cast<float>(origin_.y - origin.y),
        static_cast<float>((origin_.tile_z - origin.tile_z) * simulation::tile_size + origin_.z - origin.z),
    };
    origin_ = origin;
    engineer_.shift(delta);
}

void TrainView::gather_walk_world(const glm::vec3& around, renderer::ModelLibrary& library) {
    constexpr float reach = 25.0f;
    // Car bodies near him.
    walk_cars_.clear();
    for (const auto& unit : units_) {
        if (unit.current.empty()) continue;
        for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size(); ++i) {
            const auto& def = *unit.cars[i].definition;
            const float half_length = def.length > 1.0f ? def.length * 0.5f : 10.0f;
            const auto here = local(unit.current[i].front);
            if (glm::distance(glm::vec2{here.x, here.z}, glm::vec2{around.x, around.z}) > reach + 2.0f * half_length) continue;
            const auto body = body_pose(unit, i);
            if (!body) continue;
            walk_cars_.push_back({.inverse = glm::inverse(body->transform),
                                  .half_width = (def.width > 1.0f ? def.width : 3.1f) * 0.5f,
                                  .half_length = half_length,
                                  .height = def.height > 1.0f ? def.height : 4.5f});
        }
    }
    // The world's objects near him, their surfaces read aside (a few at a time).
    walk_objects_near_.clear();
    walk_boxes_near_.clear();
    if (!walk_objects_ || !obstacles_) return;
    std::size_t in_flight = 0;
    std::size_t box_reads = 0;
    for (auto& [path, entry] : walk_meshes_) {
        if (!entry.loading.valid()) continue;
        if (entry.loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            entry.mesh = entry.loading.get();
        } else {
            ++in_flight;
        }
    }
    for (const auto& object : *obstacles_) {
        const glm::vec3 at{object.transform[3]};
        if (const auto* model = library.find(object.source_path); model && model->bounds().radius > 0.0f) {
            const auto bounds = model->bounds();
            const glm::vec3 centre{object.transform * glm::vec4{bounds.center, 1.0f}};
            if (glm::distance(glm::vec2{centre.x, centre.z}, glm::vec2{around.x, around.z}) > reach + bounds.radius) continue;
        } else if (glm::distance(glm::vec2{at.x, at.z}, glm::vec2{around.x, around.z}) > reach + 60.0f) {
            continue; // until the model says how big it is
        }
        const std::string key(object.source_path.str());
        // RTS walks the ground among an object's boxes (ESD_Complex, else
        // ESD_Bounding_Box; 0x4322F0): an object that has them is walked so,
        // its surfaces not read (the Osvstozar4 mast's 30 boxes, its ladder).
        auto boxes = walk_boxes_.find(key);
        if (boxes == walk_boxes_.end()) {
            if (box_reads >= 16) continue;
            ++box_reads;
            std::shared_ptr<const std::vector<formats::WalkBox>> read;
            if (auto sd = formats::load_shape_description(*vfs_, formats::shape_description(*vfs_, object.source_path));
                sd.ok() && !sd.value->boxes.empty()) {
                read = std::make_shared<const std::vector<formats::WalkBox>>(std::move(sd.value->boxes));
            }
            boxes = walk_boxes_.emplace(key, std::move(read)).first;
        }
        if (boxes->second) {
            walk_boxes_near_.push_back(
                {.boxes = boxes->second.get(), .transform = object.transform, .inverse = glm::inverse(object.transform)});
            continue;
        }
        auto it = walk_meshes_.find(key);
        if (it == walk_meshes_.end()) {
            if (in_flight >= 6) continue;
            ++in_flight;
            WalkMesh entry;
            entry.loading = std::async(std::launch::async, [vfs = vfs_, path = object.source_path] {
                return CabCollision::load(*vfs, path);
            });
            walk_meshes_.emplace(key, std::move(entry));
            continue;
        }
        if (!it->second.mesh || it->second.mesh->empty()) continue;
        walk_objects_near_.push_back(
            {.mesh = it->second.mesh.get(), .transform = object.transform, .inverse = glm::inverse(object.transform)});
    }
}

std::optional<float> TrainView::walk_floor(const glm::vec3& at) const {
    constexpr float step = 0.4f;
    std::optional<float> best = ground_height(at);
    static const std::vector<std::uint8_t> none;
    for (const auto& object : walk_objects_near_) {
        const glm::vec3 p{object.inverse * glm::vec4{at, 1.0f}};
        const glm::vec3 top{object.inverse * glm::vec4{at + glm::vec3{0.0f, step, 0.0f}, 1.0f}};
        const auto floor = object.mesh->floor(p.x, p.z, -1e6f, top.y, none);
        if (!floor) continue;
        const float y = glm::vec3{object.transform * glm::vec4{p.x, *floor, p.z, 1.0f}}.y;
        if (y > at.y + step + 0.01f) continue;
        if (!best || y > *best) best = y;
    }
    return best;
}

bool TrainView::walk_blocked(const glm::vec3& from, const glm::vec3& to) const {
    constexpr float radius = 0.25f;
    for (const auto& car : walk_cars_) {
        auto inside = [&](const glm::vec3& p) {
            const glm::vec3 l{car.inverse * glm::vec4{p, 1.0f}};
            return std::abs(l.x) < car.half_width + radius && std::abs(l.z) < car.half_length + radius && l.y > -1.5f
                   && l.y < car.height;
        };
        if (inside(to) && !inside(from)) return true;
    }
    if (walk_objects_near_.empty()) return false;
    glm::vec3 ahead = to - from;
    ahead.y = 0.0f;
    const float length = glm::length(ahead);
    if (length < 1e-5f) return false;
    ahead /= length;
    static const std::vector<std::uint8_t> none;
    for (const auto& object : walk_objects_near_) {
        for (const float h : {0.45f, 0.95f, 1.45f}) {
            const glm::vec3 lift{0.0f, h, 0.0f};
            const glm::vec3 a{object.inverse * glm::vec4{from + lift, 1.0f}};
            const glm::vec3 b{object.inverse * glm::vec4{to + ahead * radius + lift, 1.0f}};
            if (object.mesh->crosses(a, b, none)) return true;
        }
    }
    return false;
}

void TrainView::update_engineer(const SceneInput& input, ViewerCamera& camera, const float elapsed,
                                renderer::ModelLibrary& library) {
    // A test turned his head (stand_at_passage): the camera that way.
    if (const auto d = std::exchange(look_once_, std::nullopt); d && glm::length(*d) > 1e-4f) {
        const glm::vec3 n = glm::normalize(*d);
        camera.yaw = std::atan2(-n.x, -n.z);
        camera.pitch = std::asin(std::clamp(-n.y, -1.0f, 1.0f));
    }
    look_ = camera.forward();
    if (!engineer_.active()) return;
    if (door_pick_test() && door_pick_stage_ < 4 && cab_ && cab_->loaded()) {
        door_pick_time_ += elapsed;
        if (door_pick_stage_ == 0 && door_pick_time_ > 2.0f) {
            std::cout << "door pick, closed:\n" << cab_->door_pick_report(cab_->eye() + glm::vec3{0.0f, 0.75f, 0.0f}) << std::flush;
            const auto& doors = cab_->description().doors;
            for (std::size_t j = 0; j < doors.size(); ++j)
                if (doors[j].can_be_opened && doors[j].parent < 0 && cab_->door_animated(j)) cab_->set_door(j, true);
            door_pick_stage_ = 1;
            door_pick_time_ = 0.0f;
        } else if (door_pick_stage_ == 1 && door_pick_time_ > 5.0f) {
            std::cout << "door pick, open:\n" << cab_->door_pick_report(cab_->eye() + glm::vec3{0.0f, 0.75f, 0.0f}) << std::flush;
            for (std::size_t j = 0; j < cab_->description().doors.size(); ++j) cab_->set_door(j, false);
            door_pick_stage_ = 2;
            door_pick_time_ = 0.0f;
        } else if ((door_pick_stage_ == 2 && door_pick_time_ > 4.0f) || (door_pick_stage_ == 3 && door_pick_time_ > 4.0f)) {
            // The car's outer doors from 1.5 m off its side, at each one's
            // middle as drawn: clicked once (stage 2), then looked at open.
            const auto& car = units_.front().cars[cab_car_];
            const auto body = body_pose(units_.front(), cab_car_, false);
            const auto* data = car_mesh(car.shape);
            if (body && data) {
                std::vector<glm::vec3> lo, hi;
                data->mesh.node_bounds(data->rig.matrices.size(), lo, hi);
                std::cout << "door pick, outer doors" << (door_pick_stage_ == 2 ? ", clicked" : ", after") << ":\n";
                for (const auto& door : car.exterior_doors) {
                    if (door.node < 0 || static_cast<std::size_t>(door.node) >= lo.size()) continue;
                    const auto n = static_cast<std::size_t>(door.node);
                    if (lo[n].x > hi[n].x) {
                        std::cout << "outer door " << door.name << ": no surfaces of its own\n";
                        continue;
                    }
                    glm::vec3 centre = (lo[n] + hi[n]) * 0.5f;
                    if (door.open > 0.0f && door.last_frame > 0.0f)
                        centre = glm::vec3{CabCollision::pose_of(data->rig, door.node, door.open * door.last_frame) * glm::vec4{centre, 1.0f}};
                    const glm::vec3 eye_car = centre + glm::vec3{centre.x >= 0.0f ? 1.5f : -1.5f, 0.0f, 0.0f};
                    const glm::vec3 eye{body->transform * glm::vec4{eye_car, 1.0f}};
                    const glm::vec3 at{body->transform * glm::vec4{centre, 1.0f}};
                    door_hover_.reset();
                    outer_door_pointer(eye, glm::normalize(at - eye), door_pick_stage_ == 2);
                    std::cout << "outer door " << door.name << " (open " << door.open << "): " << (door_hover_ ? door_hover_->text : "nothing")
                              << "\n";
                }
                std::cout << std::flush;
                door_hover_.reset();
                ++door_pick_stage_;
                door_pick_time_ = 0.0f;
            } else if (door_pick_time_ > 30.0f) {
                std::cout << "door pick, outer doors: the car's shape not read" << std::endl;
                door_pick_stage_ = 4;
            }
        }
    }
    if (!engineer_configured_) {
        engineer_configured_ = true;
        if (!rts_config_.empty()) {
            engineer_.configure(rts_config_);
        } else {
            for (const auto* name : {"RTS/config.ini", "config.ini"}) {
                const auto path = core::VirtualPath::parse(name);
                const auto bytes = path ? vfs_->read(*path) : std::nullopt;
                if (!bytes) continue;
                engineer_.configure(*bytes);
                break;
            }
        }
    }
    EngineerControls controls{.forward = input.walk_forward,
                                    .back = input.walk_back,
                                    .left = input.walk_left,
                                    .right = input.walk_right,
                                    .run = input.walk_run,
                                    .slow = input.walk_slow,
                                    .action = input.engineer_action,
                                    .action_ctrl = input.engineer_action_ctrl,
                                    .window = input.cab_window,
                                    .head_out = input.head_out};
    const Outdoors outdoors{
        .floor = [this](const glm::vec3& p) { return walk_floor(p); },
        .blocked = [this](const glm::vec3& from, const glm::vec3& to) { return walk_blocked(from, to); },
        .boxes = [this](const glm::vec3& from, const glm::vec3& to, const float height) {
            return step_among_boxes(walk_boxes_near_, from, to, height);
        },
    };
    board_prompt_.clear();
    // On a car's outside (RTS mode 6): its walkways, ladders, doors.
    if (engineer_.mode() == Engineer::Mode::on_car) {
        follow_ = false;
        const auto at = outside_car_ ? find_car(*outside_car_) : std::nullopt;
        auto outside = at ? car_outside(at->first, at->second) : std::nullopt;
        if (outside) {
            gather_walk_world(glm::vec3{outside->transform[3]}, library);
            engineer_.update_on_car(*outside, controls, outdoors, elapsed, camera);
            carry_out(engineer_.take_event());
            if (engineer_.mode() != Engineer::Mode::on_car) outside_car_.reset();
            look_ = camera.forward();
            return;
        }
        // The car went away: on the ground under him.
        outside_car_.reset();
        engineer_.stand_outside(camera.eye() - glm::vec3{0.0f, engineer_.height(), 0.0f});
    }
    // On the ground by a car's ladder or steps (RTS 0x414D24): Ctrl+Enter
    // takes them (0x431F88), up onto its outside.
    bool at_ladder = false;
    if (engineer_.mode() == Engineer::Mode::outside && !engineer_.passing()) {
        const glm::vec3 eye = engineer_.feet() + glm::vec3{0.0f, engineer_.height(), 0.0f};
        if (const auto ladder = ladder_target(eye, camera.forward())) {
            if (auto outside = car_outside(ladder->unit, ladder->car, false)) {
                at_ladder = true;
                const glm::vec3 feet_car{glm::inverse(outside->transform) * glm::vec4{engineer_.feet(), 1.0f}};
                board_prompt_ = Engineer::ladder_prompt(*outside, ladder->ladder, feet_car);
                if (input.engineer_action && input.engineer_action_ctrl) {
                    engineer_.climb(*outside, ladder->ladder);
                    outside_car_ = units_[ladder->unit].cars[ladder->car].key;
                    board_prompt_.clear();
                    look_ = camera.forward();
                    return;
                }
            }
        }
    }
    // At the door of another cab: of the driven train (the far end of a
    // multiple unit) or of another train of his (the network's own). The
    // same prompt as at his own cab; Enter makes that cab his (and the train
    // the driven one), and the same Enter then opens the door or climbs in
    // as into any cab.
    if (engineer_.mode() == Engineer::Mode::outside && !at_ladder) {
        if (const auto target = board_target(engineer_.feet())) {
            if (const auto* unit = find_unit(target->train); unit && target->car < unit->cars.size()) {
                if (const auto it = board_cabs_.find(unit->cars[target->car].key); it != board_cabs_.end() && it->second) {
                    board_prompt_ = Engineer::get_on_prompt(*it->second, target->door);
                }
            }
            if (input.engineer_action) {
                if (is_player(target->train)) {
                    walk_up_to_cab(target->car, engineer_.feet());
                } else if (const auto problem = board(target->train, target->car, false); !problem.empty()) {
                    message_ = tr(problem.c_str());
                    return;
                }
                board_prompt_.clear();
            }
        }
    }
    // By a switch on foot (RTS 0x414D24, 0x4121BC): Enter throws it to its
    // next branch. Not where Enter already has a door or a ladder.
    if (engineer_.mode() == Engineer::Mode::outside && !engineer_.passing() && !at_ladder && board_prompt_.empty()
        && engineer_.prompt().empty()) {
        const glm::vec3 eye = engineer_.feet() + glm::vec3{0.0f, engineer_.height(), 0.0f};
        if (const auto node = junction_at_hand(eye)) {
            board_prompt_ = tr("Enter: throw the switch");
            if (input.engineer_action && !input.engineer_action_ctrl) {
                throw_by_hand(*node);
                controls.action = false;
            }
        }
    }
    auto* cab = engineer_cab();
    if (!cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) {
        // No cab of his: walking goes on outside (the player's, not the
        // train's); inside a cab that went away he is out of it.
        if (engineer_.mode() != Engineer::Mode::outside) {
            set_engineer(false);
            return;
        }
        follow_ = false;
        gather_walk_world(engineer_.feet(), library);
        engineer_.update_on_foot(controls, outdoors, elapsed, camera);
        look_ = camera.forward();
        return;
    }
    const auto& unit = units_.front();
    if (unit.current.empty()) return;
    const auto body = body_pose(unit, engineer_car_, false);
    if (!body) return;
    follow_ = false;
    // On his feet only (seated, the passing world needs no surfaces read).
    if (engineer_.mode() == Engineer::Mode::seated) {
        walk_cars_.clear();
        walk_objects_near_.clear();
    } else {
        gather_walk_world(engineer_.mode() == Engineer::Mode::outside ? engineer_.feet() : camera.eye(), library);
    }
    {
        // The cab's doors with the car's way of the same name (RTS 0x4310B0 /
        // 0x431270): in up the steps and out down them.
        std::vector<bool> ways(cab->description().doors.size(), false);
        if (const auto& outside = unit.cars[engineer_car_].outside) {
            for (std::size_t d = 0; d < ways.size(); ++d) {
                const auto name = lower(cab->description().doors[d].name);
                for (const auto& door : outside->door_details)
                    if (!door.trajectory.empty() && lower(door.name) == name) ways[d] = true;
            }
        }
        engineer_.set_door_ways(std::move(ways));
    }
    engineer_.update(*cab, controls, body->transform * cab->placement(), outdoors, elapsed, camera);
    carry_out(engineer_.take_event());
    if (engineer_.mode() == Engineer::Mode::on_car) {
        look_ = camera.forward();
        return;
    }
    // RTS moves the cab camera by the body's offsets on its springs and draws
    // the cab without them (0x424488): the cab sways around the driver.
    // openRTS's head on its neck nods and leans on top (head_motion()).
    if (engineer_.inside()) {
        camera.position += sway_offset(unit, engineer_car_, camera.position);
        // A screen of the cab holding the eye keeps the head still.
        if (!engineer_.focused()) move_head(unit, engineer_car_, elapsed, camera);
        else head_.started = false;
    } else {
        head_.started = false;
    }
    if (const auto door = engineer_.take_pass()) pass_to_next_car(*door);
    look_ = camera.forward();
}

// The switch the engineer on foot stands by (RTS 0x414D24): the nearest
// junction whose world object is within 5 m of his eye. In a network game or
// a scenario only hand switches (tsection's ManualJunctionShape, TrackShape
// flag 0x10), in a scenario only those no train's path holds; in a free run
// any.
std::optional<std::uint32_t> TrainView::junction_at_hand(const glm::vec3& eye) {
    if (!network_) return std::nullopt;
    if (!junction_spots_built_) {
        junction_spots_built_ = true;
        for (std::uint32_t n = 1; n < network_->node_count(); ++n) {
            const auto* node = network_->node(n);
            if (!node || node->kind != formats::TrackNodeKind::junction || node->branch_count() < 2) continue;
            if (const auto at = network_->node_position(n)) junction_spots_.emplace_back(n, *at);
        }
    }
    constexpr float reach = 5.0f;
    const bool hand_only = network_game_ || activity_ != nullptr;
    std::optional<std::uint32_t> best;
    float nearest = reach;
    for (const auto& [n, at] : junction_spots_) {
        const float distance = glm::length(local(at) - eye);
        if (distance > nearest) continue;
        const auto* node = network_->node(n);
        const auto* state = network_->junction(n);
        if (!node || !state) continue;
        if (hand_only && !(node->shape && node->shape->manual_junction)) continue;
        if (activity_ && !state->reservations.empty()) continue;
        nearest = distance;
        best = n;
    }
    return best;
}

// Enter by a switch (RTS 0x4121BC, object type 7): its next branch, round
// to the first. A network game asks the server (0x460A), whose answer is
// what stands.
void TrainView::throw_by_hand(const std::uint32_t node) {
    const auto* info = network_ ? network_->node(node) : nullptr;
    const auto* state = network_ ? network_->junction(node) : nullptr;
    if (!info || !state || info->branch_count() < 2) return;
    const auto next = static_cast<std::uint32_t>((state->selected + 1) % info->branch_count());
    if (network_game_) {
        junction_throws_.emplace_back(node, next);
        return;
    }
    if (!network_->throw_junction(node, next)) message_ = tr("The switch is occupied or held by a train's route.");
}

std::optional<TrainView::BoardTarget> TrainView::board_target(const glm::vec3& feet) {
    std::optional<BoardTarget> best;
    float nearest = 2.5f; // as the engineer's own get-on spots
    for (std::size_t u = 0; u < units_.size(); ++u) {
        const auto& unit = units_[u];
        if ((unit.remote && !(u == 0 && helping_)) || unit.current.empty()) continue;
        // The driven train's cabs but the one he is at (its doors are the
        // engineer's own spots); his other trains' cabs.
        const bool driven = aboard(unit, u);
        if (!driven && (u == 0 || !boardable_.contains(unit.train->id()))) continue;
        for (std::size_t i = 0; i < unit.cars.size(); ++i) {
            if (driven && i == engineer_car_) continue;
            const auto& car = unit.cars[i];
            const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
            if (!engine || engine->cabin.empty()) continue;
            const auto body = body_pose(unit, i, false);
            if (!body) continue;
            const glm::vec3 centre{body->transform[3]};
            if (std::hypot(centre.x - feet.x, centre.z - feet.z) > car.definition->length * 0.5f + 10.0f) continue;
            auto& cab = board_cabs_[car.key];
            if (!cab) {
                cab = CabView::create(*vfs_, *car.definition,
                                      formats::wagon_path(car.folder, car.definition->name, true));
                if (!cab) continue;
            }
            // A driving cab: a CabView, or a driver's chair in its Cabin (the
            // 2ЭС4К's sections have no CabView, testers 2026-11-12); a motor
            // car's Cabin that is its salon has neither.
            if (!engine->cab_view && cab->description().chairs.empty()) continue;
            // Its doors as they are (the shape's nodes read: a door or a doorway).
            cab->poll();
            const auto to_scene = body->transform * cab->placement();
            for (const auto& [door, spot] : Engineer::get_on_points(*cab)) {
                const glm::vec3 at{to_scene * glm::vec4(spot, 1.0f)};
                const float d = std::hypot(at.x - feet.x, at.z - feet.z);
                if (d <= nearest) {
                    nearest = d;
                    best = BoardTarget{unit.train->id(), i, door};
                }
            }
        }
    }
    return best;
}

std::string TrainView::board(const std::uint64_t id, const std::size_t car, const bool seat) {
    if (is_player(id)) return {};
    const bool outside = engineer_.mode() == Engineer::Mode::outside;
    const auto feet = engineer_.feet();
    if (auto problem = set_role(id, Role::driven); !problem.empty()) return problem;
    // Into the engine he walked up to, when its DLL can take him there.
    if (car != SIZE_MAX && car != cab_car_ && !units_.empty() && car < units_.front().cars.size()) change_cab(car);
    if (seat || !outside) {
        if (cab_) set_engineer(true);
    } else {
        // Still on the ground at its door (attaching the cab let him go).
        visit_cab_.reset();
        engineer_car_ = cab_car_;
        engineer_.stand_outside(feet);
    }
    follow_train(id);
    std::cout << "the player takes train " << units_.front().name << std::endl;
    return {};
}

void TrainView::set_walking(const bool on, const glm::vec3& eye) {
    visit_cab_.reset();
    visit_requested_ = false;
    engineer_car_ = cab_car_;
    hovered_.reset();
    if (!on) {
        engineer_.off();
        return;
    }
    glm::vec3 feet = eye;
    if (const auto ground = ground_height(feet)) {
        feet.y = *ground;
    } else {
        feet.y -= 1.7f;
    }
    engineer_.stand_outside(feet);
}

void TrainView::set_engineer(const bool on) {
    visit_cab_.reset();
    visit_requested_ = false;
    engineer_car_ = cab_car_;
    hovered_.reset();
    if (on) {
        // By the first chair of the leading cab: a locomotive with a cab at
        // each end in one shape (the EP1M: chairs 0 and 1 at the rear, 2
        // and 3 at the front; its DLLs make the front cab the working one)
        // starts at the front, by the train's way (the car turned round in
        // the train, the cab shape turned in its car). With all chairs at
        // one end it is chair 0, as before. On foot by it, as RTS starts
        // him; Enter sits him down.
        std::size_t chair = 0;
        if (cab_ && !units_.empty() && cab_car_ < units_.front().cars.size()) {
            const auto& chairs = cab_->description().chairs;
            const float way = (units_.front().cars[cab_car_].flipped ? -1.0f : 1.0f) * (cab_->turned() ? -1.0f : 1.0f);
            for (std::size_t k = 1; k < chairs.size(); ++k) {
                if (chairs[k].position[2] * way > chairs[chair].position[2] * way + 0.5f) chair = k;
            }
        }
        if (cab_) {
            engineer_.stand_by(*cab_, chair);
        } else {
            engineer_.sit(chair);
        }
    } else {
        engineer_.off();
    }
}

namespace {

bool passage_door(const formats::CabDoor& door) {
    auto starts = [](const std::string& s) {
        return s.size() >= 5 && std::tolower(static_cast<unsigned char>(s[0])) == 'w' && std::tolower(static_cast<unsigned char>(s[1])) == 'a'
               && std::tolower(static_cast<unsigned char>(s[2])) == 'g' && std::tolower(static_cast<unsigned char>(s[3])) == 'o'
               && std::tolower(static_cast<unsigned char>(s[4])) == 'n';
    };
    return starts(door.from) || starts(door.to);
}

} // namespace

// RTS: InteriorDoor Connects ( MACHINE1 WAGON_BACK ) is the way into the car
// coupled at that end. The cab shape may be turned in its car (Cabin ( .. 1 ),
// VL10k section b), and the car in the train.
void TrainView::pass_to_next_car(const std::size_t door) {
    const auto* from = engineer_cab();
    if (!from || units_.empty() || door >= from->description().doors.size()) return;
    const auto& unit = units_.front();
    const std::size_t i = engineer_car_;
    if (i >= unit.cars.size()) return;
    const float z = (from->turned() ? -1.0f : 1.0f) * from->description().doors[door].position[2];
    // The car's +Z end looks to the front of the train unless the car is turned.
    const bool toward_front = (z > 0.0f) != unit.cars[i].flipped;
    if ((toward_front && i == 0) || (!toward_front && i + 1 >= unit.cars.size())) {
        message_ = tr("No car is coupled at that door.");
        return;
    }
    const std::size_t j = toward_front ? i - 1 : i + 1;
    // Car j's end that faces car i.
    const bool want_plus = toward_front ? unit.cars[j].flipped : !unit.cars[j].flipped;
    std::unique_ptr<CabView> created;
    CabView* to = nullptr;
    if (j == cab_car_) {
        to = cab_.get();
    } else if (const auto& car = unit.cars[j]; car.definition && CabView::has_room(*car.definition)) {
        created = take_cab(j);
        to = created.get();
    }
    if (!to) {
        message_ = tr("The next car has no interior to walk into.");
        return;
    }
    std::optional<std::size_t> arrival;
    float farthest = -1.0f;
    const auto& doors = to->description().doors;
    for (std::size_t k = 0; k < doors.size(); ++k) {
        if (!doors[k].has_position || !passage_door(doors[k])) continue;
        const float zk = (to->turned() ? -1.0f : 1.0f) * doors[k].position[2];
        if ((zk > 0.0f) != want_plus || std::abs(zk) <= farthest) continue;
        farthest = std::abs(zk);
        arrival = k;
    }
    if (!arrival) {
        keep_cab(j, std::move(created));
        message_ = tr("The next car has no door at this end.");
        return;
    }
    if (created) {
        keep_cab(i, std::move(visit_cab_));
        visit_cab_ = std::move(created);
        visit_requested_ = false;
    }
    // openRTS: straight through. Where he is against his door (beyond its
    // point towards the next car by so much, so far aside) he is against
    // the next car's door, as one walks through a gangway: 9 m from his
    // car's middle, 9 m from the next one's, not at its door's point
    // (RTS: the next car's door's way). His heading is kept.
    std::optional<glm::vec3> through;
    const auto from_body = body_pose(unit, i, false);
    const auto to_body = body_pose(unit, j, false);
    const auto& leaving = from->description().doors[door];
    const auto& entering = to->description().doors[*arrival];
    if (from_body && to_body && leaving.has_position && entering.has_position) {
        const glm::mat4 from_scene = from_body->transform * from->placement();
        const glm::mat4 to_scene = to_body->transform * to->placement();
        const glm::vec3 d{from_scene * glm::vec4{leaving.position[0], leaving.position[1], leaving.position[2], 1.0f}};
        const glm::vec3 a{to_scene * glm::vec4{entering.position[0], entering.position[1], entering.position[2], 1.0f}};
        const glm::vec3 feet{from_scene * glm::vec4{engineer_.feet(), 1.0f}};
        // Towards the next car, along the ground.
        glm::vec3 n = a - d;
        n.y = 0.0f;
        if (glm::length(n) > 1e-3f) {
            n = glm::normalize(n);
            const glm::vec3 v = feet - d;
            const float beyond = glm::dot(v, n);
            const glm::vec3 aside = v - n * beyond;
            // As far out of the next car's door, on its side of the gangway.
            glm::vec3 there = a - n * beyond + glm::vec3{aside.x, 0.0f, aside.z};
            there.y = feet.y;
            through = glm::vec3{glm::inverse(to_scene) * glm::vec4{there, 1.0f}};
        }
    }
    engineer_car_ = j;
    if (j == cab_car_) keep_cab(i, std::move(visit_cab_));
    to->poll();
    if (entering.can_be_opened) to->open_door(*arrival);
    // On the next car's floor there (its open door's plane, its planes), or
    // a step further in; else at its door's point as before.
    bool placed = false;
    if (through) {
        const glm::vec3 point{entering.position[0], through->y, entering.position[2]};
        glm::vec3 in = point - *through;
        in.y = 0.0f;
        const float length = glm::length(in);
        for (float s = 0.0f; s <= 1.0f && !placed; s += 0.1f) {
            const glm::vec3 at = length > 1e-4f ? *through + in / length * s : *through;
            if (!Engineer::cab_holds(*to, at, engineer_.height())) continue;
            engineer_.enter_cab(*to, *arrival, at);
            placed = true;
        }
    }
    if (!placed) engineer_.arrive(*to, *arrival);
    hovered_.reset();
    std::cout << "the engineer goes through into car " << j + 1 << (placed ? " straight on" : " at its door") << ": at x "
              << engineer_.feet().x << " z " << engineer_.feet().z << " of its cab (its door's point x " << entering.position[0] << " z "
              << entering.position[2] << ")" << std::endl;
    // Into another engine's cab with a desk: RTS makes it the player's at
    // once (0x435654 -> 0x436734 -> 0x468C74: Init, ChangeLoco, the groups).
    if (j != cab_car_ && !to->description().switches.empty()) change_cab(j);
}

void TrainView::keep_cab(const std::size_t car, std::unique_ptr<CabView> cab) {
    if (!cab || units_.empty() || car >= units_.front().cars.size()) return;
    board_cabs_[units_.front().cars[car].key] = std::move(cab);
}

std::unique_ptr<CabView> TrainView::take_cab(const std::size_t car) {
    if (units_.empty() || car >= units_.front().cars.size()) return nullptr;
    const auto& c = units_.front().cars[car];
    if (!c.definition) return nullptr;
    if (const auto it = board_cabs_.find(c.key); it != board_cabs_.end() && it->second) {
        auto cab = std::move(it->second);
        board_cabs_.erase(it);
        return cab;
    }
    return CabView::create(*vfs_, *c.definition, formats::wagon_path(c.folder, c.definition->name, c.definition->engine.has_value()));
}

void TrainView::stand_at_hand_switch() {
    if (!network_ || !loaded()) return;
    const auto body = body_pose(units_.front(), 0, false);
    if (!body) return;
    (void)junction_at_hand(glm::vec3{0.0f}); // the junctions' places
    const glm::vec3 head{body->transform[3]};
    const simulation::TrackPoint* spot = nullptr;
    float nearest = std::numeric_limits<float>::max();
    for (const auto& [n, at] : junction_spots_) {
        const auto* node = network_->node(n);
        if (!node || !node->shape || !node->shape->manual_junction) continue;
        const float d = glm::length(local(at) - head);
        if (d < nearest) {
            nearest = d;
            test_hand_switch_ = n;
            spot = &at;
        }
    }
    if (!spot) {
        std::cout << "test: no hand switch on the route" << std::endl;
        return;
    }
    const glm::vec3 side{std::cos(spot->heading), 0.0f, -std::sin(spot->heading)};
    set_walking(true, local(*spot) + side * 2.5f + glm::vec3{0.0f, 1.7f, 0.0f});
    const auto* node = network_->node(test_hand_switch_);
    std::cout << "test: on foot by hand switch " << test_hand_switch_ << " (" << node->shape->file_name << ", " << nearest
              << " m from the first car), branch " << network_->junction(test_hand_switch_)->selected << std::endl;
}

void TrainView::report_hand_switch() const {
    const auto* state = network_ && test_hand_switch_ ? network_->junction(test_hand_switch_) : nullptr;
    if (!state) return;
    std::cout << "test: hand switch " << test_hand_switch_ << " branch " << state->selected << ", prompt \"" << board_prompt_ << "\""
              << std::endl;
}

void TrainView::stand_at_cab_door(const std::size_t car) {
    if (!loaded() || units_.front().role != Role::driven || car >= units_.front().cars.size()) return;
    const auto& unit = units_.front();
    const auto& c = unit.cars[car];
    if (!c.definition || !c.definition->engine) return;
    auto& cab = board_cabs_[c.key];
    if (!cab) cab = CabView::create(*vfs_, *c.definition, formats::wagon_path(c.folder, c.definition->name, true));
    const auto body = body_pose(unit, car, false);
    if (!cab || !body) return;
    const auto points = Engineer::get_on_points(*cab);
    if (points.empty()) return;
    const glm::vec3 at{body->transform * cab->placement() * glm::vec4(points.front().second, 1.0f)};
    const auto mode = engineer_.mode();
    std::cout << "test: the engineer was " << (mode == Engineer::Mode::outside ? "outside" : mode == Engineer::Mode::seated ? "seated" : engineer_.inside() ? "inside" : "off")
              << " in car " << engineer_car_ + 1 << ", the cab of car " << cab_car_ + 1 << " driven; outer doors open:";
    for (std::size_t i = 0; i < unit.cars.size(); ++i) {
        std::size_t open = 0;
        for (const auto& door : unit.cars[i].exterior_doors) open += door.open > 0.5f ? 1 : 0;
        std::cout << " car " << i + 1 << " " << open;
    }
    // Each DLL car's switches as its DLL holds them.
    for (std::size_t i = 0; legacy_ && i < unit.cars.size(); ++i) {
        const auto* out = legacy_->output(i);
        if (!out || out->switches.empty()) continue;
        std::cout << "; car " << i + 1 << " switches";
        for (std::size_t k = 0; k < out->switches.size() && k < 12; ++k) std::cout << ' ' << out->switches[k].state;
    }
    std::cout << std::endl;
    engineer_.stand_outside(at);
    std::cout << "test: on foot at the door of the cab of car " << car + 1 << std::endl;
}

CabView* TrainView::take_engineer_cab(const std::size_t car) {
    if (units_.empty() || car >= units_.front().cars.size()) return nullptr;
    if (car == cab_car_) {
        keep_cab(engineer_car_, std::move(visit_cab_));
    } else if (car != engineer_car_ || !visit_cab_) {
        auto taken = take_cab(car);
        if (!taken) return nullptr;
        keep_cab(engineer_car_, std::move(visit_cab_));
        visit_cab_ = std::move(taken);
        visit_requested_ = false;
    }
    engineer_car_ = car;
    // A cab with a desk is driven from at once, as when he walks into it
    // (pass_to_next_car; RTS 0x435654 -> 0x468C74).
    if (car != cab_car_ && visit_cab_ && !visit_cab_->description().switches.empty()) change_cab(car);
    return engineer_cab();
}

void TrainView::stand_at_passage(const std::size_t car) {
    if (!loaded() || units_.empty() || car >= units_.front().cars.size()) return;
    auto& unit = units_.front();
    CabView* cab = take_engineer_cab(car);
    if (!cab) {
        std::cout << "test: car " << car + 1 << " has no cab or salon" << std::endl;
        return;
    }
    cab->poll();
    // Its passage door towards the train's rear when a car is there, else the front.
    const auto& doors = cab->description().doors;
    std::optional<std::size_t> pick;
    for (std::size_t k = 0; k < doors.size(); ++k) {
        if (!passage_door(doors[k]) || !doors[k].has_position || doors[k].parent >= 0) continue;
        const bool toward_front = ((cab->turned() ? -1.0f : 1.0f) * doors[k].position[2] > 0.0f) != unit.cars[car].flipped;
        const bool coupled = toward_front ? car > 0 : car + 1 < unit.cars.size();
        if (!coupled) continue;
        if (!pick || !toward_front) pick = k;
    }
    if (!pick) {
        std::cout << "test: car " << car + 1 << " has no passage door to a coupled car" << std::endl;
        return;
    }
    cab->open_door(*pick);
    engineer_.arrive(*cab, *pick);
    hovered_.reset();
    // Looking at the door (the next car's room is drawn only so).
    if (const auto body = body_pose(unit, car, false)) {
        const glm::vec3 door{doors[*pick].position[0], doors[*pick].position[1], doors[*pick].position[2]};
        const glm::vec3 eye = engineer_.feet() + glm::vec3{0.0f, engineer_.height(), 0.0f};
        look_once_ = glm::vec3{body->transform * cab->placement() * glm::vec4{door - eye, 0.0f}};
    }
    std::cout << "test: on foot in car " << car + 1 << " at its door " << doors[*pick].name << std::endl;
}

void TrainView::walk_up_to_cab(const std::size_t car, const glm::vec3& feet) {
    if (!take_engineer_cab(car)) return;
    engineer_.stand_outside(feet);
    std::cout << "the player goes to the cab of car " << car + 1 << std::endl;
}

std::optional<std::pair<std::size_t, std::size_t>> TrainView::find_car(const std::uint32_t key) const {
    for (std::size_t u = 0; u < units_.size(); ++u)
        for (std::size_t i = 0; i < units_[u].cars.size(); ++i)
            if (units_[u].cars[i].key == key) return std::pair{u, i};
    return std::nullopt;
}

CabView* TrainView::car_cab(const std::size_t u, const std::size_t i, const bool create) {
    if (u >= units_.size() || i >= units_[u].cars.size()) return nullptr;
    if (u == 0 && i == cab_car_ && cab_) return cab_.get();
    if (u == 0 && i == engineer_car_ && visit_cab_) return visit_cab_.get();
    const auto& car = units_[u].cars[i];
    if (const auto it = board_cabs_.find(car.key); it != board_cabs_.end() && it->second) return it->second.get();
    // A Cabin: a driving cab or a motor car's salon (the ED9M's); a
    // passenger car's Interior: its salon.
    if (!create || !car.definition || !CabView::has_room(*car.definition)) return nullptr;
    auto cab = CabView::create(*vfs_, *car.definition,
                               formats::wagon_path(car.folder, car.definition->name, car.definition->engine.has_value()));
    if (!cab) return nullptr;
    auto* raw = cab.get();
    board_cabs_[car.key] = std::move(cab);
    return raw;
}

std::optional<CarOutside> TrainView::car_outside(const std::size_t u, const std::size_t i, const bool with_cab) {
    if (u >= units_.size()) return std::nullopt;
    auto& unit = units_[u];
    if (i >= unit.cars.size() || !unit.cars[i].outside || unit.current.empty()) return std::nullopt;
    const auto body = body_pose(unit, i, false);
    if (!body) return std::nullopt;
    auto& car = unit.cars[i];
    CarOutside out;
    out.details = car.outside.get();
    out.transform = body->transform;
    out.width = car.definition && car.definition->width > 1.0f ? car.definition->width : 3.1f;
    const auto* details = car.outside.get();
    const auto* anims = &car.exterior_doors;
    out.door_open = [details, anims](std::size_t k) {
        // A leaf has its door's state (ParentObject).
        for (int guard = 0; k < details->door_details.size() && details->door_details[k].parent >= 0 && guard < 8; ++guard)
            k = static_cast<std::size_t>(details->door_details[k].parent);
        for (const auto& door : *anims)
            if (door.index == k) return door.open >= 0.6f;
        return true; // no node of its own: a doorway
    };
    if (with_cab) {
        if (const CabView* cab = car_cab(u, i, true)) {
            out.cab_holds = [cab, height = engineer_.height()](const glm::vec3& at_car) {
                return Engineer::cab_holds(*cab, glm::vec3{glm::inverse(cab->placement()) * glm::vec4{at_car, 1.0f}}, height);
            };
        }
    }
    return out;
}

std::optional<TrainView::LadderTarget> TrainView::ladder_target(const glm::vec3& eye, const glm::vec3& forward) {
    std::optional<LadderTarget> best;
    for (std::size_t u = 0; u < units_.size(); ++u) {
        const auto& unit = units_[u];
        if (unit.remote || unit.current.empty()) continue;
        for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size(); ++i) {
            const auto& car = unit.cars[i];
            if (!car.outside || car.outside->ladders.empty() || !car.definition) continue;
            const auto here = local(unit.current[i].front);
            if (glm::distance(glm::vec2{here.x, here.z}, glm::vec2{eye.x, eye.z}) > car.definition->length + 8.0f) continue;
            const auto outside = car_outside(u, i, false);
            if (!outside) continue;
            if (const auto ladder = Engineer::ladder_in_reach(*outside, eye, forward); ladder && (!best || ladder->second < best->distance))
                best = LadderTarget{u, i, ladder->first, ladder->second};
        }
    }
    return best;
}

void TrainView::carry_out(const Engineer::Event& event) {
    using Kind = Engineer::Event::Kind;
    if (event.kind == Kind::none) return;
    if (event.kind == Kind::onto_car) {
        // Out of his cab onto its car's outside (RTS 0x431E00 / 0x431270).
        auto* cab = engineer_cab();
        if (!cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) return;
        const std::size_t i = engineer_car_;
        const auto outside = car_outside(0, i);
        if (!outside) return;
        const glm::vec3 at_car{cab->placement() * glm::vec4{event.at, 1.0f}};
        std::optional<std::size_t> path;
        if (event.by_path) {
            if (event.door >= cab->description().doors.size()) return;
            const auto name = lower(cab->description().doors[event.door].name);
            for (std::size_t k = 0; k < outside->details->door_details.size(); ++k) {
                const auto& door = outside->details->door_details[k];
                if (lower(door.name) == name && !door.trajectory.empty()) path = k;
            }
            if (!path) return; // RTS: only a door with a way (flag 1)
        } else if (outside_footing(*outside->details, outside->door_open, at_car).kind == OutsideFooting::Kind::none) {
            return; // not onto anything there: he stays
        }
        // Where the way ends: the cab door's other side (RTS 0x431270), the
        // car's walkway or (EXTERIOR) the ground at the steps' foot.
        std::string end = "ext_wag";
        if (event.by_path && event.door < cab->description().doors.size()) {
            const auto& door = cab->description().doors[event.door];
            if (lower(door.from) == "exterior" || lower(door.to) == "exterior") end = "exterior";
        }
        if (end == "exterior") std::cout << "the engineer climbs down from car " << i + 1 << std::endl;
        engineer_.step_onto_car(*outside, at_car, path, end);
        outside_car_ = units_.front().cars[i].key;
        // The cab he left stays with its doors.
        if (engineer_car_ != cab_car_) keep_cab(engineer_car_, std::move(visit_cab_));
        engineer_car_ = cab_car_;
        hovered_.reset();
        std::cout << "the engineer goes out onto car " << i + 1 << std::endl;
        return;
    }
    if (event.kind == Kind::climb_in) {
        // On the ground at his cab's door: up the car's way of its name.
        auto* cab = engineer_cab();
        if (!cab || units_.empty() || engineer_car_ >= units_.front().cars.size() || event.door >= cab->description().doors.size())
            return;
        const std::size_t i = engineer_car_;
        const auto outside = car_outside(0, i);
        if (!outside) return;
        const auto name = lower(cab->description().doors[event.door].name);
        for (std::size_t k = 0; k < outside->details->door_details.size(); ++k) {
            const auto& door = outside->details->door_details[k];
            if (door.trajectory.empty() || lower(door.name) != name) continue;
            engineer_.go_through(*outside, k);
            outside_car_ = units_.front().cars[i].key;
            hovered_.reset();
            std::cout << "the engineer climbs up into car " << i + 1 << " by its door " << door.name << std::endl;
            return;
        }
        return;
    }
    const auto at = outside_car_ ? find_car(*outside_car_) : std::nullopt;
    if (!at) return;
    auto [u, i] = *at;
    auto& car = units_[u].cars[i];
    if (!car.outside) return;
    switch (event.kind) {
    case Kind::outer_door:
        work_outer_door(u, i, event.door);
        return;
    case Kind::into_cab: {
        if (event.door >= car.outside->door_details.size()) return;
        const auto name = lower(car.outside->door_details[event.door].name);
        const auto key = car.key;
        if (u != 0 || (units_[u].role != Role::driven && !helping_)) {
            // Another train's engine: his own other train becomes the driven one.
            const auto id = units_[u].train->id();
            std::string problem = boardable_.contains(id) ? set_role(id, Role::driven) : std::string("This is not your train.");
            if (problem.empty()) {
                const auto again = find_car(key);
                if (!again || again->first != 0) problem = "This is not your train.";
                if (again) std::tie(u, i) = *again;
            }
            if (!problem.empty()) {
                message_ = tr(problem.c_str());
                if (auto outside = car_outside(u, i)) engineer_.step_onto_car(*outside, event.at);
                return;
            }
        }
        CabView* cab = take_engineer_cab(i);
        if (!cab) {
            message_ = tr("The next car has no interior to walk into.");
            return;
        }
        std::size_t door = 0;
        const auto& doors = cab->description().doors;
        for (std::size_t j = 0; j < doors.size(); ++j)
            if (lower(doors[j].name) == name) door = j;
        engineer_.enter_cab(*cab, door, glm::vec3{glm::inverse(cab->placement()) * glm::vec4{event.at, 1.0f}});
        outside_car_.reset();
        hovered_.reset();
        std::cout << "the engineer goes into the cab of car " << i + 1 << std::endl;
        return;
    }
    case Kind::next_car: {
        auto& unit = units_[u];
        const auto from = car_outside(u, i, false);
        if (!from) return;
        // The car's +Z end looks to the front of the train unless it is turned.
        const bool toward_front = (event.side > 0) != unit.cars[i].flipped;
        if (!((toward_front && i == 0) || (!toward_front && i + 1 >= unit.cars.size()))) {
            const std::size_t j = toward_front ? i - 1 : i + 1;
            if (const auto to = car_outside(u, j, false)) {
                const glm::vec3 scene{from->transform * glm::vec4{event.at, 1.0f}};
                const glm::vec3 there{glm::inverse(to->transform) * glm::vec4{scene, 1.0f}};
                if (const auto held = outside_footing(*to->details, to->door_open, there); held.kind != OutsideFooting::Kind::none) {
                    engineer_.step_onto_car(*to, held.at);
                    outside_car_ = unit.cars[j].key;
                    return;
                }
            }
        }
        if (event.or_off) engineer_.get_off_car(*from, event.at);
        return;
    }
    default:
        return;
    }
}

void TrainView::work_outer_door(const std::size_t u, const std::size_t i, const std::size_t k) {
    if (u >= units_.size() || i >= units_[u].cars.size()) return;
    auto& car = units_[u].cars[i];
    if (!car.outside || k >= car.outside->door_details.size()) return;
    const auto name = lower(car.outside->door_details[k].name);
    // A cab's door: the cab's of the same name (the outer one follows it).
    if (auto* cab = car_cab(u, i, true)) {
        const auto& doors = cab->description().doors;
        for (std::size_t j = 0; j < doors.size(); ++j) {
            if (lower(doors[j].name) != name) continue;
            cab->toggle_door(j);
            return;
        }
    }
    for (auto& door : car.exterior_doors) {
        if (door.index != k) continue;
        door.wanted = !(door.open >= 0.6f);
        return;
    }
}

const TrainView::CarMeshData* TrainView::car_mesh(const core::VirtualPath& shape) {
    const std::string key(shape.str());
    auto it = car_meshes_.find(key);
    if (it == car_meshes_.end()) {
        CarMesh entry;
        entry.loading = std::async(std::launch::async, [vfs = vfs_, shape]() -> std::shared_ptr<const CarMeshData> {
            const auto body = formats::load_binary_shape_body(*vfs, shape);
            if (!body.ok()) return nullptr;
            auto model = formats::parse_binary_shape_model(*body.body);
            if (!model.ok()) return nullptr;
            auto data = std::make_shared<CarMeshData>();
            data->mesh = CabCollision::build(model);
            model.vertices.clear();
            model.vertices.shrink_to_fit();
            model.indices.clear();
            model.indices.shrink_to_fit();
            data->rig = std::move(model);
            return data;
        });
        car_meshes_.emplace(key, std::move(entry));
        return nullptr;
    }
    auto& entry = it->second;
    if (entry.loading.valid()) {
        if (entry.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
        entry.data = entry.loading.get();
    }
    return entry.data && !entry.data->mesh.empty() ? entry.data.get() : nullptr;
}

void TrainView::outer_door_pointer(const glm::vec3& eye, const glm::vec3& direction, const bool click) {
    constexpr float reach = 3.0f;
    struct Best {
        std::size_t u{}, i{}, k{};
        float distance{};
    };
    std::optional<Best> best;
    float nearest = reach;
    for (std::size_t u = 0; u < units_.size(); ++u) {
        const auto& unit = units_[u];
        if (unit.remote || unit.current.empty()) continue;
        for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size(); ++i) {
            const auto& car = unit.cars[i];
            if (car.exterior_doors.empty() || !car.outside || !car.definition) continue;
            const auto body = body_pose(unit, i, false);
            if (!body) continue;
            const float length = car.definition->length > 1.0f ? car.definition->length : 20.0f;
            if (glm::distance(glm::vec3{body->transform[3]}, eye) > length * 0.5f + reach + 3.0f) continue;
            const auto* data = car_mesh(car.shape);
            if (!data) continue;
            const auto& rig = data->rig;
            const glm::mat4 to_car = glm::inverse(body->transform);
            const glm::vec3 o{to_car * glm::vec4{eye, 1.0f}};
            const glm::vec3 d = glm::normalize(glm::vec3{to_car * glm::vec4{direction, 0.0f}});
            // Each door's node and the nodes below it.
            const auto nodes_of = [&](const std::int32_t node) {
                std::vector<std::int32_t> nodes{node};
                for (bool grew = true; grew;) {
                    grew = false;
                    for (std::size_t n = 0; n < rig.hierarchy.size(); ++n) {
                        const auto parent = rig.hierarchy[n];
                        if (parent < 0 || std::find(nodes.begin(), nodes.end(), parent) == nodes.end()) continue;
                        if (std::find(nodes.begin(), nodes.end(), static_cast<std::int32_t>(n)) != nodes.end()) continue;
                        nodes.push_back(static_cast<std::int32_t>(n));
                        grew = true;
                    }
                }
                return nodes;
            };
            std::vector<std::vector<std::int32_t>> door_nodes;
            std::vector<std::uint8_t> skip(rig.matrices.size(), 0);
            for (const auto& door : car.exterior_doors) {
                door_nodes.push_back(door.node >= 0 ? nodes_of(door.node) : std::vector<std::int32_t>{});
                // An open door is tried where it is drawn, below.
                if (door.open >= 0.6f && door.last_frame > 0.0f)
                    for (const auto n : door_nodes.back())
                        if (static_cast<std::size_t>(n) < skip.size()) skip[static_cast<std::size_t>(n)] = 1;
            }
            const auto rest = data->mesh.ray(o, d, nearest, skip);
            float limit = rest ? rest->distance : nearest;
            std::optional<std::size_t> hit_door;
            if (rest && rest->node >= 0) {
                for (std::size_t a = 0; a < door_nodes.size() && !hit_door; ++a)
                    if (std::find(door_nodes[a].begin(), door_nodes[a].end(), rest->node) != door_nodes[a].end()) hit_door = a;
            }
            for (std::size_t a = 0; a < car.exterior_doors.size(); ++a) {
                const auto& door = car.exterior_doors[a];
                if (door.node < 0 || door.last_frame <= 0.0f || door.open < 0.6f) continue;
                if (const auto hit = data->mesh.ray_posed(rig, door.node, door.open * door.last_frame, door_nodes[a], o, d, limit)) {
                    limit = hit->distance;
                    hit_door = a;
                }
            }
            if (rest || hit_door) nearest = (std::min)(nearest, limit); // a nearer car body hides what is behind it
            if (!hit_door) {
                if (rest && best && rest->distance < best->distance) best.reset();
                continue;
            }
            best = Best{u, i, car.exterior_doors[*hit_door].index, limit};
        }
    }
    if (!best) return;
    const auto& car = units_[best->u].cars[best->i];
    const auto& details = car.outside->door_details;
    // A leaf (ParentObject) is worked through the door it follows.
    std::size_t k = best->k;
    for (int guard = 0; k < details.size() && details[k].parent >= 0 && guard < 8; ++guard) k = static_cast<std::size_t>(details[k].parent);
    if (k >= details.size()) return;
    bool open = false;
    for (const auto& door : car.exterior_doors)
        if (door.index == k) open = door.open >= 0.6f;
    const bool by_hand = details[k].can_be_opened;
    door_hover_ = DoorHover{.at = eye + direction * best->distance,
                            .text = !by_hand ? tr("The passenger doors are opened from the driver's cab.")
                                    : open   ? tr("Door: click to close")
                                             : tr("Door: click to open")};
    if (click && by_hand) {
        work_outer_door(best->u, best->i, k);
        std::cout << "the engineer " << (open ? "shuts" : "opens") << " door " << details[k].name << " of car " << best->i + 1
                  << " with the mouse" << std::endl;
    }
}

void TrainView::pointer(const glm::vec3& eye, const std::optional<glm::vec3>& direction, const bool click, const bool shift,
                        const float scroll, const bool release, const glm::vec2& cursor, const bool held) {
    // A helper's hands work the engineer's cab (helper_cab_), else the DLLs here.
    LegacyCab* hands = helping_ ? helper_cab_.get() : legacy_.get();
    // A control in hand (RTS 0x4121BC) follows the cursor while the button
    // is down; the rest of the cab waits.
    if (hands && hands->dragging()) {
        if (!held || release) {
            hands->drag_end();
        } else {
            hands->drag(cursor.x - drag_cursor_.x, cursor.y - drag_cursor_.y, shift);
            drag_cursor_ = cursor;
        }
        if (hands->dragging()) return;
    }
    if (release && hands) hands->release();
    hovered_.reset();
    door_hover_.reset();
    // On foot by the train: the cars' outer doors under the mouse.
    if (direction && car_view_ == CarView::off && !engineer_.passing()
        && (engineer_.mode() == Engineer::Mode::outside || engineer_.mode() == Engineer::Mode::on_car)) {
        outer_door_pointer(eye, *direction, click);
        return;
    }
    const auto* cab = engineer_cab();
    if (!direction || !viewing_inside() || !cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) return;
    const auto body = body_pose(units_.front(), engineer_car_, false);
    if (!body) return;
    const glm::mat4 to_scene = cab_to_scene_ ? *cab_to_scene_ : body->transform * cab->placement();
    const glm::mat4 to_cab = glm::inverse(to_scene);
    const glm::vec3 origin{to_cab * glm::vec4{eye, 1.0f}};
    const glm::vec3 along = glm::normalize(glm::vec3{to_cab * glm::vec4{*direction, 0.0f}});
    // The reach is each control's Radius (RTS); the ray goes as far as any.
    hovered_ = cab->pick(origin, along, 4.0f);
    // A door under the mouse, nearer than any control (openRTS: a click
    // opens or shuts it; RTS works doors with Enter only).
    if (const auto door = cab->pick_door(origin, along, 2.5f); door && (!hovered_ || door->distance < hovered_->distance)) {
        hovered_.reset();
        const auto root = cab->door_root(door->door);
        const auto& doors = cab->description().doors;
        const bool by_hand = root < doors.size() && doors[root].can_be_opened;
        const bool open = cab->door_open(root);
        door_hover_ = DoorHover{.at = glm::vec3{to_scene * glm::vec4{origin + along * door->distance, 1.0f}},
                                .text = !by_hand ? tr("The passenger doors are opened from the driver's cab.")
                                        : open   ? tr("Door: click to close")
                                                 : tr("Door: click to open")};
        if (click && by_hand) {
            if (CabView* own = engineer_car_ == cab_car_ ? cab_.get() : visit_cab_.get()) {
                own->toggle_door(root);
                std::cout << "the engineer " << (open ? "shuts" : "opens") << " the cab's door " << doors[root].name << " with the mouse"
                          << std::endl;
            }
            return;
        }
    }
    // The wheel on a screen of the cab (TME's InsideThreeDimCamera): the eye
    // up close to read it, then back — the wheel down lets it go, and so does
    // * (restore_screen_focus). A switch in front of the screen keeps the
    // wheel: it turns the handle below.
    if (engineer_.focused()) {
        if (scroll != 0.0f) {
            if (scroll < 0.0f) engineer_.release_focus();
            return;
        }
    } else if (scroll > 0.0f && engineer_.inside() && !helping_ && !(hovered_ && hovered_->control)) {
        if (const auto screen = cab->pick_screen(origin, along, 4.0f)) {
            if (const auto focus = cab_screen_focus(screen->plane, origin, frame_fov_, frame_aspect_)) {
                engineer_.focus_screen(focus->eye, focus->forward);
                hovered_.reset();
                return;
            }
        }
    }
    // The wheel turns handles only (CONTROLLERX / CONTROLLERY, the ones a
    // drag moves, or the piece of one); over a button or a switch it is as
    // off the controls, so leaning in to read the КЛУБ no longer pressed
    // its keys (openRTS).
    float wheel = hovered_ && hovered_->control ? scroll : 0.0f;
    if (wheel != 0.0f) {
        const auto& all = cab->description().switches;
        const auto root = cab->description().root_switch(all[hovered_->index].id);
        bool handle = false;
        for (const auto& each : all) {
            if (each.id == root || each.id == all[hovered_->index].id) {
                handle = handle || (each.flags & (formats::cab_controller_x | formats::cab_controller_y)) != 0;
            }
        }
        if (!handle) wheel = 0.0f;
    }
    // The wheel off the controls at the chair: the head nearer to what the
    // cursor points at, or back.
    if (scroll != 0.0f && wheel == 0.0f && engineer_.mode() == Engineer::Mode::seated) {
        engineer_.lean(along, scroll);
        if (!click) return;
    }
    if (!hovered_ || !hovered_->control || (!click && wheel == 0.0f)) return;
    if (helping_) {
        // The engineer's cab: what his DLLs showed of it has to be here.
        if (!helper_cab_ || !helper_cab_->has_cab(engineer_car_)) {
            message_ = tr("The engineer's cab has not come over the network yet.");
            return;
        }
        const auto& all = cab->description().switches;
        const auto id = cab->description().root_switch(all[hovered_->index].id);
        std::size_t index = hovered_->index;
        for (std::size_t k = 0; k < all.size(); ++k) {
            if (all[k].id == id) {
                index = k;
                break;
            }
        }
        if (click && helper_cab_->grab(id, cab->switch_frames_per_state(index), engineer_car_)) {
            drag_cursor_ = cursor;
        } else if (click) {
            helper_cab_->click(id, 0, shift, engineer_car_);
        }
        if (wheel != 0.0f) helper_cab_->click(id, wheel > 0.0f ? 1 : -1, false, engineer_car_);
        return;
    }
    if (native_ && engineer_car_ == native_car_) {
        // A native plugin's cab: RTS's click step, the wheel one way.
        const auto id = cab->description().switches[hovered_->index].id;
        if (click) native_operate(id, 0);
        if (wheel != 0.0f) native_operate(id, wheel > 0.0f ? 1 : -1);
        return;
    }
    // The cab driven from, or the interior of another car whose DLL has it
    // (a salon's stop valves: the brakes library reads that car's switches).
    const bool driven = engineer_car_ == cab_car_ && legacy_ && legacy_->player() == cab_car_;
    const bool visited = !driven && legacy_ && engineer_car_ != cab_car_ && legacy_->has_cab(engineer_car_);
    if (!driven && !visited) {
        if (!legacy_) {
            message_ = tr("Without a DLL the train is driven with the keys (Home / End, Page Up / Page Down, [ ])");
            return;
        }
        message_ = tr("The controls work in the cab you drive from.");
        return;
    }
    const auto car = driven ? SIZE_MAX : engineer_car_;
    // A piece that follows another (ParentObject: the EP1M's blind) is worked
    // through the one it follows: taking any piece takes the whole blind.
    const auto& switches = cab->description().switches;
    const auto id = cab->description().root_switch(switches[hovered_->index].id);
    std::size_t index = hovered_->index;
    for (std::size_t k = 0; k < switches.size(); ++k) {
        if (switches[k].id == id) {
            index = k;
            break;
        }
    }
    // A CONTROLLERX / CONTROLLERY control: a press takes hold of it and only
    // moving the mouse moves it (RTS 0x4121BC); a quick click (let go within
    // the frame) leaves it where it is, it does not toggle.
    (void)held;
    if (click && legacy_->grab(id, cab->switch_frames_per_state(index), car)) {
        drag_cursor_ = cursor;
    } else if (click) {
        legacy_->click(id, 0, shift, car);
    }
    if (wheel != 0.0f) legacy_->click(id, wheel > 0.0f ? 1 : -1, false, car);
}

// ---------------------------------------------------------------------------
// The car menu (RTS Num3: 0x415A34 picks, 0x462FC8 builds, 0x463F84 acts).

void TrainView::car_menu_key(const glm::vec3& eye, const glm::mat4& view_projection, const glm::vec2 aim) {
    if (car_menu_) {
        car_menu_.reset();
        car_menu_choice_.reset();
        return;
    }
    // RTS picks only off the cab (rts1.exe 0x415A34: no current locomotive,
    // or the engineer on foot).
    if (viewing_inside() || !placed()) return;
    // On the screen: how far from the aim it shows (clip x / y), else none.
    const auto off_aim = [&](const glm::vec3& p) -> std::optional<float> {
        const glm::vec4 clip = view_projection * glm::vec4{p, 1.0f};
        if (!(clip.w > 0.0f && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w)) return std::nullopt;
        return glm::distance(glm::vec2{clip.x, clip.y} / clip.w, aim);
    };
    std::optional<CarMenu> best;
    float best_score = 1e9f;
    // Nearest to the aim; of two alike on the screen, the nearer one.
    const auto offer = [&](const glm::vec3& p, const float reach, const CarMenu& menu) {
        const float d = glm::distance(p, eye);
        if (d >= reach) return;
        const auto screen = off_aim(p);
        if (!screen) return;
        const float score = *screen + 0.01f * d;
        if (score < best_score) {
            best_score = score;
            best = menu;
        }
    };
    for (std::size_t u = 0; u < units_.size(); ++u) {
        const auto& unit = units_[u];
        if (!unit.train || unit.current.empty()) continue;
        const auto& states = unit.train->cars();
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size() && i < unit.current.size(); ++i) {
            const auto& state = states[i];
            const auto* definition = unit.cars[i].definition;
            if (!definition) continue;
            const auto body = body_pose(unit, i, false);
            if (!body) continue;
            const float length = definition->length > 1.0f ? definition->length : 20.0f;
            const float width = definition->width > 1.0f ? definition->width : 3.1f;
            const auto at = [&](const float y, const float z) { return glm::vec3{body->transform * glm::vec4{0.0f, y, z, 1.0f}}; };
            // The shape's +z end faces the train front unless the car is turned.
            const bool shape_front_is_train_front = !state.flipped;
            // Couplings 1 m above the rails at the car's ends, within 5 m,
            // of a standing car or one of the driven train.
            if (std::abs(state.velocity) < 0.1f || u == 0) {
                for (const float side : {1.0f, -1.0f}) {
                    offer(at(1.0f, side * length * 0.5f), 5.0f,
                          CarMenu{.coupling = true, .train = unit.train->id(), .car = i, .front = (side > 0.0f) == shape_front_is_train_front});
                }
            }
            // Bogies at the rails, within twice the car's width, of a car
            // slower than 3 m/s or of the driven train (RTS leaves out the
            // player's locomotive; here its bogies are offered too).
            if (std::abs(state.velocity) < 3.0f || u == 0) {
                for (const float side : {1.0f, -1.0f}) {
                    const bool train_front = (side > 0.0f) == shape_front_is_train_front;
                    float z = train_front ? state.bogie_ahead : state.bogie_behind;
                    if (z <= 0.0f) z = length * 0.25f;
                    offer(at(0.0f, side * z), 2.0f * width, CarMenu{.coupling = false, .train = unit.train->id(), .car = i, .front = train_front});
                }
            }
        }
    }
    if (best && !car_menu_items(*best).empty()) {
        car_menu_ = best;
        last_car_menu_ = best;
    }
}

std::string TrainView::car_menu_report() const {
    if (!last_car_menu_) return "no car menu";
    const auto& menu = car_menu_ ? *car_menu_ : *last_car_menu_;
    std::string text = std::string(menu.coupling ? "coupling" : "bogie") + " of car " + std::to_string(menu.car + 1)
        + (menu.front ? " (front)" : " (rear)") + (car_menu_ ? ", open:" : ", closed");
    if (car_menu_) {
        for (const auto& item : car_menu_items(menu)) text += std::string(" [") + std::to_string(item.id) + " " + item.label + "]";
    }
    if (const auto* unit = find_unit(menu.train)) {
        const auto& cars = unit->train->cars();
        char line[160];
        for (std::size_t i = menu.car > 0 ? menu.car - 1 : 0; i < cars.size() && i <= menu.car + 1; ++i) {
            std::snprintf(line, sizeof line, "; car %zu ends 0x%02X open 0x%X chocks 0x%X valves 0x%02X handbrake %.2f pipe %.2f cylinder %.2f",
                          i + 1, cars[i].pipe_ends, cars[i].coupler_open, cars[i].chocks, cars[i].valves, cars[i].handbrake, cars[i].pipe,
                          cars[i].cylinder);
            text += line;
        }
        text += "; " + std::to_string(cars.size()) + " cars in the train";
        // At a free end: the train it meets ("Couple" needs it within the couplers' reach, both standing).
        const bool free_end = menu.coupling && (menu.front ? menu.car == 0 : menu.car + 1 == cars.size());
        if (free_end) {
            std::vector<simulation::Train*> trains;
            for (const auto& u : units_) trains.push_back(u.train.get());
            if (const auto contact = simulation::end_contact(*unit->train, menu.front, trains, 5.0)) {
                const auto& other = contact->other_front ? contact->other->cars().front() : contact->other->cars().back();
                std::snprintf(line, sizeof line, "; meets a train at %.3f m, speeds %.3f / %.3f m/s", contact->gap,
                              cars[menu.car].velocity, other.velocity);
                text += line;
            } else {
                text += "; meets no train within 5 m";
            }
        }
    } else {
        text += "; its train is gone";
    }
    if (!message_.empty()) text += "; message: " + message_;
    return text;
}

std::string TrainView::systems_report() const {
    std::string text = std::to_string(light_glows_.size()) + " light glows, " + std::to_string(light_beams_.size()) + " beams, "
        + std::to_string(interior_lights_.size()) + " cab lamps, " + std::to_string(cab_texture_changes_) + " cab texture changes";
    if (native_) text += "; native plugin '" + native_->name() + "' on car " + std::to_string(native_car_ + 1);
    char line[200];
    for (const auto& unit : units_) {
        const auto& states = unit.train->cars();
        std::size_t lit = 0;
        for (const auto& car : unit.cars)
            for (const auto& l : car.lights) lit += l.level > 0.0f ? 1 : 0;
        std::snprintf(line, sizeof line, "; %s (%s, %zu cars, %s air): lights %zu", unit.name.c_str(),
                      unit.role == Role::driven ? "driven" : unit.role == Role::ai ? "ai" : "standing", unit.cars.size(),
                      "realistic", lit);
        text += line;
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            const auto& c = states[i];
            if (!c.definition->engine) continue;
            std::snprintf(line, sizeof line, ", car %zu panto 0x%X line %.0f V %.0f A heat %.0f%s cables 0x%X%s", i + 1,
                          unit.cars[i].pantograph_raised, c.line_voltage, c.line_current, c.line_heat, c.line_burnt ? " BURNT" : "",
                          c.cables, c.sanding ? " sanding" : "");
            text += line;
        }
    }
    for (const auto& load : electrification_->substation_loads()) {
        std::snprintf(line, sizeof line, "; substation %u phase %u %.2f MW", load.substation, load.phase, load.power / 1e6f);
        text += line;
    }
    // What the DLLs get of the world: the track item list (RTSGetTrackItems)
    // and the signals with their route database entry (RTSSignalData).
    if (legacy_) {
        const auto& world = legacy_->world();
        std::size_t ahead = 0, under = 0, behind = 0, named = 0, signals = 0;
        for (const auto& e : world.track_items)
            (e.pos == dllhost::item_ahead ? ahead : e.pos == dllhost::item_under ? under : behind) += 1;
        for (const auto* list : {&world.alsn_ahead, &world.alsn_behind, &world.alsn_passing}) {
            for (const auto& s : *list) {
                ++signals;
                if (s.unit >= 0) ++named;
            }
        }
        std::snprintf(line, sizeof line,
                      "; dll items %zu ahead %zu under %zu behind (searched %.0f / %.0f m, asked %.0f m), alsn signals %zu, %zu with an entry",
                      ahead, under, behind, items_searched_[0], items_searched_[1], legacy_->items_wanted(), signals, named);
        text += line;
        // Where the player is: the DLLs' player engine, the cab and the engineer (car numbers from 1).
        const char* mode = engineer_.mode() == Engineer::Mode::seated ? "seated"
                         : engineer_.mode() == Engineer::Mode::standing ? "standing"
                         : engineer_.mode() == Engineer::Mode::outside ? "outside" : "off";
        std::snprintf(line, sizeof line, "; dll player car %zu, cab car %zu, engineer %s in car %zu", legacy_->player() + 1, cab_car_ + 1,
                      mode, engineer_car_ + 1);
        text += line;
        // The host's trains: the driven one, then the parts cut off from it.
        text += "; dll host trains " + std::to_string(legacy_->trains()) + " (";
        for (std::size_t t = 0; t < legacy_->trains(); ++t) {
            std::size_t engines = 0, bound = 0;
            for (std::size_t i = 0; i < legacy_->train_size(t); ++i) {
                if (!legacy_->has_dll(t, i)) continue;
                ++engines;
                if (const auto* out = legacy_->output(t, i); out && (out->loco_flags & 0x20)) ++bound;
            }
            text += (t ? ", " : "") + std::to_string(legacy_->train_size(t)) + " cars " + std::to_string(bound) + "/"
                + std::to_string(engines) + " DLLs bound";
        }
        text += ")";
    }
    // The cab's lamps as its DLL lights them (state by .sd ID): an ALSN that
    // stays dark is the DLL's (its power off) or the drawing's.
    if (cab_ && legacy_) {
        if (const auto* out = legacy_->output(cab_car_)) {
            std::string lamps;
            for (const auto& d : cab_->description().displays) {
                if (d.type != formats::CabDisplayType::lamp || d.id >= out->displays.size()) continue;
                lamps += " " + std::to_string(d.id) + "/" + d.name + "=" + std::to_string(out->displays[d.id].state);
            }
            text += "; cab lamps:" + lamps;
            std::string on;
            for (std::size_t k = 0; k < out->switches.size(); ++k)
                if (out->switches[k].state != 0) on += " " + std::to_string(k) + "=" + std::to_string(out->switches[k].state);
            text += "; cab switches not at 0:" + on;
            // The locomotive signal lights (LocomotiveSignalization): the DLL's
            // aspect, the engine's ALSNOn (0: all out), the lights' nodes.
            if (!cab_->description().signalization.lights.empty()) {
                text += "; signalization aspect " + std::to_string(out->cab_aspect) + " of " + std::to_string(cab_->description().signalization.aspects.size())
                    + ", ALSNOn " + (out->has_engine ? std::to_string(static_cast<std::int16_t>(out->engine.alsn_on)) : std::string("-")) + ", nodes";
                for (const auto node : cab_->signal_light_nodes()) text += " " + std::to_string(node);
            }
        }
    }
    // The cab screens' labels: what the DLL wrote, and how much of it shows.
    if (const auto* layout = cab_ ? cab_->screen_layout() : nullptr) {
        std::size_t shown = 0, partly = 0;
        std::string labels;
        for (const auto& l : layout->labels_seen()) {
            if (l.shown) ++shown;
            if (l.shown && l.drawn < l.chars) ++partly;
            labels += " " + std::to_string(l.display) + "/" + std::to_string(l.object) + (l.shown ? "" : "(hidden)") + "'"
                + trainplugin::utf16_to_utf8(l.text) + "'";
            if (l.drawn < l.chars) labels += "[" + std::to_string(l.drawn) + "/" + std::to_string(l.chars) + "]";
        }
        text += "; screen labels with a text " + std::to_string(layout->labels_seen().size()) + ", shown " + std::to_string(shown)
            + ", shown in part " + std::to_string(partly) + ":" + labels;
    }
    // The next cars' rooms seen through his car's passage doors (openRTS).
    if (!neighbour_room_keys_.empty()) {
        text += "; next rooms";
        for (const auto key : neighbour_room_keys_) {
            const auto at = find_car(key);
            const auto* room = at && at->first == 0 ? const_cast<TrainView*>(this)->car_cab(0, at->second, false) : nullptr;
            if (!room) continue;
            bool through = false;
            std::string sight;
            for (const auto& r : neighbour_rooms_now_) {
                if (r.car != at->second) continue;
                through = true;
                // The shader's test on the CPU: the line from the eye to the
                // middle of that car, 1.5 m over its floor, through the doorway?
                const auto& unit = units_.front();
                const auto body = body_pose(unit, r.car, true);
                if (!body || r.portal >= portals_.size()) continue;
                const auto& p = portals_[r.portal];
                const glm::vec3 target{body->transform * glm::vec4{0.0f, 2.8f, 0.0f, 1.0f}};
                const glm::vec3 n = glm::cross(p.across, p.up);
                const glm::vec3 along = target - portal_eye_;
                const float towards = glm::dot(n, along);
                const float t = std::abs(towards) > 1e-6f ? glm::dot(n, p.centre - portal_eye_) / towards : -1.0f;
                const glm::vec3 hit = portal_eye_ + along * t - p.centre;
                const bool in = t > 0.0f && t < 1.0f && std::abs(glm::dot(hit, p.across)) <= p.half_across
                                && std::abs(glm::dot(hit, p.up)) <= p.half_up;
                char seen[160];
                std::snprintf(seen, sizeof seen, " (doorway %.2f m from the eye, %.2f x %.2f m, the gangway %.2f .. %.2f m beyond; its middle %s it)",
                              glm::distance(portal_eye_, p.centre), p.half_across * 2.0f, p.half_up * 2.0f, p.near_cut, p.far_cut,
                              in ? "seen through" : "NOT seen through");
                sight = seen;
            }
            text += " car " + std::to_string(at->second + 1) + " " + room->shape().str() + (room->bound() ? " loaded" : " loading")
                + (through ? " seen through its doorway" + sight : " (not drawn: the door shut, or not looked at)");
        }
        text += ", doorways " + std::to_string(portals_.size());
    }
    // The engineer's room and the share of the outside heard there (docs/sound.md).
    if (listener_cab_) {
        const auto& areas = listener_cab_->description().areas;
        const auto room = listener_room_ >= 0 && static_cast<std::size_t>(listener_room_) < areas.size()
            ? areas[static_cast<std::size_t>(listener_room_)].name
            : std::string("none");
        std::snprintf(line, sizeof line, "; listener room %s outside %.2f", room.c_str(), outside_gain_);
        text += line;
    }
    return text;
}

std::vector<TrainView::CarMenuItem> TrainView::car_menu_items(const CarMenu& menu) const {
    std::vector<CarMenuItem> items;
    const auto* unit = find_unit(menu.train);
    if (!unit || menu.car >= unit->train->cars().size()) return items;
    const auto& cars = unit->train->cars();
    const auto& car = cars[menu.car];
    if (!menu.coupling) {
        // Bogie (0x462FC8 type 2).
        const auto chock = menu.front ? simulation::chock_front : simulation::chock_rear;
        if (car.chocks & chock) {
            items.push_back({menu.front ? 1 : 3, "Remove drag"});
        } else {
            items.push_back({menu.front ? 2 : 4, "Place drag"});
        }
        if (car.handbrake <= 0.01f) {
            items.push_back({6, "Apply handbrake"});
        } else {
            items.push_back({5, "Release handbrake"});
        }
        if (car.valves & simulation::valve_release) {
            items.push_back({12, "Shut release valve"});
        } else if (car.definition->brakes.equipment & 0xB) {
            items.push_back({11, "Open release valve"});
        }
        if (car.valves & simulation::valve_emergency) items.push_back({7, "Shut emergency valve"});
        // openRTS: this bogie's brake shoes (their friction under the
        // realistic air), the label saying which they are.
        if (car.definition->brakes.max_brake_force > 0.0f) {
            // The menu's bogie is the one towards the train's front or rear;
            // the shoes are kept by the car's own ends.
            const std::size_t own = menu.front != car.flipped ? 0 : 1;
            switch (car.shoes[own]) {
            case formats::BrakeShoe::composite: items.push_back({43, "Brake shoes: composite (switch to wood)"}); break;
            case formats::BrakeShoe::wood: items.push_back({43, "Brake shoes: wood (switch to cast iron)"}); break;
            case formats::BrakeShoe::disc: items.push_back({43, "Brake shoes: disc brakes (switch to cast iron)"}); break;
            default: items.push_back({43, "Brake shoes: cast iron (switch to composite)"}); break;
            }
            const auto& shoe = car.shoe_state[own];
            if (shoe.wear >= 0.3f || shoe.burning) items.push_back({44, shoe.wear >= 1.0f ? "Brake shoes burnt away (fit new ones)" : "Brake shoes worn or alight (fit new ones)"});
            if (car.flat[own] >= 0.1f) items.push_back({47, "Wheel flat on this bogie (change the wheel sets)"});
            if (std::any_of(cars.begin(), cars.end(), [](const simulation::CarState& c) { return (std::max)(c.flat[0], c.flat[1]) >= 0.1f; })) {
                items.push_back({48, "Change the wheel sets with flats on the whole train"});
            }
            // The whole train at once: new shoes, or the same kind as here.
            items.push_back({45, "New brake shoes on the whole train"});
            switch (car.shoes[own]) {
            case formats::BrakeShoe::composite: items.push_back({46, "Composite shoes on the whole train (as here)"}); break;
            case formats::BrakeShoe::wood: items.push_back({46, "Wooden shoes on the whole train (as here)"}); break;
            case formats::BrakeShoe::disc: items.push_back({46, "Disc brakes on the whole train (as here)"}); break;
            default: items.push_back({46, "Cast-iron shoes on the whole train (as here)"}); break;
            }
        }
        // openRTS: the distributor's switches, with the realistic air (RTS's
        // triple valve has none). The label says the position it is in.
        if ((car.definition->brakes.system & 5)) {
            const auto& modes = car.distributor;
            if (simulation::distributor_kind(*car.definition) == simulation::DistributorKind::vr483) {
                switch (simulation::effective_load(modes.load, car.definition->mass)) {
                case simulation::DistributorLoad::empty: items.push_back({40, "Distributor: empty mode (switch to medium)"}); break;
                case simulation::DistributorLoad::medium: items.push_back({40, "Distributor: medium mode (switch to loaded)"}); break;
                default: items.push_back({40, "Distributor: loaded mode (switch to empty)"}); break;
                }
                items.push_back({41, modes.mountain ? "Distributor: mountain mode (switch to flat)" : "Distributor: flat mode (switch to mountain)"});
            } else {
                switch (modes.plug) {
                case simulation::DistributorPlug::short_train: items.push_back({42, "Distributor plug: К, short train (switch to Д)"}); break;
                case simulation::DistributorPlug::long_train: items.push_back({42, "Distributor plug: Д, long train (switch to УВ)"}); break;
                default: items.push_back({42, "Distributor plug: УВ, accelerator off (switch to К)"}); break;
                }
            }
            // The distributor itself: RTS content cannot tell a multiple
            // unit's car (305-001) from a coach (305-000), nor a freight
            // locomotive's 483 from an EP one's 305. Chosen here for the
            // car's type and kept in a mod (docs/train.md).
            const bool chosen = car.definition->brakes.distributor_in_companion;
            switch (simulation::distributor_kind(*car.definition)) {
            case simulation::DistributorKind::vr483:
                items.push_back({49, chosen ? "Distributor: 483, chosen in a mod (switch to 292)"
                                            : "Distributor: 483, by the car's type (switch to 292, kept in a mod)"});
                break;
            case simulation::DistributorKind::vr292:
                items.push_back({49, chosen ? "Distributor: 292, chosen in a mod (switch to EVR 305-000)"
                                            : "Distributor: 292, by the car's type (switch to EVR 305-000, kept in a mod)"});
                break;
            case simulation::DistributorKind::evr305:
                items.push_back({49, chosen ? "Distributor: EVR 305-000 with a 292, chosen in a mod (switch to 305-001, multiple units)"
                                            : "Distributor: EVR 305-000 with a 292, by the car's type (switch to 305-001, multiple units, kept in a mod)"});
                break;
            default:
                items.push_back({49, chosen ? "Distributor: EVR 305-001 with a 292 (multiple units), chosen in a mod (switch to 483)"
                                            : "Distributor: EVR 305-001 with a 292 (multiple units), by the car's type (switch to 483, kept in a mod)"});
                break;
            }
            // The same on every car of the train (the locomotives keep theirs).
            if (car.definition->kind != formats::WagonKind::engine) {
                switch (simulation::distributor_kind(*car.definition)) {
                case simulation::DistributorKind::vr483: items.push_back({50, "Distributor 483 on every car of the train (as here, kept in a mod)"}); break;
                case simulation::DistributorKind::vr292: items.push_back({50, "Distributor 292 on every car of the train (as here, kept in a mod)"}); break;
                case simulation::DistributorKind::evr305:
                    items.push_back({50, "EVR 305-000 on every car of the train (as here, kept in a mod)"});
                    break;
                default: items.push_back({50, "EVR 305-001 on every car of the train (as here, kept in a mod)"}); break;
                }
            }
        }
        // RTS: a freight car with cargo (FreightAnim with a mass or a cargo type).
        if (menu.car < unit->cars.size() && car.definition->kind == formats::WagonKind::freight) {
            const auto& freight = unit->cars[menu.car].freight;
            if (std::any_of(freight.begin(), freight.end(), [](const formats::ConsistFreight& f) { return f.mass > 0.0f || !f.cargo_type.empty(); }))
                items.push_back({8, "Cargo information"});
        }
        return items;
    }
    // Coupling (0x462FC8 type 1): the car ahead of the joint and the one behind.
    const simulation::CarState* ahead = nullptr;
    const simulation::CarState* behind = nullptr;
    if (menu.front) {
        behind = &car;
        if (menu.car > 0) ahead = &cars[menu.car - 1];
    } else {
        ahead = &car;
        if (menu.car + 1 < cars.size()) behind = &cars[menu.car + 1];
    }
    const auto coupler = [](const simulation::CarState& c, const bool train_front) -> const formats::Coupling& {
        return train_front ? (c.flipped ? c.definition->couplings[0] : c.definition->couplings[1])
                           : (c.flipped ? c.definition->couplings[1] : c.definition->couplings[0]);
    };
    if (ahead && behind) {
        // Bar couplers at both ends stay together.
        if (coupler(*ahead, false).type != 1 || coupler(*behind, true).type != 1) items.push_back({1, "Uncouple"});
        if ((ahead->pipe_ends & simulation::pipe_cock_rear) && (behind->pipe_ends & simulation::pipe_cock_front)) {
            items.push_back({9, "Open end valves"});
        } else {
            items.push_back({8, "Close end valves"});
        }
        if ((ahead->pipe_ends & simulation::pipe_hose_rear) || (behind->pipe_ends & simulation::pipe_hose_front)) {
            items.push_back({3, "Connect train pipe"});
        } else {
            items.push_back({2, "Disconnect train pipe"});
        }
        if ((ahead->definition->brakes.system & 4) && (behind->definition->brakes.system & 4)) {
            if ((ahead->pipe_ends & simulation::charging_hose_rear) || (behind->pipe_ends & simulation::charging_hose_front)) {
                items.push_back({5, "Connect charging pipe"});
            } else {
                items.push_back({4, "Disconnect charging pipe"});
            }
        }
        if ((ahead->definition->brakes.system & 2) && (behind->definition->brakes.system & 2)) {
            if ((ahead->pipe_ends & simulation::ep_cable_rear) || (behind->pipe_ends & simulation::ep_cable_front)) {
                items.push_back({7, "Connect EP cable"});
            } else {
                items.push_back({6, "Disconnect EP cable"});
            }
        }
        // The multiple-unit cables (0x462FC8): where the car ahead has one at
        // its rear and the car behind at its front (SMECable / HVBus of the .eng).
        const auto end_has = [](const simulation::CarState& c, const bool train_front, const bool hv) {
            if (!c.definition->engine) return false;
            const bool own_front = train_front != c.flipped;
            return (hv ? c.definition->engine->hv_bus : c.definition->engine->sme_cable)[own_front ? 0 : 1] != 0;
        };
        if (end_has(*ahead, false, false) && end_has(*behind, true, false)) {
            if ((ahead->cables & simulation::cable_sme_rear) && (behind->cables & simulation::cable_sme_front)) {
                items.push_back({11, "Disconnect SME-cable"});
            } else {
                items.push_back({10, "Connect SME-cable"});
            }
        }
        if (end_has(*ahead, false, true) && end_has(*behind, true, true)) {
            if ((ahead->cables & simulation::cable_hv_rear) && (behind->cables & simulation::cable_hv_front)) {
                items.push_back({13, "Disconnect high-voltage bus"});
            } else {
                items.push_back({12, "Connect high-voltage bus"});
            }
        }
        return items;
    }
    // A free end: couple to a train standing at it, its end cock, its coupler held open.
    const bool front = menu.front;
    {
        std::vector<simulation::Train*> trains;
        for (const auto& u : units_) trains.push_back(u.train.get());
        if (const auto contact = simulation::end_contact(*unit->train, front, trains, 5.0)) {
            const auto& other_cars = contact->other->cars();
            const auto& other = contact->other_front ? other_cars.front() : other_cars.back();
            const auto& mine = coupler(car, front);
            const auto& theirs = coupler(other, contact->other_front);
            const double reach = static_cast<double>(mine.r0_tension) + static_cast<double>(theirs.r0_tension);
            const bool standing = std::abs(car.velocity) < 0.01f && std::abs(other.velocity) < 0.01f;
            if (contact->gap < reach && standing) {
                const bool held_open = (car.coupler_open & (front ? simulation::coupler_open_front : simulation::coupler_open_rear))
                    || (other.coupler_open & (contact->other_front ? simulation::coupler_open_front : simulation::coupler_open_rear));
                const bool can = (mine.type != 0 || theirs.type != 0) ? mine.type == theirs.type : held_open && contact->gap <= 0.75 * reach;
                if (can) items.push_back({16, "Couple"});
            }
        }
    }
    if (car.pipe_ends & (front ? simulation::pipe_cock_front : simulation::pipe_cock_rear)) {
        items.push_back({9, "Open end valve"});
    } else {
        items.push_back({8, "Close end valve"});
    }
    if (car.coupler_open & (front ? simulation::coupler_open_front : simulation::coupler_open_rear)) {
        items.push_back({14, "Unlock coupling"});
    } else {
        items.push_back({15, "Lock coupling opened"});
    }
    // A coupler broken in a collision (RTS +14 0x10 / 0x20): fixed in 120 s.
    if (car.coupler_open & (front ? simulation::coupler_broken_front : simulation::coupler_broken_rear)) {
        const bool fixing = repair_ && repair_->train == menu.train && repair_->car == menu.car;
        items.push_back({17, fixing ? "Fixing the coupling..." : "Fix coupling"});
    }
    return items;
}

void TrainView::update_car_menu(const glm::vec3& eye) {
    if (!car_menu_) return;
    const auto menu = *car_menu_;
    const auto* unit = find_unit(menu.train);
    const bool gone = !unit || menu.car >= unit->train->cars().size() || menu.car >= unit->cars.size();
    // RTS closes it when the camera has gone more than 5 m past the car's half length.
    bool far = false;
    if (!gone) {
        if (const auto body = body_pose(*unit, menu.car, false)) {
            const float length = unit->cars[menu.car].definition ? unit->cars[menu.car].definition->length : 20.0f;
            far = glm::distance(glm::vec3{body->transform[3]}, eye) > length * 0.5f + 5.0f;
        }
    }
    if (gone || (!menu.remote && (far || viewing_inside()))) {
        car_menu_.reset();
        car_menu_choice_.reset();
        return;
    }
    if (const auto choice = std::exchange(car_menu_choice_, std::nullopt)) {
        // Only what the menu offers now (the state may have changed since it was drawn).
        const auto items = car_menu_items(menu);
        if (std::none_of(items.begin(), items.end(), [&](const CarMenuItem& item) { return item.id == *choice; })) return;
        car_menu_.reset();
        do_car_menu_item(menu, *choice);
    }
}

formats::WagonRules TrainView::stock_rules() const {
    if (!stock_rules_ && vfs_) {
        std::string messages;
        stock_rules_ = simulation::StockRules::load(*vfs_, messages);
        if (!messages.empty()) std::fprintf(stderr, "%s", messages.c_str());
        if (stock_rules_->scripts() > 0) std::fprintf(stderr, "stock rules: %zu scripts\n", stock_rules_->scripts());
    }
    if (!stock_rules_) return {};
    if (const auto messages = stock_rules_->take_messages(); !messages.empty()) std::fprintf(stderr, "%s", messages.c_str());
    return stock_rules_->hook();
}

void TrainView::choose_distributor(const Car& chosen, const formats::BrakeDistributor distributor) {
    if (install_root_.empty()) {
        message_ = tr("no installation to keep the mod in");
        return;
    }
    bool listed = false;
    std::string error;
    const auto changed = keep_distributor(chosen, distributor, listed, error);
    if (!error.empty()) {
        message_ = error;
        return;
    }
    const char* name = distributor == formats::BrakeDistributor::inferred ? tr("as its type gives") : formats::brake_distributor_name(distributor);
    message_ = trf("%s: distributor %s on %zu cars, kept in MODS_OPENRTS/%s", chosen.file.c_str(), name, changed, edits_mod().c_str());
    if (listed) message_ += std::string(" ") + tr("(the mod was added to openrts.ini)");
}

void TrainView::choose_distributor_for_train(const Unit& unit, const Car& chosen) {
    if (install_root_.empty() || !chosen.definition) {
        message_ = tr("no installation to keep the mod in");
        return;
    }
    // Every car type of the train but the locomotives' (they keep their own,
    // chosen on each): this car's distributor, as its own type gives it or
    // in the mod's companion (testers, 2026-11-17: as the shoes are).
    const auto kind = simulation::distributor_kind(*chosen.definition);
    std::vector<const Car*> types;
    for (const auto& car : unit.cars) {
        if (!car.definition || car.definition->kind == formats::WagonKind::engine || !(car.definition->brakes.system & 5)) continue;
        const bool seen = std::any_of(types.begin(), types.end(), [&](const Car* t) {
            return t->engine == car.engine && core::fold_case(t->folder) == core::fold_case(car.folder) &&
                   core::fold_case(t->file) == core::fold_case(car.file);
        });
        if (!seen) types.push_back(&car);
    }
    std::size_t changed = 0;
    bool listed = false;
    for (const auto* type : types) {
        using K = simulation::DistributorKind;
        const auto choice = kind == simulation::default_distributor_kind(*type->definition) ? formats::BrakeDistributor::inferred
                            : kind == K::vr483                                               ? formats::BrakeDistributor::vr483
                            : kind == K::vr292                                               ? formats::BrakeDistributor::vr292
                            : kind == K::evr305                                              ? formats::BrakeDistributor::evr305
                                                                                             : formats::BrakeDistributor::evr305_001;
        std::string error;
        bool added = false;
        changed += keep_distributor(*type, choice, added, error);
        listed = listed || added;
        if (!error.empty()) {
            message_ = error;
            return;
        }
    }
    const auto name = kind == simulation::DistributorKind::vr483    ? formats::BrakeDistributor::vr483
                      : kind == simulation::DistributorKind::vr292  ? formats::BrakeDistributor::vr292
                      : kind == simulation::DistributorKind::evr305 ? formats::BrakeDistributor::evr305
                                                                    : formats::BrakeDistributor::evr305_001;
    message_ = trf("distributor %s on %zu cars of %zu types, kept in MODS_OPENRTS/%s", formats::brake_distributor_name(name), changed, types.size(),
                   edits_mod().c_str());
    if (listed) message_ += std::string(" ") + tr("(the mod was added to openrts.ini)");
}

std::size_t TrainView::keep_distributor(const Car& chosen, const formats::BrakeDistributor distributor, bool& listed, std::string& error) {
    // The edits mod's companion of the car's file, its other entries kept.
    const auto virtual_file = formats::wagon_override_path(chosen.folder, chosen.file, chosen.engine).str();
    const auto mod = edits_mod();
    const auto file = mod_file(install_root_, mod, virtual_file);
    std::string old;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            old.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        } else if (const auto path = core::VirtualPath::parse(virtual_file); path && vfs_) {
            // Not in the mod yet: from the one in effect (another mod's, a
            // pack's), so its other lines stay.
            if (const auto text = vfs_->read(*path)) old = *text;
        }
    }
    const auto text = formats::set_override_distributor(old, distributor);
    std::error_code io_error;
    if (text.empty()) {
        std::filesystem::remove(file, io_error);
    } else {
        std::filesystem::create_directories(file.parent_path(), io_error);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            error = trf("could not write %s", utf8_path(file).c_str());
            return 0;
        }
    }
    listed = list_mod(install_root_, mod);
    remember("inspector_mod", mod);
    // Every car of this type, in every train, at once (a mod listed now is
    // mounted from the next start; its file is read then).
    const auto same = [&](const Car& car) {
        return car.engine == chosen.engine && core::fold_case(car.folder) == core::fold_case(chosen.folder) &&
               core::fold_case(car.file) == core::fold_case(chosen.file);
    };
    std::size_t changed = 0;
    for (auto& unit : units_) {
        for (const auto& car : unit.cars) {
            if (!car.definition || !same(car)) continue;
            for (auto& owned : stock_) {
                if (owned.get() != car.definition) continue;
                // Back to its type's: the file's, the stock rules' or inferred.
                owned->brakes.distributor = distributor == formats::BrakeDistributor::inferred ? owned->brakes.default_distributor : distributor;
                owned->brakes.distributor_in_companion = distributor != formats::BrakeDistributor::inferred;
                ++changed;
                break;
            }
        }
    }
    return changed;
}

void TrainView::do_car_menu_item(const CarMenu& menu, const int id) {
    using Flags = simulation::Train::CarFlags;
    auto* unit = find_unit(menu.train);
    if (!unit || menu.car >= unit->train->cars().size()) return;
    auto& train = *unit->train;
    const auto unit_index = static_cast<std::size_t>(unit - units_.data());
    // RTS's sound triggers for the hands' work (0x463F84: 80 air escaping,
    // 81 it stops, 82 / 83 a hose parted / joined, 84 / 85 the EP cable,
    // 86 / 87 an end cock shut / opened, 59 / 62 the coupler freed / held
    // open, 92 / 93 the release valve opened / shut).
    const auto trigger = [&](const std::size_t car, const std::uint32_t sound) {
        if (car < unit->cars.size()) unit->cars[car].pending_triggers.push_back(sound);
    };
    const auto& cars = train.cars();
    if (!menu.coupling) {
        const auto i = menu.car;
        switch (id) {
        case 1: train.set_car_flags(i, Flags::chocks, simulation::chock_front, false); break;
        case 2: train.set_car_flags(i, Flags::chocks, simulation::chock_front, true); break;
        case 3: train.set_car_flags(i, Flags::chocks, simulation::chock_rear, false); break;
        case 4: train.set_car_flags(i, Flags::chocks, simulation::chock_rear, true); break;
        case 5: train.set_handbrake(i, 0.0f); break;
        case 6: train.set_handbrake(i, 1.0f); break;
        case 7: train.set_car_flags(i, Flags::valves, simulation::valve_emergency, false); break;
        case 8: { // Cargo information (0x502680 -> 0x50BC20: each cargo's mass, then the car's)
            if (i >= unit->cars.size()) break;
            const auto& c = unit->cars[i];
            std::string text = trf("Car %zu (%s):", i + 1, c.definition->name.c_str());
            for (const auto& f : c.freight) {
                if (f.mass <= 0.0f && f.cargo_type.empty()) continue;
                const std::string what = f.cargo_type.empty() ? tr("cargo") : f.cargo_type;
                text += "\n" + (f.volume > 0.0f ? trf("%s: %.1f t, %.1f m3", what.c_str(), f.mass * 0.001f, f.volume)
                                                 : trf("%s: %.1f t", what.c_str(), f.mass * 0.001f));
            }
            text += "\n" + trf("Total: %.1f t", c.definition->mass * 0.001f);
            const auto& state = train.cars()[i];
            if (!state.cargo_id.empty() || state.cargo_mass > 0.0f) {
                float capacity = 0.0f;
                if (cargo_ && cargo_->ready() && state.definition) {
                    if (const auto* wagon = cargo_->catalog().wagon_for(state.definition->file.filename().string())) {
                        if (const auto* type = cargo_->catalog().cargo(state.cargo_id)) {
                            capacity = formats::cargo_capacity(*wagon, *type, state.definition->width, state.definition->height,
                                                               state.definition->length);
                        }
                    }
                }
                const std::string what = state.cargo_id.empty() ? tr("cargo") : state.cargo_id;
                text += "\n" + (capacity > 0.0f
                                    ? trf("%s: %.1f t of %.1f t", what.c_str(), state.cargo_mass * 0.001f, capacity * 0.001f)
                                    : trf("%s: %.1f t", what.c_str(), state.cargo_mass * 0.001f));
                text += "\n" + trf("With cargo: %.1f t", (c.definition->mass + state.cargo_mass) * 0.001f);
            }
            cargo_text_ = std::move(text);
            cargo_time_ = 30.0f;
            break;
        }
        case 11:
            train.set_car_flags(i, Flags::valves, simulation::valve_release, true);
            trigger(i, 92);
            break;
        case 12:
            train.set_car_flags(i, Flags::valves, simulation::valve_release, false);
            trigger(i, 93);
            break;
        case 43: { // this bogie's brake shoes: cast iron -> composite -> wood -> cast iron
            auto shoes = cars[i].shoes;
            auto& shoe = shoes[menu.front != cars[i].flipped ? 0 : 1];
            shoe = shoe == formats::BrakeShoe::cast_iron   ? formats::BrakeShoe::composite
                   : shoe == formats::BrakeShoe::composite ? formats::BrakeShoe::wood
                                                           : formats::BrakeShoe::cast_iron;
            train.set_brake_shoes(i, shoes);
            message_ = trf(shoe == formats::BrakeShoe::composite ? "car %zu: composite brake shoes on this bogie"
                           : shoe == formats::BrakeShoe::wood    ? "car %zu: wooden brake shoes on this bogie"
                                                                 : "car %zu: cast-iron brake shoes on this bogie",
                           i + 1);
            break;
        }
        case 44: // new shoes of the same kind on this bogie
            train.replace_brake_shoes(i, menu.front != cars[i].flipped ? 0 : 1);
            message_ = trf("car %zu: new brake shoes on this bogie", i + 1);
            break;
        case 47: // this bogie's wheel sets changed: no flat
            train.replace_wheelsets(i, menu.front != cars[i].flipped ? 0 : 1);
            message_ = trf("car %zu: new wheel sets under this bogie", i + 1);
            break;
        case 48: { // every bogie's with a flat
            std::size_t changed = 0;
            for (std::size_t k = 0; k < cars.size(); ++k) {
                for (std::size_t b = 0; b < 2; ++b) {
                    if (cars[k].flat[b] < 0.1f) continue;
                    train.replace_wheelsets(k, b);
                    ++changed;
                }
            }
            message_ = trf("new wheel sets under %zu bogies", changed);
            break;
        }
        case 45: { // new shoes of their own kind on every braked car of the train
            std::size_t fitted = 0;
            for (std::size_t k = 0; k < cars.size(); ++k) {
                if (!cars[k].definition || cars[k].definition->brakes.max_brake_force <= 0.0f) continue;
                train.replace_brake_shoes(k, 0);
                train.replace_brake_shoes(k, 1);
                ++fitted;
            }
            message_ = trf("new brake shoes on %zu cars", fitted);
            break;
        }
        case 46: { // this bogie's kind of shoes on every braked car of the train
            const auto kind = cars[i].shoes[menu.front != cars[i].flipped ? 0 : 1];
            std::size_t fitted = 0;
            for (std::size_t k = 0; k < cars.size(); ++k) {
                if (!cars[k].definition || cars[k].definition->brakes.max_brake_force <= 0.0f) continue;
                train.set_brake_shoes(k, {kind, kind});
                ++fitted;
            }
            message_ = trf(kind == formats::BrakeShoe::composite ? "composite brake shoes on %zu cars"
                           : kind == formats::BrakeShoe::wood    ? "wooden brake shoes on %zu cars"
                           : kind == formats::BrakeShoe::disc    ? "disc brakes on %zu cars"
                                                                 : "cast-iron brake shoes on %zu cars",
                           fitted);
            break;
        }
        case 40: { // the 483's load mode: empty -> medium -> loaded -> empty
            auto modes = cars[i].distributor;
            const auto load = simulation::effective_load(modes.load, cars[i].definition->mass);
            modes.load = load == simulation::DistributorLoad::empty    ? simulation::DistributorLoad::medium
                         : load == simulation::DistributorLoad::medium ? simulation::DistributorLoad::loaded
                                                                       : simulation::DistributorLoad::empty;
            train.set_distributor_modes(i, modes);
            message_ = trf(modes.load == simulation::DistributorLoad::empty    ? "car %zu: distributor in empty mode"
                           : modes.load == simulation::DistributorLoad::medium ? "car %zu: distributor in medium mode"
                                                                               : "car %zu: distributor in loaded mode",
                           i + 1);
            break;
        }
        case 41: { // the 483: flat <-> mountain
            auto modes = cars[i].distributor;
            modes.mountain = !modes.mountain;
            train.set_distributor_modes(i, modes);
            message_ = trf(modes.mountain ? "car %zu: distributor in mountain mode" : "car %zu: distributor in flat mode", i + 1);
            break;
        }
        case 49: { // the distributor of the car's type: 483 -> 292 -> 305-000 -> 305-001 -> 483
            if (i >= unit->cars.size()) break;
            using K = simulation::DistributorKind;
            const auto& definition = *cars[i].definition;
            const auto now = simulation::distributor_kind(definition);
            const auto next = now == K::vr483 ? K::vr292 : now == K::vr292 ? K::evr305 : now == K::evr305 ? K::evr305_001 : K::vr483;
            // The one its type gives (its file, the stock rules) needs no mod's word.
            const auto choice = next == simulation::default_distributor_kind(definition) ? formats::BrakeDistributor::inferred
                                : next == K::vr483                                         ? formats::BrakeDistributor::vr483
                                : next == K::vr292                                         ? formats::BrakeDistributor::vr292
                                : next == K::evr305                                        ? formats::BrakeDistributor::evr305
                                                                                           : formats::BrakeDistributor::evr305_001;
            choose_distributor(unit->cars[i], choice);
            break;
        }
        case 50: { // this car's distributor on every car of the train (not the locomotives)
            if (i < unit->cars.size()) choose_distributor_for_train(*unit, unit->cars[i]);
            break;
        }
        case 42: { // the 292's plug: К -> Д -> УВ -> К
            auto modes = cars[i].distributor;
            modes.plug = modes.plug == simulation::DistributorPlug::short_train ? simulation::DistributorPlug::long_train
                         : modes.plug == simulation::DistributorPlug::long_train ? simulation::DistributorPlug::no_accelerator
                                                                                 : simulation::DistributorPlug::short_train;
            train.set_distributor_modes(i, modes);
            message_ = trf(modes.plug == simulation::DistributorPlug::short_train  ? "car %zu: distributor plug at К (short train)"
                           : modes.plug == simulation::DistributorPlug::long_train ? "car %zu: distributor plug at Д (long train)"
                                                                                   : "car %zu: distributor plug at УВ (accelerator off)",
                           i + 1);
            break;
        }
        default: break;
        }
        return;
    }
    // The joint: the car ahead of it (its rear end) and the one behind (its front end).
    std::optional<std::size_t> ahead, behind;
    if (menu.front) {
        behind = menu.car;
        if (menu.car > 0) ahead = menu.car - 1;
    } else {
        ahead = menu.car;
        if (menu.car + 1 < cars.size()) behind = menu.car + 1;
    }
    const auto both = ahead && behind;
    const auto ends = [&](const std::uint8_t rear_bit, const std::uint8_t front_bit, const bool on) {
        if (ahead) train.set_car_flags(*ahead, Flags::pipe_ends, rear_bit, on);
        if (behind) train.set_car_flags(*behind, Flags::pipe_ends, front_bit, on);
    };
    switch (id) {
    case 1: // Uncouple (0x469858 with the cocks as they are and the couplers not held open)
        if (!both) break;
        if (std::abs(train.speed()) > 0.1) {
            message_ = tr("stop the train to uncouple");
            break;
        }
        split(unit_index, *behind, false, false);
        message_ = trf("uncoupled before car %zu", static_cast<std::size_t>(*behind + 1));
        break;
    case 2: { // Disconnect train pipe: air escapes where a cock is open
        if (!both) break;
        const bool ahead_open = !(cars[*ahead].pipe_ends & simulation::pipe_cock_rear);
        const bool behind_open = !(cars[*behind].pipe_ends & simulation::pipe_cock_front);
        ends(simulation::pipe_hose_rear, simulation::pipe_hose_front, true);
        trigger(*ahead, 82);
        trigger(*behind, 82);
        if (ahead_open) trigger(*ahead, 80);
        if (behind_open) trigger(*behind, 80);
        break;
    }
    case 3:
        if (!both) break;
        ends(simulation::pipe_hose_rear, simulation::pipe_hose_front, false);
        for (const auto c : {*ahead, *behind}) {
            trigger(c, 83);
            trigger(c, 81);
        }
        break;
    case 4:
        if (!both) break;
        ends(simulation::charging_hose_rear, simulation::charging_hose_front, true);
        trigger(*ahead, 82);
        trigger(*behind, 82);
        break;
    case 5:
        if (!both) break;
        ends(simulation::charging_hose_rear, simulation::charging_hose_front, false);
        trigger(*ahead, 83);
        trigger(*behind, 83);
        break;
    case 6:
        if (!both) break;
        ends(simulation::ep_cable_rear, simulation::ep_cable_front, true);
        trigger(*ahead, 84);
        trigger(*behind, 84);
        break;
    case 7:
        if (!both) break;
        ends(simulation::ep_cable_rear, simulation::ep_cable_front, false);
        trigger(*ahead, 85);
        trigger(*behind, 85);
        break;
    case 8: // Close end valve(s)
        ends(simulation::pipe_cock_rear, simulation::pipe_cock_front, true);
        for (const auto& c : {ahead, behind}) {
            if (!c) continue;
            trigger(*c, 86);
            trigger(*c, 81);
        }
        break;
    case 9: { // Open end valve(s): air escapes from a parted hose or a free end
        const bool ahead_free = ahead && (!behind || (cars[*ahead].pipe_ends & simulation::pipe_hose_rear));
        const bool behind_free = behind && (!ahead || (cars[*behind].pipe_ends & simulation::pipe_hose_front));
        ends(simulation::pipe_cock_rear, simulation::pipe_cock_front, false);
        if (ahead) {
            trigger(*ahead, 87);
            if (ahead_free) trigger(*ahead, 80);
        }
        if (behind) {
            trigger(*behind, 87);
            if (behind_free) trigger(*behind, 80);
        }
        break;
    }
    case 10: // Connect SME-cable (89), then the multiple-unit groups again
    case 11: // Disconnect SME-cable (88)
    case 12: // Connect high-voltage bus (91)
    case 13: { // Disconnect high-voltage bus (90)
        if (!both) break;
        const bool hv = id >= 12;
        const bool connect = id == 10 || id == 12;
        train.set_car_flags(*ahead, Flags::cables, hv ? simulation::cable_hv_rear : simulation::cable_sme_rear, connect);
        train.set_car_flags(*behind, Flags::cables, hv ? simulation::cable_hv_front : simulation::cable_sme_front, connect);
        const std::uint32_t sound = hv ? (connect ? 91u : 90u) : (connect ? 89u : 88u);
        trigger(*ahead, sound);
        trigger(*behind, sound);
        message_ = connect ? (hv ? tr("high-voltage bus connected") : tr("SME cable connected"))
                           : (hv ? tr("high-voltage bus disconnected") : tr("SME cable disconnected"));
        break;
    }
    case 17: // Fix coupling (0x463F84: 120 s, one at a time)
        if (both) break;
        if (repair_) {
            message_ = repair_->train == menu.train && repair_->car == menu.car ? tr("the coupling is being fixed")
                                                                                : tr("another coupling is being fixed");
            break;
        }
        repair_ = Repair{.train = menu.train, .car = menu.car, .front = menu.front, .left = 120.0f};
        message_ = tr("fixing the coupling: 2 minutes");
        break;
    case 14: // Unlock coupling: ready to couple again
    case 15: { // Lock coupling opened: it only pushes
        const bool open = id == 15;
        if (ahead) {
            train.set_car_flags(*ahead, Flags::coupler_open, simulation::coupler_open_rear, open);
            trigger(*ahead, open ? 62 : 59);
        }
        if (behind) {
            train.set_car_flags(*behind, Flags::coupler_open, simulation::coupler_open_front, open);
            trigger(*behind, open ? 62 : 59);
        }
        break;
    }
    case 16: // Couple to the train standing at this free end (0x469ED8)
        if (both) break;
        couple_standing(unit_index, menu.car, menu.front);
        break;
    default: break;
    }
}

bool TrainView::couple_standing(const std::size_t unit_index, const std::size_t car, const bool front) {
    if (unit_index >= units_.size()) return false;
    using Flags = simulation::Train::CarFlags;
    auto& train = *units_[unit_index].train;
    std::vector<simulation::Train*> trains;
    for (const auto& u : units_) trains.push_back(u.train.get());
    const auto contact = simulation::end_contact(train, front, trains, 5.0);
    if (!contact) {
        message_ = tr("no train stands at this end");
        return false;
    }
    auto* other = contact->other;
    if (std::abs(train.speed()) > 0.01 || std::abs(other->speed()) > 0.01) {
        message_ = tr("both trains must stand to couple");
        return false;
    }
    // The couplers at the joint are no longer held open.
    train.set_car_flags(car, Flags::coupler_open, front ? simulation::coupler_open_front : simulation::coupler_open_rear, false);
    const auto other_car = contact->other_front ? std::size_t{0} : other->cars().size() - 1;
    other->set_car_flags(other_car, Flags::coupler_open,
                         contact->other_front ? simulation::coupler_open_front : simulation::coupler_open_rear, false);
    // The earlier unit survives (the driven train keeps its place).
    const auto other_unit = static_cast<std::size_t>(
        std::find_if(units_.begin(), units_.end(), [&](const Unit& u) { return u.train.get() == other; }) - units_.begin());
    if (other_unit < units_.size() && units_[other_unit].remote && units_[unit_index].remote) return false;
    // Ours takes another client's train in (RTS: its own train always
    // takes the other); else the earlier unit survives.
    const bool keep_this = other_unit < units_.size() && units_[other_unit].remote ? true
                         : units_[unit_index].remote                             ? false
                                                                                 : unit_index < other_unit;
    auto* survivor = keep_this ? &train : other;
    auto* absorbed = keep_this ? other : &train;
    const bool survivor_front = keep_this ? front : contact->other_front;
    const bool absorbed_front = keep_this ? contact->other_front : front;
    if (!survivor->couple(*absorbed, survivor_front, absorbed_front, (std::max)(contact->gap, 0.0))) return false;
    const auto merged = merge_units(survivor, absorbed, survivor_front, absorbed_front == survivor_front);
    if (merged >= units_.size()) return false;
    message_ = trf("coupled: %zu cars; the hoses and cocks by hand (the car menu at the joint)", units_[merged].cars.size());
    return true;
}

void TrainView::draw_car_menu() {
#ifdef OPENRTS_HAS_IMGUI
    if (!car_menu_) return;
    const auto items = car_menu_items(*car_menu_);
    if (items.empty()) {
        car_menu_.reset();
        return;
    }
    auto& menu = *car_menu_;
    const int count = static_cast<int>(items.size());
    menu.highlight = std::clamp(menu.highlight, 0, count - 1);
    // The keys as RTS reads them while its menu is up: the arrows move,
    // Enter picks, Esc closes (the menu key too, in update()).
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) menu.highlight = (menu.highlight + count - 1) % count;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) menu.highlight = (menu.highlight + 1) % count;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        car_menu_choice_ = items[static_cast<std::size_t>(menu.highlight)].id;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        car_menu_.reset();
        car_menu_choice_.reset();
        return;
    }
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2{display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, ImVec2{0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.9f);
    const char* title = menu.coupling ? tr("Coupling") : tr("Bogie");
    if (ImGui::Begin((std::string(title) + "###car_menu").c_str(), nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize
                         | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        const auto* unit = find_unit(menu.train);
        if (unit) {
            const auto n = unit->train->cars().size();
            const auto number = menu.car + 1;
            if (menu.coupling) {
                if (menu.front && menu.car > 0) {
                    ImGui::TextDisabled("%s", trf("between cars %zu and %zu", number - 1, number).c_str());
                } else if (!menu.front && menu.car + 1 < n) {
                    ImGui::TextDisabled("%s", trf("between cars %zu and %zu", number, number + 1).c_str());
                } else {
                    ImGui::TextDisabled("%s", trf(menu.front ? "car %zu, front end" : "car %zu, rear end", number).c_str());
                }
            } else {
                ImGui::TextDisabled("%s", trf(menu.front ? "car %zu, front bogie" : "car %zu, rear bogie", number).c_str());
            }
        }
        for (int k = 0; k < count; ++k) {
            const auto& item = items[static_cast<std::size_t>(k)];
            if (ImGui::Selectable(tr(item.label), k == menu.highlight)) car_menu_choice_ = item.id;
            if (ImGui::IsItemHovered()) menu.highlight = k;
        }
        ImGui::TextDisabled("%s", trf("%s or Esc: close", car_menu_key_name()).c_str());
    }
    ImGui::End();
#endif
}

void TrainView::collect_cab_labels(std::vector<WorldLabel>& labels, const glm::vec3& eye, const bool all) const {
    // The door under the mouse, in the cab or outside.
    if (door_hover_)
        labels.push_back({.position = door_hover_->at, .text = door_hover_->text, .colour = 0xff40ffffu, .occludable = false,
                          .key = std::size_t{0xD002} << 40});
    const auto* cab = engineer_cab();
    if (!viewing_inside() || !cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) return;
    if (!all && !hovered_) return;
    const auto body = body_pose(units_.front(), engineer_car_, false);
    if (!body) return;
    // Where the cab was drawn with the camera the labels are projected with.
    const glm::mat4 to_scene = cab_to_scene_ ? *cab_to_scene_ : body->transform * cab->placement();
    const auto& sd = cab->description();
    const auto* out = legacy_ ? legacy_->output(engineer_car_) : nullptr;
    constexpr float range = 2.6f;
    struct Item {
        glm::vec3 at;
        std::string text;
        float facing;
        bool control;
        std::size_t index; // in sd.switches or sd.displays
    };
    std::vector<Item> items;
    auto add = [&](const std::optional<glm::vec3>& local, std::string text, const bool control, const std::size_t index) {
        if (!local || text.empty()) return;
        const bool under_mouse = hovered_ && hovered_->control == control && hovered_->index == index;
        if (!all && !under_mouse) return;
        const glm::vec3 at = glm::vec3(to_scene * glm::vec4(*local, 1.0f));
        const glm::vec3 to = at - eye;
        const float distance = glm::length(to);
        if (!under_mouse && (distance > range || distance < 0.05f)) return;
        const float facing = distance > 1e-4f ? glm::dot(to / distance, look_) : 1.0f;
        if (!under_mouse && facing < 0.35f) return;
        items.push_back({at, std::move(text), facing, control, index});
    };
    for (std::size_t i = 0; i < sd.switches.size(); ++i) {
        const auto& s = sd.switches[i];
        std::string text = s.hint.empty() ? s.name : s.hint;
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (!s.keys.empty()) {
            text += "  [";
            for (std::size_t k = 0; k < s.keys.size(); ++k) text += (k ? " " : "") + s.keys[k];
            text += "]";
        }
        add(cab->control_label_point(i), std::move(text), true, i);
    }
    for (std::size_t i = 0; i < sd.displays.size(); ++i) {
        std::string text = sd.displays[i].hint;
        while (!text.empty() && text.back() == ' ') text.pop_back();
        add(cab->display_label_point(i), std::move(text), false, i);
    }
    // The one under the mouse, else the one looked at most directly, stands
    // out with its position.
    std::size_t focus = items.size();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (hovered_ && hovered_->control == items[i].control && hovered_->index == items[i].index) {
            focus = i;
            break;
        }
        if (!hovered_ && items[i].facing > 0.985f && (focus == items.size() || items[i].facing > items[focus].facing)) focus = i;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        std::uint32_t colour = item.control ? 0xd0a0f0ffu : 0xb0f0e0b0u; // 0xAABBGGRR
        if (i == focus) {
            colour = 0xff40ffffu;
            if (item.control && out) {
                const auto& s = sd.switches[item.index];
                if (s.id < out->switches.size()) {
                    const auto state = out->switches[s.id].set_state;
                    const bool key = (s.flags & formats::cab_key) != 0;
                    if (key && state == 0) {
                        item.text += std::string("  — ") + tr("the key is out");
                    } else {
                        const auto index = key ? state - 1 : state;
                        if (index < s.state_hints.size() && !s.state_hints[index].empty()) {
                            item.text += "  — " + s.state_hints[index];
                        } else {
                            item.text += "  " + std::to_string(index) + "/" + std::to_string(s.num_states > 0 ? s.num_states - 1 : 0);
                        }
                    }
                }
            }
        }
        const std::size_t key = (std::size_t{0xCAB} << 40) ^ (item.control ? std::size_t{1} << 32 : 0) ^ item.index;
        labels.push_back({.position = item.at, .text = std::move(item.text), .colour = colour, .occludable = false, .key = key});
    }
}

// Pantographs (RTS: AnimParts with a pantograph number, 0x5031D8): a DLL
// engine's come from Wagon::PantoRaised; an electric engine without a DLL
// raises the rear one (by the direction it runs) while its train is driven
// or runs under a role. They move over 2.5 s between the first and the last
// frame of the shape's animation.
// The signals and speed limits ahead as RTS gives them to the DLLs
// (SpeedLimit, ALSN, Cabin::Signal): the next signal the train obeys, the
// track's limit and the next post.
const TrainView::SignalUnits* TrainView::signal_units() {
    if (!signal_units_.valid()) {
        const auto* network = units_.empty() || !units_.front().train ? nullptr : units_.front().train->network();
        if (!network) return nullptr;
        signal_units_ = std::async(std::launch::async, [vfs = vfs_, route = route_, network]() {
                            auto units = std::make_shared<SignalUnits>();
                            units->database = formats::load_route_database(*vfs, route);
                            units->item_unit = formats::rts_signal_units(network->database(), units->database);
                            return std::shared_ptr<const SignalUnits>(std::move(units));
                        }).share();
    }
    if (signal_units_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    return signal_units_.get().get();
}

namespace {

[[nodiscard]] std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] std::tm local_now() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return local;
}

} // namespace

// The engine's LocomotiveSeries (RTS's ENG +386, the tape's name), else its file.
[[nodiscard]] std::string series_of(const formats::WagonDefinition* definition, const std::string& file) {
    if (definition && definition->engine && !definition->engine->series.empty()) return definition->engine->series;
    return file;
}

std::string TrainView::car_label(const Car& car) const {
    std::string name = series_of(car.definition, car.file);
    char number[32];
    std::snprintf(number, sizeof number, "[%04u-%04u-%04u]", car.number >> 18, (car.number >> 10) & 0xFFu, car.number & 0x3FFu);
    return name + number;
}

std::string TrainView::driven_name() const {
    const auto car = driven_car();
    if (car == SIZE_MAX || !units_.front().train) return {};
    const auto& driven = units_.front().cars[car];
    if (driven.definition && !driven.definition->display_name.empty()) return driven.definition->display_name;
    return driven.file;
}

std::size_t TrainView::driven_car() const {
    if (units_.empty() || units_.front().role != Role::driven) return SIZE_MAX;
    const auto& unit = units_.front();
    if (legacy_ && legacy_->player() < unit.cars.size()) return legacy_->player();
    if (cab_car_ < unit.cars.size()) return cab_car_;
    if (native_ && native_car_ < unit.cars.size()) return native_car_;
    for (std::size_t i = 0; i < unit.cars.size(); ++i)
        if (unit.cars[i].definition && unit.cars[i].definition->engine) return i;
    return SIZE_MAX;
}

std::optional<std::pair<std::size_t, std::size_t>> TrainView::tape_car() const {
    if (!tape_car_key_) return std::nullopt;
    for (std::size_t u = 0; u < units_.size(); ++u)
        for (std::size_t i = 0; i < units_[u].cars.size(); ++i)
            if (units_[u].cars[i].key == tape_car_key_) return std::pair{u, i};
    return std::nullopt;
}

void TrainView::tape_key() {
    // RTS 0x414B40: Ctrl+Num5 stops a tape rolling (on whichever engine),
    // else starts one on the engine he drives.
    if (tape_.recording()) {
        stop_tape();
        return;
    }
    const auto car = driven_car();
    if (car == SIZE_MAX || !units_.front().cars[car].definition || !units_.front().cars[car].definition->engine) {
        message_ = tr("the tape records a locomotive: drive one first");
        return;
    }
    start_tape(0, car);
}

void TrainView::tape_which() {
    // RTS 0x414C10: Num5 names the engine while a tape rolls, else nothing.
    if (!tape_.recording()) return;
    if (const auto at = tape_car()) message_ = trf("the tape records %s", car_label(units_[at->first].cars[at->second]).c_str());
}

bool TrainView::start_tape(const std::size_t u, const std::size_t i) {
    if (u >= units_.size() || i >= units_[u].cars.size()) return false;
    const auto& car = units_[u].cars[i];
    const std::string series = series_of(car.definition, car.file);
    if (!tape_.start(user_data_directory() / "RTS_TAPES", series, local_now())) {
        message_ = trf("the tape cannot be written: %s", utf8(tape_.path()).c_str());
        tape_car_key_ = 0;
        return false;
    }
    tape_car_key_ = car.key;
    tape_signal_distance_ = 0.0f;
    open_brake_log();
    message_ = trf("tape rolling: %s, file %s", car_label(car).c_str(), utf8(tape_.path().filename()).c_str());
    std::fprintf(stderr, "tape: rolling on car %zu (%s) into %s\n", i + 1, car_label(car).c_str(), utf8(tape_.path()).c_str());
    return true;
}

void TrainView::stop_tape(const char* why) {
    if (!tape_.recording()) return;
    const auto lines = tape_.lines();
    const auto name = utf8(tape_.path().filename());
    tape_.stop();
    close_brake_log();
    tape_car_key_ = 0;
    message_ = why ? trf("tape stopped (%s): %s, %zu lines", why, name.c_str(), lines)
                   : trf("tape stopped: %s, %zu lines", name.c_str(), lines);
    std::fprintf(stderr, "tape: stopped, %zu lines in %s\n", lines, name.c_str());
}

void TrainView::take_dll_tape() {
    // RTSSetIntegerG(7) (0x43939C): a DLL starts the tape on its engine
    // (again: a new file), takes it over from another, or stops it.
    if (!legacy_ || units_.empty()) return;
    const auto request = legacy_->take_tape_request();
    if (!request.set) return;
    if (request.car < 0) {
        stop_tape(tr("by the locomotive's DLL"));
        return;
    }
    const auto i = static_cast<std::size_t>(request.car);
    if (units_.front().host_train != 0 || i >= units_.front().cars.size()) return;
    const auto at = tape_car();
    if (at && at->first == 0 && at->second == i && !request.restart) return;
    start_tape(0, i);
}

void TrainView::update_tape() {
    if (!tape_.recording()) return;
    // The engine taken out of the game stops it (RTS 0x503A90).
    const auto at = tape_car();
    if (!at || !units_[at->first].train) {
        stop_tape(tr("the locomotive is gone"));
        return;
    }
    if (tape_.advance(simulation::train_step)) tape_.write(tape_record(at->first, at->second));
    log_brakes(simulation::train_step);
}

namespace {

const char* const valve_positions[] = {"I", "II", "III", "IV", "VЭ", "V", "VI"};

// The cars with columns in the brake log: all up to 16, else the first and
// the last 8.
std::vector<std::size_t> brake_log_columns(const std::size_t cars) {
    std::vector<std::size_t> out;
    for (std::size_t k = 0; k < cars; ++k)
        if (cars <= 16 || k < 8 || k + 8 >= cars) out.push_back(k);
    return out;
}

} // namespace

void TrainView::open_brake_log() {
    close_brake_log();
    // Next to the tape: ..._trainlog_rts.txt -> ..._trainlog_openrts.csv.
    auto name = tape_.path().filename().wstring();
    const std::wstring tail = L"_rts.txt";
    if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
        name.replace(name.size() - tail.size(), tail.size(), L"_openrts.csv");
    } else {
        name += L".openrts.csv";
    }
    brake_log_path_ = tape_.path().parent_path() / name;
    brake_log_ = std::make_unique<std::ofstream>(brake_log_path_, std::ios::binary);
    if (!*brake_log_) {
        std::fprintf(stderr, "brake log: cannot write %s\n", utf8(brake_log_path_).c_str());
        brake_log_.reset();
        return;
    }
    *brake_log_ << "\xEF\xBB\xBF"; // UTF-8, for Excel
    brake_log_time_ = 0.0;
    brake_log_due_ = 0.0;
    brake_log_last_ = {};
    brake_log_cars_.clear();
    std::fprintf(stderr, "brake log: %s\n", utf8(brake_log_path_).c_str());
}

void TrainView::close_brake_log() {
    brake_log_.reset();
}

void TrainView::brake_log_header() {
    auto& f = *brake_log_;
    f << "время;с_начала;событие;скорость_кмч;км;сигнал;до_сигнала_м;воздух;поезда_с_DLL;кран;кран_состояние;"
         "редуктор;КК;блокировка_367;АЛСН_вкл;огонь_АЛСН;вспом_цель;ЭПТ;цель_ЭПТ;УР;экстренное;вагонов_в_экстренном;ГР;подача_ГР;тяга_кН;тормоз_кН;контроллер;реверс;"
         "DLL_УР;DLL_темп_ТМ;DLL_ЭПТ;DLL_вспом;DLL_системы;ТЦ_мин;ТЦ_макс;рельсы;тележек_в_юзе;колодки_влажность_ср;колодки_нагрев_макс;тележек_горит";
    for (const auto k : brake_log_cars_) {
        const auto n = std::to_string(k + 1);
        f << ";в" << n << "_ТМ;в" << n << "_ЗР;в" << n << "_РК;в" << n << "_ТЦ;в" << n << "_вспом";
    }
    f << "\r\n";
}

void TrainView::log_brakes(const double dt) {
    if (!brake_log_) return;
    const auto at = tape_car();
    if (!at || !units_[at->first].train) return;
    const auto u = at->first;
    const auto i = at->second;
    const auto& train = *units_[u].train;
    const auto& cars = train.cars();
    if (i >= cars.size()) return;
    brake_log_time_ += dt;
    brake_log_due_ -= dt;
    const auto& car = cars[i];
    const auto* asked = train.demands_of(i);
    const auto* out = dll_car(u, i) ? dll_output(u, i) : nullptr;
    // The valve's position: the keyboard's, or the cab's 395 control under
    // MaDaDev's Brakes.dll (BR_395ID1); -1 when not known.
    int position = -1;
    int raw = -1;
    trainplugin::CabBrakes controls;
    if (out && out->has_engine) {
        // As the valve reads the cab's states (the ВЛ80С numbers them VI..I).
        controls = legacy_->cab_brakes(units_[u].host_train, i);
        position = controls.position;
        raw = controls.raw;
    } else if (driver_ && u == 0 && i == driven_car()) {
        position = raw = static_cast<int>(driver_->brake);
    }
    BrakeControls now;
    now.position = position;
    now.ep = asked && (asked->systems & 2u) != 0;
    now.independent = asked ? std::round(asked->independent_brake * 10.0f) / 10.0f : -1.0f;
    now.emergency = asked && asked->emergency;
    now.throttle = out && out->has_engine ? static_cast<int>(out->engine.throttle) : driver_ ? static_cast<int>(driver_->throttle) : 0;
    now.reverser = out && out->has_engine ? out->engine.reverse : driver_ ? driver_->reverser : 0;
    std::string event;
    const auto add = [&](const std::string& what) { event += (event.empty() ? "" : ", ") + what; };
    if (brake_log_last_.position != -2) {
        if (now.position != brake_log_last_.position) add(std::string("кран ") + (position >= 0 ? valve_positions[position] : "?"));
        if (now.ep != brake_log_last_.ep) add(now.ep ? "ЭПТ вкл" : "ЭПТ выкл");
        if (now.independent != brake_log_last_.independent) add("вспом " + trf("%.1f", now.independent));
        if (now.emergency != brake_log_last_.emergency) add(now.emergency ? "экстренное" : "экстренное снято");
        if (now.throttle != brake_log_last_.throttle) add("контроллер " + std::to_string(now.throttle));
        if (now.reverser != brake_log_last_.reverser) add("реверс " + std::to_string(now.reverser));
    } else {
        add("начало");
    }
    const bool changed = !(now == brake_log_last_);
    brake_log_last_ = now;
    if (!changed && brake_log_due_ > 0.0) return;
    if (brake_log_due_ <= 0.0) brake_log_due_ += 0.5;
    if (brake_log_due_ <= 0.0) brake_log_due_ = 0.5;
    // The header again when the cars changed (coupled, cut).
    if (const auto columns = brake_log_columns(cars.size()); columns != brake_log_cars_) {
        brake_log_cars_ = columns;
        brake_log_header();
    }
    auto& f = *brake_log_;
    const auto num = [](const double v, const int digits = 3) {
        char text[32];
        std::snprintf(text, sizeof text, "%.*f", digits, v);
        return std::string(text);
    };
    const auto clock = static_cast<long>(std::floor(day_time_)) % 86400;
    char time_text[16];
    std::snprintf(time_text, sizeof time_text, "%02ld:%02ld:%02ld", clock / 3600, clock / 60 % 60, clock % 60);
    f << time_text << ';' << num(brake_log_time_, 2) << ';' << event << ';' << num(train.speed() * 3.6, 2) << ';';
    if (const auto km = limits_->ordinate(train)) f << num(*km, 3);
    f << ';';
    if (signals_) {
        if (const auto next = signals_->ahead(train); next.valid) f << formats::to_string(next.aspect) << ';' << num(next.distance, 0);
        else f << ';';
    } else {
        f << ';';
    }
    f << ';' << "реалистичный" << ';';
    if (legacy_ && out) {
        const auto mode = legacy_->air();
        f << (mode == trainplugin::DllAir::own_valve ? "кран openRTS" : mode == trainplugin::DllAir::dll_valve ? "кран DLL" : "RTS");
    } else {
        f << '-';
    }
    f << ';' << (position >= 0 ? valve_positions[position] : "-") << ';' << raw << ';';
    // The reducer's charging pressure, the combined valve (усл. 114).
    if (controls.charging > 0.0f) f << num(controls.charging, 1);
    f << ';' << (out && out->has_engine ? (controls.combined == 1 ? "двойная тяга" : controls.combined == 2 ? "экстренное" : "нормальное") : "")
      << ';' << (out && out->has_engine ? (controls.blocked ? "выкл" : "вкл") : "") << ';' << (out && out->has_engine ? std::to_string(static_cast<std::int16_t>(out->engine.alsn_on)) : std::string())
      << ';' << (out ? std::to_string(out->cab_aspect) : std::string()) << ';';
    f << (asked ? num(asked->independent_brake) : "") << ';' << (now.ep ? 1 : 0) << ';' << (asked ? num(asked->ep_value) : "") << ';';
    if (const auto ur = equalising_of(u, i)) f << num(*ur);
    f << ';' << (now.emergency ? 1 : 0) << ';' << train.emergency_cars() << ';' << num(car.main_reservoir) << ';'
      << (asked ? num(asked->main_reservoir_rate, 4) : "") << ';';
    double force = 0.0, brake = 0.0;
    float least = 99.0f, most = 0.0f;
    for (const auto& c : cars) {
        force += c.force;
        brake += c.brake_force;
        const float cylinder = (std::max)(c.cylinder, c.independent);
        least = (std::min)(least, cylinder);
        most = (std::max)(most, cylinder);
    }
    f << num(force / 1000.0, 1) << ';' << num(brake / 1000.0, 1) << ';' << now.throttle << ';' << now.reverser << ';';
    if (out && out->has_engine) {
        f << num(out->engine.equalising) << ';' << num(out->engine.train_pipe_rate) << ';' << num(out->engine.ep_value) << ';'
          << num(out->engine.independent_brake) << ';' << out->engine.systems;
    } else {
        f << ";;;;";
    }
    f << ';' << num(least) << ';' << num(most);
    // The rails, the bogies sliding, the shoes' wetness (mean) and warmth (most).
    {
        static constexpr const char* rails_names[] = {"RTS", "сухие", "мокрые", "снег", "лёд", "листва"};
        int sliding = 0, burning = 0;
        float wet = 0.0f, heat = 0.0f;
        for (const auto& c : cars) {
            for (const auto& s : c.shoe_state) {
                sliding += s.sliding ? 1 : 0;
                burning += s.burning ? 1 : 0;
                wet += s.wet;
                heat = (std::max)(heat, s.heat);
            }
        }
        f << ';' << rails_names[static_cast<int>(train.settings().rails)] << ';' << sliding << ';'
          << num(cars.empty() ? 0.0f : wet / static_cast<float>(cars.size() * 2)) << ';' << num(heat, 1) << ';' << burning;
    }
    for (const auto k : brake_log_cars_) {
        const auto& c = cars[k];
        const bool rk = c.definition && (c.definition->brakes.system & 5) &&
                        simulation::distributor_kind(*c.definition) == simulation::DistributorKind::vr483;
        f << ';' << num(c.pipe) << ';' << num(c.auxiliary) << ';' << (rk ? num(c.control) : std::string()) << ';' << num(c.cylinder) << ';'
          << num(c.independent);
    }
    f << "\r\n";
    f.flush();
}

std::optional<float> TrainView::equalising_of(const std::size_t u, const std::size_t i) const {
    // The valve working the pipe under the realistic air (the DLL's УР
    // taken, or openRTS's valve over it).
    if (u < units_.size()) {
        if (const auto* asked = units_[u].train->demands_of(i); asked) {
            if (asked->equalising_kept >= 0.0f) return asked->equalising_kept;
            if (asked->equalising >= 0.0f) return asked->equalising;
        }
    }
    if (const auto* out = dll_car(u, i) ? dll_output(u, i) : nullptr; out && out->has_engine) {
        // What its DLL keeps in Engine::UR (not every DLL fills it).
        if (out->engine.equalising > 0.0f) return out->engine.equalising;
        return std::nullopt;
    }
    if (u == 0 && driver_ && i == driven_car()) return driver_->equalising;
    return std::nullopt;
}

simulation::TapeRecord TrainView::tape_record(const std::size_t u, const std::size_t i) {
    simulation::TapeRecord r;
    const auto& unit = units_[u];
    const auto& train = *unit.train;
    const auto& states = train.cars();
    if (i >= states.size() || i >= unit.cars.size()) return r;
    const auto& car = unit.cars[i];
    const auto& state = states[i];
    r.velocity = state.velocity;
    r.game_time = static_cast<std::int32_t>(std::floor(day_time_));
    // The obeyed signal ahead (train +0xA0 / +0x7C, the signal pass 0x508520).
    if (signals_) {
        if (const auto next = signals_->ahead(train); next.valid) {
            r.signal = static_cast<std::uint32_t>(next.aspect);
            tape_signal_distance_ = static_cast<float>(next.distance);
        }
    }
    r.signal_distance = tape_signal_distance_;
    r.train_pipe = state.pipe;
    r.independent = state.independent;
    r.cylinder = state.cylinder;
    // Wagon::TrPos[0]: where the car's front is on the track.
    if (const auto placements = train.placements(); i < placements.size()) {
        const auto& p = placements[i].front;
        r.tile_x = p.tile_x;
        r.tile_z = p.tile_z;
        r.x = static_cast<float>(p.x);
        r.y = static_cast<float>(p.y);
        r.z = static_cast<float>(p.z);
    }
    r.pantographs = pantographs_up(u, i);
    const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
    if (engine) r.kind = engine->kind == formats::EngineKind::electric ? 3 : engine->kind == formats::EngineKind::diesel ? 2 : 0;
    if (const auto* out = dll_car(u, i) ? dll_output(u, i) : nullptr; out && out->has_engine) {
        // What its DLL keeps in Engine (0x5050B0 reads locoA).
        r.throttle = out->engine.throttle;
        r.sanding = static_cast<float>(out->engine.sanding);
        r.ep = (out->engine.systems & 2u) != 0;
        r.equalising = equalising_of(u, i).value_or(out->engine.equalising);
        r.consumption = r.kind == 3 ? out->engine.power_consuming * 0.001f : r.kind == 2 ? out->engine.fuel_consuming : 0.0f;
    } else if (u == 0 && driver_ && i == driven_car()) {
        // The keyboard driver's handles; the power the engine drew, kW.
        r.throttle = driver_->throttle;
        r.sanding = state.sanding ? 1.0f : 0.0f;
        r.ep = driver_->ep_brake;
        r.equalising = driver_->equalising;
        r.consumption = r.kind == 3 ? state.power * 0.001f : 0.0f;
    } else {
        r.sanding = state.sanding ? 1.0f : 0.0f;
    }
    // The speed in force (0x4FB630): the posts', else the route's SpeedLimit
    // (100 m/s when the .trk has none).
    r.speed_limit = limits_ && limits_->route_limit() > 0.0f ? limits_->route_limit() : 100.0f;
    if (limits_) {
        if (const auto now = limit_of(unit); now.current > 0.0f) r.speed_limit = now.current;
        if (const auto km = limits_->ordinate(train)) r.ordinate = static_cast<float>(*km);
    }
    const std::tm clock = local_now();
    r.clock_hour = static_cast<std::uint16_t>(clock.tm_hour);
    r.clock_minute = static_cast<std::uint16_t>(clock.tm_min);
    r.clock_second = static_cast<std::uint16_t>(clock.tm_sec);
    r.mass = train.mass();
    r.cars = static_cast<std::uint32_t>(unit.cars.size());
    for (const auto& c : unit.cars)
        if (c.definition && c.definition->engine && !c.cold) ++r.locomotives;
    r.length = train.length();
    r.season = season_;
    r.weather = weather_;
    return r;
}

void TrainView::feed_world(const Unit& unit) {
    auto& world = legacy_->world();
    // The car the tape rolls on, for RTSGetInteger 7 (one outside the
    // player's train: "another locomotive" to every DLL).
    world.tape_car = -1;
    if (tape_.recording()) {
        const auto at = tape_car();
        world.tape_car = at && at->first == 0 && unit.host_train == 0 ? static_cast<std::int32_t>(at->second)
                                                                         : std::numeric_limits<std::int32_t>::max();
    }
    world.day_time = day_time_;
    world.night = night_;
    world.route_speed_limit = limits_ ? limits_->route_limit() : 0.0f;
    world.limits_without_tolerance = limits_without_tolerance_;
    // RTSGetTrackItems (RTS 0x4808DC): the items from the train's rear
    // forward over its length and further, and back from the rear; the
    // distance from the front (negative under the train, item_under) or the
    // rear, and the signed distance from the rear. Signals are left out
    // (ALSN gives them). As far as the DLLs asked (5000 m at least).
    world.track_items.clear();
    world.tdb = nullptr;
    world.sound_pairs.clear();
    if (const auto* network = unit.train->network(); network && unit.train->placed()) {
        world.tdb = &network->database();
        const double range = (std::max)(5000.0, static_cast<double>(legacy_->items_wanted()));
        world.items_range = static_cast<float>(range);
        const double length = unit.train->length();
        const auto not_signal = [](const formats::TrackItem& item) { return item.kind != formats::TrackItemKind::signal; };
        if (const auto rear = unit.train->rear_end()) {
            const auto forward = simulation::look_ahead(*network, rear->node(), rear->offset(), rear->forward(), length + range, not_signal, 100000);
            items_searched_[0] = forward.searched - length;
            for (const auto& item : forward.items) {
                const double from_front = item.distance - length;
                world.track_items.push_back({.distance = static_cast<float>(from_front),
                                             .z = static_cast<float>(item.distance),
                                             .pos = from_front >= 0.0 ? dllhost::item_ahead : dllhost::item_under,
                                             .item = item.item});
            }
            const auto backward = simulation::look_ahead(*network, rear->node(), rear->offset(), !rear->forward(), range, not_signal, 100000);
            items_searched_[1] = backward.searched;
            for (const auto& item : backward.items) {
                world.track_items.push_back({.distance = static_cast<float>(item.distance),
                                             .z = static_cast<float>(-item.distance),
                                             .pos = dllhost::item_behind,
                                             .item = item.item});
            }
        }
    }
    // The SMS objects' trigger pairs (RTSIsSMSTriggerOn): the exterior and
    // cab scripts and the libraries' own.
    for (std::size_t i = 0; i < unit.cars.size(); ++i) {
        const auto& car = unit.cars[i];
        const auto add = [&](const audio::SoundInstance* sound, const std::int16_t object) {
            if (!sound) return;
            const auto pairs = sound->trigger_pairs();
            if (!pairs) return;
            dllhost::SoundPairState s{.car = static_cast<std::uint32_t>(i), .object = object};
            std::copy(pairs->on.begin(), pairs->on.end(), s.on);
            std::copy(pairs->known.begin(), pairs->known.end(), s.known);
            world.sound_pairs.push_back(s);
        };
        add(car.sound.get(), dllhost::sound_exterior);
        add(car.cab_sound.get(), dllhost::sound_cabin);
        for (const auto& aux : car.aux_sounds) add(aux.instance.get(), static_cast<std::int16_t>(aux.library));
    }
    world.signals_ahead.clear();
    world.route = {};
    world.signal = {};
    world.event = {};
    const auto& train = *unit.train;
    const auto limit = limit_of(unit);
    world.route = {.limit = limit.current,
                   .next_limit = limit.ahead,
                   .distance = limit.ahead >= 0.0f ? static_cast<float>(limit.ahead_distance) : -1.0f};
    // The next speed post ahead (RTS train +120 / +116, ALSN's two integers).
    world.next_limit = limit.ahead >= 0.0f ? limit.ahead : -1.0f;
    world.next_limit_distance = limit.ahead >= 0.0f ? static_cast<float>(limit.ahead_distance) : -1.0f;
    world.alsn_ahead.clear();
    world.alsn_behind.clear();
    world.alsn_passing.clear();
    if (!signals_) return;
    // The route database's entry of a signal object: its first head's TDB
    // item's (RTS 0x508520 hands the first head's +124 to ALSN).
    const auto* units = signal_units();
    world.route_database = units ? &units->database : nullptr;
    const auto unit_of = [&](const std::uint32_t object) -> std::int32_t {
        if (!units || object >= signals_->objects().size()) return -1;
        const auto& heads = signals_->objects()[object].heads;
        if (heads.empty() || heads.front() >= signals_->heads().size()) return -1;
        const auto item = signals_->heads()[heads.front()].track_item;
        return item < units->item_unit.size() ? units->item_unit[item] : -1;
    };
    // ALSN's arrays (0x508520): ahead of the front, behind the rear, and
    // those the ends pass this step (within speed x step x 1.5).
    const double passing = std::abs(train.speed()) * simulation::train_step * 1.5;
    const auto to_input = [&](const simulation::SignalPosition& p) {
        dllhost::SignalInput s;
        s.distance = static_cast<float>(p.distance);
        for (int k = 0; k < 4; ++k) s.aspect[k] = p.aspect[k];
        s.flags = p.flags;
        s.speed_limit = p.speed;
        s.unit = unit_of(p.object);
        return s;
    };
    std::vector<dllhost::SignalInput> rear_passing;
    for (const auto& p : signals_->positions(train, true)) (p.distance > passing ? world.alsn_ahead : world.alsn_passing).push_back(to_input(p));
    for (const auto& p : signals_->positions(train, false)) (p.distance > passing ? world.alsn_behind : rear_passing).push_back(to_input(p));
    world.alsn_passing.insert(world.alsn_passing.end(), rear_passing.begin(), rear_passing.end());
    const auto next = signals_->ahead(train);
    if (!next.valid) return;
    dllhost::SignalInput s;
    s.distance = static_cast<float>(next.distance);
    for (auto& a : s.aspect) a = static_cast<std::uint32_t>(next.aspect);
    s.speed_limit = next.speed;
    s.unit = unit_of(next.object);
    world.signals_ahead.push_back(s);
    world.signal = {.limit = -1.0f, .next_limit = next.speed, .distance = static_cast<float>(next.distance)};
}

std::uint32_t TrainView::signal_aspect(const Unit& unit) const {
    if (!signals_) return 0;
    const auto next = signals_->ahead(*unit.train);
    return next.valid ? static_cast<std::uint32_t>(next.aspect) : 0u;
}

// RTS takes a pantograph as up from its animation parts (0x5031D8), so the
// line comes when it has risen; without animated pantographs, from what the
// DLL wants or, without a DLL, while the train works. Then the line current,
// the wire's heat and the substations' load (0x503CE4).
std::uint8_t TrainView::pantographs_up(const std::size_t u, const std::size_t i) const {
    if (u >= units_.size() || i >= units_[u].cars.size()) return 0;
    const auto& unit = units_[u];
    const auto& car = unit.cars[i];
    const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
    if (!engine || engine->kind != formats::EngineKind::electric) return 0;
    std::uint8_t raised = 0;
    if (!car.pantograph_nodes.empty() && car.pantograph_frames >= 2) {
        const float top = static_cast<float>(car.pantograph_frames - 1u) - 0.01f;
        for (const auto& [node, number] : car.pantograph_nodes) {
            if (car.pantograph_frame[number - 1u] >= top) raised |= static_cast<std::uint8_t>(1u << (number - 1u));
        }
    } else if (const auto* out = dll_car(u, i) ? dll_output(u, i) : nullptr) {
        raised = out->panto_raised;
    } else if (u == 0 && native_ && i == native_car_) {
        raised = static_cast<std::uint8_t>(native_->outputs().pantographs);
    } else {
        const bool working = unit.role != Role::standing && !unit.train->asleep();
        raised = working && !car.cold && !(u == 0 && pantographs_down_) ? 1 : 0;
    }
    return raised;
}

void TrainView::feed_line() {
    electrification_->advance_loads(simulation::train_step);
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        if (!unit.train->placed()) continue;
        const auto& states = unit.train->cars();
        std::vector<std::uint8_t> raised(states.size(), 0);
        bool electric = false;
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            const auto& car = unit.cars[i];
            const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
            if (!engine || engine->kind != formats::EngineKind::electric) continue;
            electric = true;
            raised[i] = pantographs_up(u, i);
        }
        const auto line = electrification_->apply(*unit.train, raised, simulation::train_step);
        if (u == 0) {
            line_ = line;
            line_engines_ = electric;
        }
    }
    // A pantograph that burnt the wire (LocoFlags 0x200: no line for it for 60 s).
    for (const auto& burn : electrification_->take_burns()) {
        const auto* unit = find_unit(burn.train);
        message_ = unit == &units_.front() ? trf("car %zu burnt the contact wire (%.0f A at a pantograph)", burn.car + 1, burn.current)
                                           : trf("a train burnt the contact wire (%.0f A at a pantograph)", burn.current);
    }
}

void TrainView::apply_default_systems(const SceneInput& input) {
    if (units_.empty() || units_.front().role != Role::driven || legacy_ || native_) return;
    auto& unit = units_.front();
    // The driven engine's sound and cab sound, or every engine (the sander).
    const auto post = [&](const std::uint32_t trigger, const bool every_engine) {
        for (std::size_t i = 0; i < unit.cars.size(); ++i) {
            auto& car = unit.cars[i];
            const bool driven_engine = i == cab_car_;
            if (!(driven_engine || (every_engine && car.definition && car.definition->engine))) continue;
            car.pending_triggers.push_back(trigger);
            if (driven_engine) car.pending_cab_triggers.push_back(trigger);
        }
    };
    // MSTS's discrete triggers the installed scripts use: 8 / 9 the horn,
    // 10 / 11 the bell (whistle), 4 / 5 the sander, 48 / 49 the doors.
    if (input.horn != horn_) {
        horn_ = input.horn;
        post(horn_ ? 8u : 9u, false);
    }
    if (input.bell) {
        bell_ = !bell_;
        post(bell_ ? 10u : 11u, false);
    }
    if (driver_ && driver_->sanding != input.sander) {
        driver_->sanding = input.sander;
        post(input.sander ? 4u : 5u, true);
    }
    if (input.pantographs) {
        pantographs_down_ = !pantographs_down_;
        message_ = pantographs_down_ ? tr("pantographs lowered") : tr("pantographs raised");
    }
    for (const int side : {0, 1}) {
        if (!(side == 0 ? input.doors_right : input.doors_left)) continue;
        auto& open = doors_open_[static_cast<std::size_t>(side)];
        if (!open && std::abs(driven().speed()) > 0.1) {
            message_ = tr("stop the train to open the doors");
            continue;
        }
        open = !open;
        for (auto& car : unit.cars) {
            // The cab's side: a car turned the other way has it under the other GlobalID.
            const std::uint32_t group = (side == 0) != door_side_turned(car) ? 1u : 2u;
            if (std::find(car.door_groups.begin(), car.door_groups.end(), group) == car.door_groups.end()) continue;
            car.pending_triggers.push_back(open ? 48u : 49u);
        }
        message_ = side == 0 ? (open ? tr("right doors open") : tr("right doors shut")) : (open ? tr("left doors open") : tr("left doors shut"));
    }
}

// The exterior lights of every train: a DLL switches its engine's (LightObj
// Flags: 8 turn on, 1 on, 16 reversed); the other cars' follow their
// Conditions as RTS decides them (simulation/train_lights: 0x4F07D4,
// 0x4FFDF8), fading and cycling their States (0x4FF9B8). Type 0 glows as
// its LightTexture, flat, facing where the light points (seen from the
// front only); types 1..4 light the scene through the shape shaders (RTS
// lights.glsl), the nearest nine.
void TrainView::update_neighbour_rooms(const float elapsed, const glm::vec3& eye) {
    neighbour_rooms_now_.clear();
    neighbour_room_keys_.clear();
    portals_.clear();
    if (!neighbour_rooms_ || !viewing_inside() || units_.empty()) return;
    auto& unit = units_.front();
    const std::size_t i = engineer_car_;
    auto* cab = engineer_cab();
    if (!cab || i >= unit.cars.size() || unit.current.empty()) return;
    const auto body = body_pose(unit, i, false);
    if (!body) return;
    const glm::mat4 to_scene = body->transform * cab->placement();
    const auto& doors = cab->description().doors;
    for (std::size_t k = 0; k < doors.size(); ++k) {
        const auto& d = doors[k];
        const bool planed = d.plane.a != std::array<float, 3>{} || d.plane.b != std::array<float, 3>{};
        if (!passage_door(d) || (!d.has_position && !planed) || d.parent >= 0) continue;
        // Which end, as pass_to_next_car: the car's +Z end looks to the
        // front of the train unless the car is turned.
        const float door_z = planed ? (d.plane.a[2] + d.plane.b[2]) * 0.5f : d.position[2];
        const bool toward_front = ((cab->turned() ? -1.0f : 1.0f) * door_z > 0.0f) != unit.cars[i].flipped;
        if ((toward_front && i == 0) || (!toward_front && i + 1 >= unit.cars.size())) continue;
        const std::size_t j = toward_front ? i - 1 : i + 1;
        CabView* room = car_cab(0, j, true);
        if (!room) continue;
        neighbour_room_keys_.push_back(unit.cars[j].key);
        room->poll();
        // Its door at this end opens and shuts with his (RTS 0x4313FC opens
        // the facing door as he goes through).
        const bool want_plus = toward_front ? unit.cars[j].flipped : !unit.cars[j].flipped;
        std::optional<std::size_t> facing;
        float farthest = -1.0f;
        const auto& theirs = room->description().doors;
        for (std::size_t a = 0; a < theirs.size(); ++a) {
            if (!theirs[a].has_position || !passage_door(theirs[a])) continue;
            const float za = (room->turned() ? -1.0f : 1.0f) * theirs[a].position[2];
            if ((za > 0.0f) != want_plus || std::abs(za) <= farthest) continue;
            farthest = std::abs(za);
            facing = a;
        }
        if (facing) room->set_door(*facing, cab->door_wanted(k));
        // Its doors move here unless the car's outer doors move them (update_exterior_doors).
        if (unit.cars[j].exterior_doors.empty()) room->step_doors(elapsed);
        // Open, or shut too by the setting (his door's glass shows the next
        // car's room, its own door shut with his).
        if ((!neighbour_rooms_shut_ && cab->door_openness(k) < 0.02f) || portals_.size() >= renderer::max_portals) continue;
        // The doorway: across the door's plane, at its end towards the next
        // car, a little wider and taller than a doorway (his cab's own walls
        // hide what is beyond it).
        float x0 = d.position[0] - 0.45f, x1 = d.position[0] + 0.45f, floor = d.position[1] - 1.1f, z = d.position[2];
        if (planed) {
            x0 = (std::min)(d.plane.a[0], d.plane.b[0]);
            x1 = (std::max)(d.plane.a[0], d.plane.b[0]);
            floor = (std::min)(d.plane.a[1], d.plane.b[1]);
            z = door_z > 0.0f ? (std::max)(d.plane.a[2], d.plane.b[2]) : (std::min)(d.plane.a[2], d.plane.b[2]);
        }
        constexpr float margin = 0.15f, height = 2.4f;
        // In his car's end wall (the door's point): beyond it the gangway
        // (the plane goes on into it), then the next car's end wall at its
        // door's point. His car's outside shows in the gap past its wall's
        // thickness, the next car's outside up to its wall.
        if (d.has_position) z = d.position[2];
        renderer::ModelPortal portal;
        portal.centre = glm::vec3{to_scene * glm::vec4{(x0 + x1) * 0.5f, floor - 0.1f + height * 0.5f, z, 1.0f}};
        portal.across = glm::normalize(glm::vec3{to_scene[0]});
        portal.up = glm::normalize(glm::vec3{to_scene[1]});
        // At least the gangway's width: standing in it he sees its sides
        // through the doorway too (his cab's walls hide the rest from inside).
        portal.half_across = (std::max)((x1 - x0) * 0.5f + margin, 0.7f);
        portal.half_up = height * 0.5f;
        const auto room_body = body_pose(unit, j, false);
        const glm::vec3 next_middle = room_body ? glm::vec3{room_body->transform[3]} : portal.centre;
        if (glm::dot(glm::cross(portal.across, portal.up), next_middle - portal.centre) < 0.0f) portal.across = -portal.across;
        const glm::vec3 normal = glm::normalize(glm::cross(portal.across, portal.up));
        // The next car's end wall (its facing door's point): its outside is
        // cut beyond it, and in the doors' opening from 0.45 m short of it
        // (the ED9M's end door is part of its end wall's node and lies a
        // little nearer than the salon's door point); his own car's outside
        // fills the gangway between, but for its own door within 0.45 m.
        portal.near_cut = 0.15f;
        portal.far_cut = 0.0f;
        portal.near_door = 0.45f;
        portal.door_half_across = (x1 - x0) * 0.5f + 0.1f;
        portal.door_half_up = 0.95f;
        if (facing && room_body && theirs[*facing].has_position) {
            const auto& p = theirs[*facing].position;
            const glm::vec3 wall{room_body->transform * room->placement() * glm::vec4{p[0], p[1], p[2], 1.0f}};
            const float depth = glm::dot(wall - portal.centre, normal);
            portal.far_cut = (std::max)(0.0f, depth + 0.05f);
            portal.far_door = (std::max)(0.0f, depth - 0.45f);
            const auto& q = theirs[*facing].plane;
            if (q.a != std::array<float, 3>{} || q.b != std::array<float, 3>{})
                portal.door_half_across = (std::max)(portal.door_half_across, std::abs(q.a[0] - q.b[0]) * 0.5f + 0.1f);
        }
        // His door's plane goes on into the gangway: standing there his eye
        // is past his wall, and the doorway (the cuts with it) is put just
        // ahead of it, else nothing would be seen through it.
        if (const float past = glm::dot(eye - portal.centre, normal) + 0.15f; past > 0.0f) {
            portal.centre += normal * past;
            portal.near_cut = (std::max)(0.0f, portal.near_cut - past);
            portal.far_cut = (std::max)(0.0f, portal.far_cut - past);
            portal.near_door = (std::max)(0.0f, portal.near_door - past);
            portal.far_door = (std::max)(0.0f, portal.far_door - past);
        }
        // Only while he looks that way (a corner of the doorway in view):
        // else the room is not drawn nor the car's outside cut.
        bool in_view = false;
        for (const float a : {-1.0f, 1.0f}) {
            for (const float b : {-1.0f, 1.0f}) {
                const glm::vec3 corner = portal.centre + portal.across * (a * portal.half_across) + portal.up * (b * portal.half_up);
                const glm::vec3 to = corner - eye;
                const float distance = glm::length(to);
                in_view = in_view || distance < 0.3f || glm::dot(to / distance, look_) > 0.35f;
            }
        }
        if (!in_view) continue;
        neighbour_rooms_now_.push_back({j, portals_.size()});
        portals_.push_back(portal);
    }
}

void TrainView::update_lights(const float elapsed, const glm::vec3& eye) {
    light_glows_.clear();
    light_beams_.clear();
    interior_lights_.clear();
    if (units_.empty()) return;
    auto& unit = units_.front();
    // The lamps of the cab or salon the engineer is in (InternalLight State:
    // the DLL's SwitchLight or GM_LIGHT_ON by GlobalID), RTS's LightsInt;
    // then (openRTS) those of the next cars' rooms he sees into. As many
    // as RTS's config.ini Shader.NumLightsInt (64 without it; rts1.exe
    // 0x405D2C, the shaders' %NumLightsInt%), the first burning ones in
    // LightSources' order.
    const auto lamp_limit = static_cast<std::size_t>(
        std::clamp(rts_config_value(rts_config_, "shader.numlightsint").value_or(64.0f), 1.0f,
                   static_cast<float>(renderer::max_interior_lights)));
    const auto add_lamps = [&](const CabView* cab, const std::size_t car_index, std::vector<float>& lamp_level, const bool sway) {
        const auto& lamps = cab->description().lights;
        const auto* out = interior_output(car_index);
        const auto body = body_pose(unit, car_index, sway);
        lamp_level.resize(lamps.size(), 0.0f);
        for (std::size_t k = 0; k < lamps.size() && body; ++k) {
            const auto& lamp = lamps[k];
            // A DLL's SwitchLight / GM_LIGHT_ON; without one the .sd's Flags
            // as RTS runs a shape's lamps (0x4B0284): RUNNING burns,
            // NIGHT_AUTO by the light level (AutoWhenWorldLit, else at
            // night), NIGHT_ONLY never by day.
            bool on = out && k < out->internal_lights.size() && out->internal_lights[k] != 0;
            if (!out) {
                const std::string lamp_flags = lower(lamp.flags);
                on = lamp_flags.find("running") != std::string::npos;
                if (lamp_flags.find("night_auto") != std::string::npos)
                    on = lamp.auto_when_world_lit > 0.0f ? world_lit_ <= lamp.auto_when_world_lit : night_;
                if (lamp_flags.find("night_only") != std::string::npos && !night_) on = false;
            }
            auto& level = lamp_level[k];
            // RTS steps a lamp with LightFade's first value both ways
            // (0x4B0284 reads +44 only; the second value, +48, is unused).
            const float time = lamp.fade[0];
            const float goal = on ? 1.0f : 0.0f;
            level = time <= 0.0f ? goal : goal > level ? (std::min)(goal, level + elapsed / time) : (std::max)(goal, level - elapsed / time);
            if (level <= 0.0f || interior_lights_.size() >= lamp_limit) continue;
            const glm::vec3 from{lamp.position[0], lamp.position[1], lamp.position[2]};
            glm::vec3 to{lamp.points_to[0], lamp.points_to[1], lamp.points_to[2]};
            if (glm::distance(from, to) < 1e-3f) to = from - glm::vec3{0.0f, 1.0f, 0.0f};
            const float reach = glm::distance(from, to);
            // Light space: z towards PointsTo, x along the car's x, y the third.
            const glm::vec3 z = glm::normalize(to - from);
            glm::vec3 x = glm::vec3{1.0f, 0.0f, 0.0f} - z * z.x;
            if (glm::length(x) < 1e-3f) x = glm::vec3{0.0f, 0.0f, 1.0f} - z * z.z;
            x = glm::normalize(x);
            const glm::vec3 y = glm::cross(z, x);
            glm::mat4 local{1.0f};
            local[0] = glm::vec4{x, 0.0f};
            local[1] = glm::vec4{y, 0.0f};
            local[2] = glm::vec4{z, 0.0f};
            local[3] = glm::vec4{from, 1.0f};
            const glm::mat4 world = body->transform * cab->placement() * local;
            const std::string flags = lower(lamp.flags);
            renderer::ModelExtraLight light;
            // RTS's packing (rts1.exe 0x4AEBEC): the kind (cone 4, sphere 2,
            // box 3) plus 32 for POSITIONAL, which lights a surface whatever
            // way it faces (the shader's normal term is for the others: the
            // 2ES6's desk lamps, all POSITIONAL, lit its upright gauges from
            // above at 0.3 until 2026-11-15); a cone's Size its Radius over
            // the reach as it is (not squared: RTS's cones are that much
            // wider than the Radius), a sphere's and a box's the Radius.
            const bool cone = flags.find("light_cone") != std::string::npos;
            const float kind = cone ? 4.0f : flags.find("light_sphere") != std::string::npos ? 2.0f : 3.0f;
            light.kind = kind + (flags.find("positional") != std::string::npos ? 32.0f : 0.0f);
            const float rx = (std::max)(lamp.radius[0], 0.01f), rz = (std::max)(lamp.radius[1] > 0.0f ? lamp.radius[1] : lamp.radius[0], 0.01f);
            light.size = cone ? glm::vec4{rx / reach, rz / reach, reach, lamp.source_radius} : glm::vec4{rx, rz, reach, lamp.source_radius};
            light.to_light = glm::inverse(world);
            light.direction = glm::normalize(glm::vec3{world[2]});
            light.spot_exponent = std::clamp(lamp.spot_exponent, 0.0f, 1.0f) * 128.0f;
            light.attenuation = glm::vec4{lamp.attenuation[0], lamp.attenuation[1], lamp.attenuation[2], 0.0f};
            const std::uint32_t c = lamp.colour;
            light.diffuse = glm::vec4{static_cast<float>((c >> 16) & 0xFFu) / 255.0f * level, static_cast<float>((c >> 8) & 0xFFu) / 255.0f * level,
                                      static_cast<float>(c & 0xFFu) / 255.0f * level, 1.0f};
            interior_lights_.push_back(light);
        }
    };
    if (const auto* cab = viewing_inside() ? engineer_cab() : nullptr; cab && engineer_car_ < unit.cars.size()) {
        add_lamps(cab, engineer_car_, lamp_level_, false);
    }
    update_neighbour_rooms(elapsed, eye);
    portal_eye_ = eye;
    for (const auto& room : neighbour_rooms_now_) {
        if (const auto* cab = car_cab(0, room.car, false)) add_lamps(cab, room.car, neighbour_lamp_levels_[unit.cars[room.car].key], true);
    }
    struct Beam {
        renderer::ModelExtraLight light;
        float distance{};
    };
    std::vector<Beam> beams;
    const simulation::LightWorld light_world{.night = night_, .world_lit = world_lit_, .weather = weather_};
    for (std::size_t u = 0; u < units_.size(); ++u) {
    auto& each = units_[u];
    if (!each.train->placed() || each.current.empty()) continue;
    const auto& states = each.train->cars();
    const auto kind = each.role == Role::driven ? simulation::TrainKind::player
                    : each.role == Role::ai      ? simulation::TrainKind::ai
                    : each.train->asleep()       ? simulation::TrainKind::static_train
                                                 : simulation::TrainKind::loose;
    bool reversed = false;
    if (const auto lead = each.train->leading_end(), front = each.train->front_end(); lead && front)
        reversed = lead->node() != front->node() || std::abs(lead->offset() - front->offset()) > 1e-3;
    for (std::size_t i = 0; i < each.cars.size() && i < states.size(); ++i) {
        auto& car = each.cars[i];
        const auto& lights = car.definition->lights;
        if (lights.empty()) continue;
        // Far trains' lights are not worked out (RTS: only cars in sight).
        if (u != 0 && i < each.current.size() && glm::distance(local(each.current[i].front), eye) > 3000.0f) continue;
        const auto* out = dll_car(u, i) ? dll_output(u, i) : nullptr;
        const bool engine = car.definition->engine.has_value();
        // A remote train's lights are only what its owner sent.
        std::vector<bool> wanted;
        if (each.remote) {
            for (const auto lit : car.remote_lights) wanted.push_back(lit != 0);
        } else if (!out) {
            wanted = simulation::car_lights(*car.definition, i, each.cars.size(), kind, reversed, engine && car.cold, light_world);
        }
        car.lights.resize(lights.size());
        const auto body = body_pose(each, i, !(u == 0 && viewing_inside() && i == engineer_car_));
        for (std::size_t k = 0; k < lights.size(); ++k) {
            const auto& light = lights[k];
            const std::uint32_t flags = out && k < out->light_flags.size() ? out->light_flags[k] : 0u;
            const bool on = !light.states.empty() && (out ? (flags & 9u) != 0 : (k < wanted.size() && wanted[k]));
            auto& runner = car.lights[k];
            runner.step(light, on, elapsed);
            if (car.lit_now.size() != lights.size()) car.lit_now.assign(lights.size(), 0);
            car.lit_now[k] = on ? 1 : 0;
            const float level = runner.level;
            if (level <= 0.0f || !body) continue;
            const auto& s = light.states[(std::min)(runner.state, light.states.size() - 1)];
            float yaw = glm::radians(s.azimuth[1]);
            if (flags & 16u) yaw += pi;
            // RTS (loader 0x50036C): X angle -Elevation, Y angle Azimuth + pi,
            // the lamp facing its -Z (OpenGL's). In ours, facing +Z, that is a
            // turn by +Elevation: a positive Elevation dips (the 2ES4k's
            // headlights, +3), a negative one lifts (the ChS7's roof lamp on
            // its sloping front, -38).
            const float pitch = glm::radians(s.elevation[0]);
            glm::mat4 local = glm::translate(glm::mat4{1.0f}, glm::vec3{s.position[0], s.position[1], s.position[2]});
            local = glm::rotate(local, yaw, glm::vec3{0.0f, 1.0f, 0.0f});
            local = glm::rotate(local, pitch, glm::vec3{1.0f, 0.0f, 0.0f});
            // RTS (the car's light objects, 0x4F11A7): a light source (Type 1)
            // for the train's last car (Unit 3 or 5) is written as if at the
            // front and turned round the car's middle (Position x and z
            // negated, its angle + 180). The VL80T's rear section has its
            // marker beams at +7.6 (the coupling end) and their glows, not
            // turned, at -7.6 (its cab).
            if (light.type != 0 && (light.unit == 3 || light.unit == 5))
                local = glm::rotate(glm::mat4{1.0f}, pi, glm::vec3{0.0f, 1.0f, 0.0f}) * local;
            const glm::mat4 world = body->transform * local;
            const glm::vec3 at{world[3]};
            const glm::vec3 forward = glm::normalize(glm::vec3{world[2]});
            const std::uint32_t argb = s.colour;
            const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f, g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
                        b = static_cast<float>(argb & 0xFFu) / 255.0f, a = static_cast<float>(argb >> 24) / 255.0f;
            if (light.type == 0) {
                if (glm::dot(eye - at, forward) <= 0.0f || s.radius <= 0.0f) continue; // a lamp is seen from its front
                LightGlow glow;
                glow.sprite.position = at;
                glow.sprite.radius = s.radius;
                const auto alpha = static_cast<std::uint32_t>(std::clamp(a * level, 0.0f, 1.0f) * 255.0f);
                glow.sprite.colour = alpha << 24 | (argb & 0xFFu) << 16 | (argb & 0xFF00u) | ((argb >> 16) & 0xFFu);
                glow.sprite.axis_x = glm::normalize(glm::vec3{world[0]});
                glow.sprite.axis_y = glm::normalize(glm::vec3{world[1]});
                // openRTS: far off a lamp (a buffer light, a route board's
                // glyph) is smaller than a pixel. MSAA's four samples caught
                // it now and then, averaged; with one sample a pixel it fell
                // between the pixels' centres and went out. At least 1.5
                // pixels (half size; 3 across, so seen at a slant too), its
                // light spread over them: as bright on the whole as it is.
                glow.sprite.min_pixels = 1.5f;
                glow.sprite.spread_light = 1.0f;
                // RTS's lamp quad (GLOBAL\SHADERS\lamp.vert) has u 0 at its +x:
                // a quad turned along the beam (the ED9M's searchlight cone,
                // lights 14..19, Azimuth +-90) is bright at the lamp, not at
                // its far end.
                glow.sprite.uv = {1.0f, 0.0f, 0.0f, 1.0f};
                if (!light.texture.empty()) {
                    if (auto path = core::VirtualPath::parse("TRAINS/TRAINSET/" + light.texture)) glow.texture = *path;
                }
                light_glows_.push_back(std::move(glow));
                continue;
            }
            if (s.radius <= 0.0f) continue;
            Beam beam;
            beam.light.diffuse = glm::vec4{r * level, g * level, b * level, 1.0f};
            beam.light.to_light = glm::inverse(world);
            beam.light.direction = forward;
            const float spread = std::tan(glm::radians(std::clamp(s.angle, 0.5f, 89.0f)));
            beam.light.size = glm::vec4{spread, spread, s.radius, s.source_radius};
            beam.light.kind = static_cast<float>(light.type);
            beam.light.spot_exponent = light.spot_exponent;
            beam.light.attenuation = glm::vec4{light.attenuation[0], light.attenuation[1], light.attenuation[2], 0.0f};
            beam.distance = glm::distance(eye, at);
            beams.push_back(beam);
        }
    }
    }
    std::sort(beams.begin(), beams.end(), [](const Beam& x, const Beam& y) { return x.distance < y.distance; });
    for (std::size_t k = 0; k < beams.size() && k < 9; ++k) light_beams_.push_back(beams[k].light);
}

// A car of the driven train standing the other way round from the cab the
// engineer drives from (its Flags' flip, and the cab's own facing): its
// GlobalID 1 / 2 doors are on the cab's left / right.
bool TrainView::door_side_turned(const Car& car) const {
    if (units_.empty() || cab_car_ >= units_.front().cars.size()) return car.flipped;
    const bool cab_facing = units_.front().cars[cab_car_].flipped != (cab_ && cab_->turned());
    return car.flipped != cab_facing;
}

// RTS 0x4F5B34: a slipping axle's wheels turn 2 pi x its force over the
// adhesion per second faster than the run, and 0.1 rad a frame more (3 rad/s
// at RTS's 30 frames a second).
void TrainView::spin_slipping_wheels(const float elapsed) {
    constexpr float two_pi = 6.28318531f;
    constexpr float frame_spin = 3.0f;
    for (auto& unit : units_) {
        const auto& states = unit.train->cars();
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            const auto bits = states[i].wheelslip;
            if (bits == 0) continue;
            auto& car = unit.cars[i];
            car.wheel_spin.resize(car.wheels.size(), 0.0f);
            for (std::size_t w = 0; w < car.wheel_spin.size() && w < 32; ++w) {
                if ((bits & (1u << w)) == 0) continue;
                const float ratio = w < states[i].slip_ratio.size() ? states[i].slip_ratio[w] : 1.0f;
                const float way = ratio < 0.0f ? -1.0f : 1.0f;
                car.wheel_spin[w] = std::fmod(car.wheel_spin[w] + (two_pi * ratio + frame_spin * way) * elapsed, two_pi);
            }
        }
    }
}

void TrainView::update_exterior_doors(const float elapsed) {
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        for (std::size_t i = 0; i < unit.cars.size(); ++i) {
            auto& car = unit.cars[i];
            if (car.exterior_doors.empty()) continue;
            const auto* out = dll_output(u, i);
            CabView* cab = u == 0 && i == cab_car_ ? cab_.get() : u == 0 && i == engineer_car_ ? visit_cab_.get() : nullptr;
            // A cab he left (or stood at) keeps its doors; they move on here.
            if (!cab) {
                if (const auto kept = board_cabs_.find(car.key); kept != board_cabs_.end() && kept->second) {
                    cab = kept->second.get();
                    cab->poll();
                    cab->step_doors(elapsed);
                }
            } else if (!(u == 0 && i == engineer_car_ && engineer_.inside())) {
                // His own cab while he is outside it (on its walkways): the
                // engineer's update does not move them.
                cab->poll();
                cab->step_doors(elapsed);
            }
            // The train's doors of a GlobalID in this car (GM_DOOR_OPEN / CLOSE).
            const auto group_open = [&](const std::uint32_t group) {
                if (!out || group == 0) return false;
                for (std::size_t k = 0; k < car.door_groups.size() && k < out->doors.size(); ++k)
                    if (car.door_groups[k] == group && out->doors[k].set_state != 0) return true;
                return false;
            };
            // The passenger doors seen from inside (the cab's or salon's
            // InteriorDoors with a GlobalID) open with the train's doors.
            if (cab) {
                const auto& doors = cab->description().doors;
                for (std::size_t j = 0; j < doors.size(); ++j)
                    if (doors[j].global_id != 0) cab->set_door(j, group_open(doors[j].global_id));
            }
            for (std::size_t k = 0; k < car.exterior_doors.size(); ++k) {
                auto& door = car.exterior_doors[k];
                bool target = false;
                bool decided = false;
                if (cab && door.group == 0) {
                    // A door the engineer works (the cab's DOOR_1 / DOOR_2): the
                    // cab's door of the same name.
                    const auto& doors = cab->description().doors;
                    for (std::size_t j = 0; j < doors.size(); ++j) {
                        if (lower(doors[j].name) != lower(door.name) || !cab->door_animated(j) || doors[j].global_id != 0) continue;
                        target = cab->door_open(j);
                        decided = true;
                        break;
                    }
                }
                if (!decided && door.wanted) {
                    target = *door.wanted;
                    decided = true;
                }
                if (!decided && out && door.index < out->doors.size()) target = out->doors[door.index].set_state != 0;
                // The driven train without a DLL: the Y / U keys (GlobalID 2
                // left, 1 right, as the cab sees them; a car turned the other
                // way has them swapped).
                if (!decided && !out && u == 0 && !legacy_ && !native_ && (door.group == 1 || door.group == 2))
                    target = doors_open_[(door.group == 1) != door_side_turned(car) ? 0 : 1];
                const float goal = target ? 1.0f : 0.0f;
                const float step = elapsed / door.time;
                door.open = goal > door.open ? (std::min)(goal, door.open + step) : (std::max)(goal, door.open - step);
            }
        }
    }
}

void TrainView::update_pantographs(const float elapsed) {
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        const auto& states = unit.train->cars();
        const bool working = unit.role != Role::standing && !unit.train->asleep();
        const double speed = std::abs(unit.train->speed());
        // The train runs with its rear leading (RTS train +4 bit 0x80).
        bool reversed = false;
        if (const auto lead = unit.train->leading_end(), front = unit.train->front_end(); lead && front)
            reversed = lead->node() != front->node() || std::abs(lead->offset() - front->offset()) > 1e-3;
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            auto& car = unit.cars[i];
            // The wanted pantographs are worked out for every car, a shape
            // without their animation too: the network sends them.
            if (!car.definition) continue;
            const auto* engine = car.definition->engine ? &*car.definition->engine : nullptr;
            const auto* out = dll_car(u, i) ? dll_output(u, i) : nullptr;
            const bool plugin = u == 0 && native_ && i == native_car_;
            if (unit.remote) {
                car.pantograph_raised = car.remote_pantographs;
            } else if (out) {
                car.pantograph_raised = out->panto_raised;
            } else if (plugin) {
                car.pantograph_raised = static_cast<std::uint8_t>(native_->outputs().pantographs);
            } else if (engine && engine->kind == formats::EngineKind::electric && working && !car.cold && !(u == 0 && pantographs_down_)) {
                std::uint8_t main = 1, second = 2;
                if (unit.role == Role::ai) {
                    // RTS's AI trains (0x424488): pantograph 1 unless the car
                    // is turned or the train runs reversed (it compares bit 0
                    // with bit 0x80, so both together give 2 as well).
                    if (states[i].flipped || reversed) std::swap(main, second);
                } else {
                    // The driven train: the rear pantograph by the direction
                    // of travel (the train front when standing), in the car's
                    // own axes.
                    float lowest = 1e9f, highest = -1e9f;
                    std::uint8_t rear = 1, front = 1;
                    for (const auto& [node, number] : car.pantograph_nodes) {
                        const float z = car.pantograph_z[number - 1u];
                        if (z < lowest) { lowest = z; rear = number; }
                        if (z > highest) { highest = z; front = number; }
                    }
                    const bool backward = unit.train->speed() < -0.1;
                    const bool towards_shape_front = backward == states[i].flipped;
                    const std::uint8_t used = towards_shape_front ? rear : front;
                    main = static_cast<std::uint8_t>(1u << (used - 1u));
                    second = static_cast<std::uint8_t>(1u << ((used == rear ? front : rear) - 1u));
                    if (second == main) second = 0;
                }
                std::uint8_t raised = main;
                // RaiseSecondPantoWhenSpeedBelow, LowerPantoOnStop (an AI
                // train standing at a stop: its waiting point; PROBABLE),
                // AlwaysUsePanto, ExcludePantograph.
                if (engine->raise_second_panto_below > 0.0f && speed < engine->raise_second_panto_below) raised |= second;
                if (unit.role == Role::ai && engine->lower_panto_on_stop && speed < 0.1 && unit.wait_left > 0.0f)
                    raised = static_cast<std::uint8_t>(raised & ~engine->lower_panto_on_stop);
                raised |= engine->always_use_panto;
                raised = static_cast<std::uint8_t>(raised & ~engine->exclude_pantographs);
                car.pantograph_raised = raised;
            } else {
                car.pantograph_raised = 0;
            }
            // Without a DLL the pantographs sound as MSTS scripts expect:
            // 45 / 46 the first up / down, 66 / 67 the second.
            if (!out && car.pantograph_raised != car.pantographs_heard) {
                for (std::uint8_t n = 0; n < 2; ++n) {
                    const bool was = (car.pantographs_heard >> n) & 1u, now = (car.pantograph_raised >> n) & 1u;
                    if (was == now) continue;
                    const std::uint32_t trigger = n == 0 ? (now ? 45u : 46u) : (now ? 66u : 67u);
                    car.pending_triggers.push_back(trigger);
                    if (u == 0 && i == cab_car_) car.pending_cab_triggers.push_back(trigger);
                }
                car.pantographs_heard = car.pantograph_raised;
            }
            if (car.pantograph_nodes.empty() || car.pantograph_frames < 2) continue;
            const float last = static_cast<float>(car.pantograph_frames - 1u);
            // PantographRaiseTime (default 3 s).
            const float step = last * elapsed / (std::max)(car.definition->pantograph_raise_time, 0.1f);
            for (std::size_t n = 0; n < car.pantograph_frame.size(); ++n) {
                const float target = (car.pantograph_raised >> n) & 1u ? last : 0.0f;
                float& frame = car.pantograph_frame[n];
                frame = frame < target ? (std::min)(frame + step, target) : (std::max)(frame - step, target);
            }
        }
    }
}

// The listener's room in the engineer's cab or salon and how much of the
// outside reaches it (RTS 0x44AB7C: 0x51434C, then 0x5143DC to the open air).
// RTS takes the player's position in the car; openRTS the camera's.
void TrainView::update_listener(const ViewerCamera& camera) {
    listener_cab_ = nullptr;
    listener_room_ = audio::InteriorAcoustics::outside;
    outside_gain_ = 1.0f;
    if (viewing_inside() && !units_.empty() && engineer_car_ < units_.front().cars.size()) {
        const auto* cab = engineer_cab();
        if (cab && !cab->acoustics().empty()) {
            if (const auto body = body_pose(units_.front(), engineer_car_, false)) {
                const glm::mat4 to_cab = glm::inverse(body->transform * cab->placement());
                listener_local_ = glm::vec3(to_cab * glm::vec4(camera.eye(), 1.0f));
                listener_room_ = cab->acoustics().room_at(listener_local_.x, listener_local_.z);
                outside_gain_ = cab->sound_transmission(listener_room_, audio::InteriorAcoustics::outside);
                listener_cab_ = cab;
            }
        }
    }
    if (listener_room_ != traced_room_ && sound_trace()) {
        traced_room_ = listener_room_;
        std::fprintf(stderr, "[sound] listener room %d at (%.2f %.2f %.2f), outside heard %.2f\n", listener_room_, listener_local_.x,
                     listener_local_.y, listener_local_.z, outside_gain_);
    }
}

// The cabs' door and window sounds (CabView::take_sounds) to every sound of
// their car, as RTS's 0x46AF1C: the exterior script, the cab script and each
// library's.
void TrainView::post_cab_sounds() {
    const auto post = [](Car& car, const std::vector<std::uint32_t>& codes) {
        if (codes.empty()) return;
        car.pending_triggers.insert(car.pending_triggers.end(), codes.begin(), codes.end());
        car.pending_cab_triggers.insert(car.pending_cab_triggers.end(), codes.begin(), codes.end());
        std::size_t libraries = 0;
        for (const auto& aux : car.aux_sounds) libraries = (std::max)(libraries, aux.library + 1);
        if (car.pending_aux_triggers.size() < libraries) car.pending_aux_triggers.resize(libraries);
        for (auto& pending : car.pending_aux_triggers) pending.insert(pending.end(), codes.begin(), codes.end());
    };
    const auto find_car = [&](const std::uint32_t key) -> Car* {
        for (auto& unit : units_)
            for (auto& car : unit.cars)
                if (car.key == key) return &car;
        return nullptr;
    };
    if (!units_.empty()) {
        auto& cars = units_.front().cars;
        if (cab_ && cab_car_ < cars.size()) post(cars[cab_car_], cab_->take_sounds());
        if (visit_cab_ && engineer_car_ < cars.size()) post(cars[engineer_car_], visit_cab_->take_sounds());
    }
    for (auto& [key, cab] : board_cabs_) {
        if (!cab) continue;
        auto codes = cab->take_sounds();
        if (auto* car = find_car(key)) post(*car, codes);
    }
}

float TrainView::flat_uniform(const float low, const float high) {
    flat_random_ = flat_random_ * 1664525u + 1013904223u;
    return low + (high - low) * static_cast<float>(flat_random_ >> 8) / static_cast<float>(1u << 24);
}

void TrainView::play_wheel_flats(audio::AudioEngine& sound) {
    if (!flat_clips_tried_) {
        flat_clips_tried_ = true;
        for (const char* name : {"ra_euro_joint", "mp_joint"}) {
            for (int k = 1; k <= 12; ++k) {
                const auto path = core::VirtualPath::parse("SOUND/" + std::string(name) + std::to_string(k) + ".wav");
                if (!path || !vfs_->exists(*path)) continue;
                if (auto clip = bank_->get(*path)) flat_clips_.push_back(std::move(clip));
            }
            if (!flat_clips_.empty()) break;
        }
        std::cout << "wheel flats: " << flat_clips_.size() << " knocks found" << std::endl;
    }
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        const auto& states = unit.train->cars();
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            auto& car = unit.cars[i];
            if (car.pending_flats.empty()) continue;
            const auto flats = std::exchange(car.pending_flats, {});
            if (flat_clips_.empty() || i >= unit.current.size()) continue;
            const auto here = pose(unit, i);
            const float kmh = std::abs(states[i].velocity) * 3.6f;
            // The blow grows to some 40 km/h, then the wheel flies over the
            // flat more and more; deeper, louder.
            const float speed = std::pow((std::min)(kmh / 40.0f, 1.0f), 0.8f) * (kmh <= 80.0f ? 1.0f : (std::max)(0.8f, 1.0f - (kmh - 80.0f) / 200.0f));
            for (const auto& [bogie, depth] : flats) {
                // The bogie's place: its own front is the train's rear in a turned car.
                const bool train_front = (bogie == 0) != states[i].flipped;
                const glm::vec3 at = (train_front ? here.front : here.rear) + glm::vec3{0.0f, 0.5f, 0.0f};
                const float level = speed * (std::min)(depth / 2.0f, 1.5f) * flat_uniform(0.75f, 1.1f) * (viewing_inside() ? outside_gain_ : 1.0f);
                if (level < 0.01f) continue;
                const auto& clip = flat_clips_[static_cast<std::size_t>(flat_uniform(0.0f, static_cast<float>(flat_clips_.size()) - 0.001f))];
                if (const auto voice = sound.play(clip, audio::VoicePlayback::once, audio::VoiceSpace::positional)) {
                    sound.set(voice, 0.9f * level, flat_uniform(0.94f, 1.06f), at);
                }
            }
        }
    }
}

void TrainView::update_sound(const float elapsed, const ViewerCamera& camera, audio::AudioEngine& sound) {
    OPENRTS_ZONE("trains: sound");
    for (auto& gone : retired_sounds_) gone->stop(sound);
    retired_sounds_.clear();
    if (!bank_) bank_ = std::make_unique<audio::SoundBank>(vfs_);
    play_wheel_flats(sound);
    // First every car's script, group and clips, then the updates: a car
    // loading its clips must not delay the cars after it while the ones
    // before it already play (identical loops a fraction of a second apart
    // sound like an echo).
    std::uint32_t seed = 0;
    for (std::size_t u = 0; u < units_.size(); ++u) {
    auto& unit = units_[u];
    const auto& states = unit.train->cars();
    // RTS: the player's train plays group 7 of each script, other trains
    // the first group numbered 5 or 0 (none: that car is silent).
    const auto unit_owner = aboard(unit, u) ? formats::SoundOwner::player_train : formats::SoundOwner::train;
    for (std::size_t i = 0; i < unit.cars.size() && i < states.size() && i < unit.current.size(); ++i, ++seed) {
        auto& car = unit.cars[i];
        // Another player's train (network): RTS gives its cars their sound
        // when it loads them (0x4F0BC8, train kind 2 + 0x10000): an engine
        // neither cold nor AI as the player's own (group 7, else the first),
        // a cold one and a wagon their file's own (group 0 only).
        auto owner = unit_owner;
        if (unit.remote) {
            const bool engine = car.definition && car.definition->engine;
            owner = car.sound_owner.value_or(engine && !car.cold ? formats::SoundOwner::player_train
                                                                 : formats::SoundOwner::car);
        }
        if (!car.script_tried && !car.definition->sound.empty()) {
            car.script_tried = true;
            // Wagon sounds: <wagon folder>/SOUND/<file>, then the global SOUND.
            const auto base = "TRAINS/TRAINSET/" + car.folder + "/SOUND";
            std::vector<core::VirtualPath> candidates;
            if (auto folder = core::VirtualPath::parse(base)) {
                if (auto path = folder->resolve(car.definition->sound)) candidates.push_back(*path);
            }
            if (auto global = core::VirtualPath::parse("SOUND")->resolve(car.definition->sound)) candidates.push_back(*global);
            for (const auto& candidate : candidates) {
                if (!vfs_->exists(candidate)) continue;
                auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(*vfs_, candidate));
                if (!script->ok()) break;
                car.script = script;
                car.script_path = candidate;
                break;
            }
        }
        if (car.script && car.sound_owner != owner) {
            // New, or the train became / stopped being the player's.
            if (car.sound) car.sound->stop(sound);
            car.sound.reset();
            car.sound_owner = owner;
            if (const auto* group = car.script->select_group(owner)) {
                auto lookup = [this, path = car.script_path](const std::string& name) -> std::shared_ptr<const audio::AudioClip> {
                    const auto file = audio::find_sound_file(*vfs_, path, name);
                    return file ? bank_->get(*file) : nullptr;
                };
                car.sound = std::make_unique<audio::SoundInstance>(car.script, *group, owner, std::move(lookup), seed * 7919u + 17u);
                car.sound->preload();
                if (!car.remote_sound_restore.empty()) car.sound->restore(std::exchange(car.remote_sound_restore, {}));
                // OPENRTS_SOUND_TRACE: the player's train's cars too.
                if (sound_trace() && u == 0) car.sound->set_trace("car " + std::to_string(i + 1) + " " + car.definition->name);
            }
        }
        if (car.sound) car.sound->set_random_loop_phase(random_loop_phase_);
    }
    }
    for (std::size_t u = 0; u < units_.size(); ++u) {
    auto& unit = units_[u];
    const auto& states = unit.train->cars();
    for (std::size_t i = 0; i < unit.cars.size() && i < states.size() && i < unit.current.size(); ++i) {
        auto& car = unit.cars[i];
        const auto& state = states[i];
        const auto here = pose(unit, i);
        const auto position = (here.front + here.rear) * 0.5f;
        // Other trains are heard by their outside groups (RTS 0x44AB7C).
        audio::SoundInputs inputs{.camera = u == 0 ? sound_camera() : audio::SoundCamera::external, .position = position};
        // Every car's sound as heard from the engineer's room (0x44AB7C).
        inputs.gain = outside_gain_;
        // RTS 0x44AB7C: the speed without its sign (fabs of the wagon's), the
        // acceleration along the way it moves (speeding up positive). Signed,
        // a turned section or running back gave a speed below 0: the 3СЛ2М's
        // Speed_Inc_Past 1.38 loop never started there (testers 2026-11-15).
        inputs[formats::SoundVariable::speed] = std::abs(state.velocity);
        inputs[formats::SoundVariable::distance] = glm::distance(camera.eye(), position);
        inputs[formats::SoundVariable::distance_travelled] = static_cast<float>(std::abs(state.travelled));
        inputs[formats::SoundVariable::acceleration] = state.velocity >= 0.0f ? state.acceleration : -state.acceleration;
        // RTS (0x4F6DA9): the curve's radius without its sign, the straight
        // (0) as 10 000 000 m. The VL80's curve squeal starts below 680 m and
        // stops above it, its volume 0 beyond 870 m: with 0 on the straight
        // it went on at full volume after the curve, standing too.
        inputs[formats::SoundVariable::radius] = state.radius == 0.0f ? 1.0e7f : std::abs(state.radius);
        const auto* dll_vars = dll_sound_variables(u, i);
        if (dll_vars) {
            // The DLL sets the engine variables itself (SMSObject Var1..Var8).
            for (int k = 0; k < 8; ++k)
                inputs.variables[static_cast<std::size_t>(formats::SoundVariable::variable1) + static_cast<std::size_t>(k)] =
                    (*dll_vars)[static_cast<std::size_t>(k)];
        } else if (unit.remote && car.remote_sound_vars) {
            // Another client's train: its owner sends the variables.
            for (int k = 0; k < 8; ++k)
                inputs.variables[static_cast<std::size_t>(formats::SoundVariable::variable1) + static_cast<std::size_t>(k)] =
                    (*car.remote_sound_vars)[static_cast<std::size_t>(k)];
        } else if (car.definition->engine && unit.traffic) {
            // A traffic engine (RTS 0x424488): the seeming throttle from its
            // acceleration, as a pull while it accelerates.
            const float throttle = unit.traffic->throttle();
            inputs[formats::SoundVariable::variable1] = throttle;
            inputs[formats::SoundVariable::variable2] = unit.traffic->acceleration() > 0.0f ? throttle : 0.0f;
            inputs[formats::SoundVariable::variable3] = 0.0f;
        } else if (car.definition->engine && driver_ && u == 0) {
            // MSTS meaning of the engine variables (a DLL sets them in RTS).
            inputs[formats::SoundVariable::variable1] =
                static_cast<float>(driver_->throttle) / static_cast<float>((std::max)(driver_->notches, 1u));
            const float limit = (std::max)(car.definition->mass * 10.0f * (std::max)(car.definition->adhesion[1], 0.1f), 1.0f);
            inputs[formats::SoundVariable::variable2] = std::abs(state.force) / limit;
            inputs[formats::SoundVariable::variable3] = state.cylinder / 5.0f;
        }
        const bool shared = !unit.remote && shared_sound_.contains(unit.train->id());
        if (shared) {
            for (int k = 0; k < 8; ++k)
                car.net_vars[static_cast<std::size_t>(k)] =
                    inputs.variables[static_cast<std::size_t>(formats::SoundVariable::variable1) + static_cast<std::size_t>(k)];
            car.net_travelled = inputs[formats::SoundVariable::distance_travelled];
        }
        if (car.sound) {
            inputs.events = car.pending_events;
            inputs.discrete = car.pending_triggers;
            car.sound->update(elapsed, inputs, sound);
            inputs.events = {};
            inputs.discrete = {};
        }
        if (shared) {
            // The codes the exterior sound took through its trigger pairs
            // (SoundInstance::passed): a pair's On code a DLL posts every step
            // goes once, as rts1.exe sends them. Without a sound object (sound
            // off) the codes as posted.
            const auto* group = car.sound ? &car.sound->group() : nullptr;
            for (const auto code : car.sound ? car.sound->passed() : car.pending_triggers) {
                // NonDistributableTriggers: not sent over the network (RTS).
                if (group && std::find(group->non_distributable.begin(), group->non_distributable.end(), code) !=
                                 group->non_distributable.end())
                    continue;
                if (car.net_triggers.size() < 96) car.net_triggers.push_back(code); // RTS's queue holds 96
            }
        }
        car.pending_events.clear();
        car.pending_triggers.clear();
        update_cab_sound(unit, u, i, inputs, elapsed, camera, sound);
        update_aux_sounds(unit, u, i, inputs, elapsed, camera, sound);
        // The scenario's sounds on the car (SendSoundTrigger): heard as its
        // cab's while the player is in it, else from the car.
        for (auto& extra : car.activity_sounds) {
            if (!extra.tried) {
                extra.tried = true;
                auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(*vfs_, extra.path));
                if (script->ok()) {
                    extra.script = script;
                    if (const auto* group = script->select_group(formats::SoundOwner::player_train)) {
                        auto lookup = [this, path = extra.path](const std::string& name) -> std::shared_ptr<const audio::AudioClip> {
                            const auto file = audio::find_sound_file(*vfs_, path, name);
                            return file ? bank_->get(*file) : nullptr;
                        };
                        extra.instance = std::make_unique<audio::SoundInstance>(script, *group, formats::SoundOwner::player_train,
                                                                                std::move(lookup), 7919u + static_cast<std::uint32_t>(extra.object));
                        extra.instance->preload();
                    }
                } else {
                    std::cout << "activity sound " << extra.path.str() << ": " << script->error << '\n';
                }
            }
            if (!extra.instance) {
                extra.pending.clear();
                continue;
            }
            audio::SoundInputs heard = inputs;
            const bool in_it = aboard(unit, u) && viewing_inside() && i == cab_car_;
            if (in_it) {
                heard.at_listener = true;
                heard.gain = 1.0f;
            }
            heard.discrete = extra.pending;
            heard.events = {};
            extra.instance->update(elapsed, heard, sound);
            extra.pending.clear();
        }
    }
    }
}

void TrainView::stop_sound(audio::AudioEngine& sound) {
    for (auto& unit : units_) {
        for (auto& car : unit.cars) {
            if (car.sound) car.sound->stop(sound);
            if (car.cab_sound) car.cab_sound->stop(sound);
            for (auto& aux : car.aux_sounds)
                if (aux.instance) aux.instance->stop(sound);
            for (auto& extra : car.activity_sounds)
                if (extra.instance) extra.instance->stop(sound);
        }
    }
}

// AuxLibrarySound (docs/sound.md): each library of the player's engines has
// an SMS of its own, fed with what that library posts. SingleWagon: only
// while the player is in that car; External: it fades with distance, else
// it is heard as if at the listener; AMBIENT has no position.
void TrainView::update_aux_sounds(Unit& unit, const std::size_t u, const std::size_t i, const audio::SoundInputs& exterior,
                                  const float elapsed, const ViewerCamera& camera, audio::AudioEngine& sound) {
    auto& car = unit.cars[i];
    // The libraries of every train the DLL host runs: the player's, and the
    // parts cut off from it. RTS makes a library's sound object once, when
    // the library is bound (0x56DC3C), of the kind AuxLibrarySoundType says
    // (WAGON, the default: 5, group 7; 0x4D4B08, 0x4AA540), and updates it
    // every frame whatever its train (0x44AB7C). Only the camera rules see
    // the train: CabCam and PassengerCam need the car in the player's train,
    // ExternalCam a car out of it, SingleWagon the player's own engine.
    const bool player_train = u == 0 && unit.role == Role::driven;
    const bool host = legacy_ && unit.host_train != SIZE_MAX && car.definition->engine;
    if (!host) {
        for (auto& aux : car.aux_sounds) {
            if (aux.instance) aux.instance->stop(sound);
            aux.instance.reset();
        }
        car.pending_aux_triggers.clear();
        return;
    }
    const auto& libraries = car.definition->engine->aux_libraries;
    if (!car.aux_sounds_tried) {
        car.aux_sounds_tried = true;
        for (std::size_t k = 0; k < libraries.size(); ++k) {
            const auto& library = libraries[k];
            if (library.sound.empty()) continue;
            std::vector<core::VirtualPath> candidates;
            if (library.sound_type == formats::AuxSoundType::route_for_wagon) {
                if (auto folder = core::VirtualPath::parse(route_.str() + "/SOUND")) {
                    if (auto path = folder->resolve(library.sound)) candidates.push_back(*path);
                }
            }
            if (auto folder = core::VirtualPath::parse("TRAINS/TRAINSET/" + car.folder)) {
                if (auto path = folder->resolve(library.sound)) candidates.push_back(*path);
            }
            for (const auto& candidate : candidates) {
                if (!vfs_->exists(candidate)) continue;
                auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(*vfs_, candidate));
                if (!script->ok()) {
                    std::cout << "aux sound " << candidate.str() << ": " << script->error << '\n';
                    break;
                }
                car.aux_sounds.push_back({k, std::move(script), candidate, nullptr});
                break;
            }
        }
    }
    const bool in_car = player_train && viewing_inside() && i == cab_car_;
    static const std::vector<std::uint32_t> none;
    const auto owner = formats::SoundOwner::player_train;
    for (auto& aux : car.aux_sounds) {
        const auto& library = libraries[aux.library];
        auto* pending = aux.library < car.pending_aux_triggers.size() ? &car.pending_aux_triggers[aux.library] : nullptr;
        if (library.sound_single_wagon && !in_car) {
            if (aux.instance && aux.instance->active()) aux.instance->stop(sound);
            if (pending) pending->clear();
            continue;
        }
        if (!aux.instance) {
            const auto* group = aux.script->select_group(owner);
            if (!group) {
                if (pending) pending->clear();
                continue;
            }
            auto lookup = [this, path = aux.path](const std::string& name) -> std::shared_ptr<const audio::AudioClip> {
                const auto file = audio::find_sound_file(*vfs_, path, name);
                return file ? bank_->get(*file) : nullptr;
            };
            aux.instance = std::make_unique<audio::SoundInstance>(aux.script, *group, owner, std::move(lookup),
                                                                  static_cast<std::uint32_t>(7907u * (i + 1) + aux.library));
            aux.instance->preload();
            if (sound_trace())
                aux.instance->set_trace((u == 0 ? std::string() : "train " + std::to_string(u + 1) + " ") + "car " + std::to_string(i + 1)
                                        + " " + car.definition->name + " library " + library.name);
        }
        audio::SoundInputs inputs = exterior;
        inputs.camera = player_train ? sound_camera() : audio::SoundCamera::external;
        const auto here = pose(unit, i);
        const glm::vec3 centre = (here.front + here.rear) * 0.5f;
        glm::vec3 forward = here.front - here.rear;
        forward = glm::length(forward) > 1e-3f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        glm::vec3 right = glm::cross(up, forward);
        right = glm::length(right) > 1e-3f ? glm::normalize(right) : glm::vec3(1.0f, 0.0f, 0.0f);
        const auto& offset = library.sound_position;
        const glm::vec3 at = centre + right * offset[0] + up * offset[1] + forward * offset[2];
        const float distance = glm::distance(camera.eye(), at);
        inputs[formats::SoundVariable::distance] = library.sound_type == formats::AuxSoundType::ambient ? 0.0f : distance;
        // No fading: heard at the listener (the distance still activates it).
        // RTS places no sound: one that is not external does not fade with
        // distance, in any train.
        inputs.at_listener = library.sound_type == formats::AuxSoundType::ambient || !library.sound_external;
        inputs.position = inputs.at_listener ? camera.eye() : at;
        // A library sound placed in the car the engineer is in passes from
        // its own room to his (0x44AB7C: objects with a place in the car,
        // 0x56DC3C flag 0x40); the others as anything outside.
        if (listener_cab_ && u == 0 && i == engineer_car_ && library.sound_type != formats::AuxSoundType::ambient) {
            const bool turned = listener_cab_->turned();
            const float x = turned ? -offset[0] : offset[0], z = turned ? -offset[2] : offset[2];
            const int room = listener_cab_->acoustics().room_at(x, z);
            if (room != audio::InteriorAcoustics::outside) inputs.gain = listener_cab_->sound_transmission(room, listener_room_);
        }
        const auto t = unit.host_train;
        const auto* vars = library.sound_own_stack ? legacy_->aux_sound_variables(t, i, aux.library) : legacy_->sound_variables(t, i);
        if (vars) {
            for (int k = 0; k < 8; ++k)
                inputs.variables[static_cast<std::size_t>(formats::SoundVariable::variable1) + static_cast<std::size_t>(k)] =
                    (*vars)[static_cast<std::size_t>(k)];
        }
        inputs.events = {};
        inputs.discrete = pending ? std::span<const std::uint32_t>(*pending) : std::span<const std::uint32_t>(none);
        aux.instance->update(elapsed, inputs, sound);
        if (pending) pending->clear();
    }
}

// The cab sound (RTSCabinSound, docs/sound.md) of the engine the player
// drives: its own SMS, the DLL's cab triggers and Var1..Var8, heard at the
// driver's seat. Its streams' CabCam activation keeps it silent outside.
void TrainView::update_cab_sound(Unit& unit, const std::size_t u, const std::size_t i, const audio::SoundInputs& exterior,
                                 const float elapsed, const ViewerCamera& camera, audio::AudioEngine& sound) {
    auto& car = unit.cars[i];
    const bool mine = aboard(unit, u) && i == cab_car_;
    // The player's other engines with a DLL keep their cab sound, silent:
    // what their DLLs post to it waits (RTS's queue of a sound object that
    // is off, 0x4A8284), so the loops the second section's DLL started long
    // before (its fans, the compressor) go on when the engineer drives from
    // there (testers 2026-11-15: its cab was nearly silent).
    const bool kept = !mine && u == 0 && unit.role == Role::driven && dll_car(0, i) && !car.definition->cab_sound.empty();
    if (!mine && !kept) {
        if (car.cab_sound) {
            car.cab_sound->stop(sound);
            car.cab_sound.reset();
        }
        car.pending_cab_triggers.clear();
        return;
    }
    if (!car.cab_script_tried && !car.definition->cab_sound.empty()) {
        car.cab_script_tried = true;
        std::vector<core::VirtualPath> candidates;
        if (auto folder = core::VirtualPath::parse("TRAINS/TRAINSET/" + car.folder + "/SOUND")) {
            if (auto path = folder->resolve(car.definition->cab_sound)) candidates.push_back(*path);
        }
        if (auto folder = core::VirtualPath::parse("TRAINS/TRAINSET/" + car.folder)) {
            if (auto path = folder->resolve(car.definition->cab_sound)) candidates.push_back(*path);
        }
        if (auto global = core::VirtualPath::parse("SOUND")->resolve(car.definition->cab_sound)) candidates.push_back(*global);
        for (const auto& candidate : candidates) {
            if (!vfs_->exists(candidate)) continue;
            auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(*vfs_, candidate));
            if (!script->ok()) break;
            car.cab_script = script;
            car.cab_script_path = candidate;
            break;
        }
    }
    if (car.cab_script && !car.cab_sound) {
        const auto owner = formats::SoundOwner::player_train;
        if (const auto* group = car.cab_script->select_group(owner)) {
            auto lookup = [this, path = car.cab_script_path](const std::string& name) -> std::shared_ptr<const audio::AudioClip> {
                const auto file = audio::find_sound_file(*vfs_, path, name);
                return file ? bank_->get(*file) : nullptr;
            };
            car.cab_sound = std::make_unique<audio::SoundInstance>(car.cab_script, *group, owner, std::move(lookup), 104729u);
            car.cab_sound->preload();
            // OPENRTS_SOUND_TRACE: what its streams do, in the log.
            if (sound_trace()) car.cab_sound->set_trace("cab " + car.definition->name);
        }
    }
    if (!car.cab_sound) {
        car.pending_cab_triggers.clear();
        return;
    }
    if (kept) {
        // Its CabCam streams off (the passenger camera) and nothing heard of
        // one without an Activation; its triggers and variables kept.
        audio::SoundInputs inputs = exterior;
        inputs.camera = audio::SoundCamera::passenger;
        inputs.gain = 0.0f;
        if (const auto* vars = legacy_ ? legacy_->cab_sound_variables(i) : nullptr) {
            for (int k = 0; k < 8; ++k)
                inputs.variables[static_cast<std::size_t>(formats::SoundVariable::variable1) + static_cast<std::size_t>(k)] =
                    (*vars)[static_cast<std::size_t>(k)];
        }
        inputs.events = {};
        inputs.discrete = car.pending_cab_triggers;
        car.cab_sound->update(elapsed, inputs, sound);
        car.pending_cab_triggers.clear();
        return;
    }
    audio::SoundInputs inputs = exterior;
    inputs.camera = sound_camera();
    // The cab script is the cab's own: RTS sets its volume itself (0x44B6B4),
    // without the outside share. With the head out of a window it keeps
    // playing, only its streams allowed there (AllowStreamOnWindowCamera).
    inputs.gain = 1.0f;
    if (engineer_.heading_out() && engineer_car_ == i && u == 0) {
        inputs.camera = audio::SoundCamera::cab;
        inputs.window = true;
    }
    // SoundSources: each point's volume times what reaches the listener's
    // room from its room, and its distance (0x44B6B4). RTS takes the points
    // in the car and turns them with a turned cab; the listener here is in
    // the cab's own frame (update_listener: the turn is in its placement), so
    // the points are taken as written. Turned again, a turned cab's points
    // went to its back wall: the ЭД9М's rear cab heard its reverser,
    // controller and switches muffled (testers 2026-11-18).
    source_levels_.clear();
    if (listener_cab_ && engineer_car_ == i && u == 0) {
        for (const auto& point : car.cab_sound->group().sources) {
            const glm::vec3 at{point.x, point.y, point.z};
            const int room = listener_cab_->acoustics().room_at(at.x, at.z);
            source_levels_.push_back({.level = point.volume * listener_cab_->sound_transmission(listener_room_, room),
                                      .distance = glm::distance(at, listener_local_)});
        }
    }
    inputs.sources = source_levels_;
    const bool inside = inputs.camera != audio::SoundCamera::external;
    // Heard at the seat when inside, at the cab end of the body otherwise.
    if (inside) {
        inputs.position = camera.eye();
        inputs.at_listener = true;
    }
    inputs[formats::SoundVariable::distance] = inside ? 0.0f : glm::distance(camera.eye(), inputs.position);
    const auto* helper_out = helping_ ? interior_output(i) : nullptr;
    if (const auto* vars = helper_out ? &helper_out->sms_cab : legacy_ ? legacy_->cab_sound_variables(i) : nullptr) {
        for (int k = 0; k < 8; ++k)
            inputs.variables[static_cast<std::size_t>(formats::SoundVariable::variable1) + static_cast<std::size_t>(k)] =
                (*vars)[static_cast<std::size_t>(k)];
    }
    inputs.events = {};
    inputs.discrete = car.pending_cab_triggers;
    car.cab_sound->update(elapsed, inputs, sound);
    car.pending_cab_triggers.clear();
}

void TrainView::submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library, const SubmitView& view) {
    OPENRTS_ZONE("trains: submit");
    screen_batches_.clear();
    screen_planes_.clear();
    cab_to_scene_.reset();
    cab_focus_.reset();
    cab_room_.reset();
    if (!placed()) return;
    const float t = alpha();
    std::vector<renderer::NodeAdjustment> adjustments;
    for (auto& unit : units_) {
    if (unit.current.empty()) continue;
    const auto& states = unit.train->cars();
    for (std::size_t i = 0; i < unit.cars.size() && i < unit.current.size() && i < states.size(); ++i) {
        auto& car = unit.cars[i];
        auto* model = library.find(car.shape);
        const bool engineer_here = viewing_inside() && &unit == &units_.front() && i == engineer_car_;
        // The cab is drawn even when the car's own shape is missing.
        if (!model && !engineer_here) continue;
        const auto body = body_pose(unit, i, !engineer_here);
        if (!body) continue;
        const auto& here = body->here;
        const auto& state = states[i];
        const float body_heading = body->heading;
        const auto& transform = body->transform;
        // Out of view (or too far): nothing, or its shadow without a pose.
        if (!engineer_here) {
            const auto bounds = model->bounds();
            const glm::vec3 centre{transform * glm::vec4{bounds.center, 1.0f}};
            if (view.draw_distance > 0.0f) {
                const float dx = centre.x - view.eye.x, dz = centre.z - view.eye.z;
                const float limit = view.draw_distance + bounds.radius;
                if (dx * dx + dz * dz > limit * limit) continue;
            }
            if (view.frustum && !view.frustum->intersects_sphere(centre, bounds.radius)) {
                if (view.shadow_range > 0.0f && glm::distance(centre, view.eye) - bounds.radius < view.shadow_range) {
                    renderer.submit_shadow_caster(*model, transform);
                }
                continue;
            }
        }
        const auto* inside = viewing_inside() && &unit == &units_.front() && i == engineer_car_ ? engineer_cab() : nullptr;
        if (inside) {
            // In the cab: the cab shape instead of the body (RTS hides the
            // exterior unless the engine says DoNotHideExtInCabin); the body
            // until the cab shape is there.
            if (auto* cab_model = library.find(inside->shape()); cab_model && inside->bound()) {
                adjustments.clear();
                const CabView::Blend blend{&cab_switch_frames_before_, &cab_display_values_before_, alpha(),
                                           legacy_ ? legacy_->dragged_switch() : std::nullopt};
                const auto gauges = air_gauges(unit, i);
                inside->adjustments(&unit == &units_.front() ? cab_state(i) : nullptr, adjustments, signal_aspect(unit),
                                    i == cab_before_car_ ? &blend : nullptr, gauges ? &*gauges : nullptr);
                const glm::mat4 to_scene = transform * inside->placement();
                // TextureChanges a DLL switched on (TexChange SetState): the
                // cab's image TexID drawn as the change's file.
                std::vector<renderer::TextureOverride> textures;
                if (const auto* out = &unit == &units_.front() ? interior_output(i) : nullptr) {
                    const auto& changes = inside->description().texture_changes;
                    for (std::size_t k = 0; k < changes.size() && k < out->tex_changes.size(); ++k) {
                        if (out->tex_changes[k] == 0 || changes[k].file.empty()) continue;
                        const auto path = inside->shape_file().parent().resolve(changes[k].file);
                        if (!path) continue;
                        auto& texture = cab_textures_[path->key()];
                        if (!texture) texture = renderer.texture(*vfs_, *path);
                        if (texture) textures.push_back({changes[k].texture, texture});
                    }
                }
                // RTS draws the cab in object pass 3 (0x438D50).
                renderer.submit(*cab_model, to_scene, adjustments, textures, renderer::ObjectPass::last);
                cab_texture_changes_ = textures.size();
                cab_to_scene_ = to_scene;
                if (const auto box = cab_model->bounds(); box.max.x > box.min.x)
                    cab_room_ = renderer::LightShaftRoom{.to_room = glm::inverse(to_scene), .lower = box.min, .upper = box.max};
                if (engineer_.mode() != Engineer::Mode::seated) {
                    // On foot (in a salon, the ED9M's head car's too, or the
                    // cab away from the desk): the whole room, the shape's
                    // box, fixed while he walks. The desk's 4.5 m round the
                    // eye left the rest of a 25 m car to lamps shining
                    // through its walls, and the sphere moving on every
                    // metre made their shadows come in steps.
                    const auto box = cab_model->bounds();
                    cab_focus_ = renderer::ShadowFocus{
                        .centre = glm::vec3{to_scene * glm::vec4{box.center, 1.0f}},
                        .radius = std::clamp(box.radius, 4.5f, 16.0f),
                        .axis = glm::vec3{to_scene[2]},
                    };
                    cab_focus_anchor_.reset();
                } else {
                    // 4.5 m around the eye: the desk, the windows and the
                    // seat; the rest of the cab is left to the cascades.
                    constexpr float focus_radius = 4.5f;
                    const glm::vec3 eye_in_cab{glm::inverse(to_scene) * glm::vec4{view.eye, 1.0f}};
                    if (!cab_focus_anchor_ || cab_focus_cab_ != inside || glm::distance(*cab_focus_anchor_, eye_in_cab) > 1.0f) {
                        cab_focus_anchor_ = eye_in_cab;
                        cab_focus_cab_ = inside;
                    }
                    cab_focus_ = renderer::ShadowFocus{
                        .centre = glm::vec3{to_scene * glm::vec4{*cab_focus_anchor_, 1.0f}},
                        .radius = focus_radius,
                        .axis = glm::vec3{to_scene[2]},
                    };
                }
                if (auto* cab = engineer_cab()) {
                    const auto first = screen_batches_.size();
                    const auto first_plane = screen_planes_.size();
                    cab->screens(cab_state(i), frame_elapsed_, screen_batches_, &screen_planes_);
                    for (auto b = first; b < screen_batches_.size(); ++b) {
                        for (auto& v : screen_batches_[b].vertices) v.position = glm::vec3{to_scene * glm::vec4{v.position, 1.0f}};
                    }
                    const auto place = [&](glm::vec3& p) { p = glm::vec3{to_scene * glm::vec4{p, 1.0f}}; };
                    for (auto p = first_plane; p < screen_planes_.size(); ++p) {
                        place(screen_planes_[p].top_left);
                        place(screen_planes_[p].top_right);
                        place(screen_planes_[p].bottom_right);
                    }
                }
                // RTS 0x438D50: the exterior comes back with the head out of
                // the window; DoNotHideExtInCabin keeps it in the cab,
                // DoNotHideExtInSalon in a passenger car's salon.
                const bool keep = inside->salon() ? car.definition->keep_exterior_in_salon != 0
                                                  : car.definition->engine && car.definition->engine->keep_exterior_in_cab != 0;
                if (!engineer_.heading_out() && !keep) {
                    // openRTS: through an open passage door his car's half
                    // of the gangway (its outside between its end wall and
                    // the next car's), no shadow (ModelPortal).
                    if (model && !neighbour_rooms_now_.empty()) {
                        adjustments.clear();
                        for (const auto& door : car.exterior_doors) {
                            if (door.last_frame > 0.0f) adjustments.push_back({.matrix = door.node, .frame = door.open * door.last_frame});
                        }
                        for (const auto& room : neighbour_rooms_now_) {
                            renderer.submit(*model, transform, adjustments);
                            renderer.set_portal(static_cast<std::int32_t>(9 + room.portal));
                        }
                    }
                    continue;
                }
            }
        }
        if (!model) continue;

        adjustments.clear();
        // Wheels turn by the distance their bogie's wheels rolled (they stand
        // while it slides, юз), interpolated like the body.
        const double radius = car.definition->wheel_radius > 0.05f ? car.definition->wheel_radius : 0.5;
        float bogie_angle[2]{};
        for (std::size_t b = 0; b < 2; ++b) {
            const double rolled = i < unit.current_rolled.size() && i < unit.previous_rolled.size()
                ? unit.previous_rolled[i][b] + (unit.current_rolled[i][b] - unit.previous_rolled[i][b]) * t
                : state.rolled[b];
            bogie_angle[b] = static_cast<float>(std::fmod(rolled / radius, 2.0 * pi)) * (car.flipped ? -1.0f : 1.0f);
        }
        // The body's move on its springs taken back: wheels and bogies stay
        // on the rails under it.
        const bool moved = body->motion != glm::mat4{1.0f};
        const glm::mat4 back = moved ? glm::inverse(body->motion) : glm::mat4{1.0f};
        const auto on_rails = [&](const glm::mat4& rest) { return glm::inverse(rest) * back * rest; };
        for (std::size_t w = 0; w < car.wheels.size(); ++w) {
            // Its bogie by where it stands: the shape's front half (+Z, the
            // car's own front) is bogie 0.
            const std::size_t bogie = w < car.wheel_rest.size() && car.wheel_rest[w][3].z < 0.0f ? 1 : 0;
            // And what it spun slipping (along the car's own front, as the run here).
            const float spun = w < car.wheel_spin.size() ? car.wheel_spin[w] : 0.0f;
            const auto turn = glm::rotate(glm::mat4{1.0f}, bogie_angle[bogie] + spun, glm::vec3{1.0f, 0.0f, 0.0f});
            const bool loose = moved && w < car.wheel_loose.size() && car.wheel_loose[w];
            adjustments.push_back({.matrix = car.wheels[w], .local = loose ? on_rails(car.wheel_rest[w]) * turn : turn});
        }
        if (car.pantograph_frames > 1) {
            for (const auto& [node, number] : car.pantograph_nodes) {
                adjustments.push_back({.matrix = node, .frame = car.pantograph_frame[number - 1u]});
            }
        }
        for (const auto& door : car.exterior_doors) {
            if (door.last_frame > 0.0f) adjustments.push_back({.matrix = door.node, .frame = door.open * door.last_frame});
        }
        // A wheel chock is drawn only while it lies at its end of the car
        // (the chock flags go by the train's front: a turned car's shape
        // front is the train's rear).
        for (const auto& [node, shape_front] : car.chock_nodes) {
            const auto chock = shape_front != car.flipped ? simulation::chock_front : simulation::chock_rear;
            if ((state.chocks & chock) == 0) adjustments.push_back({.matrix = node, .hidden = true});
        }
        for (std::size_t k = 0; k < car.bogies.size(); ++k) {
            const auto& [matrix, shape_front] = car.bogies[k];
            // A bogie follows the track heading at its own position.
            const bool train_front = shape_front != car.flipped;
            float delta = wrap_angle((train_front ? here.front_heading : here.rear_heading) - body_heading);
            if (std::abs(delta) > 0.5f) delta = wrap_angle(delta + pi); // a heading along the node's other direction
            if (std::abs(delta) > 0.5f) delta = 0.0f;
            const auto turn = glm::rotate(glm::mat4{1.0f}, delta, glm::vec3{0.0f, 1.0f, 0.0f});
            adjustments.push_back({.matrix = matrix, .local = moved && k < car.bogie_rest.size() ? on_rails(car.bogie_rest[k]) * turn : turn});
        }
        renderer.submit(*model, transform, adjustments);
        // openRTS: the next car's salon or cab seen through the open passage
        // door of the car he is in (update_neighbour_rooms): only through the
        // doorway, and the car's outside everywhere but there.
        if (&unit == &units_.front()) {
            for (const auto& room : neighbour_rooms_now_) {
                if (room.car != i) continue;
                const auto* room_cab = car_cab(0, i, false);
                auto* room_model = room_cab ? library.find(room_cab->shape()) : nullptr;
                if (!room_model || !room_cab->bound()) continue;
                const auto code = static_cast<std::int32_t>(room.portal + 1);
                renderer.set_portal(-code);
                std::vector<renderer::NodeAdjustment> room_adjustments;
                const auto room_gauges = air_gauges(unit, i);
                room_cab->adjustments(cab_state(i), room_adjustments, signal_aspect(unit), nullptr, room_gauges ? &*room_gauges : nullptr);
                renderer.submit(*room_model, transform * room_cab->placement(), room_adjustments);
                renderer.set_portal(code);
            }
        }
        // FreightAnim: objects fastened to the car, at FreightAnimPosition.
        for (std::size_t k = 0; k < car.freight_shapes.size() && k < car.freight.size(); ++k) {
            if (car.freight_shapes[k].str().empty()) continue;
            auto* freight = library.find(car.freight_shapes[k]);
            if (!freight) continue;
            const auto& p = car.freight[k].position;
            renderer.submit(*freight, transform * glm::translate(glm::mat4{1.0f}, glm::vec3{p[0], p[1], p[2]}));
        }
    }
    }
}

TrainView::BodyOffset TrainView::body_sway(const Unit& unit, const std::size_t i) const {
    if (i < unit.current_body.size()) {
        const auto& now = unit.current_body[i];
        const auto& before = i < unit.previous_body.size() ? unit.previous_body[i] : now;
        const float t = alpha();
        return {glm::mix(before.shift, now.shift, t), glm::mix(before.turn, now.turn, t)};
    }
    if (i >= unit.train->cars().size()) return {};
    const auto& car = unit.train->cars()[i];
    return {glm::vec3{car.body[0], car.body[1], car.body[2]}, glm::vec3{car.body_turn[0], car.body_turn[1], car.body_turn[2]}};
}

float TrainView::cab_sway_share(const Unit& unit) const {
    if (!unit.train || unit.train->settings().body_motion != simulation::BodyMotionModel::rts) return 1.0f;
    const float mass = rts_config_value(rts_config_, "engineer.mass").value_or(90.0f);
    return mass / 180.0f;
}

glm::vec3 TrainView::sway_offset(const Unit& unit, const std::size_t i, const glm::vec3& eye) const {
    if (sway_scale_ <= 0.0f || i >= unit.cars.size() || i >= unit.train->cars().size()) return {};
    const auto rest = body_pose(unit, i, false);
    const auto moving = body_pose(unit, i, true);
    if (!rest || !moving) return {};
    const glm::vec3 carried{moving->transform * glm::inverse(rest->transform) * glm::vec4{eye, 1.0f}};
    return (carried - eye) * cab_sway_share(unit);
}

void TrainView::move_head(const Unit& unit, const std::size_t i, const float elapsed, ViewerCamera& camera) {
    const float amount = head_motion();
    if (amount <= 0.0f || i >= unit.cars.size() || i >= unit.train->cars().size()) {
        head_.started = false;
        return;
    }
    const auto& state = unit.train->cars()[i];
    // The body as the camera is carried by it (the sway slider's size).
    const glm::vec3 body = body_sway(unit, i).shift * sway_scale_ * cab_sway_share(unit);
    // At rest in a car he entered; after a long frame (a load, a hitch) not
    // thrown by it, nor snapped back to rest: until 2026-11-18 a hitch reset
    // the head, and on a stop's nod (some 8 cm and 7 degrees) the camera
    // jumped (testers: "камеру даже иногда телепортирует").
    if (!head_.started || head_car_ != unit.cars[i].key) {
        head_.reset(body);
        head_car_ = unit.cars[i].key;
    }
    // The train's own acceleration in the car's axes: towards the curve's
    // inside v^2 / R, along the front its acceleration.
    const glm::vec3 carried{state.velocity * state.velocity * state.curvature, 0.0f, state.acceleration};
    if (elapsed > 0.25f) {
        head_.hold(body);
    } else {
        head_.step(body, carried, elapsed);
    }
    const auto here = pose(unit, i);
    const auto axis = here.front - here.rear;
    const float span = glm::length(axis);
    if (span < 0.01f) return;
    const float heading = std::atan2(axis.x, axis.z);
    const glm::vec3 turning{std::cos(heading), 0.0f, -std::sin(heading)};
    const auto lag = head_.lag();
    const glm::vec3 lag_world = turning * lag.x + glm::vec3{0.0f, lag.y, 0.0f} + axis / span * lag.z;
    const glm::vec3 forward{-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw)};
    const auto head = head_pose(lag_world, lag.y, forward, amount);
    // And the lean into the seat with the train's smoothed acceleration.
    const auto seat = seat_lean(head_.lean, axis / span, forward, amount);
    camera.position += head.shift + seat.shift;
    camera.head_nod = head.nod + seat.nod;
    camera.head_tilt = head.tilt;
}

std::optional<glm::mat4> TrainView::car_transform(const std::size_t car) const {
    if (units_.empty() || car >= units_[0].cars.size()) return std::nullopt;
    const auto body = body_pose(units_[0], car, false);
    if (!body) return std::nullopt;
    return body->transform;
}

std::optional<TrainView::BodyPose> TrainView::body_pose(const Unit& unit, const std::size_t i, const bool sway) const {
    const auto& car = unit.cars[i];
    const auto& state = unit.train->cars()[i];
    auto here = pose(unit, i);
    if (const auto slid = slid_pose(unit, i)) here = *slid;
    const float ahead = state.bogie_ahead > 0.0f ? state.bogie_ahead : car.definition->length * 0.35f;
    const float behind = state.bogie_behind > 0.0f ? state.bogie_behind : car.definition->length * 0.35f;
    const auto axis = here.front - here.rear;
    const float span = glm::length(axis);
    if (span < 0.01f) return std::nullopt;
    auto centre = here.rear + axis * (behind / (ahead + behind));
    const float body_heading = std::atan2(axis.x, axis.z);
    const float up = std::asin(std::clamp(axis.y / span, -1.0f, 1.0f));
    // The body on its springs (simulation: RTS 0x4F5B34 or openRTS's
    // body_motion), in its own axes (lateral, up, along the train front):
    // shifted, and turned about a point 1.1 m above the rails (roll, pitch,
    // yaw).
    glm::mat4 motion{1.0f};
    if (sway && sway_scale_ > 0.0f) {
        const auto body = body_sway(unit, i);
        const glm::vec3 shift = body.shift * sway_scale_;
        const glm::vec3 turn = body.turn * sway_scale_;
        motion = glm::translate(glm::mat4{1.0f}, shift);
        if (turn != glm::vec3{0.0f}) {
            constexpr float pivot = 1.1f;
            motion = glm::translate(motion, glm::vec3{0.0f, pivot, 0.0f});
            motion = glm::rotate(motion, turn.z, glm::vec3{0.0f, 1.0f, 0.0f});
            motion = glm::rotate(motion, -turn.y, glm::vec3{1.0f, 0.0f, 0.0f});
            motion = glm::rotate(motion, -turn.x, glm::vec3{0.0f, 0.0f, 1.0f});
            motion = glm::translate(motion, glm::vec3{0.0f, -pivot, 0.0f});
        }
    }
    // A turned car: its shape's front to the train's rear.
    const glm::mat4 flip = car.flipped ? glm::rotate(glm::mat4{1.0f}, pi, glm::vec3{0.0f, 1.0f, 0.0f}) : glm::mat4{1.0f};
    auto transform = glm::mat4{1.0f};
    if (state.derail != simulation::DerailKind::none && state.tilt != 0.0f) {
        // Overturning about the rail on the side it falls to: the roll
        // turns the body towards the side the car turned to for a
        // positive tilt (see CarState::tilt).
        const glm::vec3 turning{std::cos(body_heading), 0.0f, -std::sin(body_heading)};
        const auto pivot = centre + turning * (state.tilt > 0.0f ? 0.75f : -0.75f);
        const auto along = glm::normalize(glm::vec3{axis.x, 0.0f, axis.z});
        // Ground contact (RTS 0x58A390, its body against the ground):
        // where the body lands lies lower than the rail (an embankment),
        // the car goes down onto it as it rolls.
        const auto landing = centre + turning * (state.tilt > 0.0f ? 2.5f : -2.5f);
        if (const auto ground = ground_height(landing)) {
            const float drop = std::clamp(*ground + 0.2f - pivot.y, -6.0f, 0.0f);
            transform = glm::translate(transform, glm::vec3{0.0f, drop * (std::min)(1.0f, std::abs(state.tilt) / 1.2f), 0.0f});
        }
        transform = glm::translate(transform, pivot);
        transform = glm::rotate(transform, -state.tilt, along);
        transform = glm::translate(transform, -pivot);
    }
    transform = glm::translate(transform, centre);
    transform = glm::rotate(transform, body_heading, glm::vec3{0.0f, 1.0f, 0.0f});
    transform = glm::rotate(transform, -up, glm::vec3{1.0f, 0.0f, 0.0f});
    transform = glm::rotate(transform, -state.cant, glm::vec3{0.0f, 0.0f, 1.0f});
    transform = transform * motion * flip;
    return BodyPose{.transform = transform, .here = here, .heading = body_heading, .motion = glm::inverse(flip) * motion * flip};
}

void TrainView::list_consists() {
    consists_listed_ = true;
    std::function<void(const core::VirtualPath&)> walk = [&](const core::VirtualPath& directory) {
        for (const auto& entry : vfs_->list(directory)) {
            if (entry.directory) {
                walk(entry.path);
                continue;
            }
            const auto name = lower(std::string(entry.path.filename()));
            if (name.size() > 4 && name.ends_with(".con")) consists_.push_back(entry.path.str());
        }
    };
    for (const auto* folder : {"TRAINS/CONSISTS_RTS", "TRAINS/CONSISTS"}) {
        if (const auto path = core::VirtualPath::parse(folder)) walk(*path);
    }
    std::sort(consists_.begin(), consists_.end(), [](const std::string& a, const std::string& b) {
        return lower(a.substr(a.find_last_of('/') + 1)) < lower(b.substr(b.find_last_of('/') + 1));
    });
}

void TrainView::draw_cab_controls() {
#ifdef OPENRTS_HAS_IMGUI
    if (!cab_) return;
    bool on = engineer_.active();
    if (ImGui::Checkbox(tr("3D cab view"), &on)) set_engineer(on);
    if (on && !cab_->bound()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("(loading the cab)"));
    } else if (on) {
        ImGui::SameLine();
        ImGui::TextDisabled(tr("(%zu controls in the shape)"), cab_->switches_bound());
    }
    if (on) {
        ImGui::TextDisabled("%s", tr("Arrows: walk (Shift: run), Enter: stand up / sit down / a door, "
                                     "Ctrl+Enter: steps, ladders, through a door, RMB: look round, Alt: captions of the controls"));
        ImGui::TextDisabled("%s", trf("Mouse: a click works a switch, the wheel turns it a step; %s: the window, %s: head out",
                                      window_key_name(), head_out_key_name()).c_str());
        ImGui::Checkbox(tr("On foot outside: collide with objects (off: only the ground and trains)"), &walk_objects_);
        ImGui::Checkbox(tr("Through an open passage door: the next car's salon or cab"), &neighbour_rooms_);
        ImGui::Checkbox(tr("And through a shut one (its glass)"), &neighbour_rooms_shut_);
        // The paces (config.ini engineer.velocity / engineer.velocity1, as RTS), for this session.
        float walk = engineer_.walk_speed(), run = engineer_.run_speed();
        ImGui::SetNextItemWidth(160.0f);
        bool changed = ImGui::SliderFloat(tr("Walk, m/s"), &walk, 0.5f, 10.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        changed |= ImGui::SliderFloat(tr("With Shift, m/s"), &run, 1.0f, 150.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        if (changed) engineer_.set_speeds(walk, run);
        ImGui::TextDisabled("%s", tr("End: slower, Shift+End: twice the Shift pace. From config.ini (engineer.velocity, engineer.velocity1)."));
    }
#endif
}

void TrainView::draw_overlay() {
#ifdef OPENRTS_HAS_IMGUI
    if (legacy_ && panels_shown()) legacy_->draw_window([this] { draw_cab_controls(); });
    // What Enter does where the engineer stands (unless the hints are off).
    if (key_hints() && engineer_.active() && car_view_ == CarView::off && (!board_prompt_.empty() || !engineer_.prompt().empty())) {
        const auto size = ImGui::GetIO().DisplaySize;
        const char* text = !board_prompt_.empty() ? board_prompt_.c_str() : engineer_.prompt().c_str();
        const auto text_size = ImGui::CalcTextSize(text);
        const ImVec2 corner{(size.x - text_size.x) * 0.5f, size.y * 0.78f};
        auto* list = ImGui::GetForegroundDrawList();
        list->AddRectFilled({corner.x - 8.0f, corner.y - 4.0f}, {corner.x + text_size.x + 8.0f, corner.y + text_size.y + 4.0f},
                            0xa0000000u, 4.0f);
        list->AddText(corner, 0xffffffffu, text);
    }
#endif
}

void TrainView::draw_panel() {
#ifdef OPENRTS_HAS_IMGUI
    // The buttons and switches first.
    // The network's own trains: walk to a cab door, or as a reserve "Drive".
    if (!boardable_.empty()) {
        ImGui::TextUnformatted(tr("Your trains (network): on foot, Enter at a cab door takes one"));
        for (const auto& unit : units_) {
            const auto id = unit.train->id();
            if (!boardable_.contains(id)) continue;
            ImGui::PushID(static_cast<int>(id));
            ImGui::BulletText("%s", unit.name.c_str());
            ImGui::SameLine();
            if (is_player(id)) {
                ImGui::TextDisabled("(%s)", tr("driven"));
            } else if (ImGui::SmallButton(tr("Drive"))) {
                if (const auto problem = board(id); !problem.empty()) message_ = tr(problem.c_str());
            }
            ImGui::PopID();
        }
    }
    draw_cab_controls();
    if (legacy_) {
        // With the free camera a choice (they take its WASD); on foot always on.
        bool keys = legacy_->keyboard();
        ImGui::BeginDisabled(legacy_->on_foot());
        if (ImGui::Checkbox(tr("Cab keys of the locomotive DLL (on foot always on)"), &keys)) legacy_->set_keyboard(keys);
        ImGui::EndDisabled();
    } else if (!legacy_status_.empty()) {
        ImGui::TextWrapped("%s %s", tr("Locomotive DLLs do not run:"), legacy_status_.c_str());
    }

    // Paths, consists and situations put and replace trains here: not in a
    // network game, where the server and its dispatchers own them.
    if (!network_game_) {
        if (!paths_.empty()) {
            path_choice_ = std::clamp(path_choice_, 0, static_cast<int>(paths_.size()) - 1);
            const auto label = [](const formats::TrainPath& path) {
                return path.file + (path.name.empty() ? "" : ": " + path.name);
            };
            if (ImGui::BeginCombo(tr("Path"), label(paths_[static_cast<std::size_t>(path_choice_)]).c_str())) {
                for (int i = 0; i < static_cast<int>(paths_.size()); ++i) {
                    if (ImGui::Selectable(label(paths_[static_cast<std::size_t>(i)]).c_str(), i == path_choice_)) path_choice_ = i;
                }
                ImGui::EndCombo();
            }
            if (loaded() && ImGui::Button(tr("Put on the path start"))) pending_path_ = true;
        } else {
            ImGui::TextDisabled(tr("The route has no paths (PATHS/*.pat)."));
        }
        if (ImGui::TreeNode(tr("Consist"))) {
            if (!consists_listed_) list_consists();
            ImGui::InputText(tr("Filter"), filter_, sizeof(filter_));
            const auto filter = lower(filter_);
            if (ImGui::BeginListBox("##consists", ImVec2(-1.0f, 160.0f))) {
                int shown = 0;
                for (int i = 0; i < static_cast<int>(consists_.size()) && shown < 300; ++i) {
                    const auto& path = consists_[static_cast<std::size_t>(i)];
                    const auto name = path.substr(path.find_last_of('/') + 1);
                    if (!filter.empty() && lower(name).find(filter) == std::string::npos) continue;
                    ++shown;
                    // The same file name in two folders: each its own item, the folder said.
                    const bool twin = (i > 0 && consists_[static_cast<std::size_t>(i - 1)].ends_with("/" + name)) ||
                                      (i + 1 < static_cast<int>(consists_.size()) && consists_[static_cast<std::size_t>(i + 1)].ends_with("/" + name));
                    const auto label = twin ? name + "  (" + path.substr(0, path.find_last_of('/')) + ")" : name;
                    ImGui::PushID(i);
                    if (ImGui::Selectable(label.c_str(), i == consist_choice_)) consist_choice_ = i;
                    ImGui::PopID();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                }
                ImGui::EndListBox();
            }
            // The DLLs' start: cold (everything off), on (standing ready), running.
            int start = start_state_ == 0 ? 1 : start_state_ == 1 ? 2 : 0;
            const char* starts[] = {tr("running"), tr("cold"), tr("on, standing")};
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo(tr("Start state##consist"), &start, starts, 3)) start_state_ = start == 1 ? 0 : start == 2 ? 1 : -1;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("(at the next load)"));
            if (consist_choice_ >= 0 && consist_choice_ < static_cast<int>(consists_.size()) && ImGui::Button(tr("Load on the path"))) {
                pending_load_ = consists_[static_cast<std::size_t>(consist_choice_)];
            }
            ImGui::TreePop();
        }
        // The trains as they stand: saved, and put back (here or from the start menu).
        if (ImGui::TreeNode(tr("Situations"))) {
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##situation_name", tr("name"), situation_name_.data(), situation_name_.size());
            ImGui::SameLine();
            if (ImGui::Button(tr("Save the trains"))) {
                const std::string name(situation_name_.data());
                const auto error = save_situation(name);
                message_ = error.empty() ? trf("situation %s saved", name.c_str()) : error;
            }
            const auto saved = situations();
            if (saved.empty()) {
                ImGui::TextDisabled("%s", tr("No saved situations on this route yet."));
            } else {
                situation_choice_ = std::clamp(situation_choice_, 0, static_cast<int>(saved.size()) - 1);
                if (ImGui::BeginListBox("##situations", ImVec2(-1.0f, 110.0f))) {
                    for (int i = 0; i < static_cast<int>(saved.size()); ++i) {
                        if (ImGui::Selectable(saved[static_cast<std::size_t>(i)].c_str(), i == situation_choice_)) situation_choice_ = i;
                    }
                    ImGui::EndListBox();
                }
                const auto& chosen = saved[static_cast<std::size_t>(situation_choice_)];
                const auto file = situation_folder() / std::filesystem::path(std::u8string(chosen.begin(), chosen.end())).replace_extension(".situation");
                if (ImGui::Button(tr("Put the trains back"))) request_situation(file);
                ImGui::SameLine();
                if (ImGui::Button(tr("Delete"))) {
                    std::error_code fs_error;
                    std::filesystem::remove(file, fs_error);
                    message_ = trf("situation %s deleted", chosen.c_str());
                }
            }
            ImGui::TreePop();
        }
        draw_placements();
    }
    if (!message_.empty()) ImGui::TextUnformatted(message_.c_str());
    const bool own = loaded() && !units_.front().remote;
    if (own && placed()) {
        const auto& train = driven();
        const auto driven = driven_car();
        const bool dll_driven = driven != SIZE_MAX && dll_car(0, driven);
        ImGui::SeparatorText(tr("Train settings"));
        ImGui::Checkbox(tr("Camera follows (V)"), &follow_);
        if (!dll_driven && driver_) {
            ImGui::SameLine();
            ImGui::Checkbox(tr("EP brake (K)"), &driver_->ep_brake);
        }
        ImGui::SliderFloat(tr("Body sway"), &sway_scale_, 0.0f, 10.0f, "%.1f");
        ImGui::SameLine();
        ImGui::TextDisabled(tr("(1 = RTS model; RTS shows it in the cab only)"));
        if (ImGui::Checkbox(tr("Speed limits without the route's +3 km/h"), &limits_without_tolerance_)) {
            remember("limits_without_tolerance", limits_without_tolerance_ ? "1" : "0");
            limits_->set_without_tolerance(limits_without_tolerance_);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr(limits_without_tolerance_
                                         ? "(103 is 100, 83 is 80, 28 is 25: the order's limit, as a real КЛУБ-У's map has it; for the monitor, F5, the drivers and the DLLs. MaDaDev's КЛУБ then blinks from 3 km/h below the limit, sounds from 2 below and whistles over it: the route's +3 hid that)"
                                         : "(as the route gives them: many give 3 km/h over the order's, 103 for 100)"));
        ImGui::Checkbox(tr("Car loops start at random points"), &random_loop_phase_);
        ImGui::SameLine();
        ImGui::TextDisabled(tr("(openRTS: identical loops on many cars do not add up in phase; RTS starts them together)"));
        if (ImGui::SliderInt(tr("Coupler sub-steps"), &substeps_, 1, 4)) {
            for (auto& unit : units_) unit.train->set_coupler_substeps(static_cast<std::uint32_t>(substeps_));
        }
        ImGui::SameLine();
        ImGui::TextDisabled(substeps_ == 1 ? "(RTS)" : "(experimental)");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("(flow along the pipe, 483 / 292 / 305 distributors, 280 m/s emergency wave, the shoes' friction by speed)"));
        if (!gauges_air_loaded_) {
            gauges_air_loaded_ = true;
            gauges_air_ = remembered("gauges_openrts_air").value_or("1") == "1";
        }
        {
            // One choice for the brakes' settings below (BrakeSet); changed one
            // by one they are the player's own, kept for when chosen again.
            const auto now = brake_set();
            const int preset = now == rts_brakes ? 0 : now == real_brakes ? 1 : 2;
            if (brake_custom_code_.empty()) brake_custom_code_ = remembered("brakes_custom").value_or("");
            if (preset == 2) {
                const auto code = brake_set_code(now);
                if (code != brake_custom_code_) {
                    brake_custom_code_ = code;
                    remember("brakes_custom", code);
                }
            }
            const char* presets[] = {tr("as RTS"), tr("realistic (openRTS)"), tr("my own")};
            int chosen = preset;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo(tr("Brakes##preset"), &chosen, presets, 3) && chosen != preset) {
                if (chosen == 0) {
                    apply_brake_set(rts_brakes);
                } else if (chosen == 1) {
                    apply_brake_set(real_brakes);
                } else if (const auto saved = brake_set_from(brake_custom_code_)) {
                    apply_brake_set(*saved);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr(preset == 0   ? "(RTS's air and adhesion, the DLLs' gauges)"
                                         : preset == 1 ? "(openRTS's air, its valve over Brakes.dll, rails by the weather, real brake forces)"
                                                       : "(your own: the settings below as you set them)"));
        }
        if (ImGui::Checkbox(tr("Resistance by the ПТР"), &ptr_resistance_)) {
            remember("ptr_resistance", ptr_resistance_ ? "1" : "0");
            for (auto& unit : units_)
                unit.train->set_resistance_model(ptr_resistance_ ? simulation::ResistanceModel::ptr
                                                                 : simulation::ResistanceModel::msts);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(tr("(w0 = b0 + (b1 + b2 V + b3 V^2) / q; off: the car's Friction)"));
        if (ImGui::Checkbox(tr("Wheelsets slip"), &axle_dynamics_)) {
            remember("axle_dynamics", axle_dynamics_ ? "1" : "0");
            for (auto& unit : units_) unit.train->set_axle_dynamics(axle_dynamics_);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(tr("(every axle pulls at the creep its own turning allows)"));
        {
            // Trains with locomotive DLLs under the realistic air (docs/train.md).
            const char* modes[] = {tr("RTS air (their valves are made for it)"), tr("the DLL's valve, its УР into the realistic air"),
                                   tr("openRTS valve over Brakes.dll (experimental)")};
            ImGui::SetNextItemWidth(330.0f);
            if (ImGui::Combo(tr("Trains with DLLs##dll_air"), &dll_air_, modes, 3)) {
                dll_air_loaded_ = true;
                remember("dll_air", std::to_string(dll_air_));
                if (legacy_) legacy_->set_air(static_cast<trainplugin::DllAir>(dll_air_));
            }
            ImGui::TextDisabled("%s", tr(dll_air_ == 0 ? "(as RTS: the DLL works the train's pipe and triple valves)"
                                                 : dll_air_ == 1 ? "(the DLL keeps its valve, gauges and protections; the pipe follows its УР, its ЭПК vents it)"
                                                                 : "(the cab's 395 handle (BR_395ID1) works openRTS's valve; the УР gauge shows it; Brakes.dll's ЭПК, КЛУБ and ЭПТ stay)"));
            // Brakes.dll's brakes or openRTS's devices in their place
            // (docs/brakes-dll-replacement.md), and whose numbers.
            const bool can_own = realistic_air_ && dll_air_ != 0;
            ImGui::BeginDisabled(!can_own);
            const char* owners[] = {tr("Brakes.dll"), tr("openRTS (experimental)")};
            ImGui::SetNextItemWidth(330.0f);
            if (ImGui::Combo(tr("Brakes##own_brakes"), &own_brakes_, owners, 2)) {
                own_brakes_loaded_ = true;
                remember("own_brakes", std::to_string(own_brakes_));
            }
            const char* numbers[] = {tr("as Brakes.dll"), tr("openRTS (from the books)")};
            ImGui::SetNextItemWidth(330.0f);
            if (ImGui::Combo(tr("Brake numbers##brake_numbers"), &brake_numbers_, numbers, 2)) {
                own_brakes_loaded_ = true;
                remember("brake_numbers", std::to_string(brake_numbers_));
            }
            ImGui::SetNextItemWidth(330.0f);
            if (ImGui::Combo(tr("КЛУБ##own_klub"), &own_klub_, owners, 2)) {
                own_brakes_loaded_ = true;
                remember("own_klub", std::to_string(own_klub_));
            }
            ImGui::EndDisabled();
            if (legacy_) legacy_->set_own_klub(own_klub_ == 1);
            if (legacy_) legacy_->set_own_brakes(own_brakes_ == 1, brake_numbers_ == 1);
            ImGui::TextDisabled("%s", tr(!can_own ? "(works with the realistic air on and Trains with DLLs not as RTS)"
                                         : own_brakes_ == 0 ? "(MaDaDev's Brakes.dll works the 395, the 254, the cocks and the ЭПТ)"
                                         : brake_numbers_ == 0 ? "(openRTS's 395, 254, cocks and ЭПТ with Brakes.dll's numbers; Brakes.dll keeps the КЛУБ, the ЭПК and the sounds)"
                                                               : "(openRTS's 395, 254, cocks and ЭПТ with the books' numbers; Brakes.dll keeps the КЛУБ, the ЭПК and the sounds)"));
            ImGui::TextDisabled("%s", tr(!can_own ? "(works with the realistic air on and Trains with DLLs not as RTS)"
                                         : own_klub_ == 0 ? "(MaDaDev's Brakes.dll's КЛУБ-У)"
                                                          : "(openRTS's КЛУБ-У as Brakes.dll's: the ЭПК, the screen, the lamps; switched on, the track and the train's data are entered again)"));
            if (can_own && own_klub_ == 1 && legacy_)
                ImGui::TextUnformatted(legacy_->klub_cars() ? tr("This train's КЛУБ is openRTS's.")
                                                            : tr("This train keeps Brakes.dll's КЛУБ (none, or a locomotive with two cabs)."));
            // The player's locomotive: openRTS's devices, or why it keeps Brakes.dll.
            if (can_own && own_brakes_ == 1 && !units_.empty()) {
                const auto& player_train = *units_.front().train;
                const auto player_car = driven_car();
                if (player_car != SIZE_MAX && player_car < player_train.cars().size() && player_train.cars()[player_car].definition &&
                    player_train.cars()[player_car].definition->engine) {
                    const auto& parameters = brakes_parameters_of(*player_train.cars()[player_car].definition->engine);
                    if (!parameters.present) {
                        ImGui::TextUnformatted(tr("This locomotive has no Brakes.dll."));
                    } else if (!parameters.understood()) {
                        ImGui::Text(tr("This locomotive keeps Brakes.dll: openRTS does not know its %s."), parameters.unknown.c_str());
                    } else {
                        ImGui::TextUnformatted(tr("This locomotive's brakes are openRTS's."));
                    }
                }
            }
            // The cab's gauges of the pipe, the cylinders and the main
            // reservoir from the model's air (CabView::AirGauges).
            if (ImGui::Checkbox(tr("Cab gauges show openRTS's air (pipe, cylinders, main reservoir)"), &gauges_air_))
                remember("gauges_openrts_air", gauges_air_ ? "1" : "0");
            ImGui::TextDisabled("%s", tr(gauges_air_ ? "(the gauges named DTM, DTC1, DTC2, DGR show what F5 shows; the others stay the DLL's)"
                                                     : "(as the DLL draws them: its own needles, which may lag or run ahead of F5)"));
        }
        {
            // The rails' adhesion (adhesion.hpp): traction and, with the realistic
            // air, wheel slide and the shoes' wetness and warmth.
            const char* rails[] = {tr("as RTS (no weather)"), tr("by the weather"), tr("dry"), tr("wet"), tr("snow"), tr("ice"), tr("leaves")};
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo(tr("Rails##rails"), &rails_, rails, 7)) {
                rails_loaded_ = true;
                remember("rails", std::to_string(rails_));
            }
            ImGui::SameLine();
            const auto now = rails_now();
            ImGui::TextDisabled("%s", tr(now == simulation::RailCondition::rts    ? "(RTS's adhesion; no wheel slide limit by weather)"
                                         : now == simulation::RailCondition::dry  ? "(now dry: adhesion 0.33 standing, 0.21 at 100 km/h)"
                                         : now == simulation::RailCondition::wet  ? "(now wet: 0.65 of dry; the shoes get wet, braking dries them)"
                                         : now == simulation::RailCondition::snow ? "(now snow: 0.5 of dry; the shoes get wet, braking dries them)"
                                         : now == simulation::RailCondition::ice  ? "(now ice: 0.35 of dry)"
                                                                                  : "(now leaves: 0.3 of dry)"));
        }
        if (ImGui::Checkbox(tr("Limit the brake force to real stock's (openRTS)"), &limit_brake_force_)) {
            rails_loaded_ = true;
            remember("limit_brake_force", limit_brake_force_ ? "1" : "0");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr(limit_brake_force_
                                         ? "(with the realistic air: at most what real stock with its shoes gives; some content brakes 3x harder and slides on dry rails)"
                                         : "(off: the brake force as the stock's files give it)"));
        {
            // The brakes' condition and pressing (realistic air; docs/train.md
            // "Brake condition", testers' proposal 2026-11-17).
            ImGui::BeginDisabled(!realistic_air_);
            std::array<std::string, 11> names;
            names[0] = tr("off: as the files give it");
            for (int c = 1; c <= 10; ++c) names[static_cast<std::size_t>(c)] = std::to_string(c);
            names[1] = tr("1: due for an overhaul");
            names[6] = tr("6: in service");
            names[10] = tr("10: from the works");
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo(tr("Brake condition##condition"), names[static_cast<std::size_t>(brake_condition_)].c_str())) {
                for (int c = 0; c <= 10; ++c) {
                    if (ImGui::Selectable(names[static_cast<std::size_t>(c)].c_str(), c == brake_condition_)) {
                        brake_condition_ = c;
                        remember("brake_condition", std::to_string(c));
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr(!realistic_air_ ? "(works with the realistic air on)"
                                         : brake_condition_ == 0
                                             ? "(every car as its files give it: no leaks of its own, its distributor as the book's)"
                                             : "(the worse, the more the pipe, cylinders and main reservoir leak, the slower and less alike the distributors, the weaker the rigging; even at 10 the cars differ a little)"));
            // What the driven train has now.
            if (realistic_air_ && brake_condition_ != 0 && !units_.empty() && units_.front().train) {
                const auto& cars = units_.front().train->cars();
                float leak = 0.0f, slow = 9.0f, fast = 0.0f, weak = 9.0f, strong = 0.0f;
                for (const auto& car : cars) {
                    leak += car.wear.pipe_leak;
                    slow = (std::min)(slow, car.wear.pace);
                    fast = (std::max)(fast, car.wear.pace);
                    weak = (std::min)(weak, car.wear.rigging);
                    strong = (std::max)(strong, car.wear.rigging);
                }
                if (!cars.empty())
                    ImGui::TextDisabled(tr("(this train: the pipe leaks %.2f atm/min by itself, the distributors' pace %.0f-%.0f %%, the rigging %.0f-%.0f %%)"),
                                        leak / static_cast<float>(cars.size()), slow * 100.0f, fast * 100.0f, weak * 100.0f, strong * 100.0f);
            }
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderInt(tr("Brake pressing##pressing"), &brake_pressing_, 50, 150, "%d %%")) remember("brake_pressing", std::to_string(brake_pressing_));
            ImGui::SameLine();
            if (realistic_air_ && !units_.empty() && units_.front().train)
                ImGui::TextDisabled(tr("(the shoes' force of every car; the driven train now %.0f tf per 100 tf of weight, as cast-iron shoes)"),
                                    units_.front().train->brake_pressing());
            else
                ImGui::TextDisabled("%s", tr("(the shoes' force of every car, as a factor; works with the realistic air on)"));
            ImGui::EndDisabled();
        }
        if (ImGui::Checkbox(tr("Distributor stability in the RTS model (openRTS)"), &distributor_stability_)) {
            distributor_stability_loaded_ = true;
            remember("distributor_stability", distributor_stability_ ? "1" : "0");
            for (auto& unit : units_) unit.train->set_distributor_stability(distributor_stability_);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr(distributor_stability_
                                         ? "(a slow pipe fall, as the stabiliser's, brakes nothing: 483 up to 0.3, 292 up to 0.5 atm/min)"
                                         : "(off: as RTS, any pipe fall below the reservoir brakes, the stabiliser's slow one too)"));
        if (line_engines_) {
            ImGui::Checkbox(tr("pantographs can burn the wire"), &electrification_->wire_burning);
            ImGui::SameLine();
            ImGui::Checkbox(tr("each engine its own voltage##line"), &electrification_->own_voltage);
            if (electrification_->mode() != simulation::Electrification::Mode::sections && !electrification_->own_voltage) {
                ImGui::SameLine();
                ImGui::TextDisabled(electrification_->mode() == simulation::Electrification::Mode::single_type
                                        ? tr("(the route's one supply type)")
                                    : electrification_->mode() == simulation::Electrification::Mode::by_engine
                                        ? tr("(no .electrotype: the engine's own voltage, as RTS)")
                                        : tr("(the route is not electrified)"));
            }
        }
        if (ImGui::TreeNode(tr("Cars and coupling"))) {
            // Uncoupling as from the RTS menu: both end cocks closed, the
            // couplers left open so the parts do not couple again by themselves.
            const auto& cars = units_.front().cars;
            for (std::size_t i = 0; i < cars.size(); ++i) {
                const auto& state = train.cars()[i];
                ImGui::Text("%2zu %s%s%s", i + 1, cars[i].definition ? cars[i].definition->name.c_str() : "?",
                            state.flipped ? tr(" (turned)") : "", state.derailed ? tr(" DERAILED") : "");
                if (i + 1 < cars.size()) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton(tr("uncouple here"))) pending_uncouple_ = i + 1;
                    ImGui::PopID();
                }
            }
            if (units_.size() > 1) {
                ImGui::Separator();
                for (std::size_t u = 1; u < units_.size(); ++u) {
                    const auto& cut = *units_[u].train;
                    ImGui::Text(tr("Cut %zu: %zu cars, %.1f km/h, pipe %.2f atm%s"), u, cut.cars().size(), cut.speed() * 3.6,
                                cut.cars().empty() ? 0.0f : cut.cars().front().pipe, cut.derailed() ? ", derailed" : "");
                }
                ImGui::TextDisabled(tr("Push gently into a cut with automatic couplers to couple; open couplers only push."));
            }
            ImGui::TreePop();
        }
    }

    // What the train is doing.
    if (!loaded()) return;
    ImGui::SeparatorText(tr("The train now"));
    // The network: the player's train was taken away or taken off the
    // track and another client's is first now; nothing below is his.
    if (units_.front().remote) {
        if (helping_ && units_.front().train->id() == helping_) {
            ImGui::TextWrapped("%s", trf("You are the engineer's helper in %s: its cab shows what his DLLs show, your hands work it.",
                                         units_.front().name.c_str()).c_str());
            return;
        }
        ImGui::TextUnformatted(tr("You have no train: the free camera (the dispatcher assigns one)"));
        return;
    }
    const auto& train = driven();

    ImGui::Text(tr("%s: %zu cars, %.0f m, %.0f t"), units_.front().name.c_str(), units_.front().cars.size(), train.length(),
                train.mass() / 1000.0);
    if (!placed()) {
        ImGui::TextUnformatted(tr("Not on the track: put it with Track control (Ctrl+Alt+F7) or on a path start."));
        return;
    }
    const auto& head = train.cars().front();
    double worst_coupler = 0.0;
    for (const auto& car : train.cars()) worst_coupler = (std::max)(worst_coupler, std::abs(static_cast<double>(car.coupler_force)));
    ImGui::Text(tr("Speed %.1f km/h, acceleration %.2f m/s2%s"), train.speed() * 3.6, head.acceleration,
                train.derailed() ? "  DERAILED" : "");
    const auto driven = driven_car();
    if (driven != SIZE_MAX && dll_car(0, driven)) {
        // A DLL's cab: its handles are the cab's; the УР is the valve's
        // working the pipe (the DLL's, or openRTS's over Brakes.dll).
        const auto ur = equalising_of(0, driven);
        const auto mode = legacy_ ? legacy_->air() : trainplugin::DllAir::rts;
        const auto* asked = train.demands_of(driven);
        const char* valve = asked && asked->cab_brakes ? tr("openRTS brakes")
                          : mode == trainplugin::DllAir::own_valve && train.cars()[driven].definition->engine->brakes_library.valve >= 0
                              ? tr("openRTS valve over Brakes.dll")
                              : tr("the DLL's valve");
        ImGui::TextUnformatted(tr("The locomotive DLL drives it: its handles are in the cab."));
        if (ur) {
            ImGui::Text(tr("Equalising reservoir %.2f atm (%s)"), *ur, valve);
        } else {
            ImGui::Text(tr("Equalising reservoir: its DLL does not give it (%s)"), valve);
        }
    } else if (driver_) {
        ImGui::Text(tr("Throttle %u/%u  reverser %s  brake %s  independent %.1f atm"), driver_->throttle, driver_->notches,
                    driver_->reverser > 0 ? tr("forward") : driver_->reverser < 0 ? tr("back") : tr("neutral"),
                    handle_name(driver_->brake), driver_->independent);
        ImGui::Text(tr("Equalising reservoir %.2f atm (II keeps %.1f)"), driver_->equalising, driver_->charging_pressure);
        if (driver_->ep_brake) ImGui::Text(tr("EP brake: demand %.2f atm (VE/V apply, III/IV hold, I/II release)"), driver_->ep_target);
    }
    ImGui::Text(tr("Pipe %.2f  cylinder %.2f  aux %.2f  main reservoir %.2f atm"), head.pipe, head.cylinder, head.auxiliary,
                head.main_reservoir);
    if (driver_ && !legacy_ && !native_) {
        // openRTS's default logic for a train without a DLL (its keys).
        ImGui::TextDisabled("%s", tr("Horn Space, bell B, sander X, pantographs P, doors Y (left) and U (right)"));
        std::string now;
        const auto add = [&](const char* what) { now += (now.empty() ? "" : ", ") + std::string(tr(what)); };
        if (horn_) add("horn");
        if (bell_) add("bell");
        if (driver_->sanding) add("sanding");
        if (pantographs_down_) add("pantographs lowered");
        if (doors_open_[1]) add("left doors open");
        if (doors_open_[0]) add("right doors open");
        if (!now.empty()) ImGui::TextUnformatted(now.c_str());
    }
    if (native_) ImGui::Text(tr("Native plugin: %s (the cab's keys and mouse)"), native_->name().c_str());
    if (line_engines_) {
        // Each electric engine's pantograph current and the wire's heat (RTS 0x503CE4).
        for (std::size_t i = 0; i < train.cars().size(); ++i) {
            const auto& c = train.cars()[i];
            if (!c.definition->engine || c.definition->engine->kind != formats::EngineKind::electric) continue;
            const auto& e = *c.definition->engine;
            const float limit = e.max_line_current * e.max_line_current_burn_time;
            if (c.line_burnt) {
                ImGui::Text(tr("Car %zu: the wire burnt, %.0f s to cool"), i + 1, (std::max)(60.0f - c.line_heat, 0.0f));
            } else if (limit > 0.0f) {
                ImGui::Text(tr("Car %zu: line current %.0f A, wire heat %.0f %%"), i + 1, c.line_current, c.line_heat / limit * 100.0f);
            } else {
                ImGui::Text(tr("Car %zu: line current %.0f A"), i + 1, c.line_current);
            }
        }
        if (line_.voltage <= 0.0f) {
            ImGui::TextUnformatted(tr("Overhead line: no voltage (pantographs down, no wire or a dead section)"));
        } else if (line_.frequency > 0.0f) {
            ImGui::Text(tr("Overhead line: %.1f kV AC %.0f Hz"), line_.voltage / 1000.0f, line_.frequency);
        } else {
            ImGui::Text(tr("Overhead line: %.2f kV DC"), line_.voltage / 1000.0f);
        }
    }
    const auto& last = train.cars().back();
    ImGui::Text(tr("Last car: pipe %.2f cylinder %.2f; worst coupler %.0f kN"), last.pipe, last.cylinder, worst_coupler / 1000.0);
    ImGui::Text(tr("Joints %zu, junctions %zu%s; coupler breaks %zu, derailments %zu"), joints_, junctions_,
                end_of_track_ ? ", END OF TRACK" : "", coupler_breaks_, derailments_);
    ImGui::TextUnformatted(tr("Home/End: throttle  PgUp/PgDn: reverser  ] [: brake valve V..I (hold [ in I)  , .: independent"));
#endif
}

} // namespace openrts::viewer
#endif
