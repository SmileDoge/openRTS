#ifdef OPENRTS_HAS_WGPU
#include "train_view.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <openrts/formats/sound.hpp>
#include <openrts/simulation/track_traveller.hpp>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

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
    std::vector<formats::SoundEvent> pending_events;
};

TrainView::TrainView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const formats::RoutePosition origin,
                     simulation::TrackNetwork& network)
    : vfs_(std::move(vfs)), route_(std::move(route)), origin_(origin), network_(&network) {
    paths_ = formats::load_train_paths(*vfs_, route_);
    std::erase_if(paths_, [](const formats::TrainPath& path) { return !path.ok(); });
}

TrainView::~TrainView() = default;

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

std::string TrainView::load(const std::string_view consist_name) {
    const auto consist = formats::load_consist(*vfs_, consist_name);
    if (!consist.ok()) return "consist '" + std::string(consist_name) + "': " + consist.error;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    std::vector<Car> cars;
    std::vector<simulation::CarSetup> setups;
    for (const auto& entry : consist.entries) {
        auto wagon = std::make_unique<formats::WagonDefinition>(
            formats::load_wagon(*vfs_, entry.folder, entry.name, entry.engine));
        if (!wagon->ok()) return entry.folder + "/" + entry.name + ": " + wagon->error;
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
    consist_ = consist.name;
    train_.reset();
    cars_ = std::move(cars);
    stock_ = std::move(stock);
    train_ = std::make_unique<simulation::Train>(std::move(setups));
    train_->set_coupler_substeps(static_cast<std::uint32_t>(substeps_));
    driver_ = simulation::attach_keyboard_driver(*train_);
    previous_.clear();
    current_.clear();
    joints_ = junctions_ = coupler_breaks_ = derailments_ = 0;
    end_of_track_ = false;
    return {};
}

std::string TrainView::place(const std::uint32_t node, const double offset, const bool forward) {
    if (!train_) return "no consist loaded";
    std::string error;
    if (!train_->place(*network_, node, offset, forward, &error)) return error;
    previous_ = current_ = train_->placements();
    previous_travelled_.assign(train_->cars().size(), 0.0);
    current_travelled_ = previous_travelled_;
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
    if (!train_) return "no consist loaded";
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
    front.move(train_->length(), simulation::JunctionMode::plain, placement_owner);
    auto error = place(front.node(), front.offset(), front.forward());
    if (!error.empty()) return error;
    message_ = "on path " + path.file + (path.name.empty() ? "" : " (" + path.name + ")");
    return {};
}

std::optional<simulation::TrackTraveller> TrainView::ahead() const {
    if (!placed()) return std::nullopt;
    const double speed = train_->speed();
    const bool backward = speed < -0.1 || (std::abs(speed) <= 0.1 && driver_ && driver_->reverser < 0);
    if (!backward) return train_->front_end();
    const auto rear = train_->rear_end();
    if (!rear) return std::nullopt;
    return simulation::TrackTraveller(*network_, rear->node(), rear->offset(), !rear->forward());
}

glm::vec3 TrainView::head_position() const {
    if (current_.empty()) return {};
    return pose(0).front;
}

void TrainView::select_path(const formats::TrainPath& path) {
    path_choice_ = static_cast<int>(&path - paths_.data());
    pending_path_ = true;
}

void TrainView::request_models(renderer::ModelLibrary& library) {
    for (auto& car : cars_) {
        if (car.requested || car.shape.empty() || library.find(car.shape)) continue;
        // One load per shape: cars sharing a shape wait for the first.
        const bool pending = std::any_of(cars_.begin(), cars_.end(), [&](const Car& other) {
            return &other != &car && other.requested && other.shape == car.shape && other.loading.valid();
        });
        if (pending) continue;
        car.requested = true;
        car.loading = std::async(std::launch::async, [vfs = vfs_, path = car.shape] {
            return renderer::load_model_asset(*vfs, path, {});
        });
    }
    for (auto& car : cars_) {
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
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        auto& car = cars_[i];
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
                train_->set_bogies(i, -rear_z, front_z);
            } else {
                train_->set_bogies(i, front_z, -rear_z);
            }
        }
    }
}

void TrainView::apply_handles(const SceneInput& input) {
    using Handle = simulation::DriverHandles::BrakeHandle;
    if (!driver_) return;
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
    if (!train_ || !train_->turn_around()) {
        message_ = "stop the train to change ends";
        return;
    }
    std::reverse(cars_.begin(), cars_.end());
    for (auto& car : cars_) car.flipped = !car.flipped;
    // Controllers moved with their cars; the master must be the new front
    // engine, so the driver is put on again with the old handles.
    const auto old = driver_ ? std::optional<simulation::DriverHandles>{*driver_} : std::nullopt;
    driver_ = simulation::attach_keyboard_driver(*train_);
    if (driver_ && old) {
        const auto notches = driver_->notches;
        *driver_ = *old;
        driver_->notches = notches;
        driver_->throttle = 0;
        driver_->reverser = 0;
    }
    previous_ = current_ = train_->placements();
    current_travelled_.resize(train_->cars().size());
    for (std::size_t i = 0; i < current_travelled_.size(); ++i) current_travelled_[i] = train_->cars()[i].travelled;
    previous_travelled_ = current_travelled_;
    message_ = "now driving from the other end";
}

void TrainView::step_train() {
    train_->step();
    for (const auto& event : train_->take_events()) {
        switch (event.kind) {
        case simulation::TrainEventKind::joint:
            ++joints_;
            if (event.car < cars_.size()) {
                cars_[event.car].pending_events.push_back(formats::SoundEvent::wagon_on_joint);
                cars_[event.car].pending_events.push_back(formats::SoundEvent::wagon_on_any_joint);
            }
            break;
        case simulation::TrainEventKind::junction:
            ++junctions_;
            if (event.car < cars_.size()) cars_[event.car].pending_events.push_back(formats::SoundEvent::wagon_on_junction);
            break;
        case simulation::TrainEventKind::end_of_track:
            end_of_track_ = true;
            break;
        case simulation::TrainEventKind::coupler_break:
            ++coupler_breaks_;
            break;
        case simulation::TrainEventKind::derailment:
            ++derailments_;
            break;
        }
    }
}

float TrainView::alpha() const noexcept {
    return static_cast<float>(std::clamp(accumulator_ / simulation::train_step, 0.0, 1.0));
}

// The car's bogie points between the last two steps, at the render time.
TrainView::Pose TrainView::pose(const std::size_t car) const {
    const auto& now = current_[car];
    const auto& before = car < previous_.size() ? previous_[car] : now;
    const float t = alpha();
    const float lift = cars_[car].lift;
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
    if (!train_) return;
    request_models(library);
    fit_shapes(library);
    if (pending_path_) {
        // Bogies of every car first: they set the car spacing on the track.
        const bool shapes_ready = std::all_of(cars_.begin(), cars_.end(), [](const Car& car) {
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
    apply_handles(input);

    accumulator_ += (std::min)(elapsed, 0.25f);
    while (accumulator_ >= simulation::train_step) {
        accumulator_ -= simulation::train_step;
        previous_ = std::move(current_);
        previous_travelled_ = std::move(current_travelled_);
        step_train();
        current_ = train_->placements();
        current_travelled_.resize(train_->cars().size());
        for (std::size_t i = 0; i < current_travelled_.size(); ++i) current_travelled_[i] = train_->cars()[i].travelled;
    }
    if (previous_.size() != current_.size()) previous_ = current_;
    if (previous_travelled_.size() != current_travelled_.size()) previous_travelled_ = current_travelled_;

    if (follow_ && !current_.empty()) {
        // From the interpolated head car, as it is drawn.
        const auto head = pose(0);
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
    const auto& states = train_->cars();
    for (std::size_t i = 0; i < cars_.size() && i < states.size() && i < current_.size(); ++i) {
        auto& car = cars_[i];
        const auto& state = states[i];
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
            if (car.script) {
                if (const auto* group = car.script->select_group(formats::SoundOwner::player_train)) {
                    auto lookup = [this, path = car.script_path](const std::string& name) -> std::shared_ptr<const audio::AudioClip> {
                        const auto file = audio::find_sound_file(*vfs_, path, name);
                        return file ? bank_->get(*file) : nullptr;
                    };
                    car.sound = std::make_unique<audio::SoundInstance>(car.script, *group, formats::SoundOwner::player_train,
                                                                       std::move(lookup), static_cast<std::uint32_t>(i * 7919 + 17));
                }
            }
        }
        if (!car.sound) {
            car.pending_events.clear();
            continue;
        }
        const auto here = pose(i);
        const auto position = (here.front + here.rear) * 0.5f;
        const float direction = state.flipped ? -1.0f : 1.0f;
        audio::SoundInputs inputs{.camera = audio::SoundCamera::external, .position = position};
        inputs[formats::SoundVariable::speed] = state.velocity * direction;
        inputs[formats::SoundVariable::distance] = glm::distance(camera.eye(), position);
        inputs[formats::SoundVariable::distance_travelled] = static_cast<float>(std::abs(state.travelled));
        inputs[formats::SoundVariable::acceleration] = state.acceleration * direction;
        inputs[formats::SoundVariable::radius] = state.radius;
        if (car.definition->engine && driver_) {
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

void TrainView::stop_sound(audio::AudioEngine& sound) {
    for (auto& car : cars_) {
        if (car.sound) car.sound->stop(sound);
    }
}

void TrainView::submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library) {
    if (!placed() || current_.empty()) return;
    const auto& states = train_->cars();
    const float t = alpha();
    std::vector<renderer::NodeAdjustment> adjustments;
    for (std::size_t i = 0; i < cars_.size() && i < current_.size(); ++i) {
        auto& car = cars_[i];
        auto* model = library.find(car.shape);
        if (!model) continue;
        const auto here = pose(i);
        const auto& state = states[i];
        const float ahead = state.bogie_ahead > 0.0f ? state.bogie_ahead : car.definition->length * 0.35f;
        const float behind = state.bogie_behind > 0.0f ? state.bogie_behind : car.definition->length * 0.35f;
        const auto axis = here.front - here.rear;
        const float span = glm::length(axis);
        if (span < 0.01f) continue;
        const auto centre = here.rear + axis * (behind / (ahead + behind));
        const float body_heading = std::atan2(axis.x, axis.z);
        float heading = body_heading;
        float up = std::asin(std::clamp(axis.y / span, -1.0f, 1.0f));
        if (car.flipped) {
            heading += pi;
            up = -up;
        }
        auto transform = glm::translate(glm::mat4{1.0f}, centre);
        transform = glm::rotate(transform, heading, glm::vec3{0.0f, 1.0f, 0.0f});
        transform = glm::rotate(transform, -up, glm::vec3{1.0f, 0.0f, 0.0f});

        adjustments.clear();
        // Wheels turn by the distance run, interpolated like the body.
        const double travelled = i < current_travelled_.size()
            ? previous_travelled_[i] + (current_travelled_[i] - previous_travelled_[i]) * t
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
        if (train_ && ImGui::Button("Put on the path start")) pending_path_ = true;
        if (train_) {
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
    if (!train_) return;

    ImGui::Text("%s: %zu cars, %.0f m, %.0f t", consist_.c_str(), cars_.size(), train_->length(),
                [&] {
                    double mass = 0.0;
                    for (const auto& wagon : stock_) mass += wagon->mass;
                    return mass / 1000.0;
                }());
    if (!placed()) {
        ImGui::TextUnformatted("Not on the track: C puts it on the track nearest to the camera.");
        return;
    }
    const auto& head = train_->cars().front();
    double worst_coupler = 0.0;
    for (const auto& car : train_->cars()) worst_coupler = (std::max)(worst_coupler, std::abs(static_cast<double>(car.coupler_force)));
    ImGui::Text("Speed %.1f km/h, acceleration %.2f m/s2", train_->speed() * 3.6, head.acceleration);
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
    const auto& last = train_->cars().back();
    ImGui::Text("Last car: pipe %.2f cylinder %.2f; worst coupler %.0f kN", last.pipe, last.cylinder, worst_coupler / 1000.0);
    ImGui::Text("Joints %zu, junctions %zu%s%s%s", joints_, junctions_, end_of_track_ ? ", END OF TRACK" : "",
                coupler_breaks_ ? ", coupler over break force" : "", derailments_ ? ", derailment force exceeded" : "");
    if (ImGui::SliderInt("Coupler sub-steps", &substeps_, 1, 4)) {
        train_->set_coupler_substeps(static_cast<std::uint32_t>(substeps_));
    }
    ImGui::SameLine();
    ImGui::TextDisabled(substeps_ == 1 ? "(RTS)" : "(experimental)");
    ImGui::Checkbox("Camera follows (V)", &follow_);
    ImGui::TextUnformatted("Home/End: throttle  PgUp/PgDn: reverser  ] [: brake valve V..I (hold [ in I)  , .: independent");
#endif
}

} // namespace openrts::viewer
#endif
