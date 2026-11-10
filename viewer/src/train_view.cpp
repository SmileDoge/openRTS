#ifdef OPENRTS_HAS_WGPU
#include "train_view.hpp"

#include <openrts/core/profile.hpp>

#include <openrts/renderer/route_space.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <iomanip>
#include <set>
#include <sstream>
#include <iostream>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <fstream>

#include <openrts/formats/sound.hpp>
#include <openrts/formats/terrain.hpp>
#include <openrts/simulation/track_traveller.hpp>
#include <openrts/simulation/train_lights.hpp>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include "key_layout.hpp"
#include "ui_text.hpp"
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
    };
    std::vector<ExteriorDoorAnim> exterior_doors;
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
    // The bodies on their springs (CarState::body) at the last two steps:
    // drawn between them like the placements, not stepping at 20 Hz.
    std::vector<glm::vec3> previous_body, current_body;
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
        take_body();
        previous_body = current_body;
    }
    void take_body() {
        current_body.resize(train->cars().size());
        for (std::size_t i = 0; i < current_body.size(); ++i) {
            const auto& body = train->cars()[i].body;
            current_body[i] = glm::vec3{body[0], body[1], body[2]};
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
    electrification_ = simulation::Electrification::load(*vfs_, route_, network);
    if (!electrification_->data().error.empty()) std::cout << "electrification: " << electrification_->data().error << '\n';
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
    if (unit == units_.end() || unit->current.empty()) return false;
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
    const auto& cars = unit->current;
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
    if (key == "velocity") return signed_along(state.velocity);
    if (key == "acceleration") return signed_along(state.acceleration);
    if (key == "smsstream") {
        const auto& car = unit->cars[index];
        const auto* sound = lower(std::string(area)) == "cabin" ? car.cab_sound.get() : car.sound.get();
        return sound && sub >= 0 ? static_cast<double>(sound->stream_sounding(static_cast<std::size_t>(sub))) : 0.0;
    }
    return std::nullopt;
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

void TrainView::activity_end_valves(const ActivityId train, const std::int32_t first, const std::int32_t last, const bool open) {
    const auto unit = std::find_if(units_.begin(), units_.end(), [&](const Unit& u) { return u.activity_id == train; });
    if (unit == units_.end() || unit->cars.empty()) return;
    const auto count = static_cast<std::int32_t>(unit->train->cars().size());
    const auto a = static_cast<std::size_t>(std::clamp(first < 0 ? 0 : first, 0, count - 1));
    const auto b = static_cast<std::size_t>(std::clamp(last < 0 ? count - 1 : last, 0, count - 1));
    // The cock is "closed" in the car's pipe ends: open clears it.
    unit->train->set_car_flags(a, simulation::Train::CarFlags::pipe_ends, simulation::pipe_cock_front, !open);
    unit->train->set_car_flags(b, simulation::Train::CarFlags::pipe_ends, simulation::pipe_cock_rear, !open);
}

void TrainView::retag_activity(const ActivityId from, const ActivityId to) {
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
        library.remove(it->second);
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

std::uint32_t TrainView::new_car_key() {
    static std::atomic<std::uint32_t> next_key{0};
    return ++next_key;
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
            formats::load_wagon(*vfs_, entry.folder, entry.name, entry.engine));
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
                          .handbrake = entry.handbrake ? *entry.handbrake / 100.0f : 0.0f});
        stock.push_back(std::move(wagon));
        cars.push_back(std::move(car));
    }
    unit.name = std::move(name);
    unit.freight = !passenger;
    unit.cars = std::move(cars);
    unit.train = std::make_unique<simulation::Train>(std::move(setups));
    unit.train->set_coupler_substeps(static_cast<std::uint32_t>(substeps_));
    if (realistic_air_) unit.train->set_brake_model(simulation::BrakeModel::realistic);
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
            std::fprintf(stderr, "locomotive DLLs: the consist changed, starting them again\n");
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
    if (legacy_) std::fprintf(stderr, "locomotive DLLs: kept through the change, the player is in car %zu\n", legacy_->player() + 1);
    return true;
}

void TrainView::attach_legacy() {
    legacy_.reset();
    legacy_status_.clear();
    if (units_.empty() || units_.front().role != Role::driven) return;
    auto& unit = units_.front();
    std::vector<LegacyCarRef> cars;
    for (const auto& car : unit.cars) cars.push_back({car.definition, car.folder, car.uid, car.key});
    legacy_ = LegacyCab::attach(*vfs_, cars, *unit.train, legacy_status_, start_state_, &route_, std::exchange(fresh_train_, false));
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
        if (more && !reconsist_host()) std::fprintf(stderr, "locomotive DLLs: the player's other trains not taken in\n");
    }
    attach_cab(legacy_ ? legacy_->player() : SIZE_MAX);
    if (!legacy_status_.empty()) {
        message_ = tr("locomotive DLLs: ") + legacy_status_;
        std::fprintf(stderr, "locomotive DLLs: %s\n", legacy_status_.c_str());
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
    if (legacy_->player() != player && !legacy_->enter(player, train)) return false;
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

const dllhost::CarOutput* TrainView::cab_state(const std::size_t car) const {
    if (native_ && car == native_car_) return &native_output_;
    return legacy_ ? legacy_->output(car) : nullptr;
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

void TrainView::split(const std::size_t unit_index, const std::size_t index, const bool close_cocks, const bool keep_open) {
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
    cuts_.push_back({from, unit.train->id(), cut.train->id(), index, swapped});
    if (cuts_.size() > 16) cuts_.erase(cuts_.begin());
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

std::size_t TrainView::merge_units(const simulation::Train* survivor_train, const simulation::Train* absorbed_train,
                                   const bool absorbed_ahead, const bool absorbed_turned) {
    const auto keys = driven_keys();
    const auto find = [&](const simulation::Train* train) {
        return static_cast<std::size_t>(std::find_if(units_.begin(), units_.end(), [&](const Unit& unit) {
                                            return unit.train.get() == train;
                                        }) - units_.begin());
    };
    const auto s = find(survivor_train);
    const auto a = find(absorbed_train);
    if (s >= units_.size() || a >= units_.size() || s == a) return units_.size();
    couplings_.push_back({units_[s].train->id(), units_[a].train->id(), absorbed_ahead, absorbed_turned});
    if (couplings_.size() > 16) couplings_.erase(couplings_.begin());
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
        message_ = trf("coupled: %zu cars", units_[survivor].cars.size());
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
                    message_ = u == 0 ? trf("derailed: car %u; C or \"Put on the path start\" puts the train back", event.car + 1)
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
    return assign_path(id, std::move(*plan));
}

void TrainView::clear_path(const std::uint64_t id) {
    dispatcher_->clear(id);
}

std::string TrainView::set_role(const std::uint64_t id, const Role role) {
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

std::string TrainView::spawn(const std::string_view consist, const simulation::TrackLocation& where, const Role role) {
    const bool first = units_.empty();
    Unit unit;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    if (auto error = make_unit(consist, unit, stock); !error.empty()) return error;
    std::string error;
    if (!unit.train->place(*network_, where.node, where.offset, where.forward, &error)) return error;
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
    // one put on the route becomes theirs.
    if (first) return set_role(units_.front().train->id(), Role::driven);
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
    }
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
        for (const auto& car : unit.cars) {
            output << "car=" << car.folder << '\t' << car.file << '\t' << (car.engine ? 1 : 0) << '\t' << (car.flipped ? 1 : 0) << '\t'
                   << car.uid << '\n';
        }
        ++saved;
    }
    if (!output) return "cannot write " + file.string();
    if (saved == 0) return tr("there are no trains on the track to save");
    return {};
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
    return {};
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
    auto error = put_back(std::move(trains), "activity", true);
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
    car_view_ = before == CarView::off ? CarView::head : before == CarView::head ? CarView::tail : CarView::off;
    if (car_view_ == CarView::tail && unit->cars.size() < 2) car_view_ = CarView::off;
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

std::vector<std::pair<std::string, double>> TrainView::stations_ahead(const Unit& unit) const {
    std::vector<std::pair<std::string, double>> result;
    const auto& items = network_->database().items;
    std::set<std::string> seen;
    const auto add = [&](const std::uint32_t item, const double distance) {
        if (item >= items.size() || items[item].kind != formats::TrackItemKind::platform) return;
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
        if (label.empty() || !seen.insert(label).second) return;
        result.emplace_back(label, distance);
    };
    // Along the path when the train has one, else along the junctions as set.
    const auto id = unit.train->id();
    const auto* route = dispatcher_->route(id);
    const auto* progress = dispatcher_->progress(id);
    if (route && progress && progress->on_route) {
        for (std::size_t l = progress->leg; l < route->legs.size() && result.size() < 10; ++l) {
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
                                              [](const formats::TrackItem& item) { return item.kind == formats::TrackItemKind::platform; }, 40);
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
    ImGui::SetNextWindowPos(ImVec2(place.x * display.x, place.y * display.y), ImGuiCond_Always, ImVec2(place.pivot_x, 0.0f));
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
    // Default places: 10 px from the side, 60 px from the top.
    const float margin_x = display.x > 0.0f ? 10.0f / display.x : 0.0f;
    const float margin_y = display.y > 0.0f ? 60.0f / display.y : 0.0f;
    const auto limit = limits_->evaluate(train, unit->speed_category());
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
                for (std::size_t i = car_page_; i < last && i < states.size(); ++i) {
                    const auto& car = unit->cars[i];
                    const auto& state = states[i];
                    ImGui::Text(tr("%02zu - %s. Mass %.1f t"), i + 1, car.definition ? car.definition->name.c_str() : "?",
                                car.definition ? car.definition->mass / 1000.0f : 0.0f);
                    // Marks: the handbrake, the chocks (by the car's own
                    // ends, as the car menu puts them), a derailment.
                    std::string marks;
                    if (state.handbrake > 0.0f) marks += trf(" [HANDBRAKE %d%%]", static_cast<int>(state.handbrake * 100.0f));
                    if (state.chocks != 0) {
                        const bool front = (state.chocks & simulation::chock_front) != 0, rear = (state.chocks & simulation::chock_rear) != 0;
                        marks += front && rear ? tr(" [CHOCKS front and rear]") : front ? tr(" [CHOCK front]") : tr(" [CHOCK rear]");
                    }
                    ImGui::Text(tr("     Pipe %.3f Aux %.3f Cylinder %.3f  Velocity %.2f Acceleration %.3f  Force %.1f kN BF %.1f kN CF %.0f kN%s%s"),
                                state.pipe, state.auxiliary, state.cylinder, state.velocity, state.acceleration, state.force / 1000.0f,
                                state.brake_force / 1000.0f, state.coupler_force / 1000.0f, marks.c_str(),
                                state.derailed ? tr(" [DERAILED]") : "");
                }
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
            }
        }
        end_panel(limits_place_);
    }
    if (stations_panel_) {
        if (begin_panel("##stations", stations_place_, 1.0f - margin_x, 0.45f)) {
            ImGui::Text(tr("Stations ahead: %s"), unit->name.c_str());
            ImGui::Separator();
            const auto stations = stations_ahead(*unit);
            if (stations.empty()) ImGui::TextDisabled(tr("none within 40 km"));
            // Two columns sized to the longest name: names never run into
            // the distances, which are right-aligned.
            if (!stations.empty() && ImGui::BeginTable("##stations_ahead", 2, ImGuiTableFlags_SizingFixedFit)) {
                for (std::size_t k = 0; k < stations.size() && k < 8; ++k) {
                    const auto& [name, distance] = stations[k];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::TableNextColumn();
                    char text[32];
                    std::snprintf(text, sizeof text, distance >= 1000.0 ? tr("in %.2f km") : tr("in %.0f m"),
                                  distance >= 1000.0 ? distance / 1000.0 : distance);
                    ImGui::TextUnformatted(text);
                }
                ImGui::EndTable();
            }
        }
        end_panel(stations_place_);
    }
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
    // While the car menu is open its window has the arrows and Enter.
    SceneInput menu_input;
    if (car_menu_) {
        menu_input = given_input;
        menu_input.walk_forward = menu_input.walk_back = menu_input.walk_left = menu_input.walk_right = false;
        menu_input.engineer_action = menu_input.engineer_action_ctrl = false;
    }
    const SceneInput& input = car_menu_ ? menu_input : given_input;
    frame_elapsed_ = elapsed;
    if (network_ && network_->rail_joints().layout() != rail_joint_layout()) network_->set_rail_joint_layout(rail_joint_layout());
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
    if (pending_path_) {
        // Bogies of every car first: they set the car spacing on the track.
        const bool shapes_ready = std::all_of(units_.front().cars.begin(), units_.front().cars.end(), [](const Car& car) {
            return car.fitted || car.shape.empty() || (car.requested && !car.loading.valid());
        });
        if (shapes_ready) {
            pending_path_ = false;
            if (paths_.empty()) {
                message_ = tr("the route has no paths: C puts the train on the track nearest to the camera");
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
    if (legacy_) {
        if (keys_live) {
            for (const auto& key : input.keys) legacy_->key(key, key_eye);
        } else if (keys_were_live_) {
            legacy_->release_keys();
        }
    }
    if (native_ && keys_live) {
        // The native cab's AssignKey keys (presses only).
        for (const auto& key : input.keys) {
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
            unit.take_body();
            if (unit.previous_body.size() != unit.current_body.size()) unit.previous_body = unit.current_body;
        }
    }

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
    if (!walk_objects_ || !obstacles_) return;
    std::size_t in_flight = 0;
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
    look_ = camera.forward();
    if (!engineer_.active()) return;
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
    const EngineerControls controls{.forward = input.walk_forward,
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
    };
    // At the door of another cab: of the driven train (the far end of a
    // multiple unit) or of another train of his (the network's own). The
    // same prompt as at his own cab; Enter makes that cab his (and the train
    // the driven one), and the same Enter then opens the door or climbs in
    // as into any cab.
    board_prompt_.clear();
    if (engineer_.mode() == Engineer::Mode::outside) {
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
    engineer_.update(*cab, controls, body->transform * cab->placement(), outdoors, elapsed, camera);
    // RTS moves the cab camera by the body's offsets on its springs and draws
    // the cab without them (0x424488): the cab sways around the driver.
    // openRTS's head on its neck nods and leans on top (head_motion()).
    if (engineer_.inside()) {
        camera.position += sway_offset(unit, engineer_car_);
        move_head(unit, engineer_car_, elapsed, camera);
    } else {
        head_.started = false;
    }
    if (const auto door = engineer_.take_pass()) pass_to_next_car(*door);
    look_ = camera.forward();
}

std::optional<TrainView::BoardTarget> TrainView::board_target(const glm::vec3& feet) {
    std::optional<BoardTarget> best;
    float nearest = 2.5f; // as the engineer's own get-on spots
    for (std::size_t u = 0; u < units_.size(); ++u) {
        const auto& unit = units_[u];
        if (unit.remote || unit.current.empty()) continue;
        // The driven train's cabs but the one he is at (its doors are the
        // engineer's own spots); his other trains' cabs.
        const bool driven = u == 0 && unit.role == Role::driven;
        if (!driven && (u == 0 || !boardable_.contains(unit.train->id()))) continue;
        for (std::size_t i = 0; i < unit.cars.size(); ++i) {
            if (driven && i == engineer_car_) continue;
            const auto& car = unit.cars[i];
            const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
            if (!engine || !engine->cab_view || engine->cabin.empty()) continue;
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
    } else if (const auto& car = unit.cars[j]; car.definition && car.definition->engine) {
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
    engineer_car_ = j;
    if (j == cab_car_) keep_cab(i, std::move(visit_cab_));
    to->poll();
    engineer_.arrive(*to, *arrival);
    hovered_.reset();
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
    return CabView::create(*vfs_, *c.definition, formats::wagon_path(c.folder, c.definition->name, true));
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

void TrainView::walk_up_to_cab(const std::size_t car, const glm::vec3& feet) {
    if (units_.empty() || car >= units_.front().cars.size()) return;
    if (car == cab_car_) {
        keep_cab(engineer_car_, std::move(visit_cab_));
    } else if (car != engineer_car_ || !visit_cab_) {
        auto taken = take_cab(car);
        if (!taken) return;
        keep_cab(engineer_car_, std::move(visit_cab_));
        visit_cab_ = std::move(taken);
        visit_requested_ = false;
    }
    engineer_car_ = car;
    // A cab with a desk is driven from at once, as when he walks into it
    // (pass_to_next_car; RTS 0x435654 -> 0x468C74).
    if (car != cab_car_ && visit_cab_ && !visit_cab_->description().switches.empty()) change_cab(car);
    engineer_.stand_outside(feet);
    std::cout << "the player goes to the cab of car " << car + 1 << std::endl;
}

void TrainView::pointer(const glm::vec3& eye, const std::optional<glm::vec3>& direction, const bool click, const bool shift,
                        const float scroll, const bool release, const glm::vec2& cursor, const bool held) {
    // A control in hand (RTS 0x4121BC) follows the cursor while the button
    // is down; the rest of the cab waits.
    if (legacy_ && legacy_->dragging()) {
        if (!held || release) {
            legacy_->drag_end();
        } else {
            legacy_->drag(cursor.x - drag_cursor_.x, cursor.y - drag_cursor_.y, shift);
            drag_cursor_ = cursor;
        }
        if (legacy_->dragging()) return;
    }
    if (release && legacy_) legacy_->release();
    hovered_.reset();
    const auto* cab = engineer_cab();
    if (!direction || !viewing_inside() || !cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) return;
    const auto body = body_pose(units_.front(), engineer_car_, false);
    if (!body) return;
    const glm::mat4 to_cab = glm::inverse(cab_to_scene_ ? *cab_to_scene_ : body->transform * cab->placement());
    const glm::vec3 origin{to_cab * glm::vec4{eye, 1.0f}};
    const glm::vec3 along = glm::normalize(glm::vec3{to_cab * glm::vec4{*direction, 0.0f}});
    // The reach is each control's Radius (RTS); the ray goes as far as any.
    hovered_ = cab->pick(origin, along, 4.0f);
    // The wheel off the controls at the chair: the head nearer to what the
    // cursor points at, or back.
    if (scroll != 0.0f && (!hovered_ || !hovered_->control) && engineer_.mode() == Engineer::Mode::seated) {
        engineer_.lean(along, scroll);
        return;
    }
    if (!hovered_ || !hovered_->control || (!click && scroll == 0.0f)) return;
    if (native_ && engineer_car_ == native_car_) {
        // A native plugin's cab: RTS's click step, the wheel one way.
        const auto id = cab->description().switches[hovered_->index].id;
        if (click) native_operate(id, 0);
        if (scroll != 0.0f) native_operate(id, scroll > 0.0f ? 1 : -1);
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
    if (scroll != 0.0f) legacy_->click(id, scroll > 0.0f ? 1 : -1, false, car);
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
        + std::to_string(interior_lights_.size()) + " cab lamps";
    if (native_) text += "; native plugin '" + native_->name() + "' on car " + std::to_string(native_car_ + 1);
    char line[200];
    for (const auto& unit : units_) {
        const auto& states = unit.train->cars();
        std::size_t lit = 0;
        for (const auto& car : unit.cars)
            for (const auto& l : car.lights) lit += l.level > 0.0f ? 1 : 0;
        std::snprintf(line, sizeof line, "; %s (%s, %zu cars): lights %zu", unit.name.c_str(),
                      unit.role == Role::driven ? "driven" : unit.role == Role::ai ? "ai" : "standing", unit.cars.size(), lit);
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
    if (gone || far || viewing_inside()) {
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
    case 16: { // Couple to the train standing at this free end (0x469ED8)
        if (both) break;
        const bool front = menu.front;
        std::vector<simulation::Train*> trains;
        for (const auto& u : units_) trains.push_back(u.train.get());
        const auto contact = simulation::end_contact(train, front, trains, 5.0);
        if (!contact) break;
        auto* other = contact->other;
        if (std::abs(train.speed()) > 0.01 || std::abs(other->speed()) > 0.01) {
            message_ = tr("both trains must stand to couple");
            break;
        }
        // The couplers at the joint are no longer held open.
        train.set_car_flags(menu.car, Flags::coupler_open, front ? simulation::coupler_open_front : simulation::coupler_open_rear, false);
        const auto other_car = contact->other_front ? std::size_t{0} : other->cars().size() - 1;
        other->set_car_flags(other_car, Flags::coupler_open,
                             contact->other_front ? simulation::coupler_open_front : simulation::coupler_open_rear, false);
        // The earlier unit survives (the driven train keeps its place).
        const auto other_unit = static_cast<std::size_t>(
            std::find_if(units_.begin(), units_.end(), [&](const Unit& u) { return u.train.get() == other; }) - units_.begin());
        if (other_unit < units_.size() && units_[other_unit].remote && units_[unit_index].remote) break;
        // Ours takes another client's train in (RTS: its own train always
        // takes the other); else the earlier unit survives.
        const bool keep_this = other_unit < units_.size() && units_[other_unit].remote ? true
                             : units_[unit_index].remote                             ? false
                                                                                     : unit_index < other_unit;
        auto* survivor = keep_this ? &train : other;
        auto* absorbed = keep_this ? other : &train;
        const bool survivor_front = keep_this ? front : contact->other_front;
        const bool absorbed_front = keep_this ? contact->other_front : front;
        if (survivor->couple(*absorbed, survivor_front, absorbed_front, (std::max)(contact->gap, 0.0))) {
            const auto merged = merge_units(survivor, absorbed, survivor_front, absorbed_front == survivor_front);
            if (merged < units_.size()) message_ = trf("coupled: %zu cars", units_[merged].cars.size());
        }
        break;
    }
    default: break;
    }
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

void TrainView::feed_world(const Unit& unit) {
    auto& world = legacy_->world();
    world.day_time = day_time_;
    world.night = night_;
    world.route_speed_limit = limits_ ? limits_->route_limit() : 0.0f;
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
    const auto limit = limits_->evaluate(train, unit.speed_category());
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
void TrainView::feed_line() {
    electrification_->advance_loads(simulation::train_step);
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        if (!unit.train->placed()) continue;
        const auto& states = unit.train->cars();
        const bool working = unit.role != Role::standing && !unit.train->asleep();
        std::vector<std::uint8_t> raised(states.size(), 0);
        bool electric = false;
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            const auto& car = unit.cars[i];
            const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
            if (!engine || engine->kind != formats::EngineKind::electric) continue;
            electric = true;
            if (!car.pantograph_nodes.empty() && car.pantograph_frames >= 2) {
                const float top = static_cast<float>(car.pantograph_frames - 1u) - 0.01f;
                for (const auto& [node, number] : car.pantograph_nodes) {
                    if (car.pantograph_frame[number - 1u] >= top) raised[i] |= static_cast<std::uint8_t>(1u << (number - 1u));
                }
            } else if (const auto* out = dll_car(u, i) ? dll_output(u, i) : nullptr) {
                raised[i] = out->panto_raised;
            } else if (u == 0 && native_ && i == native_car_) {
                raised[i] = static_cast<std::uint8_t>(native_->outputs().pantographs);
            } else {
                raised[i] = working && !car.cold && !(u == 0 && pantographs_down_) ? 1 : 0;
            }
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
void TrainView::update_lights(const float elapsed, const glm::vec3& eye) {
    light_glows_.clear();
    light_beams_.clear();
    interior_lights_.clear();
    if (units_.empty()) return;
    auto& unit = units_.front();
    // The lamps of the cab or salon the engineer is in (InternalLight State:
    // the DLL's SwitchLight or GM_LIGHT_ON by GlobalID), RTS's LightsInt.
    if (const auto* cab = viewing_inside() ? engineer_cab() : nullptr; cab && engineer_car_ < unit.cars.size()) {
        const auto& lamps = cab->description().lights;
        const auto* out = legacy_ && legacy_->has_dll(engineer_car_) ? legacy_->output(engineer_car_) : nullptr;
        const auto body = body_pose(unit, engineer_car_, false);
        lamp_level_.resize(lamps.size(), 0.0f);
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
            auto& level = lamp_level_[k];
            // RTS steps a lamp with LightFade's first value both ways
            // (0x4B0284 reads +44 only; the second value, +48, is unused).
            const float time = lamp.fade[0];
            const float goal = on ? 1.0f : 0.0f;
            level = time <= 0.0f ? goal : goal > level ? (std::min)(goal, level + elapsed / time) : (std::max)(goal, level - elapsed / time);
            if (level <= 0.0f || interior_lights_.size() >= 8) continue;
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
            light.kind = flags.find("light_cone") != std::string::npos ? 4.0f : flags.find("light_sphere") != std::string::npos ? 2.0f : 3.0f;
            const float rx = (std::max)(lamp.radius[0], 0.01f), rz = (std::max)(lamp.radius[1] > 0.0f ? lamp.radius[1] : lamp.radius[0], 0.01f);
            // A cone takes the squared spread at the PointsTo plane; a sphere and a box their half sizes.
            light.size = light.kind == 4.0f ? glm::vec4{(rx / reach) * (rx / reach), (rz / reach) * (rz / reach), reach, lamp.source_radius}
                                            : glm::vec4{rx, rz, reach, lamp.source_radius};
            light.to_light = glm::inverse(world);
            light.direction = glm::normalize(glm::vec3{world[2]});
            light.spot_exponent = std::clamp(lamp.spot_exponent, 0.0f, 1.0f) * 128.0f;
            light.attenuation = glm::vec4{lamp.attenuation[0], lamp.attenuation[1], lamp.attenuation[2], 0.0f};
            const std::uint32_t c = lamp.colour;
            light.diffuse = glm::vec4{static_cast<float>((c >> 16) & 0xFFu) / 255.0f * level, static_cast<float>((c >> 8) & 0xFFu) / 255.0f * level,
                                      static_cast<float>(c & 0xFFu) / 255.0f * level, 1.0f};
            interior_lights_.push_back(light);
        }
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

void TrainView::update_sound(const float elapsed, const ViewerCamera& camera, audio::AudioEngine& sound) {
    OPENRTS_ZONE("trains: sound");
    for (auto& gone : retired_sounds_) gone->stop(sound);
    retired_sounds_.clear();
    if (!bank_) bank_ = std::make_unique<audio::SoundBank>(vfs_);
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
    const auto unit_owner = u == 0 && unit.role == Role::driven ? formats::SoundOwner::player_train : formats::SoundOwner::train;
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
        const float direction = state.flipped ? -1.0f : 1.0f;
        // Other trains are heard by their outside groups (RTS 0x44AB7C).
        audio::SoundInputs inputs{.camera = u == 0 ? sound_camera() : audio::SoundCamera::external, .position = position};
        // Every car's sound as heard from the engineer's room (0x44AB7C).
        inputs.gain = outside_gain_;
        inputs[formats::SoundVariable::speed] = state.velocity * direction;
        inputs[formats::SoundVariable::distance] = glm::distance(camera.eye(), position);
        inputs[formats::SoundVariable::distance_travelled] = static_cast<float>(std::abs(state.travelled));
        inputs[formats::SoundVariable::acceleration] = state.acceleration * direction;
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
        if (!unit.remote && shared_sound_.contains(unit.train->id())) {
            for (int k = 0; k < 8; ++k)
                car.net_vars[static_cast<std::size_t>(k)] =
                    inputs.variables[static_cast<std::size_t>(formats::SoundVariable::variable1) + static_cast<std::size_t>(k)];
            car.net_travelled = inputs[formats::SoundVariable::distance_travelled];
            const auto* group = car.sound ? &car.sound->group() : nullptr;
            for (const auto code : car.pending_triggers) {
                // NonDistributableTriggers: not sent over the network (RTS).
                if (group && std::find(group->non_distributable.begin(), group->non_distributable.end(), code) !=
                                 group->non_distributable.end())
                    continue;
                if (car.net_triggers.size() < 96) car.net_triggers.push_back(code); // RTS's queue holds 96
            }
        }
        if (car.sound) {
            inputs.events = car.pending_events;
            inputs.discrete = car.pending_triggers;
            car.sound->update(elapsed, inputs, sound);
            inputs.events = {};
            inputs.discrete = {};
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
            const bool in_it = u == 0 && unit.role == Role::driven && viewing_inside() && i == cab_car_;
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
    const bool mine = u == 0 && unit.role == Role::driven && i == cab_car_;
    if (!mine) {
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
    // room from its room, and its distance (0x44B6B4). The points are in the
    // car; a cab turned round in it has them turned too.
    source_levels_.clear();
    if (listener_cab_ && engineer_car_ == i && u == 0) {
        const bool turned = listener_cab_->turned();
        for (const auto& point : car.cab_sound->group().sources) {
            const glm::vec3 at{turned ? -point.x : point.x, point.y, turned ? -point.z : point.z};
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
    if (const auto* vars = legacy_ ? legacy_->cab_sound_variables(i) : nullptr) {
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
    cab_to_scene_.reset();
    cab_focus_.reset();
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
                inside->adjustments(&unit == &units_.front() ? cab_state(i) : nullptr, adjustments, signal_aspect(unit),
                                    i == cab_before_car_ ? &blend : nullptr);
                const glm::mat4 to_scene = transform * inside->placement();
                // TextureChanges a DLL switched on (TexChange SetState): the
                // cab's image TexID drawn as the change's file.
                std::vector<renderer::TextureOverride> textures;
                if (const auto* out = legacy_ ? legacy_->output(i) : nullptr) {
                    const auto& changes = inside->description().texture_changes;
                    for (std::size_t k = 0; k < changes.size() && k < out->tex_changes.size(); ++k) {
                        if (out->tex_changes[k] == 0 || changes[k].file.empty()) continue;
                        const auto path = inside->shape().parent().resolve(changes[k].file);
                        if (!path) continue;
                        auto& texture = cab_textures_[path->key()];
                        if (!texture) texture = renderer.texture(*vfs_, *path);
                        if (texture) textures.push_back({changes[k].texture, texture});
                    }
                }
                // RTS draws the cab in object pass 3 (0x438D50).
                renderer.submit(*cab_model, to_scene, adjustments, textures, renderer::ObjectPass::last);
                cab_to_scene_ = to_scene;
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
                    cab->screens(cab_state(i), frame_elapsed_, screen_batches_);
                    for (auto b = first; b < screen_batches_.size(); ++b) {
                        for (auto& v : screen_batches_[b].vertices) v.position = glm::vec3{to_scene * glm::vec4{v.position, 1.0f}};
                    }
                }
                // RTS 0x438D50: the exterior comes back with the head out of
                // the window; DoNotHideExtInCabin keeps it in the cab.
                const bool keep = car.definition->engine && car.definition->engine->keep_exterior_in_cab != 0;
                if (!engineer_.heading_out() && !keep) continue;
            }
        }
        if (!model) continue;

        adjustments.clear();
        // Wheels turn by the distance run, interpolated like the body.
        const double travelled = i < unit.current_travelled.size() && i < unit.previous_travelled.size()
            ? unit.previous_travelled[i] + (unit.current_travelled[i] - unit.previous_travelled[i]) * t
            : state.travelled;
        const double radius = car.definition->wheel_radius > 0.05f ? car.definition->wheel_radius : 0.5;
        const float angle = static_cast<float>(std::fmod(travelled / radius, 2.0 * pi)) * (car.flipped ? -1.0f : 1.0f);
        for (const auto wheel : car.wheels) {
            adjustments.push_back({.matrix = wheel, .local = glm::rotate(glm::mat4{1.0f}, angle, glm::vec3{1.0f, 0.0f, 0.0f})});
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
        for (const auto& [matrix, shape_front] : car.bogies) {
            // A bogie follows the track heading at its own position.
            const bool train_front = shape_front != car.flipped;
            float delta = wrap_angle((train_front ? here.front_heading : here.rear_heading) - body_heading);
            if (std::abs(delta) > 0.5f) delta = wrap_angle(delta + pi); // a heading along the node's other direction
            if (std::abs(delta) > 0.5f) delta = 0.0f;
            adjustments.push_back({.matrix = matrix, .local = glm::rotate(glm::mat4{1.0f}, delta, glm::vec3{0.0f, 1.0f, 0.0f})});
        }
        renderer.submit(*model, transform, adjustments);
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

glm::vec3 TrainView::body_sway(const Unit& unit, const std::size_t i) const {
    if (i < unit.current_body.size()) {
        const auto& now = unit.current_body[i];
        return glm::mix(i < unit.previous_body.size() ? unit.previous_body[i] : now, now, alpha());
    }
    if (i >= unit.train->cars().size()) return {};
    const auto& body = unit.train->cars()[i].body;
    return {body[0], body[1], body[2]};
}

glm::vec3 TrainView::sway_offset(const Unit& unit, const std::size_t i) const {
    if (sway_scale_ <= 0.0f || i >= unit.cars.size() || i >= unit.train->cars().size()) return {};
    const auto body = body_sway(unit, i);
    const auto here = pose(unit, i);
    const auto axis = here.front - here.rear;
    const float span = glm::length(axis);
    if (span < 0.01f) return {};
    const float heading = std::atan2(axis.x, axis.z);
    const glm::vec3 turning{std::cos(heading), 0.0f, -std::sin(heading)};
    return (turning * body.x + glm::vec3{0.0f, body.y, 0.0f} + axis / span * body.z) * sway_scale_;
}

void TrainView::move_head(const Unit& unit, const std::size_t i, const float elapsed, ViewerCamera& camera) {
    const float amount = head_motion();
    if (amount <= 0.0f || i >= unit.cars.size() || i >= unit.train->cars().size()) {
        head_.started = false;
        return;
    }
    const auto& state = unit.train->cars()[i];
    // The body as the camera is carried by it (the sway slider's size).
    const glm::vec3 body = body_sway(unit, i) * sway_scale_;
    // At rest in a car he entered; after a long frame (a load, a hitch) not
    // thrown by it.
    if (!head_.started || head_car_ != unit.cars[i].key || elapsed > 0.25f) {
        head_.reset(body);
        head_car_ = unit.cars[i].key;
    }
    // The train's own acceleration in the car's axes: towards the curve's
    // inside v^2 / R, along the front its acceleration.
    const glm::vec3 carried{state.velocity * state.velocity * state.curvature, 0.0f, state.acceleration};
    head_.step(body, carried, elapsed);
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
    camera.position += head.shift;
    camera.head_nod = head.nod;
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
    if (sway && sway_scale_ > 0.0f) {
        // The body on its springs (simulation: RTS 0x4F5B34): lateral
        // towards the side the car turns to, vertical, along the train.
        const glm::vec3 turning{std::cos(body_heading), 0.0f, -std::sin(body_heading)};
        const auto along = axis / span;
        const auto body = body_sway(unit, i);
        centre += (turning * body.x + glm::vec3{0.0f, body.y, 0.0f} + along * body.z) * sway_scale_;
    }
    float heading = body_heading;
    float up = std::asin(std::clamp(axis.y / span, -1.0f, 1.0f));
    if (car.flipped) {
        heading += pi;
        up = -up;
    }
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
    transform = glm::rotate(transform, heading, glm::vec3{0.0f, 1.0f, 0.0f});
    transform = glm::rotate(transform, -up, glm::vec3{1.0f, 0.0f, 0.0f});
    return BodyPose{.transform = transform, .here = here, .heading = body_heading};
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

void TrainView::draw_overlay() {
#ifdef OPENRTS_HAS_IMGUI
    // The 3-D cab and the engineer on foot.
    const auto cab_controls = [this] {
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
                                         "RMB: look round, Alt: captions of the controls"));
            ImGui::TextDisabled("%s", trf("Mouse: a click works a switch, the wheel turns it a step; %s: the window, %s: head out",
                                          window_key_name(), head_out_key_name()).c_str());
            ImGui::Checkbox(tr("On foot outside: collide with objects (off: only the ground and trains)"), &walk_objects_);
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
    };
    if (legacy_) legacy_->draw_window(cab_controls);
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
    if (!ImGui::CollapsingHeader(tr("Train"), ImGuiTreeNodeFlags_DefaultOpen)) return;
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
    cab_controls();
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
                    if (ImGui::Selectable(name.c_str(), i == consist_choice_)) consist_choice_ = i;
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
    }
    if (!message_.empty()) ImGui::TextUnformatted(message_.c_str());
    if (!loaded()) return;
    // The network: the player's train was taken away or taken off the
    // track and another client's is first now; nothing below is his.
    if (units_.front().remote) {
        ImGui::TextUnformatted(tr("You have no train: the free camera (the dispatcher assigns one)"));
        return;
    }
    const auto& train = driven();

    ImGui::Text(tr("%s: %zu cars, %.0f m, %.0f t"), units_.front().name.c_str(), units_.front().cars.size(), train.length(),
                train.mass() / 1000.0);
    if (!placed()) {
        ImGui::TextUnformatted(tr("Not on the track: C puts it on the track nearest to the camera."));
        return;
    }
    const auto& head = train.cars().front();
    double worst_coupler = 0.0;
    for (const auto& car : train.cars()) worst_coupler = (std::max)(worst_coupler, std::abs(static_cast<double>(car.coupler_force)));
    ImGui::Text(tr("Speed %.1f km/h, acceleration %.2f m/s2%s"), train.speed() * 3.6, head.acceleration,
                train.derailed() ? "  DERAILED" : "");
    if (driver_) {
        ImGui::Text(tr("Throttle %u/%u  reverser %s  brake %s  independent %.1f atm"), driver_->throttle, driver_->notches,
                    driver_->reverser > 0 ? tr("forward") : driver_->reverser < 0 ? tr("back") : tr("neutral"),
                    handle_name(driver_->brake), driver_->independent);
        ImGui::Text(tr("Equalising reservoir %.2f atm (II keeps %.1f)"), driver_->equalising, driver_->charging_pressure);
        ImGui::Checkbox(tr("EP brake (K)"), &driver_->ep_brake);
        if (driver_->ep_brake) {
            ImGui::SameLine();
            ImGui::Text(tr("demand %.2f atm (VE/V apply, III/IV hold, I/II release)"), driver_->ep_target);
        }
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
        ImGui::SameLine();
        ImGui::Checkbox(tr("pantographs can burn the wire"), &electrification_->wire_burning);
    }
    if (line_engines_) {
        if (line_.voltage <= 0.0f) {
            ImGui::TextUnformatted(tr("Overhead line: no voltage (pantographs down, no wire or a dead section)"));
        } else if (line_.frequency > 0.0f) {
            ImGui::Text(tr("Overhead line: %.1f kV AC %.0f Hz"), line_.voltage / 1000.0f, line_.frequency);
        } else {
            ImGui::Text(tr("Overhead line: %.2f kV DC"), line_.voltage / 1000.0f);
        }
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
    const auto& last = train.cars().back();
    ImGui::Text(tr("Last car: pipe %.2f cylinder %.2f; worst coupler %.0f kN"), last.pipe, last.cylinder, worst_coupler / 1000.0);
    ImGui::Text(tr("Joints %zu, junctions %zu%s; coupler breaks %zu, derailments %zu"), joints_, junctions_,
                end_of_track_ ? ", END OF TRACK" : "", coupler_breaks_, derailments_);
    ImGui::SliderFloat(tr("Body sway"), &sway_scale_, 0.0f, 10.0f, "%.1f");
    ImGui::SameLine();
    ImGui::TextDisabled(tr("(1 = RTS model; RTS shows it in the cab only)"));
    ImGui::Checkbox(tr("Car loops start at random points"), &random_loop_phase_);
    ImGui::SameLine();
    ImGui::TextDisabled(tr("(openRTS: identical loops on many cars do not add up in phase; RTS starts them together)"));
    if (ImGui::SliderInt(tr("Coupler sub-steps"), &substeps_, 1, 4)) {
        for (auto& unit : units_) unit.train->set_coupler_substeps(static_cast<std::uint32_t>(substeps_));
    }
    ImGui::SameLine();
    ImGui::TextDisabled(substeps_ == 1 ? "(RTS)" : "(experimental)");
    if (ImGui::Checkbox(tr("Realistic air (openRTS)"), &realistic_air_)) {
        for (auto& unit : units_)
            unit.train->set_brake_model(realistic_air_ ? simulation::BrakeModel::realistic : simulation::BrakeModel::rts);
    }
    ImGui::SameLine();
    if (!units_.empty() && realistic_air_ && units_.front().train->brake_model() == simulation::BrakeModel::rts) {
        ImGui::TextDisabled("%s", tr("(the player's train keeps RTS air: its DLL's valve is made for it)"));
    } else {
        ImGui::TextDisabled("%s", tr("(off: RTS model; flow along the pipe, 483 / 292 / 305 distributors, 280 m/s emergency wave)"));
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
    ImGui::Checkbox(tr("Camera follows (V)"), &follow_);
    ImGui::TextUnformatted(tr("Home/End: throttle  PgUp/PgDn: reverser  ] [: brake valve V..I (hold [ in I)  , .: independent"));
#endif
}

} // namespace openrts::viewer
#endif
