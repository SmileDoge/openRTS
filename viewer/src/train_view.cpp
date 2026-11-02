#ifdef OPENRTS_HAS_WGPU
#include "train_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <set>
#include <iostream>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <fstream>

#include <openrts/formats/sound.hpp>
#include <openrts/formats/terrain.hpp>
#include <openrts/simulation/track_traveller.hpp>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include "key_layout.hpp"
#include "ui_text.hpp"
#include "viewer_state.hpp"

namespace openrts::viewer {
namespace {

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
    core::VirtualPath shape;
    std::string folder;
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
    std::vector<float> light_level; // by LightID: 0 off .. 1 on (FadeIn / FadeOut)
    std::vector<std::pair<std::int32_t, bool>> bogies; // matrix, is front bogie (in the shape)
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
    std::unique_ptr<simulation::AutoDriver> ai;
    float wait_left = -1.0f; // s still to wait at a waiting point
    float still = 0.0f;      // s standing still (RTS: a LOOSE train becomes STATIC after 30 s)
    std::vector<simulation::CarPlacement> previous, current;
    std::vector<double> previous_travelled, current_travelled;

    // Both interpolation ends at the train's present state.
    void settle() {
        previous = current = train->placements();
        current_travelled.resize(train->cars().size());
        for (std::size_t i = 0; i < current_travelled.size(); ++i) current_travelled[i] = train->cars()[i].travelled;
        previous_travelled = current_travelled;
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
    return std::filesystem::current_path() / "user_paths" / std::string(route_.filename());
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
    } else {
        consist = formats::load_consist(*vfs_, consist_name);
    }
    if (!consist.ok()) return "consist '" + std::string(consist_name) + "': " + consist.error;
    std::vector<Car> cars;
    std::vector<simulation::CarSetup> setups;
    bool passenger = false;
    for (const auto& entry : consist.entries) {
        auto wagon = std::make_unique<formats::WagonDefinition>(
            formats::load_wagon(*vfs_, entry.folder, entry.name, entry.engine));
        if (!wagon->ok()) return entry.folder + "/" + entry.name + ": " + wagon->error;
        passenger = passenger || wagon->kind == formats::WagonKind::passenger;
        Car car;
        car.definition = wagon.get();
        car.uid = entry.uid;
        car.folder = entry.folder;
        car.flipped = entry.flipped;
        car.shape = core::VirtualPath::parse("TRAINS/TRAINSET/" + entry.folder + "/" + wagon->shape).value_or(core::VirtualPath{});
        setups.push_back({.definition = wagon.get(),
                          .flipped = entry.flipped,
                          .handbrake = entry.handbrake ? *entry.handbrake / 100.0f : 0.0f});
        stock.push_back(std::move(wagon));
        cars.push_back(std::move(car));
    }
    unit.name = consist.name;
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

std::string TrainView::place_on_path(const formats::TrainPath& path) {
    if (!loaded()) return "no consist loaded";
    const auto& first = path.nodes.front();
    const auto start = path.points[first.point].position;
    const auto location = network_->locate(
        simulation::TrackPoint{.tile_x = start.tile_x, .tile_z = start.tile_z, .x = start.x, .y = start.y, .z = start.z},
        20.0);
    if (!location) return "no track at the start of path '" + path.file + "'";

    // The first leg: towards the next node's point.
    glm::vec3 direction{0.0f};
    if (first.next != formats::PathNode::invalid && first.next < path.nodes.size()) {
        direction = local(path.points[path.nodes[first.next].point].position) - local(start);
    }
    simulation::TrackTraveller rear(*network_, location->node, location->offset, true);
    {
        auto ahead = rear;
        const auto here = local(rear.pose());
        ahead.move(1.0);
        if (glm::dot(local(ahead.pose()) - here, direction) < 0.0f) {
            rear = simulation::TrackTraveller(*network_, location->node, location->offset, false);
        }
    }
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
    for (auto* each : all) {
        auto& car = *each;
        if (car.requested || car.shape.empty() || library.find(car.shape)) continue;
        // One load per shape: cars sharing a shape wait for the first.
        const bool pending = std::any_of(all.begin(), all.end(), [&](const Car* other) {
            return other != &car && other->requested && other->shape == car.shape && other->loading.valid();
        });
        if (pending) continue;
        car.requested = true;
        car.loading = std::async(std::launch::async, [vfs = vfs_, path = car.shape] {
            return renderer::load_model_asset(*vfs, path, {});
        });
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
                // The pantograph's number is the last digit of the node name.
                const char last = name.empty() ? '0' : name.back();
                const auto n = static_cast<std::uint8_t>(last >= '1' && last <= '4' ? last - '0' : 1);
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
        if (!car.pantograph_nodes.empty()) car.pantograph_frames = model->animation().frame_count;
        if (const auto bytes = vfs_->read(car.shape.with_extension(".sd"))) {
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
    if (input.reverser_forward && d.reverser < 1) ++d.reverser;
    if (input.reverser_back && d.reverser > -1) --d.reverser;
    if (input.brake_more && d.brake != Handle::emergency) d.brake = static_cast<Handle>(static_cast<int>(d.brake) + 1);
    if (input.brake_less && d.brake != Handle::charge) d.brake = static_cast<Handle>(static_cast<int>(d.brake) - 1);
    if (input.independent_more) d.independent = (std::min)(d.independent + 0.5f, 4.0f);
    if (input.independent_less) d.independent = (std::max)(d.independent - 0.5f, 0.0f);
    if (input.toggle_ep) d.ep_brake = !d.ep_brake;
}

void TrainView::turn_around() {
    if (!loaded() || !driven().turn_around()) {
        message_ = tr("stop the train to change ends");
        return;
    }
    auto& unit = units_.front();
    std::reverse(unit.cars.begin(), unit.cars.end());
    for (auto& car : unit.cars) car.flipped = !car.flipped;
    // Controllers moved with their cars; the master must be the new front
    // engine, so the driver is put on again with the old handles.
    reattach_driver();
    if (driver_) {
        driver_->throttle = 0;
        driver_->reverser = 0;
    }
    unit.settle();
    message_ = tr("now driving from the other end");
}

void TrainView::reattach_driver() {
    if (!loaded()) return;
    if (units_.front().role != Role::driven) {
        attach_role(units_.front());
        return;
    }
    auto& train = driven();
    for (std::size_t i = 0; i < train.cars().size(); ++i) train.set_controller(i, nullptr);
    const auto old = driver_ ? std::optional<simulation::DriverHandles>{*driver_} : std::nullopt;
    driver_ = simulation::attach_keyboard_driver(train);
    if (driver_ && old) {
        const auto notches = driver_->notches;
        *driver_ = *old;
        driver_->notches = notches;
        driver_->throttle = (std::min)(driver_->throttle, notches);
    }
    attach_legacy();
}

void TrainView::attach_legacy() {
    legacy_.reset();
    legacy_status_.clear();
    if (units_.empty() || units_.front().role != Role::driven) return;
    auto& unit = units_.front();
    std::vector<LegacyCarRef> cars;
    for (const auto& car : unit.cars) cars.push_back({car.definition, car.folder, car.uid});
    legacy_ = LegacyCab::attach(*vfs_, cars, *unit.train, legacy_status_, start_state_);
    // The 3-D cab of the engine the player drives: the DLL's one, else the first engine.
    engineer_.off();
    visit_cab_.reset();
    cab_.reset();
    cab_car_ = SIZE_MAX;
    engineer_car_ = SIZE_MAX;
    cab_requested_ = false;
    std::size_t player = legacy_ ? legacy_->player() : SIZE_MAX;
    for (std::size_t i = 0; player == SIZE_MAX && i < unit.cars.size(); ++i)
        if (unit.cars[i].definition && unit.cars[i].definition->engine) player = i;
    if (player != SIZE_MAX) {
        const auto& car = unit.cars[player];
        cab_ = CabView::create(*vfs_, *car.definition,
                               formats::wagon_path(car.folder, car.definition->name, car.definition->engine.has_value()));
        if (cab_) cab_car_ = player;
    }
    if (!legacy_status_.empty()) {
        message_ = tr("locomotive DLLs: ") + legacy_status_;
        std::fprintf(stderr, "locomotive DLLs: %s\n", legacy_status_.c_str());
    }
    if (legacy_) std::fprintf(stderr, "locomotive DLLs: running, the player is in car %zu\n", legacy_->player() + 1);
}

bool TrainView::keyboard_captured() const noexcept { return legacy_ && legacy_->keyboard() && cab_keys_live(); }

audio::SoundCamera TrainView::sound_camera() const noexcept {
    // RTS (0x44AB7C, docs/sound.md): outside - on foot, the free or an
    // outside camera, the head out of a window - the ExternalCam groups; in
    // the driven car the CabCam ones; in another car of the train the
    // PassengerCam ones.
    if (!engineer_.inside() || engineer_.heading_out()) return audio::SoundCamera::external;
    return engineer_car_ == cab_car_ ? audio::SoundCamera::cab : audio::SoundCamera::passenger;
}

bool TrainView::cab_keys_live() const noexcept {
    if (engineer_.mode() == Engineer::Mode::off) return true;
    return engineer_.inside() && engineer_car_ == cab_car_;
}

void TrainView::split(const std::size_t unit_index, const std::size_t index, const bool close_cocks, const bool keep_open) {
    if (unit_index >= units_.size()) return;
    auto& unit = units_[unit_index];
    // The master engine is the first engine (attach_keyboard_driver).
    std::optional<std::size_t> master;
    for (std::size_t i = 0; i < unit.cars.size(); ++i) {
        if (unit.cars[i].definition && unit.cars[i].definition->engine) {
            master = i;
            break;
        }
    }
    auto cut_train = unit.train->uncouple(index, close_cocks, keep_open);
    if (!cut_train) return;
    Unit cut;
    cut.train = std::move(cut_train);
    const auto split_at = unit.cars.begin() + static_cast<std::ptrdiff_t>(index);
    cut.cars.assign(std::make_move_iterator(split_at), std::make_move_iterator(unit.cars.end()));
    unit.cars.erase(split_at, unit.cars.end());
    // The driver goes with the master engine.
    if (unit_index == 0 && master && *master >= index) std::swap(unit, cut);
    for (std::size_t i = 0; i < cut.train->cars().size(); ++i) cut.train->set_controller(i, nullptr);
    if (unit_index == 0) {
        reattach_driver();
    } else {
        attach_role(unit);
    }
    cut.name = unit.name + " (cut)";
    cut.freight = unit.freight;
    unit.settle();
    cut.settle();
    units_.push_back(std::move(cut));
}

std::size_t TrainView::merge_units(const simulation::Train* survivor_train, const simulation::Train* absorbed_train,
                                   const bool absorbed_ahead, const bool absorbed_turned) {
    const auto find = [&](const simulation::Train* train) {
        return static_cast<std::size_t>(std::find_if(units_.begin(), units_.end(), [&](const Unit& unit) {
                                            return unit.train.get() == train;
                                        }) - units_.begin());
    };
    const auto s = find(survivor_train);
    const auto a = find(absorbed_train);
    if (s >= units_.size() || a >= units_.size() || s == a) return units_.size();
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
    if (survivor == 0) {
        reattach_driver();
    } else {
        attach_role(units_[survivor]);
    }
    units_[survivor].settle();
    return survivor;
}

void TrainView::step_trains() {
    // Meetings first, on the positions the step starts from (the earlier
    // unit survives a coupling, so the driven train keeps its place).
    std::vector<simulation::Train*> trains;
    for (auto& unit : units_) trains.push_back(unit.train.get());
    for (const auto& result : simulation::resolve_contacts(trains)) {
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
    for (auto& unit : units_) {
        if (unit.role == Role::ai && unit.ai && unit.train->placed()) {
            unit.ai->step(simulation::train_step, *unit.train, drive_orders(unit));
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> breaks;
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
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
                message_ = u == 0 ? trf("derailed: car %u; C or \"Put on the path start\" puts the train back", event.car + 1)
                                  : trf("derailed: car %u of a cut", event.car + 1);
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

simulation::DriveOrders TrainView::drive_orders(Unit& unit) {
    return simulation::path_orders(*unit.train, *dispatcher_, signals_, *limits_, unit.freight, unit.wait_left);
}

void TrainView::attach_role(Unit& unit) {
    auto& train = *unit.train;
    for (std::size_t i = 0; i < train.cars().size(); ++i) train.set_controller(i, nullptr);
    unit.ai.reset();
    if (unit.role == Role::ai) {
        auto handles = simulation::attach_keyboard_driver(train);
        if (handles) {
            unit.ai = std::make_unique<simulation::AutoDriver>(std::move(handles));
        } else {
            unit.role = Role::standing;
        }
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
                          .target = unit.ai ? unit.ai->target_speed() : -1.0f,
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
    if (role == Role::driven) {
        const auto index = static_cast<std::size_t>(unit - units_.data());
        if (index != 0) {
            units_.front().role = Role::standing;
            attach_role(units_.front());
            std::swap(units_.front(), units_[index]);
        }
        units_.front().role = Role::driven;
        units_.front().ai.reset();
        if (driver_) {
            driver_->throttle = 0;
            driver_->reverser = 0;
        }
        reattach_driver();
        follow_id_ = 0;
        consist_ = units_.front().name;
        return {};
    }
    unit->role = role;
    attach_role(*unit);
    if (role == Role::ai && unit->role != Role::ai) return "the train has no engine to drive";
    return {};
}

std::string TrainView::remove(const std::uint64_t id) {
    auto* unit = find_unit(id);
    if (!unit) return "no such train";
    if (unit == &units_.front()) return "make another train the driven one first";
    dispatcher_->clear(id);
    unit->train->vacate();
    units_.erase(units_.begin() + (unit - units_.data()));
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
    return {};
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
    const auto* unit = shown_unit();
    if (!unit) return;
    const auto& train = *unit->train;
    const auto display = ImGui::GetIO().DisplaySize;
    // Default places: 10 px from the side, 60 px from the top.
    const float margin_x = display.x > 0.0f ? 10.0f / display.x : 0.0f;
    const float margin_y = display.y > 0.0f ? 60.0f / display.y : 0.0f;
    const auto limit = limits_->evaluate(train, unit->freight);
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
                    ImGui::Text(tr("     Pipe %.3f Aux %.3f Cylinder %.3f  Velocity %.2f Acceleration %.3f  Force %.1f kN BF %.1f kN CF %.0f kN%s%s"),
                                state.pipe, state.auxiliary, state.cylinder, state.velocity, state.acceleration, state.force / 1000.0f,
                                state.brake_force / 1000.0f, state.coupler_force / 1000.0f,
                                state.handbrake > 0.0f ? (" [HANDBRAKE " + std::to_string(static_cast<int>(state.handbrake * 100.0f)) + "%]").c_str() : "",
                                state.derailed ? " [DERAILED]" : "");
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
            const auto ahead = limits_->limits_ahead(train, unit->freight, 10000.0, 6);
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

void TrainView::update(const SceneInput& given_input, ViewerCamera& camera, const float elapsed,
                       renderer::ModelLibrary& library, audio::AudioEngine* sound) {
    // While the car menu is open its window has the arrows and Enter.
    SceneInput menu_input;
    if (car_menu_) {
        menu_input = given_input;
        menu_input.walk_forward = menu_input.walk_back = menu_input.walk_left = menu_input.walk_right = false;
        menu_input.engineer_action = menu_input.engineer_action_ctrl = false;
    }
    const SceneInput& input = car_menu_ ? menu_input : given_input;
    frame_elapsed_ = elapsed;
    if (!pending_load_.empty()) {
        if (sound) stop_sound(*sound);
        const auto name = std::exchange(pending_load_, {});
        const auto error = load(name);
        message_ = error.empty() ? "loaded " + consist_ : error;
        if (error.empty()) pending_path_ = true;
    }
    if (!loaded()) return;
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
    update_car_menu(camera.eye());
    if (input.toggle_follow) follow_ = !follow_;
    if (input.turn_around || pending_turn_) {
        pending_turn_ = false;
        turn_around();
    }
    if (pending_uncouple_) {
        const auto index = *std::exchange(pending_uncouple_, std::nullopt);
        if (std::abs(driven().speed()) > 0.1) {
            message_ = tr("stop the train to uncouple");
        } else {
            split(0, index, true, true);
            message_ = trf("uncoupled before car %zu (end cocks closed)", static_cast<std::size_t>(index + 1));
        }
    }
    const bool keys_live = cab_keys_live();
    if (keys_live) {
        apply_handles(input);
    } else {
        apply_handles(SceneInput{}); // a held handle position still springs back
    }
    if (legacy_) {
        if (keys_live) {
            for (const auto& key : input.keys) legacy_->key(key);
        } else if (keys_were_live_) {
            legacy_->release_keys();
        }
    }
    keys_were_live_ = keys_live;

    accumulator_ += (std::min)(elapsed, 0.25f);
    while (accumulator_ >= simulation::train_step) {
        accumulator_ -= simulation::train_step;
        for (auto& unit : units_) {
            unit.previous = std::move(unit.current);
            unit.previous_travelled = std::move(unit.current_travelled);
        }
        feed_line();
        if (legacy_ && !units_.empty() && units_.front().role == Role::driven) {
            auto& unit = units_.front();
            feed_world(unit);
            std::vector<std::vector<std::uint32_t>> triggers, cab_triggers;
            std::vector<std::vector<std::vector<std::uint32_t>>> aux_triggers;
            legacy_->step(*unit.train, triggers, cab_triggers, aux_triggers);
            for (std::size_t i = 0; i < aux_triggers.size() && i < unit.cars.size(); ++i) {
                auto& pending = unit.cars[i].pending_aux_triggers;
                if (pending.size() < aux_triggers[i].size()) pending.resize(aux_triggers[i].size());
                for (std::size_t k = 0; k < aux_triggers[i].size(); ++k)
                    pending[k].insert(pending[k].end(), aux_triggers[i][k].begin(), aux_triggers[i][k].end());
            }
            for (std::size_t i = 0; i < triggers.size() && i < unit.cars.size(); ++i) {
                auto& pending = unit.cars[i].pending_triggers;
                pending.insert(pending.end(), triggers[i].begin(), triggers[i].end());
            }
            for (std::size_t i = 0; i < cab_triggers.size() && i < unit.cars.size(); ++i) {
                auto& pending = unit.cars[i].pending_cab_triggers;
                pending.insert(pending.end(), cab_triggers[i].begin(), cab_triggers[i].end());
            }
        }
        step_trains();
        collide_slides(library);
        for (auto& unit : units_) {
            unit.current = unit.train->placements();
            unit.current_travelled.resize(unit.train->cars().size());
            for (std::size_t i = 0; i < unit.current_travelled.size(); ++i) {
                unit.current_travelled[i] = unit.train->cars()[i].travelled;
            }
            if (unit.previous.size() != unit.current.size()) unit.previous = unit.current;
            if (unit.previous_travelled.size() != unit.current_travelled.size()) unit.previous_travelled = unit.current_travelled;
        }
    }

    const Unit* followed = follow_id_ != 0 ? find_unit(follow_id_) : nullptr;
    if (!followed) followed = &units_.front();
    if (follow_ && !followed->current.empty()) {
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
    update_engineer(input, camera, elapsed, library);
    update_pantographs(elapsed);
    update_exterior_doors(elapsed);
    update_lights(elapsed, input.light_eye.value_or(camera.eye()));
    if (sound) {
        update_sound(elapsed, camera, *sound);
    } else {
        // Silent (--no-sound): what the DLLs post must not pile up.
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
    auto* cab = engineer_cab();
    if (!cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) {
        set_engineer(false);
        return;
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
    const auto& unit = units_.front();
    if (unit.current.empty()) return;
    const auto body = body_pose(unit, engineer_car_, false);
    if (!body) return;
    follow_ = false;
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
    // On his feet only (seated, the passing world needs no surfaces read).
    if (engineer_.mode() == Engineer::Mode::seated) {
        walk_cars_.clear();
        walk_objects_near_.clear();
    } else {
        gather_walk_world(engineer_.mode() == Engineer::Mode::outside ? engineer_.feet() : camera.eye(), library);
    }
    const Outdoors outdoors{
        .floor = [this](const glm::vec3& p) { return walk_floor(p); },
        .blocked = [this](const glm::vec3& from, const glm::vec3& to) { return walk_blocked(from, to); },
    };
    engineer_.update(*cab, controls, body->transform * cab->placement(), outdoors, elapsed, camera);
    // RTS moves the cab camera by the body's offsets on its springs and draws
    // the cab without them (0x424488): the cab sways around the driver.
    if (engineer_.inside()) camera.position += sway_offset(unit, engineer_car_);
    if (const auto door = engineer_.take_pass()) pass_to_next_car(*door);
    look_ = camera.forward();
}

void TrainView::set_engineer(const bool on) {
    visit_cab_.reset();
    visit_requested_ = false;
    engineer_car_ = cab_car_;
    hovered_.reset();
    if (on) {
        engineer_.sit(0);
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
        created = CabView::create(*vfs_, *car.definition,
                                  formats::wagon_path(car.folder, car.definition->name, car.definition->engine.has_value()));
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
        message_ = tr("The next car has no door at this end.");
        return;
    }
    if (created) {
        visit_cab_ = std::move(created);
        visit_requested_ = false;
    }
    engineer_car_ = j;
    if (j == cab_car_) visit_cab_.reset();
    to->poll();
    engineer_.arrive(*to, *arrival);
    hovered_.reset();
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
    if (!direction || !engineer_.inside() || !cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) return;
    const auto body = body_pose(units_.front(), engineer_car_, false);
    if (!body) return;
    const glm::mat4 to_cab = glm::inverse(cab_to_scene_ ? *cab_to_scene_ : body->transform * cab->placement());
    const glm::vec3 origin{to_cab * glm::vec4{eye, 1.0f}};
    const glm::vec3 along = glm::normalize(glm::vec3{to_cab * glm::vec4{*direction, 0.0f}});
    // The reach is each control's Radius (RTS); the ray goes as far as any.
    hovered_ = cab->pick(origin, along, 4.0f);
    if (!hovered_ || !hovered_->control || (!click && scroll == 0.0f)) return;
    // The cab driven from, or the interior of another car whose DLL has it
    // (a salon's stop valves: the brakes library reads that car's switches).
    const bool driven = engineer_car_ == cab_car_ && legacy_ && legacy_->player() == cab_car_;
    const bool visited = !driven && legacy_ && engineer_car_ != cab_car_ && legacy_->has_cab(engineer_car_);
    if (!driven && !visited) {
        message_ = tr("The controls work in the cab you drive from.");
        return;
    }
    const auto car = driven ? SIZE_MAX : engineer_car_;
    const auto id = cab->description().switches[hovered_->index].id;
    // A CONTROLLERX / CONTROLLERY control: a press takes hold of it and only
    // moving the mouse moves it (RTS 0x4121BC); a quick click (let go within
    // the frame) leaves it where it is, it does not toggle.
    (void)held;
    if (click && legacy_->grab(id, cab->switch_frames_per_state(hovered_->index), car)) {
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
    if (engineer_.inside() || !placed()) return;
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
    } else {
        text += "; its train is gone";
    }
    if (!message_.empty()) text += "; message: " + message_;
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
    if (gone || far || engineer_.inside()) {
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
        const bool keep_this = unit_index < other_unit;
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
    if (!engineer_.inside() || !cab || units_.empty() || engineer_car_ >= units_.front().cars.size()) return;
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
void TrainView::feed_world(const Unit& unit) {
    auto& world = legacy_->world();
    world.day_time = day_time_;
    world.night = night_;
    world.signals_ahead.clear();
    world.route = {};
    world.signal = {};
    world.event = {};
    const auto& train = *unit.train;
    const auto limit = limits_->evaluate(train, unit.freight);
    world.route = {.limit = limit.current,
                   .next_limit = limit.ahead,
                   .distance = limit.ahead >= 0.0f ? static_cast<float>(limit.ahead_distance) : -1.0f};
    if (!signals_) return;
    const auto next = signals_->ahead(train);
    if (!next.valid) return;
    dllhost::SignalInput s;
    s.distance = static_cast<float>(next.distance);
    for (auto& a : s.aspect) a = static_cast<std::uint32_t>(next.aspect);
    s.speed_limit = next.speed;
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
// DLL wants or, without a DLL, while the train works.
void TrainView::feed_line() {
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        if (!unit.train->placed()) continue;
        const auto& states = unit.train->cars();
        const bool working = unit.role != Role::standing && !unit.train->asleep();
        std::vector<bool> raised(states.size(), false);
        bool electric = false;
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            const auto& car = unit.cars[i];
            const auto* engine = car.definition && car.definition->engine ? &*car.definition->engine : nullptr;
            if (!engine || engine->kind != formats::EngineKind::electric) continue;
            electric = true;
            if (!car.pantograph_nodes.empty() && car.pantograph_frames >= 2) {
                const float top = static_cast<float>(car.pantograph_frames - 1u) - 0.01f;
                for (const auto& [node, number] : car.pantograph_nodes) {
                    if (car.pantograph_frame[number - 1u] >= top) raised[i] = true;
                }
            } else if (const auto* out = u == 0 && legacy_ && legacy_->has_dll(i) ? legacy_->output(i) : nullptr) {
                raised[i] = out->panto_raised != 0;
            } else {
                raised[i] = working;
            }
        }
        const auto line = electrification_->apply(*unit.train, raised);
        if (u == 0) {
            line_ = line;
            line_engines_ = electric;
        }
    }
}

// The exterior lights a DLL switched on (LightObj Flags: 8 turn on, 1 on, 16
// reversed). Type 0 glows as its LightTexture, flat, facing where the light
// points (seen from the front only); types 1..4 light the scene through the
// shape shaders (RTS lights.glsl), the nearest nine.
void TrainView::update_lights(const float elapsed, const glm::vec3& eye) {
    light_glows_.clear();
    light_beams_.clear();
    interior_lights_.clear();
    if (units_.empty() || !legacy_) return;
    auto& unit = units_.front();
    // The lamps of the cab or salon the engineer is in (InternalLight State:
    // the DLL's SwitchLight or GM_LIGHT_ON by GlobalID), RTS's LightsInt.
    if (const auto* cab = engineer_.inside() ? engineer_cab() : nullptr; cab && engineer_car_ < unit.cars.size()) {
        const auto& lamps = cab->description().lights;
        const auto* out = legacy_->output(engineer_car_);
        const auto body = body_pose(unit, engineer_car_, false);
        lamp_level_.resize(lamps.size(), 0.0f);
        for (std::size_t k = 0; k < lamps.size() && body; ++k) {
            const auto& lamp = lamps[k];
            const bool on = out && k < out->internal_lights.size() && out->internal_lights[k] != 0;
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
    for (std::size_t i = 0; i < unit.cars.size(); ++i) {
        auto& car = unit.cars[i];
        const auto& lights = car.definition->lights;
        if (lights.empty()) continue;
        const auto* out = legacy_->output(i);
        if (!out) continue;
        car.light_level.resize(lights.size(), 0.0f);
        const auto body = body_pose(unit, i, !(engineer_.inside() && i == engineer_car_));
        for (std::size_t k = 0; k < lights.size(); ++k) {
            const auto& light = lights[k];
            const std::uint32_t flags = k < out->light_flags.size() ? out->light_flags[k] : 0u;
            const bool on = (flags & 9u) != 0 && !light.states.empty();
            auto& level = car.light_level[k];
            const float time = on ? light.fade_in : light.fade_out;
            const float goal = on ? 1.0f : 0.0f;
            level = time <= 0.0f ? goal : goal > level ? (std::min)(goal, level + elapsed / time) : (std::max)(goal, level - elapsed / time);
            if (level <= 0.0f || !body) continue;
            const auto& s = light.states.front();
            float yaw = glm::radians(s.azimuth[1]);
            if (flags & 16u) yaw += pi;
            const float pitch = glm::radians(s.elevation[0]);
            glm::mat4 local = glm::translate(glm::mat4{1.0f}, glm::vec3{s.position[0], s.position[1], s.position[2]});
            local = glm::rotate(local, yaw, glm::vec3{0.0f, 1.0f, 0.0f});
            local = glm::rotate(local, -pitch, glm::vec3{1.0f, 0.0f, 0.0f});
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
    std::sort(beams.begin(), beams.end(), [](const Beam& x, const Beam& y) { return x.distance < y.distance; });
    for (std::size_t k = 0; k < beams.size() && k < 9; ++k) light_beams_.push_back(beams[k].light);
}

void TrainView::update_exterior_doors(const float elapsed) {
    for (std::size_t u = 0; u < units_.size(); ++u) {
        auto& unit = units_[u];
        for (std::size_t i = 0; i < unit.cars.size(); ++i) {
            auto& car = unit.cars[i];
            if (car.exterior_doors.empty()) continue;
            const auto* out = u == 0 && legacy_ ? legacy_->output(i) : nullptr;
            CabView* cab = u == 0 && i == cab_car_ ? cab_.get() : u == 0 && i == engineer_car_ ? visit_cab_.get() : nullptr;
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
        for (std::size_t i = 0; i < unit.cars.size() && i < states.size(); ++i) {
            auto& car = unit.cars[i];
            if (car.pantograph_nodes.empty() || car.pantograph_frames < 2) continue;
            const auto* engine = car.definition->engine ? &*car.definition->engine : nullptr;
            const auto* out = u == 0 && legacy_ && legacy_->has_dll(i) ? legacy_->output(i) : nullptr;
            if (out) {
                car.pantograph_raised = out->panto_raised;
            } else if (engine && engine->kind == formats::EngineKind::electric && working) {
                // The rear pantograph by the direction of travel (the
                // train front when standing), in the car's own axes.
                float lowest = 1e9f, highest = -1e9f;
                std::uint8_t rear = 1, front = 1;
                for (const auto& [node, number] : car.pantograph_nodes) {
                    const float z = car.pantograph_z[number - 1u];
                    if (z < lowest) { lowest = z; rear = number; }
                    if (z > highest) { highest = z; front = number; }
                }
                const bool backward = unit.train->speed() < -0.1;
                const bool towards_shape_front = backward == states[i].flipped;
                car.pantograph_raised = static_cast<std::uint8_t>(1u << ((towards_shape_front ? rear : front) - 1u));
            } else {
                car.pantograph_raised = 0;
            }
            const float last = static_cast<float>(car.pantograph_frames - 1u);
            const float step = last * elapsed / 2.5f;
            for (std::size_t n = 0; n < car.pantograph_frame.size(); ++n) {
                const float target = (car.pantograph_raised >> n) & 1u ? last : 0.0f;
                float& frame = car.pantograph_frame[n];
                frame = frame < target ? (std::min)(frame + step, target) : (std::max)(frame - step, target);
            }
        }
    }
}

void TrainView::update_sound(const float elapsed, const ViewerCamera& camera, audio::AudioEngine& sound) {
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
    const auto owner = u == 0 && unit.role == Role::driven ? formats::SoundOwner::player_train : formats::SoundOwner::train;
    for (std::size_t i = 0; i < unit.cars.size() && i < states.size() && i < unit.current.size(); ++i, ++seed) {
        auto& car = unit.cars[i];
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
        inputs[formats::SoundVariable::speed] = state.velocity * direction;
        inputs[formats::SoundVariable::distance] = glm::distance(camera.eye(), position);
        inputs[formats::SoundVariable::distance_travelled] = static_cast<float>(std::abs(state.travelled));
        inputs[formats::SoundVariable::acceleration] = state.acceleration * direction;
        inputs[formats::SoundVariable::radius] = state.radius;
        const auto* dll_vars = u == 0 && legacy_ ? legacy_->sound_variables(i) : nullptr;
        if (dll_vars) {
            // The DLL sets the engine variables itself (SMSObject Var1..Var8).
            for (int k = 0; k < 8; ++k)
                inputs.variables[static_cast<std::size_t>(formats::SoundVariable::variable1) + static_cast<std::size_t>(k)] =
                    (*dll_vars)[static_cast<std::size_t>(k)];
        } else if (car.definition->engine && driver_ && u == 0) {
            // MSTS meaning of the engine variables (a DLL sets them in RTS).
            inputs[formats::SoundVariable::variable1] =
                static_cast<float>(driver_->throttle) / static_cast<float>((std::max)(driver_->notches, 1u));
            const float limit = (std::max)(car.definition->mass * 10.0f * (std::max)(car.definition->adhesion[1], 0.1f), 1.0f);
            inputs[formats::SoundVariable::variable2] = std::abs(state.force) / limit;
            inputs[formats::SoundVariable::variable3] = state.cylinder / 5.0f;
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
    const bool player_train = u == 0 && unit.role == Role::driven && legacy_ && car.definition->engine;
    if (!player_train) {
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
    const bool in_car = engineer_.inside() && i == cab_car_;
    static const std::vector<std::uint32_t> none;
    for (auto& aux : car.aux_sounds) {
        const auto& library = libraries[aux.library];
        auto* pending = aux.library < car.pending_aux_triggers.size() ? &car.pending_aux_triggers[aux.library] : nullptr;
        if (library.sound_single_wagon && !in_car) {
            if (aux.instance && aux.instance->active()) aux.instance->stop(sound);
            if (pending) pending->clear();
            continue;
        }
        if (!aux.instance) {
            const auto owner = formats::SoundOwner::player_train;
            const auto* group = aux.script->select_group(owner);
            if (!group) continue;
            auto lookup = [this, path = aux.path](const std::string& name) -> std::shared_ptr<const audio::AudioClip> {
                const auto file = audio::find_sound_file(*vfs_, path, name);
                return file ? bank_->get(*file) : nullptr;
            };
            aux.instance = std::make_unique<audio::SoundInstance>(aux.script, *group, owner, std::move(lookup),
                                                                  static_cast<std::uint32_t>(7907u * (i + 1) + aux.library));
            aux.instance->preload();
        }
        audio::SoundInputs inputs = exterior;
        inputs.camera = sound_camera();
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
        inputs.position = library.sound_type == formats::AuxSoundType::ambient || !library.sound_external ? camera.eye() : at;
        const auto* vars = library.sound_own_stack ? legacy_->aux_sound_variables(i, aux.library) : legacy_->sound_variables(i);
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
        }
    }
    if (!car.cab_sound) {
        car.pending_cab_triggers.clear();
        return;
    }
    audio::SoundInputs inputs = exterior;
    inputs.camera = sound_camera();
    const bool inside = inputs.camera != audio::SoundCamera::external;
    // Heard at the seat when inside, at the cab end of the body otherwise.
    if (inside) inputs.position = camera.eye();
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
        if (!model) continue;
        const bool engineer_here = engineer_.inside() && &unit == &units_.front() && i == engineer_car_;
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
        const auto* inside = engineer_.inside() && &unit == &units_.front() && i == engineer_car_ ? engineer_cab() : nullptr;
        if (inside) {
            // In the cab: the cab shape instead of the body (RTS hides the
            // exterior unless the engine says DoNotHideExtInCabin); the body
            // until the cab shape is there.
            if (auto* cab_model = library.find(inside->shape()); cab_model && inside->bound()) {
                adjustments.clear();
                inside->adjustments(legacy_ ? legacy_->output(i) : nullptr, adjustments, signal_aspect(unit));
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
                {
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
                    cab->screens(legacy_ ? legacy_->output(i) : nullptr, frame_elapsed_, screen_batches_);
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
        for (const auto& [matrix, shape_front] : car.bogies) {
            // A bogie follows the track heading at its own position.
            const bool train_front = shape_front != car.flipped;
            float delta = wrap_angle((train_front ? here.front_heading : here.rear_heading) - body_heading);
            if (std::abs(delta) > 0.5f) delta = wrap_angle(delta + pi); // a heading along the node's other direction
            if (std::abs(delta) > 0.5f) delta = 0.0f;
            adjustments.push_back({.matrix = matrix, .local = glm::rotate(glm::mat4{1.0f}, delta, glm::vec3{0.0f, 1.0f, 0.0f})});
        }
        renderer.submit(*model, transform, adjustments);
    }
    }
}

glm::vec3 TrainView::sway_offset(const Unit& unit, const std::size_t i) const {
    if (sway_scale_ <= 0.0f || i >= unit.cars.size() || i >= unit.train->cars().size()) return {};
    const auto& state = unit.train->cars()[i];
    const auto here = pose(unit, i);
    const auto axis = here.front - here.rear;
    const float span = glm::length(axis);
    if (span < 0.01f) return {};
    const float heading = std::atan2(axis.x, axis.z);
    const glm::vec3 turning{std::cos(heading), 0.0f, -std::sin(heading)};
    return (turning * state.body[0] + glm::vec3{0.0f, state.body[1], 0.0f} + axis / span * state.body[2]) * sway_scale_;
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
        centre += (turning * state.body[0] + glm::vec3{0.0f, state.body[1], 0.0f} + along * state.body[2]) * sway_scale_;
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
    // What Enter does where the engineer stands.
    if (engineer_.active() && !engineer_.prompt().empty()) {
        const auto size = ImGui::GetIO().DisplaySize;
        const char* text = engineer_.prompt().c_str();
        const auto text_size = ImGui::CalcTextSize(text);
        const ImVec2 corner{(size.x - text_size.x) * 0.5f, size.y * 0.78f};
        auto* list = ImGui::GetForegroundDrawList();
        list->AddRectFilled({corner.x - 8.0f, corner.y - 4.0f}, {corner.x + text_size.x + 8.0f, corner.y + text_size.y + 4.0f},
                            0xa0000000u, 4.0f);
        list->AddText(corner, 0xffffffffu, text);
    }
    if (!ImGui::CollapsingHeader(tr("Train"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    cab_controls();
    if (legacy_) {
        bool keys = legacy_->keyboard();
        if (ImGui::Checkbox(tr("Cab keys of the locomotive DLL"), &keys)) legacy_->set_keyboard(keys);
    } else if (!legacy_status_.empty()) {
        ImGui::TextWrapped("%s %s", tr("Locomotive DLLs do not run:"), legacy_status_.c_str());
    }

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
        if (loaded()) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Change ends (X)"))) pending_turn_ = true;
        }
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
    if (!message_.empty()) ImGui::TextUnformatted(message_.c_str());
    if (!loaded()) return;
    const auto& train = driven();

    ImGui::Text(tr("%s: %zu cars, %.0f m, %.0f t"), consist_.c_str(), units_.front().cars.size(), train.length(),
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
