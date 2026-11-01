#ifdef OPENRTS_HAS_WGPU
#include "train_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
        return "I charge";
    case simulation::DriverHandles::BrakeHandle::running:
        return "II running";
    case simulation::DriverHandles::BrakeHandle::lap:
        return "III lap";
    case simulation::DriverHandles::BrakeHandle::lap_feed:
        return "IV lap+feed";
    case simulation::DriverHandles::BrakeHandle::ep_service:
        return "VE EP service";
    case simulation::DriverHandles::BrakeHandle::service:
        return "V service";
    case simulation::DriverHandles::BrakeHandle::emergency:
        return "VI emergency";
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
    std::vector<std::pair<std::int32_t, bool>> bogies; // matrix, is front bogie (in the shape)
    std::shared_ptr<const formats::SoundScript> script;
    core::VirtualPath script_path;
    std::unique_ptr<audio::SoundInstance> sound;
    std::optional<formats::SoundOwner> sound_owner; // what `sound` was built for (none: no group for it)
    std::vector<formats::SoundEvent> pending_events;
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
    unit.role = Role::driven;
    driver_ = simulation::attach_keyboard_driver(*unit.train);
    units_.push_back(std::move(unit));
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
    message_ = "on path " + path.file + (path.name.empty() ? "" : " (" + path.name + ")");
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
        message_ = "stop the train to change ends";
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
    message_ = "now driving from the other end";
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
        const auto find = [&](const simulation::Train* train) {
            return static_cast<std::size_t>(std::find_if(units_.begin(), units_.end(), [&](const Unit& unit) {
                                                return unit.train.get() == train;
                                            }) - units_.begin());
        };
        const auto s = find(result.survivor);
        const auto a = find(result.absorbed);
        if (s >= units_.size() || a >= units_.size()) continue;
        auto absorbed_cars = std::move(units_[a].cars);
        if (result.absorbed_turned) {
            std::reverse(absorbed_cars.begin(), absorbed_cars.end());
            for (auto& car : absorbed_cars) car.flipped = !car.flipped;
        }
        auto& cars = units_[s].cars;
        if (result.absorbed_ahead) {
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
        message_ = "coupled: " + std::to_string(units_[survivor].cars.size()) + " cars";
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
                message_ = "derailed: car " + std::to_string(event.car + 1)
                    + (u == 0 ? "; C or \"Put on the path start\" puts the train back" : " of a cut");
                break;
            }
        }
    }
    // A broken coupler uncouples there with the cocks open (RTS: "Train
    // damaged"); both parts lose their air and brake.
    for (auto it = breaks.rbegin(); it != breaks.rend(); ++it) {
        split(it->first, it->second, false, false);
        message_ = "train damaged: the coupler behind car " + std::to_string(it->second) + " broke";
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
                ImGui::Text("Overall: mass %.1f t, length %.0f m, units %zu (passenger %zu, freight %zu, engines %zu)",
                            train.mass() / 1000.0, train.length(), unit->cars.size(), passenger, freight, engines);
            } else {
                char next[64] = "none";
                if (limit.ahead > 0.0f) std::snprintf(next, sizeof next, "%.0f km/h in %.0f m", limit.ahead * 3.6f, limit.ahead_distance);
                ImGui::Text("Overall: speed %.2f km/h, acceleration %.3f m/s2, limit %.0f km/h, next limit %s, km %s",
                            train.speed() * 3.6, train.cars().front().acceleration, limit.current * 3.6f, next,
                            ordinate ? (std::to_string(static_cast<int>(*ordinate)) + "." + std::to_string(static_cast<int>(std::abs(*ordinate) * 100.0) % 100)).c_str() : "-");
                const auto& states = train.cars();
                const auto last = (std::min)(car_page_ + car_page_size, unit->cars.size());
                for (std::size_t i = car_page_; i < last && i < states.size(); ++i) {
                    const auto& car = unit->cars[i];
                    const auto& state = states[i];
                    ImGui::Text("%02zu - %s. Mass %.1f t", i + 1, car.definition ? car.definition->name.c_str() : "?",
                                car.definition ? car.definition->mass / 1000.0f : 0.0f);
                    ImGui::Text("     Pipe %.3f Aux %.3f Cylinder %.3f  Velocity %.2f Acceleration %.3f  Force %.1f kN BF %.1f kN CF %.0f kN%s%s",
                                state.pipe, state.auxiliary, state.cylinder, state.velocity, state.acceleration, state.force / 1000.0f,
                                state.brake_force / 1000.0f, state.coupler_force / 1000.0f,
                                state.handbrake > 0.0f ? (" [HANDBRAKE " + std::to_string(static_cast<int>(state.handbrake * 100.0f)) + "%]").c_str() : "",
                                state.derailed ? " [DERAILED]" : "");
                }
                ImGui::TextDisabled("F5: next page (%zu-%zu of %zu)  Shift+F5: the whole train", car_page_ + 1, last, unit->cars.size());
            }
        }
        end_panel(car_info_place_);
    }
    if (limits_panel_) {
        if (begin_panel("##limits", limits_place_, 1.0f - margin_x, margin_y)) {
            if (ordinate) {
                const double km = std::floor(*ordinate);
                const int picket = static_cast<int>((*ordinate - km) * 10.0) + 1;
                ImGui::Text("km %.0f  pk %d  (%.3f)", km, picket, *ordinate);
            } else {
                ImGui::TextDisabled("km -");
            }
            ImGui::Text("Limit %.0f km/h", limit.current * 3.6f);
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
                    ImGui::Text("Signal %s in %.0f m%s", formats::to_string(next.aspect), next.distance,
                                next.speed > 0.0f ? (", " + std::to_string(static_cast<int>(next.speed * 3.6f)) + " km/h").c_str() : "");
                }
            }
        }
        end_panel(limits_place_);
    }
    if (stations_panel_) {
        if (begin_panel("##stations", stations_place_, 1.0f - margin_x, 0.45f)) {
            ImGui::Text("Stations ahead: %s", unit->name.c_str());
            ImGui::Separator();
            const auto stations = stations_ahead(*unit);
            if (stations.empty()) ImGui::TextDisabled("none within 40 km");
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
                    std::snprintf(text, sizeof text, distance >= 1000.0 ? "in %.2f km" : "in %.0f m",
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
    std::snprintf(text, sizeof text, "%s: leg %zu/%zu, %.0f of %.0f m, way reserved for %.0f m", route->name.c_str(),
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
                message_ = "a derailed car hit " + std::string(object.source_path.filename());
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

void TrainView::update(const SceneInput& input, ViewerCamera& camera, const float elapsed,
                       renderer::ModelLibrary& library, audio::AudioEngine* sound) {
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
                message_ = "the route has no paths: C puts the train on the track nearest to the camera";
            } else {
                const auto index = static_cast<std::size_t>(std::clamp(path_choice_, 0, static_cast<int>(paths_.size()) - 1));
                if (const auto error = place_on_path(paths_[index]); !error.empty()) message_ = error;
            }
        }
    }
    if (!placed()) return;
    if (input.toggle_follow) follow_ = !follow_;
    if (input.turn_around || pending_turn_) {
        pending_turn_ = false;
        turn_around();
    }
    if (pending_uncouple_) {
        const auto index = *std::exchange(pending_uncouple_, std::nullopt);
        if (std::abs(driven().speed()) > 0.1) {
            message_ = "stop the train to uncouple";
        } else {
            split(0, index, true, true);
            message_ = "uncoupled before car " + std::to_string(index + 1) + " (end cocks closed)";
        }
    }
    apply_handles(input);

    accumulator_ += (std::min)(elapsed, 0.25f);
    while (accumulator_ >= simulation::train_step) {
        accumulator_ -= simulation::train_step;
        for (auto& unit : units_) {
            unit.previous = std::move(unit.current);
            unit.previous_travelled = std::move(unit.current_travelled);
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
    if (sound) update_sound(elapsed, camera, *sound);
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
        if (!car.sound) {
            car.pending_events.clear();
            continue;
        }
        const auto here = pose(unit, i);
        const auto position = (here.front + here.rear) * 0.5f;
        const float direction = state.flipped ? -1.0f : 1.0f;
        audio::SoundInputs inputs{.camera = audio::SoundCamera::external, .position = position};
        inputs[formats::SoundVariable::speed] = state.velocity * direction;
        inputs[formats::SoundVariable::distance] = glm::distance(camera.eye(), position);
        inputs[formats::SoundVariable::distance_travelled] = static_cast<float>(std::abs(state.travelled));
        inputs[formats::SoundVariable::acceleration] = state.acceleration * direction;
        inputs[formats::SoundVariable::radius] = state.radius;
        if (car.definition->engine && driver_ && u == 0) {
            // MSTS meaning of the engine variables (a DLL sets them in RTS).
            inputs[formats::SoundVariable::variable1] =
                static_cast<float>(driver_->throttle) / static_cast<float>((std::max)(driver_->notches, 1u));
            const float limit = (std::max)(car.definition->mass * 10.0f * (std::max)(car.definition->adhesion[1], 0.1f), 1.0f);
            inputs[formats::SoundVariable::variable2] = std::abs(state.force) / limit;
            inputs[formats::SoundVariable::variable3] = state.cylinder / 5.0f;
        }
        inputs.events = car.pending_events;
        car.sound->update(elapsed, inputs, sound);
        car.pending_events.clear();
    }
    }
}

void TrainView::stop_sound(audio::AudioEngine& sound) {
    for (auto& unit : units_) {
        for (auto& car : unit.cars) {
            if (car.sound) car.sound->stop(sound);
        }
    }
}

void TrainView::submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library) {
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
        auto here = pose(unit, i);
        const auto& state = states[i];
        if (const auto slid = slid_pose(unit, i)) here = *slid;
        const float ahead = state.bogie_ahead > 0.0f ? state.bogie_ahead : car.definition->length * 0.35f;
        const float behind = state.bogie_behind > 0.0f ? state.bogie_behind : car.definition->length * 0.35f;
        const auto axis = here.front - here.rear;
        const float span = glm::length(axis);
        if (span < 0.01f) continue;
        auto centre = here.rear + axis * (behind / (ahead + behind));
        const float body_heading = std::atan2(axis.x, axis.z);
        if (sway_scale_ > 0.0f) {
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
    if (!ImGui::CollapsingHeader("Train", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (!paths_.empty()) {
        path_choice_ = std::clamp(path_choice_, 0, static_cast<int>(paths_.size()) - 1);
        const auto label = [](const formats::TrainPath& path) {
            return path.file + (path.name.empty() ? "" : ": " + path.name);
        };
        if (ImGui::BeginCombo("Path", label(paths_[static_cast<std::size_t>(path_choice_)]).c_str())) {
            for (int i = 0; i < static_cast<int>(paths_.size()); ++i) {
                if (ImGui::Selectable(label(paths_[static_cast<std::size_t>(i)]).c_str(), i == path_choice_)) path_choice_ = i;
            }
            ImGui::EndCombo();
        }
        if (loaded() && ImGui::Button("Put on the path start")) pending_path_ = true;
        if (loaded()) {
            ImGui::SameLine();
            if (ImGui::Button("Change ends (X)")) pending_turn_ = true;
        }
    } else {
        ImGui::TextDisabled("The route has no paths (PATHS/*.pat).");
    }
    if (ImGui::TreeNode("Consist")) {
        if (!consists_listed_) list_consists();
        ImGui::InputText("Filter", filter_, sizeof(filter_));
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
        if (consist_choice_ >= 0 && consist_choice_ < static_cast<int>(consists_.size()) && ImGui::Button("Load on the path")) {
            pending_load_ = consists_[static_cast<std::size_t>(consist_choice_)];
        }
        ImGui::TreePop();
    }
    if (!message_.empty()) ImGui::TextUnformatted(message_.c_str());
    if (!loaded()) return;
    const auto& train = driven();

    ImGui::Text("%s: %zu cars, %.0f m, %.0f t", consist_.c_str(), units_.front().cars.size(), train.length(),
                train.mass() / 1000.0);
    if (!placed()) {
        ImGui::TextUnformatted("Not on the track: C puts it on the track nearest to the camera.");
        return;
    }
    const auto& head = train.cars().front();
    double worst_coupler = 0.0;
    for (const auto& car : train.cars()) worst_coupler = (std::max)(worst_coupler, std::abs(static_cast<double>(car.coupler_force)));
    ImGui::Text("Speed %.1f km/h, acceleration %.2f m/s2%s", train.speed() * 3.6, head.acceleration,
                train.derailed() ? "  DERAILED" : "");
    if (driver_) {
        ImGui::Text("Throttle %u/%u  reverser %s  brake %s  independent %.1f atm", driver_->throttle, driver_->notches,
                    driver_->reverser > 0 ? "forward" : driver_->reverser < 0 ? "back" : "neutral",
                    handle_name(driver_->brake), driver_->independent);
        ImGui::Text("Equalising reservoir %.2f atm (II keeps %.1f)", driver_->equalising, driver_->charging_pressure);
        ImGui::Checkbox("EP brake (K)", &driver_->ep_brake);
        if (driver_->ep_brake) {
            ImGui::SameLine();
            ImGui::Text("demand %.2f atm (VE/V apply, III/IV hold, I/II release)", driver_->ep_target);
        }
    }
    ImGui::Text("Pipe %.2f  cylinder %.2f  aux %.2f  main reservoir %.2f atm", head.pipe, head.cylinder, head.auxiliary,
                head.main_reservoir);
    const auto& last = train.cars().back();
    ImGui::Text("Last car: pipe %.2f cylinder %.2f; worst coupler %.0f kN", last.pipe, last.cylinder, worst_coupler / 1000.0);
    ImGui::Text("Joints %zu, junctions %zu%s; coupler breaks %zu, derailments %zu", joints_, junctions_,
                end_of_track_ ? ", END OF TRACK" : "", coupler_breaks_, derailments_);
    ImGui::SliderFloat("Body sway", &sway_scale_, 0.0f, 10.0f, "%.1f");
    ImGui::SameLine();
    ImGui::TextDisabled("(1 = RTS model; RTS shows it in the cab only)");
    ImGui::Checkbox("Car loops start at random points", &random_loop_phase_);
    ImGui::SameLine();
    ImGui::TextDisabled("(openRTS: identical loops on many cars do not add up in phase; RTS starts them together)");
    if (ImGui::SliderInt("Coupler sub-steps", &substeps_, 1, 4)) {
        for (auto& unit : units_) unit.train->set_coupler_substeps(static_cast<std::uint32_t>(substeps_));
    }
    ImGui::SameLine();
    ImGui::TextDisabled(substeps_ == 1 ? "(RTS)" : "(experimental)");
    if (ImGui::TreeNode("Cars and coupling")) {
        // Uncoupling as from the RTS menu: both end cocks closed, the
        // couplers left open so the parts do not couple again by themselves.
        const auto& cars = units_.front().cars;
        for (std::size_t i = 0; i < cars.size(); ++i) {
            const auto& state = train.cars()[i];
            ImGui::Text("%2zu %s%s%s", i + 1, cars[i].definition ? cars[i].definition->name.c_str() : "?",
                        state.flipped ? " (turned)" : "", state.derailed ? " DERAILED" : "");
            if (i + 1 < cars.size()) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("uncouple here")) pending_uncouple_ = i + 1;
                ImGui::PopID();
            }
        }
        if (units_.size() > 1) {
            ImGui::Separator();
            for (std::size_t u = 1; u < units_.size(); ++u) {
                const auto& cut = *units_[u].train;
                ImGui::Text("Cut %zu: %zu cars, %.1f km/h, pipe %.2f atm%s", u, cut.cars().size(), cut.speed() * 3.6,
                            cut.cars().empty() ? 0.0f : cut.cars().front().pipe, cut.derailed() ? ", derailed" : "");
            }
            ImGui::TextDisabled("Push gently into a cut with automatic couplers to couple; open couplers only push.");
        }
        ImGui::TreePop();
    }
    ImGui::Checkbox("Camera follows (V)", &follow_);
    ImGui::TextUnformatted("Home/End: throttle  PgUp/PgDn: reverser  ] [: brake valve V..I (hold [ in I)  , .: independent");
#endif
}

} // namespace openrts::viewer
#endif
