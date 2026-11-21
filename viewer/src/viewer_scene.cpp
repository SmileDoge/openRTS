#include "viewer_scene.hpp"

#include <openrts/core/profile.hpp>
#include "rts_config.hpp"

#include "ui_text.hpp"
#include "key_layout.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <thread>
#include <iostream>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <optional>

#include <glm/common.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <openrts/formats/environment.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/terrain.hpp>
#include <openrts/audio/world_sounds.hpp>
#include <deque>
#include <unordered_set>
#include <openrts/renderer/bc7.hpp>
#include <openrts/renderer/depth.hpp>
#include <openrts/renderer/forest_clearance.hpp>
#include <openrts/renderer/occlusion_probe.hpp>
#include <openrts/renderer/frustum.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/route_space.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/renderer/native_sky.hpp>
#include <openrts/renderer/lightning.hpp>
#include <openrts/renderer/sky_renderer.hpp>
#include <openrts/renderer/terrain_renderer.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/renderer/glow_sprites.hpp>
#include <openrts/renderer/task_pool.hpp>
#include <openrts/renderer/screen_quads.hpp>
#include <openrts/renderer/wide_lines.hpp>
#include <openrts/net/world.hpp>
#include <openrts/simulation/track_network.hpp>

#include "track_view.hpp"
#include "exit_codes.hpp"
#include "players_panel.hpp"
#include "crossing_view.hpp"
#include "particle_effects.hpp"
#include "road_traffic.hpp"
#include "signal_view.hpp"
#include "viewer_state.hpp"
#include "track_control.hpp"
#include "inspector.hpp"
#include "tape_view.hpp"
#include "net_session.hpp"
#include "console_panel.hpp"
#include "activity_view.hpp"
#include "train_view.hpp"
#include "thunder_view.hpp"
#include "weather_cycle.hpp"

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

using renderer::ModelRenderer;
using renderer::RenderModel;

void report_snapshot(const char* what, const net::WorldApplied& applied) {
    std::cout << "snapshot: " << applied.applied << ' ' << what;
    if (!applied.problems.empty()) {
        std::cout << ", " << applied.problems.size() << " do not fit (" << applied.problems.front() << ")";
    }
    std::cout << std::endl;
}

// An RTS startup snapshot (--mp-snapshot, docs/net.md): the junctions as it
// has them and its trains, put on the track at the next update. The
// client's own train (assigned "mine") is driven; the rest stand. The
// heads' aspects follow once the signals have loaded (the scene's update).
void apply_snapshot(const net::Snapshot& snapshot, simulation::TrackNetwork& network, TrainView& trains) {
    report_snapshot("junctions set", net::apply_junctions(network, snapshot));
    std::vector<TrainView::PlacedTrain> placed;
    bool driven = false;
    for (const auto& train : snapshot.trains) {
        TrainView::PlacedTrain p;
        p.name = net::train_name(train);
        std::string why;
        p.front = net::train_front(network, train, &why);
        if (!p.front) {
            std::cout << "snapshot: train " << train.id << ": " << why << '\n';
            continue;
        }
        if (train.mine() && !driven) {
            p.role = TrainView::Role::driven;
            driven = true;
        }
        for (const auto& car : train.cars) {
            const auto file = net::car_file(car);
            formats::ConsistEntry entry;
            entry.engine = file.engine;
            entry.uid = car.uid;
            entry.name = file.name;
            entry.folder = file.folder;
            entry.flipped = car.flipped();
            entry.cold = (car.flags & 2u) != 0;
            if (car.handbrake) entry.handbrake = *car.handbrake * 100.0f;
            p.cars.push_back(std::move(entry));
        }
        placed.push_back(std::move(p));
    }
    std::cout << "snapshot: " << placed.size() << " of " << snapshot.trains.size() << " trains to put on the track" << std::endl;
    if (!placed.empty()) trains.request_trains(std::move(placed), "snapshot");
}

[[nodiscard]] core::VirtualPath child(const core::VirtualPath& directory, const std::string_view name) {
    return directory.resolve(name).value_or(directory);
}

// The world a scenario's events see (ActivityView): the trains by their
// scenario ids (TrainView), the signals by their world objects.
class SceneActivityWorld final : public ActivityWorld {
public:
    SceneActivityWorld(TrainView& trains, simulation::SignalSystem* signals) : trains_(trains), signals_(signals) {}

    [[nodiscard]] bool in_cab(const ActivityTrainId train) const override { return trains_.activity_in_cab({train.service, train.train}); }
    [[nodiscard]] std::optional<std::size_t> reverse_points_reached(const ActivityTrainId train) const override {
        return trains_.activity_reverse_points({train.service, train.train});
    }
    [[nodiscard]] std::size_t car_count(const ActivityTrainId train) const override {
        return trains_.activity_car_count({train.service, train.train});
    }
    [[nodiscard]] std::vector<ActivityUncoupling> take_uncouplings() override {
        std::vector<ActivityUncoupling> out;
        for (const auto& cut : trains_.take_activity_cuts()) {
            ActivityUncoupling u{.train = {cut.train.first, cut.train.second}, .head = cut.head, .tail = cut.tail, .cut = {}};
            for (const auto& [origin, uid] : cut.cut)
                u.cut.push_back({.train = {origin.first, origin.second}, .number = static_cast<std::int32_t>(uid), .by_id = true});
            out.push_back(std::move(u));
        }
        return out;
    }
    void set_default_uncouple(const ActivityTrainId train, const ActivityUncoupleRule& rule) override {
        TrainView::UncoupleRule own;
        if (rule.new_id) own.new_id = TrainView::ActivityId{rule.new_id->service, rule.new_id->train};
        std::string type = rule.new_type;
        for (auto& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        own.standing = type == "static";
        if (rule.main_wagon)
            own.main = std::pair{TrainView::ActivityId{rule.main_wagon->train.service, rule.main_wagon->train.train},
                                 static_cast<std::uint32_t>(rule.main_wagon->number)};
        trains_.activity_default_uncouple({train.service, train.train}, std::move(own));
    }
    void set_service_startup(const std::uint32_t service, const bool starts) override { trains_.activity_service_startup(service, starts); }
    void spawn_train(const ActivityTrainId train) override { trains_.activity_spawn_train(train.service, train.train); }
    void set_speed_limit(const ActivityTrainId train, const float limit) override {
        trains_.activity_speed_limit({train.service, train.train}, limit);
    }
    [[nodiscard]] bool near(const ActivityTrainId train, const formats::ActivityPoint& at, const double radius,
                            const std::string_view reach) const override {
        return trains_.activity_near({train.service, train.train}, route(at), radius, reach);
    }
    [[nodiscard]] std::optional<double> car_value(const ActivityCarRef& car, const std::string_view area, const std::string_view name,
                                                  const int idx, const int sub) const override {
        return trains_.activity_car_value({car.train.service, car.train.train}, car.number, car.by_id, area, name, idx, sub);
    }
    void sound_trigger(const std::int32_t object, const std::uint32_t code, const ActivityCarRef& car) override {
        trains_.activity_sound_trigger(object, code, {car.train.service, car.train.train}, car.number);
    }
    void modify_wagons(const ActivityTrainId train, const formats::ActivityWagonGroup& group) override {
        trains_.activity_modify_wagons({train.service, train.train}, group);
    }
    [[nodiscard]] std::vector<std::pair<ActivityTrainId, ActivityTrainId>> take_couplings() override {
        std::vector<std::pair<ActivityTrainId, ActivityTrainId>> out;
        for (const auto& [a, b] : trains_.take_activity_couplings()) out.emplace_back(ActivityTrainId{a.first, a.second}, ActivityTrainId{b.first, b.second});
        return out;
    }
    void reassign(const ActivityTrainId from, const ActivityTrainId to) override {
        trains_.retag_activity({from.service, from.train}, {to.service, to.train});
    }
    [[nodiscard]] std::vector<ActivityTrainId> take_started() override {
        std::vector<ActivityTrainId> out;
        for (const auto& [service, train] : trains_.take_activity_started()) out.push_back({service, train});
        return out;
    }
    void set_block_point(const std::uint32_t service, const std::int32_t point, const bool enabled, const double distance) override {
        trains_.activity_block_point(service, point, enabled, distance);
    }
    void modify_velocity(const ActivityTrainId train, const float speed, const float acceleration, const float deceleration) override {
        trains_.activity_modify_velocity({train.service, train.train}, speed, acceleration, deceleration);
    }
    void wait_here(const ActivityTrainId train, const double time) override {
        trains_.activity_wait_here({train.service, train.train}, time);
    }
    bool override_signal(const formats::ActivityPoint& at, const std::optional<simulation::SignalOverride>& value) override {
        if (!signals_) return false;
        // The signal object standing there (within 10 m): all its heads.
        const auto& objects = signals_->objects();
        std::size_t best = objects.size();
        double nearest = 10.0;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            const auto& p = objects[i].placement.position;
            const double dx = static_cast<double>(p.tile_x - at.tile_x) * 2048.0 + p.x - at.x;
            const double dz = static_cast<double>(p.tile_z - at.tile_z) * 2048.0 + p.z - at.z;
            const double d = std::hypot(dx, dz);
            if (d < nearest) {
                nearest = d;
                best = i;
            }
        }
        if (best >= objects.size()) return false;
        for (const auto head : objects[best].heads) {
            if (value) {
                signals_->set_override(head, *value);
            } else {
                signals_->clear_override(head);
            }
        }
        return true;
    }

private:
    [[nodiscard]] static formats::RoutePosition route(const formats::ActivityPoint& p) {
        return {.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
    }
    TrainView& trains_;
    simulation::SignalSystem* signals_;
};

// Content state shared by both scenes: VFS change polling for hot reload and
// the overlay block describing sources, generations and reloads.
class ContentView {
public:
    explicit ContentView(SceneContent content) : content_(std::move(content)) {}

    [[nodiscard]] const std::shared_ptr<core::Vfs>& vfs() const noexcept { return content_.vfs; }
    [[nodiscard]] const std::string& consist() const noexcept { return content_.consist; }
    [[nodiscard]] int start_state() const noexcept { return content_.start_state; }
    [[nodiscard]] const std::filesystem::path& situation() const noexcept { return content_.situation; }
    [[nodiscard]] int start_view() const noexcept { return content_.start_view; }
    [[nodiscard]] bool realistic_air() const noexcept { return content_.realistic_air; }
    [[nodiscard]] const std::string& save_situation() const noexcept { return content_.save_situation; }
    [[nodiscard]] const std::shared_ptr<const net::Snapshot>& snapshot() const noexcept { return content_.snapshot; }
    [[nodiscard]] const std::shared_ptr<net::RtsConnection>& connection() const noexcept { return content_.connection; }
    [[nodiscard]] const std::string& server_address() const noexcept { return content_.server_address; }
    [[nodiscard]] std::vector<net::ReceivedPackage> take_early() { return std::exchange(content_.early, {}); }
    [[nodiscard]] const std::string& path() const noexcept { return content_.path; }
    [[nodiscard]] const std::shared_ptr<const formats::Activity>& activity() const noexcept { return content_.activity; }
    [[nodiscard]] const std::string& activity_name() const noexcept { return content_.activity_name; }
    [[nodiscard]] const std::filesystem::path& load_game() const noexcept { return content_.load_game; }
    [[nodiscard]] const core::VirtualPath& activity_folder() const noexcept { return content_.activity_folder; }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return content_.root; }
    [[nodiscard]] bool track_control() const noexcept { return content_.track_control; }
    [[nodiscard]] bool console() const noexcept { return content_.console; }
    // RTS's config.ini text (read once from the installation folder).
    [[nodiscard]] const std::string& rts_config() const {
        if (!rts_config_read_) {
            rts_config_ = read_rts_config(content_.root);
            rts_config_read_ = true;
        }
        return rts_config_;
    }

    // Hands this frame's VFS changes to the model library.
    void forward_changes(renderer::ModelLibrary& library) {
        if (!content_.watching) return;
        auto changes = content_.vfs->poll_changes();
        if (changes.empty()) return;
        last_changes_ = changes.size();
        library.notify_changes(changes);
    }

    void draw_overlay([[maybe_unused]] const renderer::ModelLibrary* library) const {
#ifdef OPENRTS_HAS_IMGUI
        if (!ImGui::CollapsingHeader(tr("Content"), ImGuiTreeNodeFlags_DefaultOpen)) return;
        const auto stats = content_.vfs->stats();
        ImGui::Text(tr("VFS: %zu sources, %zu files, generation %llu"), stats.sources, stats.files,
                    static_cast<unsigned long long>(stats.generation));
        ImGui::Text(tr("Overrides: %zu  |  rescans: %zu (lost events: %zu)"), stats.conflicts, stats.rescans,
                    stats.lost_event_rescans);
        ImGui::Text(tr("Hot reload: %s  |  last change batch: %zu file(s)"), content_.watching ? "on" : "off", last_changes_);
        if (library) {
            const auto models = library->stats();
            ImGui::Text(tr("Reloaded: %zu models, %zu textures in %zu batches%s"), models.reloaded_models,
                        models.reloaded_textures, models.reload_batches, models.reloading ? " (loading...)" : "");
            for (const auto& error : models.errors) ImGui::TextColored({1.0f, 0.5f, 0.3f, 1.0f}, "%s", error.c_str());
        }
        for (const auto& source : content_.vfs->sources()) {
            if (source.error.empty()) continue;
            ImGui::TextColored({1.0f, 0.5f, 0.3f, 1.0f}, "%s: %s", source.name.c_str(), source.error.c_str());
        }
#endif
    }

private:
    mutable std::string rts_config_;
    mutable bool rts_config_read_ = false;
    SceneContent content_;
    std::size_t last_changes_ = 0;
};

// Shows a single shape file, orbiting around its bounding box.
class ShapeScene final : public ViewerScene {
public:
    ShapeScene(SceneContent content, renderer::StreamedModelAsset asset)
        : content_(std::move(content)), asset_(std::move(asset)), path_(asset_->source_path) {}

    [[nodiscard]] std::string_view title() const override { return "Shape model"; }

    [[nodiscard]] std::string attach(const SceneGpu& gpu, ViewerCamera& camera) override {
        frame_vertices(camera, asset_->model);
        library_ = std::make_unique<renderer::ModelLibrary>(gpu.models, content_.vfs());
        library_->add(std::move(*asset_));
        asset_.reset();
        library_->update();
        if (!library_->find(path_)) return "GPU shape model creation failed.";
        return {};
    }

    void detach() override { library_.reset(); }

    void update(ModelRenderer&, ViewerCamera&, const SceneClock& clock, const SceneInput& input) override {
        content_.forward_changes(*library_);
        library_->update();
        // Up/Down step through the shape's animation (switch blades, doors).
        if (auto* model = library_->find(path_); model && model->animation().frame_count > 0) {
            const auto animation = model->animation();
            const float rate = static_cast<float>((std::max)(animation.frame_rate, 1u)) * 0.1f;
            if (input.step_up) frame_ += clock.elapsed * rate;
            if (input.step_down) frame_ -= clock.elapsed * rate;
            frame_ = std::clamp(frame_, 0.0f, static_cast<float>(animation.frame_count));
        }
    }

    void submit(ModelRenderer& renderer, const FrameView&) override {
        if (auto* model = library_->find(path_)) renderer.submit(*model, renderer::ModelTransform{1.0f}, frame_);
    }

    void draw_overlay([[maybe_unused]] const ViewerCamera& camera) const override {
#ifdef OPENRTS_HAS_IMGUI
        const auto eye = camera.eye();
        ImGui::Text("%s", path_.str().c_str());
        ImGui::Text(tr("XYZ: %.1f, %.1f, %.1f"), eye.x, eye.y, eye.z);
        if (auto* model = const_cast<ShapeScene*>(this)->library_->find(path_); model && model->animation().frame_count > 0) {
            ImGui::Text(tr("Animation frame %.2f of %u (Up/Down)"), frame_, model->animation().frame_count);
        }
        content_.draw_overlay(library_.get());
#endif
    }

private:
    static void frame_vertices(ViewerCamera& camera, const formats::ShapeModel& model) {
        const auto position = [](const formats::ShapeMeshVertex& vertex) {
            return glm::vec3{vertex.position[0], vertex.position[1], vertex.position[2]};
        };
        auto low = position(model.vertices.front());
        auto high = low;
        for (const auto& vertex : model.vertices) {
            low = (glm::min)(low, position(vertex));
            high = (glm::max)(high, position(vertex));
        }

        const auto size = high - low;
        camera.target = (low + high) * 0.5f;
        camera.distance = (std::max)((std::max)({size.x, size.y, size.z}) * 1.6f, 1.0f);
    }

    ContentView content_;
    std::optional<renderer::StreamedModelAsset> asset_;
    core::VirtualPath path_;
    std::unique_ptr<renderer::ModelLibrary> library_;
    float frame_ = 0.0f;
};

// Streams the static objects of a route around the camera and draws those
// within draw distance and the view frustum.
class RouteScene final : public ViewerScene {
public:
    RouteScene(SceneContent content,
               core::VirtualPath route,
               const formats::RoutePosition origin,
               std::vector<formats::RouteEnvironment> environments,
               std::unique_ptr<simulation::TrackNetwork> network)
        : content_(std::move(content)),
          route_(std::move(route)),
          origin_(origin),
          environments_(std::move(environments)) {
        if (network) track_.emplace(std::move(network), origin_);
    }

    [[nodiscard]] std::string_view title() const override { return "Route props"; }

    [[nodiscard]] std::string attach(const SceneGpu& gpu, ViewerCamera& camera) override {
        // The tracks' and roads' axes the forests keep their trees from (an
        // openRTS option), before any forest is built.
        {
            const auto started = std::chrono::steady_clock::now();
            auto axes = route_way_axes(*content_.vfs(), route_, track_ ? &track_->network() : nullptr);
            std::cout << "forest clearance: " << axes->size() << " pieces of track and road, "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << " ms"
                      << std::endl;
            renderer::set_forest_clearance_axes(std::move(axes));
            forest_distance_ = renderer::forest_clearance_distance();
        }
        streamer_ = std::make_unique<renderer::RouteStreamer>(content_.vfs(), route_, origin_, tile_radius);
        streamer_->update(origin_);
        library_ = std::make_unique<renderer::ModelLibrary>(gpu.models, content_.vfs(), streamer_->texture_directories());
        model_cache_.clear();
        model_cache_generation_ = ~std::uint64_t{0};

        sky_renderer_ = std::make_unique<renderer::SkyRenderer>(gpu.device, gpu.target_format);
        if (!sky_renderer_->ready()) sky_renderer_.reset();
        native_sky_ = std::make_unique<renderer::NativeSky>(gpu.device, gpu.target_format);
        if (!native_sky_->ready()) native_sky_.reset();
        lightning_ = std::make_unique<renderer::LightningRenderer>(gpu.device, gpu.target_format);
        if (!lightning_->ready()) lightning_.reset();
        thunder_ = std::make_unique<ThunderView>(content_.vfs());
        test_strikes_.clear();
        terrain_ = std::make_unique<renderer::TerrainRenderer>(gpu.device, gpu.target_format, content_.vfs(), route_,
                                                               origin_, terrain_radius);
        if (!terrain_->ready()) terrain_.reset();
        wide_lines_ = std::make_unique<renderer::WideLineRenderer>(gpu.device, gpu.target_format);
        if (!wide_lines_->ready()) wide_lines_.reset();
        screens_ = std::make_unique<renderer::ScreenQuadRenderer>(gpu.device, gpu.target_format);
        if (!screens_->ready()) screens_.reset();
        train_glows_ = std::make_unique<renderer::GlowSpriteRenderer>(gpu.device, gpu.target_format);
        if (!train_glows_->ready()) train_glows_.reset();
        train_glow_textures_.clear();
        screen_textures_.clear();
        probe_ = std::make_unique<renderer::OcclusionProbe>(gpu.device, gpu.target_format);
        if (!probe_->ready()) probe_.reset();
        if (track_) {
            track_->attach(gpu.device, gpu.target_format);
            signals_ = std::make_unique<SignalView>(content_.vfs(), route_, origin_, track_->network());
            signals_->attach(gpu.device, gpu.target_format);
            crossings_ = std::make_unique<CrossingView>(content_.vfs(), route_, &track_->network());
        }
        roads_ = std::make_unique<RoadTraffic>(content_.vfs(), route_, origin_);
        if (!roads_->ready()) roads_.reset();
        precipitation_ = std::make_unique<PrecipitationView>(gpu.device, gpu.target_format, content_.vfs());
        if (!precipitation_->ready()) precipitation_.reset();
        smoke_ = std::make_unique<SmokeView>(gpu.device, gpu.target_format, content_.vfs());
        if (!smoke_->ready()) smoke_.reset();
        audio_ = gpu.audio;
        if (audio_) world_sounds_ = std::make_unique<audio::WorldSounds>(content_.vfs(), route_, origin_);

        // Start at the route start, a little above the ground, looking along it.
        camera.distance = 1000.0f;
        camera.position = glm::vec3{0.0f, start_ground_height() + 30.0f, 0.0f};
        camera.pitch = 0.08f;
        camera.fly = true;
        if (track_) {
            train_ = std::make_unique<TrainView>(content_.vfs(), route_, origin_, track_->network());
            train_->set_rts_config(content_.rts_config());
            train_->set_install_root(content_.root());
            // A shape the route places too stays while the route keeps it.
            train_->set_shape_keeper([this](const std::string& key) { return streamer_ && streamer_->holds_model(key); });
            if (content_.realistic_air()) train_->set_realistic_air(true);
            // Not in a network game: there the server and its dispatchers
            // own the trains and the track (docs/net.md); but the
            // dispatcher's console is the map.
            if (!content_.connection() || content_.console()) {
                track_control_ = std::make_unique<TrackControl>(track_->network(), origin_);
                if (content_.track_control() || content_.console()) track_control_->open();
            }
            if (signals_ && track_control_) {
                track_control_->set_signal_names([signals = signals_.get()](const std::uint32_t object) {
                    return signals->object_name(object);
                });
            }
            // Signals beyond a routed train's reserved way stay at danger.
            if (signals_) {
                signals_->set_authority([dispatcher = &train_->dispatcher()](const simulation::Train& train) {
                    return dispatcher->authority_ahead(train.id());
                });
                // The keyboard-driven train is the player's (RTS type 1).
                signals_->set_player([trains = train_.get()](const simulation::Train& train) { return trains->is_player(train.id()); });
            }
            train_->set_start_state(content_.start_state());
            train_->set_start_view(static_cast<TrainView::StartView>(std::clamp(content_.start_view(), 0, 3)));
            if (!content_.situation().empty()) {
                // A bare name: the route's situation folder.
                auto file = content_.situation();
                if (!file.has_parent_path()) file = train_->situation_folder() / file.replace_extension(".situation");
                train_->request_situation(file);
            }
            if (!content_.save_situation().empty()) train_->save_situation_when_placed(content_.save_situation());
            if (const auto& snapshot = content_.snapshot()) {
                if (content_.connection()) {
                    // Watching a live game: the session puts the trains on
                    // the track and keeps the world as the server says.
                    net_ = std::make_unique<NetSession>(content_.connection(), *snapshot, content_.take_early(), content_.console());
                    train_->set_network(true);
                    train_->set_console(content_.console());
                    track_->network().set_switches_stay(true);
                    if (track_control_) {
                        // The console's map: junctions and signals are the server's.
                        track_control_->set_network_actions({
                            .throw_junction = [net = net_.get()](const std::uint32_t node, const std::uint32_t branch) {
                                net->send_command("junction " + std::to_string(node) + " " + std::to_string(branch));
                            },
                            .set_signal = [net = net_.get()](const std::uint32_t item, const std::optional<formats::SignalAspect> aspect) {
                                net->send_command("signal " + std::to_string(item) + " " +
                                                  (aspect ? std::to_string(static_cast<int>(*aspect)) : std::string("auto")));
                            },
                            .place_consist = [net = net_.get(), vfs = content_.vfs()](const std::string& consist,
                                                                                    const simulation::TrackLocation& front,
                                                                                    const TrackControl::NetworkActions::PlaceOptions& options) {
                                // By its cars, not its name: the consist is the
                                // dispatcher's, the server needs only the cars' files.
                                std::string error;
                                if (auto place = consist_place(*vfs, consist, error)) {
                                    place->node = front.node;
                                    place->offset = front.offset;
                                    place->forward = front.forward;
                                    place->number = options.number;
                                    place->name = net::from_utf8(options.name);
                                    place->kind = options.kind;
                                    place->state = options.state;
                                    put_chocks(*place, options.chocks);
                                    net->send_place(*place);
                                } else {
                                    net->note(consist + ": " + error);
                                }
                            },
                            .remove_train = [net = net_.get()](const std::uint64_t train) {
                                if (const auto id = net->server_train(train)) net->send_command("remove " + std::to_string(id));
                            },
                            // RTS's hand route: the way from the train's head
                            // (or rear) to the place, its junctions thrown on the
                            // server one by one, then the length (0x4619).
                            .route_to = [net = net_.get(), trains = train_.get(), network = &track_->network()](
                                            const std::uint64_t train, const std::vector<simulation::TrackLocation>& via,
                                            const simulation::TrackLocation& to) -> std::string {
                                const auto id = net->server_train(train);
                                const auto* t = trains->train_by_id(train);
                                if (!id || !t || !t->placed()) return tr("the train is not on the track");
                                // Another train on the way: the first one met
                                // along the leg (its cars looked at every 5 m).
                                const auto blocker = [&](const simulation::RouteLeg& leg) -> std::optional<std::string> {
                                    for (const auto& s : trains->summaries()) {
                                        if (s.id == train) continue;
                                        const auto* other = trains->train_by_id(s.id);
                                        const auto front = other && other->placed() ? other->front_end() : std::nullopt;
                                        if (!front) continue;
                                        auto walk = *front;
                                        walk.reverse();
                                        for (double done = 0.0; done <= other->length() + 0.1; done += 5.0) {
                                            if (const auto at = leg.locate(walk.node(), walk.offset()); at && *at > 1.0) return s.name;
                                            if (!walk.move((std::min)(5.0, other->length() - done + 0.1)).ok()) break;
                                        }
                                    }
                                    return std::nullopt;
                                };
                                std::optional<simulation::TrainRoute> best;
                                bool rear_first = false;
                                std::string blocked;
                                for (const bool rear : {false, true}) {
                                    const auto end = rear ? t->rear_end() : t->front_end();
                                    if (!end) continue;
                                    simulation::PathPlan plan;
                                    plan.name = "route";
                                    plan.start = {end->node(), end->offset(), rear ? !end->forward() : end->forward()};
                                    // The points to pass, in order (as the single
                                    // player's path editor's), then the end.
                                    for (const auto& p : via) plan.points.push_back({.kind = simulation::PlanPointKind::via, .node = p.node, .offset = p.offset});
                                    plan.points.push_back({.kind = simulation::PlanPointKind::end, .node = to.node, .offset = to.offset});
                                    auto route = simulation::build_route(*network, plan);
                                    if (!route.ok() || route.legs.size() != 1) continue;
                                    // Not through a train standing there (testers
                                    // 2026-11-21: the shortest way ran through them).
                                    if (const auto who = blocker(route.legs[0])) {
                                        if (blocked.empty()) blocked = *who;
                                        continue;
                                    }
                                    if (!best || route.legs[0].length() < best->legs[0].length()) {
                                        best = std::move(route);
                                        rear_first = rear;
                                    }
                                }
                                if (!best) {
                                    const auto text = blocked.empty() ? trf("train %u: no way to there", id)
                                                                      : trf("train %u: the way runs into %s; add a point to pass round it", id, blocked.c_str());
                                    net->note(text);
                                    return text;
                                }
                                // The train's old route first lets its switches go.
                                if (net->route_left(id)) net->send_route(id, -1.0f, false);
                                for (const auto& j : best->legs[0].junctions) {
                                    const auto* state = network->junction(j.junction);
                                    if (state && state->selected != j.branch) net->request_junction(j.junction, j.branch);
                                }
                                net->send_route(id, static_cast<float>(best->legs[0].length()), rear_first);
                                return {};
                            },
                            .cancel_route = [net = net_.get()](const std::uint64_t train) {
                                if (const auto id = net->server_train(train)) net->send_route(id, -1.0f, false);
                            },
                            .route_length = [net = net_.get()](const std::uint64_t train, const float length, const bool rear_first) {
                                if (const auto id = net->server_train(train)) net->send_route(id, length, rear_first);
                            },
                            .route_left = [net = net_.get()](const std::uint64_t train) -> std::optional<std::pair<float, bool>> {
                                const auto id = net->server_train(train);
                                return id ? net->route_left(id) : std::nullopt;
                            },
                            .train_names = [net = net_.get()](const std::uint64_t train) -> std::pair<std::string, std::string> {
                                const auto id = net->server_train(train);
                                if (!id) return {};
                                const auto* names = net->train_names(id);
                                const auto number = net->train_number(id);
                                std::string title = number ? "№" + std::to_string(number) : std::string();
                                if (names && !names->name.empty()) title += (title.empty() ? "" : " ") + net::to_utf8(names->name);
                                return {title, names ? net::to_utf8(names->notes) : std::string()};
                            },
                            .train_form = [net = net_.get()](const std::uint64_t train) -> std::pair<std::uint32_t, std::string> {
                                const auto id = net->server_train(train);
                                if (!id) return {};
                                const auto* names = net->train_names(id);
                                return {net->train_number(id), names ? net::to_utf8(names->name) : std::string()};
                            },
                            .set_train_form = [net = net_.get()](const std::uint64_t train, const std::uint32_t number, const std::string& name) {
                                const auto id = net->server_train(train);
                                if (!id) return;
                                if (number && number != net->train_number(id)) net->send_command("number " + std::to_string(id) + " " + std::to_string(number));
                                const auto* names = net->train_names(id);
                                const auto old_name = names ? names->name : std::u16string();
                                if (net::from_utf8(name) != old_name)
                                    net->send_train_names(id, net::from_utf8(name), names ? names->notes : std::u16string());
                            },
                            .move_train = [net = net_.get()](const std::uint64_t train, const simulation::TrackLocation& head) {
                                const auto id = net->server_train(train);
                                if (!id) return;
                                char line[96];
                                std::snprintf(line, sizeof line, "move %u %u %.3f %d", id, head.node, head.offset, head.forward ? 1 : 0);
                                net->send_command(line);
                            },
                            .set_chocks = [net = net_.get()](const std::uint64_t train, const std::uint8_t chocks) {
                                static constexpr const char* words[] = {"none", "head", "tail", "both"};
                                if (const auto id = net->server_train(train)) net->send_command("chocks " + std::to_string(id) + " " + words[chocks & 3u]);
                            },
                        });
                    }
                } else {
                    apply_snapshot(*snapshot, track_->network(), *train_);
                    snapshot_signals_ = true;
                }
            }
            if (const auto& activity = content_.activity()) {
                // A scenario: its trains and the player's start (TrainView),
                // its messages (ActivityView) once they stand.
                activity_ = std::make_unique<ActivityView>(activity);
                train_->request_activity(activity, content_.activity_folder());
            } else if (!content_.consist().empty()) {
                if (const auto error = train_->load(content_.consist()); !error.empty()) {
                    std::cout << "train: " << error << '\n';
                } else if (const auto* path = train_->find_path(content_.path())) {
                    train_->select_path(*path);
                } else {
                    std::cout << "train: no path '" << content_.path() << "' on the route\n";
                }
            }
        }
        std::cout << "route streaming: 3x3 tiles, async world/shape/ACE jobs\n";
        std::cout << "flycam: RMB look, WASD move, Q/E vertical, Shift boost, F toggle" << std::endl;
        // The loading screen till the world around the start is in; time
        // stands for a start in the cab (its DLLs start when it is shown).
        loading_ = {};
        loading_.done = false;
        loading_.hold_time = track_ && ((!content_.consist().empty() && content_.start_view() == 1)
                                        || (content_.activity() && content_.activity()->player_train));
        loading_.progress = 0.0f;
        loading_.stage = "Loading the route";
        loading_.title = std::string(route_.filename());
        if (!content_.consist().empty()) loading_.title += "  \xE2\x80\x94  " + content_.consist();
        if (content_.activity()) loading_.title += "  \xE2\x80\x94  " + content_.activity()->name;
        loading_since_ = std::chrono::steady_clock::now();
        loading_quiet_ = 0;
        loading_peak_ = 0;
        loading_report_ = 0.0;
        return {};
    }

    void detach() override {
        // Off the server first, while the GPU is still up: the reader thread
        // waits in recv, and a graphics layer that hooks it (ReShade) is
        // unloaded with the device; returning into it crashed the viewer on
        // closing (seen 2026-11-06).
        if (content_.connection()) content_.connection()->close();
        if (world_sounds_ && audio_) world_sounds_->stop(*audio_);
        if (train_ && audio_) train_->stop_sound(*audio_);
        if (crossings_ && audio_) crossings_->stop_sound(*audio_);
        if (weather_sound_ && audio_) weather_sound_->stop(*audio_);
        weather_sound_.reset();
        if (thunder_ && audio_) thunder_->stop(*audio_);
        thunder_.reset();
        track_control_.reset();
        if (signals_) {
            signals_->set_authority({});
            signals_->set_player({});
        }
        train_.reset();
        world_sounds_.reset();
        audio_ = nullptr;
        signals_.reset();
        if (track_) track_->detach();
        terrain_.reset();
        sky_renderer_.reset();
        native_sky_.reset();
        lightning_.reset();
        lightning_channels_.clear();
        wide_lines_.reset();
        screens_.reset();
        screen_textures_.clear();
        train_glows_.reset();
        train_glow_textures_.clear();
        probe_.reset();
        model_cache_.clear();
        model_cache_generation_ = ~std::uint64_t{0};
        library_.reset();
        streamer_.reset();
    }

    void update(ModelRenderer&, ViewerCamera& camera, const SceneClock& given, const SceneInput& input) override {
        // The dynamic weather (weather_cycle.hpp) decides the weather this
        // frame: RTS's weather and the storm from it.
        SceneClock clock = given;
        apply_dynamic_weather(clock);
        last_clock_ = clock;
        OPENRTS_ZONE("scene: update");
        if (input.toggle_labels) labels_ = !labels_;
        rebase_near(camera);
        // Track control's "Teleport here": the camera at eye height over the
        // point (the track's height, else the ground), free flight.
        if (track_control_) {
            if (const auto teleport = track_control_->take_teleport()) {
                constexpr double tile = formats::route_tile_size;
                constexpr float eye_height = 1.7f;
                const auto x = static_cast<float>((teleport->at.tile_x - origin_.tile_x) * tile + teleport->at.x - origin_.x);
                const auto z = static_cast<float>((teleport->at.tile_z - origin_.tile_z) * tile + teleport->at.z - origin_.z);
                float y = camera.eye().y;
                if (teleport->on_track) {
                    y = static_cast<float>(teleport->at.y - origin_.y) + eye_height;
                } else if (train_) {
                    if (const auto ground = train_->ground_at({x, 0.0f, z})) y = *ground + eye_height;
                }
                if (train_) train_->stop_following();
                camera.fly = true;
                camera.position = {x, y, z};
                camera.yaw = teleport->yaw;
                camera.pitch = 0.0f;
                camera.target = camera.position + camera.forward() * camera.distance;
            }
        }
        if (track_) {
            track_->update(input, clock.elapsed);
        }
        if (input.toggle_track_control && track_control_) track_control_->toggle();
        if (input.signal_names) signal_names_ = !signal_names_;
        if (input.place_names) place_names_ = !place_names_;
        cab_labels_ = input.cab_labels;
        if (input.inspector) inspector_.toggle();
        if (input.tape_window) tape_view_.toggle();
        // The ray under the mouse, from the last frame's camera.
        std::optional<glm::vec3> direction;
        if (input.pointer) {
            const auto inverse = glm::inverse(last_view_projection_);
            const glm::vec4 far = inverse * glm::vec4{input.pointer_x, input.pointer_y, 0.5f, 1.0f};
            if (std::abs(far.w) > 1e-9f) {
                const glm::vec3 to = glm::vec3(far) / far.w - last_eye_;
                if (glm::length(to) > 1e-6f) direction = glm::normalize(to);
            }
        }
        // The inspector choosing with a click: the click is its, not a control's.
        bool click = input.click;
        if (click && direction && inspector_.picking()) {
            inspector_.pick(last_eye_, *direction, inspector_world());
            click = false;
        }
        // Ctrl+F (openRTS): from the engineer to the free camera where his
        // eyes are, as the inspector's "fly to" leaves him; again, back by
        // the chair of the driving cab. In a cab with a DLL every letter may
        // be the cab's, F among them, and nothing let the player out but the
        // panel (testers 2026-11-18).
        // Without a driving cab (the dispatcher, or no train): between the
        // free camera and on foot where it is (2026-11-21, the organiser).
        const bool walk_toggle = train_ && train_->take_walk_toggle();
        if ((input.free_camera || walk_toggle) && train_) {
            if (train_->walking()) {
                const auto eye = camera.eye();
                train_->set_walking(false, last_eye_);
                train_->stop_following();
                camera.fly = true;
                camera.position = eye;
                camera.target = eye + camera.forward() * camera.distance;
            } else if (train_->has_driving_cab() && !train_->console()) {
                train_->enter_cab();
            } else {
                train_->set_walking(true, camera.eye());
            }
        }
        if (const auto fly = inspector_.take_fly_to()) {
            if (train_) {
                if (train_->walking()) train_->set_walking(false, last_eye_);
                train_->stop_following();
            }
            const glm::vec3 look = glm::normalize(fly->second - fly->first);
            camera.fly = true;
            camera.position = fly->first;
            camera.yaw = std::atan2(-look.x, -look.z);
            camera.pitch = std::asin(std::clamp(-look.y, -1.0f, 1.0f));
            camera.target = fly->second;
        }
        if (train_) {
            // The cursor in RTS's pixels (a dragged control moves as in RTS).
            const float scale = rts_pixels_per_pixel();
            train_->pointer(last_eye_, direction, click, input.click_shift, input.scroll, input.release,
                            {input.cursor_x * scale, input.cursor_y * scale}, input.held);
            if (input.car_menu) {
                // Aimed with the cursor when it is over the scene, else the screen's middle.
                const glm::vec2 aim = input.pointer ? glm::vec2{input.pointer_x, input.pointer_y} : glm::vec2{0.0f};
                train_->car_menu_key(last_eye_, last_view_projection_, aim);
            }
        }
        if (train_) {
            if (input.car_info) train_->car_info_key(input.car_info_shift);
            if (input.walk_collision) train_->toggle_walk_objects();
            if (input.tape) train_->tape_key();
            if (input.tape_which) train_->tape_which();
            if (input.limits_panel) train_->toggle_limits_panel();
            if (input.stations_panel) train_->toggle_stations_panel();
            if (input.profile_strip) train_->toggle_profile_strip();
            if (input.activity_journal && activity_) activity_->toggle_journal();
            if (input.activity_menu && activity_) activity_->toggle_menu();
            if (input.car_view) train_->car_view_key(camera);
        }
        if (train_) {
            train_->set_signals(signals_ ? signals_->system() : nullptr);
            if (signals_ && !signal_names_set_) {
                signal_names_set_ = true;
                train_->set_signal_names([this](const std::uint32_t object) { return signals_ ? signals_->object_name(object) : std::string{}; });
            }
            train_->set_obstacles(streamer_ ? &streamer_->visible_instances() : nullptr);
            train_->set_world_clock(clock.time_of_day, sky_state_ && sky_state_->night);
            train_->set_world_light(sky_state_ ? sky_state_->world_lit : 1.0f, static_cast<std::uint8_t>(clock.weather),
                                    static_cast<std::uint8_t>(clock.season));
            if (net_) {
                net_->update(track_->network(), signals_ ? signals_->system() : nullptr, *train_,
                             renderer::route_position(camera.eye(), origin_));
            }
            // OPENRTS_SPAWN_CHECK=<consist>: with no train, 3 s after the
            // loading one put on the track as Track control would, and for 6 s
            // what holds the keyboard from the free camera, logged.
            spawn_check(clock, camera);
            // Placements: a click in the free flight picks trains.
            if (train_->selecting() && input.click && input.pointer && !train_->walking())
                train_->select_at(last_eye_, last_view_projection_, {input.pointer_x, input.pointer_y}, input.click_ctrl, input.click_shift);
            train_->update(input, camera, clock.elapsed, *library_, audio_);
            // --load: the saved game once the world stands (a scenario's
            // trains placed, its start not run: the save has its state).
            if (!content_.load_game().empty() && !start_game_loaded_ && loading_.done
                && (!activity_ || train_->activity_started())) {
                start_game_loaded_ = true;
                if (const auto error = load_game(content_.load_game()); !error.empty())
                    std::cout << "saved game: " << content_.load_game().string() << ": " << error << std::endl;
            }
            if (activity_) {
                // The start's events once the trains stand and the loading
                // screen is gone; then the events every frame.
                SceneActivityWorld world(*train_, signals_ ? signals_->system() : nullptr);
                if (!activity_->started() && train_->activity_started() && loading_.done) activity_->start(clock.time_of_day_exact, world);
                activity_->update(clock.elapsed, clock.time_of_day_exact, world);
            }
        }
        if (signals_) {
            const auto trains = train_ ? train_->trains() : std::vector<const simulation::Train*>{};
            OPENRTS_ZONE("signals");
            signals_->update(clock.elapsed, train_ ? train_->train() : nullptr, trains, camera.eye());
            if (snapshot_signals_ && signals_->system() && content_.snapshot()) {
                snapshot_signals_ = false;
                report_snapshot("signal heads set", net::apply_signals(*signals_->system(), *content_.snapshot()));
            }
        }
        const auto camera_position = renderer::route_position(camera.eye(), origin_);
        apply_conditions(clock.season, clock.weather);
        if (terrain_) {
            terrain_->update(camera_position);
            terrain_->update_water(clock.elapsed, camera.eye());
        }
        if (sky_simulation_) {
            sky_state_ = sky_simulation_->update(clock.time_of_day_exact, clock.elapsed, camera_position.tile_z, clock.season);
        }
        // A thunderstorm: the game's (or the server's) with the rain.
        // The hello's thunderstorm only from a server that sends no weather.
        storm_ = clock.weather == renderer::rts::Weather::rain && (clock.storm || (net_ && !net_->server_weather() && net_->server_storm()));
        take_probe_results();
        {
            // How open the sky is round the eye (sky_open_seen_): the flashes
            // light the scene as much, up in half a second, down in two.
            const float target = storm_ ? sky_open_seen_ : 1.0f;
            const float speed = target > sky_open_ ? 3.0f : 0.7f;
            sky_open_ += (target - sky_open_) * (1.0f - std::exp(-speed * clock.elapsed));
            // The sky's light near the eye (PBR stage 2): open in half a
            // second, closed in a second (a tunnel's mouth passed).
            const float light_speed = sky_light_seen_ > sky_light_open_ ? 2.0f : 1.0f;
            sky_light_open_ += (sky_light_seen_ - sky_light_open_) * (1.0f - std::exp(-light_speed * clock.elapsed));
        }
        update_lightning(clock, camera.eye(), camera.forward(), camera_position);
        if (native_sky_ && sky_state_) {
            // openRTS's own sky: RTS's lighting satellite as its sun (well
            // below the horizon while RTS hides it), the eye's place, the
            // weather's cover, the environment's lowest wind three times as
            // strong up at the clouds.
            renderer::NativeSkyFrame sky;
            const auto sun = sky_state_->satellites.empty() ? renderer::rts::SatelliteState{} : sky_state_->satellites.front();
            sky.to_sun = -renderer::rts_sun_direction(sun.azimuth_degrees, sun.visible ? sun.elevation_degrees : -30.0f);
            sky.altitude = static_cast<float>(camera_position.y);
            sky.eye_route = {static_cast<double>(camera_position.tile_x) * formats::route_tile_size + camera_position.x,
                             static_cast<double>(camera_position.tile_z) * formats::route_tile_size + camera_position.z};
            // The cover the player chose, else the dynamic weather's, else the weather's.
            sky.coverage = renderer::cloud_cover() >= 0.0f                    ? renderer::cloud_cover()
                           : dynamic_weather_                               ? weather_cycle().shown.cloud
                           : storm_                                         ? 0.97f
                           : clock.weather == renderer::rts::Weather::rain ? 0.92f
                           : clock.weather == renderer::rts::Weather::snow ? 0.85f
                                                                           : 0.42f;
            glm::vec2 wind{0.0f};
            if (environment_ && !environment_->wind.empty()) {
                const auto& lowest = environment_->wind.front();
                wind = glm::vec2{lowest.direction[0], lowest.direction[2]} * lowest.speed * 3.0f;
            }
            if (glm::length(wind) < 3.0f) wind = glm::vec2{4.0f, 1.5f};
            sky.wind = wind;
            // The clouds drift with the time of day: still while it is
            // stopped, a time-lapse while it runs faster, up to 60 times (at
            // 600 or 3600 they would jump hundreds of metres a frame).
            sky.elapsed = clock.elapsed * std::clamp(clock.time_speed, 0.0f, 60.0f);
            // The dynamic weather's storm darkens the clouds as it gathers.
            sky.storm = dynamic_weather_ ? std::clamp(weather_cycle().shown.storm, 0.0f, 1.0f) : storm_ ? 1.0f : 0.0f;
            sky.flash_id = sky_flash_id_;
            sky.flash_at = sky_flash_at_;
            sky.flash = sky_flash_;
            native_sky_->update(sky);
        }

        streamer_->update(camera_position);
        // Behind the loading screen the models load on most of the cores; in
        // the background too, so its 30 frames/s do not slow the streaming.
        streamer_->set_task_limit(loading_.done && !clock.background ? 4 : (std::max)(4u, std::thread::hardware_concurrency() / 2 + 2));
        streamer_->poll();
        for (auto& asset : streamer_->take_ready_models()) library_->add(std::move(asset));
        let_go_world_models();

        // GPU uploads stay on the render thread; one model per frame keeps the
        // frame time bounded while a tile fills in. Hot-reload batches are
        // published here as a whole.
        // Night, season and weather for the models' alternative textures
        // (TEXTURES\NIGHT, \WINTER, ...): RTS reloads them when night falls.
        const renderer::TextureVariant variant{.night = sky_state_ && sky_state_->night && renderer::night_textures(),
                                               .season = clock.season,
                                               .weather = clock.weather,
                                               .fullbright_lit = experimental_ && renderer::light_fullbright()};
        library_->set_texture_variant(variant);
        streamer_->set_texture_variant(variant);
        // The textures' size limit changed: every loaded model again, its
        // textures uploaded anew at the new size.
        if (const auto limit = renderer::texture_size_limit(); !texture_limit_ || *texture_limit_ != limit) {
            if (texture_limit_) library_->reload_textures();
            texture_limit_ = limit;
        }
        // The forests' clearance changed: the loaded forests are built again.
        if (const float distance = renderer::forest_clearance_distance(); distance != forest_distance_) {
            forest_distance_ = distance;
            library_->reload_extension(".forest");
        }
        content_.forward_changes(*library_);
        // Behind the loading screen uploads for up to 20 ms a frame: nobody
        // watches it.
        // Made on loading threads: more at once when nobody watches.
        library_->set_making_limit(loading_.done && !clock.background ? 2 : 4);
        library_->update();
        if (renderer::models_made_off_thread()) {
            // Taken in as made, however many a frame.
        } else if (!loading_.done) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            while (library_->stats().queued_uploads > 0 && std::chrono::steady_clock::now() < until) library_->update();
        } else if (clock.background) {
            // In the background a frame comes every 33 ms (frame_limiter.hpp),
            // where in front it is one model uploaded and the tasks refilled
            // each of many more frames: here they go on for up to 12 ms of
            // it, so the models and their textures come in no slower.
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(12);
            while (std::chrono::steady_clock::now() < until) {
                streamer_->poll();
                for (auto& asset : streamer_->take_ready_models()) library_->add(std::move(asset));
                if (library_->stats().queued_uploads == 0) break;
                library_->update();
            }
        }
        if (input.flashlight) {
            flashlight_ = !flashlight_;
            if (train_) train_->say(flashlight_ ? tr("flashlight on") : tr("flashlight off"));
        }
        flashlight_forward_ = camera.forward();
        collect_lights(camera.eye(), clock.elapsed);

        // The free camera is an external camera for sound activation; RTS
        // activates the world's sounds by ExternalCam in the cab too
        // (0x44AB7C: not the player's train). Heard from the engineer's room,
        // they pass its doors and windows (docs/sound.md).
        const float outside_gain = train_ ? train_->outside_gain() : 1.0f;
        if (world_sounds_ && audio_) {
            // Night as the night textures have it (the environment's sunset to sunrise).
            const auto night = sky_state_ ? std::optional<bool>(sky_state_->night) : std::nullopt;
            world_sounds_->update(clock.elapsed, camera.eye(), audio::SoundCamera::external, *audio_, outside_gain, night);
        }
        if (crossings_) {
            const auto trains = train_ ? train_->trains() : std::vector<const simulation::Train*>{};
            crossings_->update(clock.elapsed, streamer_->visible_instances(), trains, camera.eye(), audio_, outside_gain);
        }
        if (roads_ && effects_.traffic) roads_->update(clock.elapsed, streamer_->visible_instances(), crossings_.get(), *library_);
        if (precipitation_ || smoke_) {
            // The light on a surface facing up, as RTS's prec / smoke shaders take it.
            const auto& lighting = environment().lighting;
            const float factor = std::clamp(-lighting.sun_direction.y * 2.0f, 0.0f, 1.0f);
            const glm::vec4 light{glm::max(glm::max(lighting.sun_ambient, lighting.sun_diffuse * factor), glm::vec4{0.0f}) + lighting.flash};
            const glm::vec4 opaque{glm::vec3{light}, 1.0f};
            if (precipitation_) {
                const auto forward = camera.forward();
                glm::vec3 right{forward.z, 0.0f, -forward.x};
                right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3{1.0f, 0.0f, 0.0f};
                // Under a thunderstorm heavier; the dynamic weather's by its amount.
                const float amount = dynamic_weather_ ? 0.3f + 1.4f * weather_cycle().shown.precipitation : storm_ ? 1.7f : 1.0f;
                precipitation_->update(clock.elapsed, camera.eye(), right, effects_.precipitation ? amount : 0.0f, opaque);
            }
            if (smoke_) {
                std::vector<ExhaustSource> sources;
                if (train_ && effects_.smoke) train_->collect_exhausts(sources);
                smoke_->update(clock.elapsed, sources, opaque);
            }
        }
        update_weather_sound(clock.elapsed, camera.eye(), outside_gain);
        // The thunder of the strikes seen (update_lightning), as it comes.
        if (thunder_) thunder_->update(clock.time_of_day_exact, outside_gain, audio_);
        update_loading();
    }

    // The loading screen's end: the train on the track with its start view,
    // its shapes (and the cab, for a start in it), the world tiles around
    // the camera with their models on the GPU, the terrain with its
    // textures; all of it for a few frames running (a tile asks for its
    // ground textures when first drawn). Given up after loading_limit.
    void update_loading() {
        if (loading_.done) return;
        TrainView::Loading train;
        if (train_) train = train_->loading();
        const auto streamed = streamer_->stats();
        const auto library = library_->stats();
        std::size_t world = streamed.waiting_tiles + streamed.pending_model_assets + streamed.ready_model_assets
            + library.queued_uploads + (library.reloading ? 1 : 0);
        if (terrain_) world += terrain_->pending_jobs() + terrain_->pending_textures();
        const std::size_t pending = train.pending + world;
        loading_.stage = !train.settled ? "Loading the train" : train.cab && train.pending > 0 ? "Loading the cab" : "Loading the route";
        // The train put on the track is the first tenth; the rest by what is
        // left to load against the most there was.
        if (train.settled) {
            loading_peak_ = (std::max)(loading_peak_, pending);
            const float share = loading_peak_ ? 1.0f - static_cast<float>(pending) / static_cast<float>(loading_peak_) : 1.0f;
            loading_.progress = (std::max)(loading_.progress, 0.1f + 0.9f * share);
        }
        loading_quiet_ = train.settled && pending == 0 ? loading_quiet_ + 1 : 0;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - loading_since_).count();
        if (seconds >= loading_report_ + 5.0) {
            // What the start still waits for, every 5 s (a slow start's log).
            loading_report_ = seconds;
            std::cout << "loading: " << static_cast<int>(seconds) << " s, train settled " << train.settled << ", " << train.pending
                      << " of its shapes; tiles " << streamed.waiting_tiles << ", models "
                      << streamed.pending_model_assets + streamed.ready_model_assets << ", uploads " << library.queued_uploads
                      << ", terrain " << (terrain_ ? terrain_->pending_jobs() + terrain_->pending_textures() : 0) << std::endl;
        }
        if (loading_quiet_ < 5 && seconds < loading_limit) return;
        loading_.done = true;
        loading_.hold_time = false;
        loading_.progress = 1.0f;
        if (loading_quiet_ >= 5) {
            std::cout << "loading: done in " << seconds << " s" << std::endl;
        } else {
            std::cout << "loading: given up after " << seconds << " s, " << pending << " still loading (train settled "
                      << train.settled << ", " << train.pending << " of its shapes; tiles " << streamed.waiting_tiles << ", models "
                      << streamed.pending_model_assets + streamed.ready_model_assets << ", uploads " << library.queued_uploads
                      << ", terrain " << (terrain_ ? terrain_->pending_jobs() + terrain_->pending_textures() : 0) << ")" << std::endl;
        }
    }

    [[nodiscard]] SceneEnvironment environment() const override {
        if (!sky_state_) return {};
        // The sky's fog block is shared by every RTS shader (terrain, shapes).
        auto lighting = sky_state_->lighting;
        lighting.flash = flash_light_;
        lighting.light_rings = light_rings_;
        if (train_) lighting.interior_lights = train_->interior_lights();
        if (train_) lighting.portals = train_->portals();
        if (train_) {
            if (const auto room = train_->light_shaft_room()) lighting.room = renderer::ModelRoom{room->to_room, room->lower, room->upper};
        }
        lighting.fog_colour = sky_state_->fog_colour;
        lighting.fog_start = sky_state_->fog_start;
        lighting.fog_end = sky_state_->fog_end;
        glm::vec4 clear{glm::vec3{sky_state_->clear_colour}, 1.0f};
        if (native_sky_on()) {
            // The land fogs into openRTS's sky's horizon behind it (FogRing;
            // RTS's fog strength kept: its colour's alpha). RTS's fog ends at
            // its dome's edge (Novokuznetsk: 2000 m), the land beyond the dome
            // hidden; without the dome only the land's end is to hide, the
            // terrain's loaded tiles round the eye.
            lighting.fog_colour = glm::vec4{native_sky_->horizon_colour(), lighting.fog_colour.w};
            lighting.fog_ring = native_sky_->fog_ring();
            lighting.fog_end = land_end;
            lighting.fog_start = 0.4f * land_end;
            // Black under the sky: it adds itself over the moon drawn first.
            clear = glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
        }
        return {
            .lighting = lighting,
            .clear_colour = clear,
            .time_of_day_applies = true,
            .cloud_shadow = native_sky_on() ? native_sky_->cloud_shadow() : renderer::CloudShadow{},
            .sky_probe = sky_renderer_ ? sky_renderer_->reflection_probe() : nullptr,
            .sky_openness = sky_light_open_,
        };
    }

    // Sky first (no depth test), then terrain; models follow.
    void draw_background(const WGPURenderPassEncoder pass, const FrameView& frame) override {
        // openRTS's own sky is drawn after the opaque objects instead
        // (draw_before_translucent), where only the sky is left; the RTS
        // moon goes first, on the far plane, the clouds then over it.
        if (sky_renderer_ && sky_state_) {
            if (native_sky_on()) {
                sky_renderer_->render_satellites(pass, frame.projection, frame.view, *sky_state_, 0);
            } else {
                sky_renderer_->render(pass, frame.projection, frame.view, *sky_state_);
            }
        }
        if (terrain_) {
            terrain_->set_sky_probe(sky_renderer_ ? sky_renderer_->reflection_probe() : nullptr);
            terrain_->render(pass, frame.projection * frame.view, environment().lighting, frame.effects);
        }
    }

    void draw_depth(const WGPURenderPassEncoder pass, const std::uint32_t slot, const glm::mat4& view_projection,
                    const renderer::DepthPassKind kind) override {
        if (terrain_) terrain_->render_depth(pass, slot, view_projection, kind);
    }

    void prepare_depth() override {
        if (terrain_) terrain_->prepare_depth();
    }

    void submit(ModelRenderer& renderer, const FrameView& frame) override {
        OPENRTS_ZONE("scene: submit");
        // The terrain is drawn before (draw_background): it takes the path from the next frame.
        if (terrain_) terrain_->set_shading_path(renderer.shading_path());
        experimental_ = renderer.shading_path() == renderer::ShadingPath::experimental;
        last_view_projection_ = frame.projection * frame.view;
        last_eye_ = frame.eye;
        last_viewport_height_ = frame.viewport_height;
        last_vertical_fov_ = frame.vertical_fov;
        const auto frustum = renderer::Frustum::from_view_projection(frame.projection * frame.view);

        drawn_instances_ = 0;
        // Which instances are drawn, or only cast a shadow into view: decided
        // on the workers (the library is only read meanwhile), applied below
        // in order on this thread.
        const auto& visible = streamer_->visible_instances();
        picks_.resize(visible.size());
        // Each instance's model, found once a set of tiles (a model loaded
        // later is looked for until it is there; a reload keeps its slot).
        if (model_cache_generation_ != streamer_->visible_generation() || model_cache_.size() != visible.size()) {
            model_cache_.assign(visible.size(), nullptr);
            model_cache_generation_ = streamer_->visible_generation();
        }
        const auto decide = [&](const std::size_t from, const std::size_t to) {
            for (std::size_t k = from; k < to; ++k) {
                const auto& instance = visible[k];
                auto* model = model_cache_[k];
                if (!model) model = model_cache_[k] = library_->find(instance.source_path);
                picks_[k] = {model, Pick::skip};
                if (!model) continue;
                const auto bounds = model->bounds();
                const auto center = glm::vec3{instance.transform * glm::vec4{bounds.center, 1.0f}};
                if (culling_ && beyond_draw_distance(center, bounds.radius, frame.eye)) continue;
                if (culling_ && !frustum.intersects_sphere(center, bounds.radius)) {
                    // Out of view but near: it may still cast a shadow into it.
                    if (frame.shadow_range > 0.0f && glm::distance(center, frame.eye) - bounds.radius < frame.shadow_range)
                        picks_[k].kind = Pick::shadow;
                    continue;
                }
                picks_[k].kind = Pick::draw;
            }
        };
        constexpr std::size_t chunk = 512;
        if (frame.pool && visible.size() > chunk) {
            std::vector<std::function<void()>> tasks;
            for (std::size_t from = 0; from < visible.size(); from += chunk)
                tasks.emplace_back([&decide, from, &visible] { decide(from, (std::min)(from + chunk, visible.size())); });
            frame.pool->start(std::move(tasks));
            frame.pool->wait();
        } else {
            decide(0, visible.size());
        }
        for (std::size_t k = 0; k < visible.size(); ++k) {
            const auto& pick = picks_[k];
            if (pick.kind == Pick::skip) continue;
            const auto& instance = visible[k];
            auto* model = pick.model;
            // Its tag (the instance's place + 1): its lamps carry it (collect_lights).
            const auto owner = static_cast<std::uint32_t>(k + 1);
            if (pick.kind == Pick::shadow) {
                renderer.submit_shadow_caster(*model, instance.transform);
                renderer.set_world_object(instance.static_flags, instance.kind == formats::WorldObjectKind::transfer);
                renderer.set_owner(owner);
                continue;
            }

            // Switch shapes follow their junction; other shapes stay at rest.
            // Switch blades and crossing gates move: the sun's shadow cache
            // draws them every frame.
            if (const auto blade = track_ ? track_->switch_frame(instance) : std::nullopt) {
                renderer.set_moving(true);
                renderer.submit(*model, instance.transform, *blade);
                renderer.set_moving(false);
            } else if (const auto gates = crossings_ ? crossings_->adjustments(instance, *library_) : std::nullopt) {
                renderer.set_moving(true);
                renderer.submit(*model, instance.transform, *gates);
                renderer.set_moving(false);
            } else if (const auto hidden = signals_ ? signals_->hidden_parts(instance, *library_)
                                                    : std::vector<renderer::NodeAdjustment>{};
                       !hidden.empty()) {
                renderer.submit(*model, instance.transform, hidden);
            } else {
                renderer.submit(*model, instance.transform);
            }
            renderer.set_world_object(instance.static_flags, instance.kind == formats::WorldObjectKind::transfer);
            renderer.set_owner(owner);
            ++drawn_instances_;
        }
        // Trains and road traffic move (the sun's shadow cache).
        renderer.set_moving(true);
        if (train_) {
            train_->submit(renderer, *library_,
                           {.frustum = culling_ ? &frustum : nullptr, .eye = frame.eye, .draw_distance = culling_ ? draw_distance : 0.0f,
                            .shadow_range = frame.shadow_range});
        }
        if (roads_ && effects_.traffic) roads_->submit(renderer, *library_, culling_ ? &frustum : nullptr, frame.eye, culling_ ? draw_distance : 0.0f);
        renderer.set_moving(false);
        if (signals_) signals_->collect_lights(streamer_->visible_instances(), *library_, frame.eye, draw_distance, frame.view_zoom);
    }

    [[nodiscard]] std::optional<formats::RoutePosition> origin() const override { return origin_; }

    // Signal and train light glows where RTS draws its lights (0x408574):
    // after every object of passes -2..2, the glass included, and before the
    // cab (pass 3), without writing depth. RTS lays them over by their alpha;
    // the experimental path adds them (emissive, HDR).
    void draw_after_translucent(const WGPURenderPassEncoder pass, const FrameView& frame) override {
        // Rain or snow, then the smoke (RTS 0x408574: the precipitation in
        // the object passes, the smoke after them).
        if (precipitation_) precipitation_->render(pass, frame.projection, frame.view, frame.viewport_height);
        if (smoke_) smoke_->render(pass, frame.projection, frame.view, frame.viewport_height);
        const auto blend = experimental_ ? renderer::GlowBlend::added : renderer::GlowBlend::over;
        if (signals_) signals_->draw_lights(pass, frame, frame.viewport_height, blend);
        if (train_glows_ && train_ && !train_->light_glows().empty()) {
            std::vector<renderer::GlowSprite> sprites;
            for (const auto& glow : train_->light_glows()) {
                auto sprite = glow.sprite;
                sprite.texture = glow.texture.empty() ? 0u : glow_texture(glow.texture);
                sprites.push_back(sprite);
            }
            train_glows_->set_sprites(std::move(sprites));
            train_glows_->render(pass, frame.projection, frame.view, frame.viewport_height, blend);
        }
    }

    // The bloom's lights: the signals' and the train's glows as drawn (a
    // scenery lamp blooms by its bulb's texture: its light's Position is
    // where it lights from, often away from the bulb).
    void draw_bloom_sources(const WGPURenderPassEncoder pass, const FrameView& frame) override {
        if (!experimental_) return;
        if (lightning_ && !lightning_->empty())
            lightning_->render_bloom(pass, frame.projection * frame.view, {frame.viewport_height * frame.aspect, frame.viewport_height});
        if (signals_) signals_->draw_bloom(pass, frame, frame.viewport_height);
        if (train_glows_ && train_ && !train_->light_glows().empty())
            train_glows_->render_bloom(pass, frame.projection, frame.view, frame.viewport_height);
    }

    // The track lines, the helper lines and the labels' occlusion tests:
    // after the solid models, before the glass (which writes depth: anything
    // drawn after it vanished behind the windows).
    void draw_before_translucent(const WGPURenderPassEncoder pass, const FrameView& frame) override {
        // openRTS's own sky where the opaque objects left the far plane
        // (over the RTS moon, drawn there first); then the openRTS water over
        // the opaque models (what stands in it shows through), before the
        // translucent ones.
        if (native_sky_on()) native_sky_->render(pass, frame.projection, frame.view, renderer::NativeSky::frame_slot);
        // Lightning over the sky, behind whatever stands in front of it.
        if (lightning_ && !lightning_->empty())
            lightning_->render(pass, frame.projection * frame.view, {frame.viewport_height * frame.aspect, frame.viewport_height});
        if (terrain_) terrain_->render_water(pass);
        if (signals_) signals_->draw(pass, frame, frame.viewport_height);
        if (track_) track_->draw(pass, frame.projection * frame.view, train_ && train_->placed() ? train_->head_position() : frame.eye);
        if (wide_lines_) {
            wide_lines_->render(pass, pending_lines_, frame.projection * frame.view,
                                {frame.viewport_height * frame.aspect, frame.viewport_height});
        }
        pending_lines_.clear();
        if (probe_) {
            // The labels' tests (draw_world_labels) and, under a storm, how
            // much of the view is sky: points on the far plane over the
            // whole view pass only where nothing is drawn in front.
            auto points = std::move(label_probe_points_);
            auto keys = std::move(label_probe_keys_);
            label_probe_points_.clear();
            label_probe_keys_.clear();
            std::size_t sky = 0;
            if (storm_) {
                for (int row = 0; row < sky_probe_rows; ++row) {
                    for (int column = 0; column < sky_probe_columns; ++column) {
                        if (points.size() >= renderer::OcclusionProbe::max_points) break;
                        const float x = -0.9f + 1.8f * (static_cast<float>(column) + 0.5f) / static_cast<float>(sky_probe_columns);
                        const float y = -0.8f + 1.75f * (static_cast<float>(row) + 0.5f) / static_cast<float>(sky_probe_rows);
                        points.push_back({.ndc = {x, y}, .depth = 0.0f});
                        ++sky;
                    }
                }
            }
            // The sky's openness round the eye for its light (PBR stage 2):
            // ways of the view above the horizon, each tested twice, 6 m out
            // (past the cab or salon round the eye: a way out through a
            // window) and on the far plane (the sky itself). Of the ways
            // out, the share that reach the sky; none out (looking down, at
            // a wall close by): the last answer kept.
            std::size_t sky_light_ways = 0;
            std::vector<std::uint8_t> sky_light_bins;
            if (renderer::frame_sky_light()) {
                const glm::mat4 to_world = glm::inverse(frame.projection * frame.view);
                const glm::vec4 out = frame.projection * glm::vec4{0.0f, 0.0f, -6.0f, 1.0f};
                const float out_depth = out.w != 0.0f ? out.z / out.w : 0.0f;
                for (int row = 0; row < sky_light_rows; ++row) {
                    for (int column = 0; column < sky_light_columns; ++column) {
                        if (points.size() + 2 > renderer::OcclusionProbe::max_points) break;
                        const float x = -0.92f + 1.84f * (static_cast<float>(column) + 0.5f) / static_cast<float>(sky_light_columns);
                        const float y = -0.9f + 1.85f * (static_cast<float>(row) + 0.5f) / static_cast<float>(sky_light_rows);
                        const glm::vec4 far = to_world * glm::vec4{x, y, 0.5f, 1.0f};
                        if (far.w == 0.0f) continue;
                        const glm::vec3 way = glm::normalize(glm::vec3{far} / far.w - frame.eye);
                        if (way.y < 0.05f) continue;
                        points.push_back({.ndc = {x, y}, .depth = out_depth});
                        points.push_back({.ndc = {x, y}, .depth = 0.0f});
                        ++sky_light_ways;
                        // Its bin: the azimuth in twelfths, the height in thirds of the way up.
                        const float turn = (std::atan2(way.z, way.x) + 3.14159265f) / 6.2831853f;
                        const int azimuth = std::clamp(static_cast<int>(turn * open_azimuths), 0, open_azimuths - 1);
                        const int height = way.y < 0.35f ? 0 : way.y < 0.7f ? 1 : 2;
                        sky_light_bins.push_back(static_cast<std::uint8_t>(height * open_azimuths + azimuth));
                    }
                }
            }
            ++probe_tag_;
            probe_->request(points, probe_tag_, {frame.viewport_height * frame.aspect, frame.viewport_height});
            if (!points.empty())
                probe_frames_.push_back({probe_tag_, std::move(keys), sky, sky_light_ways, std::move(sky_light_bins), frame.eye});
            while (probe_frames_.size() > 8) probe_frames_.pop_front();
            probe_->draw(pass);
        }
    }

    // The probe's newest answer: the labels hidden behind the scene, the sky's share of the view.
    void take_probe_results() {
        if (!probe_) return;
        std::uint64_t tag = 0;
        std::vector<bool> visible;
        if (!probe_->take(tag, visible)) return;
        while (!probe_frames_.empty() && probe_frames_.front().tag < tag) probe_frames_.pop_front();
        if (probe_frames_.empty() || probe_frames_.front().tag != tag) return;
        const auto& asked = probe_frames_.front();
        if (!asked.labels.empty()) {
            hidden_labels_.clear();
            for (std::size_t i = 0; i < asked.labels.size() && i < visible.size(); ++i) {
                if (!visible[i]) hidden_labels_.insert(asked.labels[i]);
            }
        }
        if (asked.sky > 0) {
            std::size_t open = 0;
            for (std::size_t i = asked.labels.size(); i < asked.labels.size() + asked.sky && i < visible.size(); ++i) open += visible[i] ? 1 : 0;
            // A quarter of the view's sky (a field, a cab's windows: less) lights it all.
            sky_open_seen_ = std::clamp(4.0f * static_cast<float>(open) / static_cast<float>(asked.sky), 0.0f, 1.0f);
        }
        if (asked.sky_light > 0) {
            // This answer's ways out by their bins: each bin seen now takes
            // its share reaching the sky.
            std::array<int, open_azimuths * open_heights> out{};
            std::array<int, open_azimuths * open_heights> open{};
            const std::size_t first = asked.labels.size() + asked.sky;
            for (std::size_t k = 0; k < asked.sky_light && k < asked.sky_light_bins.size(); ++k) {
                const std::size_t at = first + 2 * k;
                if (at + 1 >= visible.size()) break;
                if (!visible[at]) continue; // a wall within 6 m: says nothing of the sky
                const auto bin = asked.sky_light_bins[k];
                ++out[bin];
                open[bin] += visible[at + 1] ? 1 : 0;
            }
            for (std::size_t b = 0; b < open_bins_.size(); ++b) {
                if (out[b] == 0) continue;
                open_bins_[b] = {.known = true, .open = static_cast<float>(open[b]) / static_cast<float>(out[b]), .where = asked.eye};
            }
            // The bins' mean, weighing less those seen 30 m and more off.
            // Half the ways out reaching the sky (a street, a station): all
            // of its light; a twentieth or less (a tunnel): none here, the
            // renderer keeps the least its mouths let in
            // (SceneEffectSettings::sky_light_tunnel, a tenth by default).
            float sum = 0.0f, weight = 0.0f;
            int known = 0;
            for (const auto& bin : open_bins_) {
                if (!bin.known) continue;
                const float w = std::exp(-glm::distance(bin.where, asked.eye) / 30.0f);
                sum += w * bin.open;
                weight += w;
                ++known;
            }
            if (known >= 2 && weight > 1e-6f) sky_light_seen_ = std::clamp(2.0f * sum / weight - 0.1f, 0.0f, 1.0f);
        }
        probe_frames_.pop_front();
    }

    // A thunderstorm's lightning (lightning.hpp): the strikes alight round
    // the eye, their light on the scene (every one by its distance, as much
    // as the sky round the eye is open: none deep in a tunnel), on the
    // clouds (the one most seen, kept between its strokes) and their
    // channels. None while the clock stands (a flash would stay lit) nor in a
    // time-lapse past 16 times.
    void update_lightning(const SceneClock& clock, const glm::vec3& eye, const glm::vec3& forward, const formats::RoutePosition& at) {
        flash_light_ = glm::vec4{0.0f};
        sky_flash_id_ = 0;
        sky_flash_ = 0.0f;
        std::vector<renderer::LightningStrip> strips;
        // The panel's test strike: taken now, kept only under a running storm.
        const auto test = renderer::lightning_test_request();
        renderer::lightning_test_request().reset();
        if (!storm_ || clock.time_speed <= 0.0f || clock.time_speed > 16.0f) {
            lightning_channels_.clear();
            test_strikes_.clear();
            if (lightning_) lightning_->set_strips({});
            return;
        }
        const glm::dvec2 eye_route{static_cast<double>(at.tile_x) * formats::route_tile_size + at.x,
                                   static_cast<double>(at.tile_z) * formats::route_tile_size + at.z};
        // The dynamic weather's storm strikes as often as it is strong.
        const float rate = renderer::lightning_rate() * (dynamic_weather_ ? std::clamp(weather_cycle().shown.storm, 0.0f, 1.0f) : 1.0f);
        auto strikes = renderer::lightning_strikes(eye_route, clock.time_of_day_exact, rate);
        if (test) {
            // Straight ahead along the ground (scene x, z are the route's).
            glm::dvec2 way{forward.x, forward.z};
            way = glm::length(way) > 1e-4 ? glm::normalize(way) : glm::dvec2{0.0, 1.0};
            ++test_strikes_made_;
            renderer::LightningStrike strike;
            strike.id = (0xF1ull << 56) | test_strikes_made_;
            strike.ground = eye_route + way * static_cast<double>(std::clamp(*test, 50.0f, renderer::lightning_radius));
            strike.start = clock.time_of_day_exact;
            strike.to_ground = true;
            strike.seed = static_cast<std::uint32_t>(test_strikes_made_ * 2654435761ull);
            strike.strength = 1.0f;
            test_strikes_.push_back(strike);
            std::cout << "lightning: a test strike " << static_cast<int>(*test) << " m ahead" << std::endl;
        }
        std::erase_if(test_strikes_, [&clock](const renderer::LightningStrike& s) {
            const double age = clock.time_of_day_exact - s.start;
            return age < 0.0 || age >= renderer::lightning_duration;
        });
        strikes.insert(strikes.begin(), test_strikes_.begin(), test_strikes_.end());
        const float brightness = std::clamp(renderer::lightning_brightness(), 0.0f, 3.0f);
        // The clouds' base and top above sea (the last frame's).
        const float base = native_sky_on() ? native_sky_->cloud_base() : static_cast<float>(at.y) + 1200.0f;
        const float top = native_sky_on() ? native_sky_->cloud_top() : base + 2200.0f;
        // The scene's height of the base (its y: the route's less the origin's).
        const float scene_base = base - static_cast<float>(origin_.y);
        // The frame's encoding (gamma 2.2) of the flash's colour.
        const glm::vec3 encoded = glm::pow(renderer::lightning_colour, glm::vec3{1.0f / 2.2f});
        float land = 0.0f;
        float most_seen = 0.0f;
        std::unordered_set<std::uint64_t> alive;
        // The frame's span of the clock: each strike's brightest within it.
        const double span = std::clamp(static_cast<double>(clock.elapsed) * clock.time_speed, 0.0, 0.1);
        for (const auto& strike : strikes) {
            alive.insert(strike.id);
            const float light = renderer::lightning_flash_seen(strike, clock.time_of_day_exact - strike.start, span) * brightness;
            const glm::dvec2 offset = strike.ground - eye_route;
            const auto distance = static_cast<float>(glm::length(offset));
            // The land lit by the clouds round it: falling off over kilometres.
            const float near = 1.0f / (1.0f + (distance / 2500.0f) * (distance / 2500.0f));
            land += light * (strike.to_ground ? 1.0f : 0.6f) * near;
            const float seen = strike.strength / (1.0f + (distance / 4000.0f) * (distance / 4000.0f));
            if (seen > most_seen) {
                most_seen = seen;
                sky_flash_id_ = strike.id;
                sky_flash_ = light;
                // A strike to the ground lights the cloud's base; one within
                // the cloud, its middle.
                const float height = strike.to_ground ? base + 300.0f : base + 0.4f * (top - base);
                sky_flash_at_ = glm::vec3{static_cast<float>(offset.x), height, static_cast<float>(offset.y)};
            }
            // Its thunder: from the ground point (a strike to it), else from
            // the flash in the cloud; heard once, when its sound arrives.
            glm::vec3 foot{eye.x + static_cast<float>(offset.x), eye.y - 2.0f, eye.z + static_cast<float>(offset.y)};
            if (train_) {
                if (const auto ground = train_->ground_at(foot)) foot.y = *ground;
            }
            if (thunder_) {
                const float up = strike.to_ground ? 0.0f : (std::max)(scene_base - eye.y, 0.0f);
                thunder_->strike(strike, std::sqrt(distance * distance + up * up), foot + glm::vec3{0.0f, up, 0.0f});
            }
            if (!strike.to_ground || light <= 0.01f || !lightning_) continue;
            // Its channel from the ground there up into the cloud's base.
            auto& channel = lightning_channels_[strike.id];
            if (channel.empty()) channel = renderer::lightning_channel(strike, (std::max)(scene_base - foot.y + 250.0f, 300.0f));
            // Dimmed by the rain's haze over the way.
            const float core = light * 7.0f * std::exp(-distance / 6000.0f);
            for (const auto& piece : channel) {
                renderer::LightningStrip strip;
                strip.a = foot + piece.a;
                strip.b = foot + piece.b;
                strip.width = 1.5f * piece.width;
                strip.light_a = glm::vec4{encoded * (core * piece.brightness_a), 1.0f};
                strip.light_b = glm::vec4{encoded * (core * piece.brightness_b), 1.0f};
                strips.push_back(strip);
            }
        }
        std::erase_if(lightning_channels_, [&alive](const auto& entry) { return !alive.contains(entry.first); });
        flash_light_ = glm::vec4{encoded * ((std::min)(land, 1.5f) * 0.45f * sky_open_), 0.0f};
        // OPENRTS_LIGHTNING_TRACE: each strike as it begins, and its
        // brightest moment's light on the scene and the channel drawn.
        static const bool trace = [] {
#ifdef _WIN32
            char* value = nullptr;
            std::size_t size = 0;
            const bool set = _dupenv_s(&value, &size, "OPENRTS_LIGHTNING_TRACE") == 0 && value != nullptr;
            std::free(value);
            return set;
#else
            return std::getenv("OPENRTS_LIGHTNING_TRACE") != nullptr;
#endif
        }();
        if (trace) {
            for (const auto& strike : strikes) {
                const auto [entry, fresh] = lightning_traced_.try_emplace(strike.id, 0.0f, std::size_t{0});
                auto& seen = entry->second;
                const float light = renderer::lightning_flash_seen(strike, clock.time_of_day_exact - strike.start, span) * brightness;
                if (fresh) {
                    const auto distance = glm::length(strike.ground - eye_route);
                    std::cout << "lightning: strike " << (strike.id % 100000) << (strike.to_ground ? " to the ground" : " within the cloud")
                              << " " << static_cast<int>(distance) << " m away, strength " << strike.strength << std::endl;
                }
                if (light > seen.first) seen.first = light;
                seen.second = (std::max)(seen.second, strike.to_ground ? lightning_channels_[strike.id].size() : std::size_t{0});
            }
            for (auto it = lightning_traced_.begin(); it != lightning_traced_.end();) {
                if (alive.contains(it->first)) {
                    ++it;
                    continue;
                }
                std::cout << "lightning: strike " << (it->first % 100000) << " out: brightest " << it->second.first << ", "
                          << it->second.second << " pieces; the scene's flash now " << flash_light_.z << ", sky open " << sky_open_
                          << " (seen " << sky_open_seen_ << ")" << std::endl;
                it = lightning_traced_.erase(it);
            }
            if (flash_light_.z > lightning_traced_peak_) {
                lightning_traced_peak_ = flash_light_.z;
                std::cout << "lightning: the scene's flash up to " << flash_light_.z << " (sky open " << sky_open_ << ", strips "
                          << strips.size() << ")" << std::endl;
            }
        }
        if (lightning_) lightning_->set_strips(std::move(strips));
    }

    // The cab screens (КЛУБ, САУТ ...) after the whole cab, as RTS draws them
    // (0x408574 calls 0x46B668 after the object passes, depth writes off):
    // the cab is drawn last of the models (its pass 3), and its panel would
    // cover screens drawn before it.
    void draw_foreground(const WGPURenderPassEncoder pass, const FrameView& frame) override {
        if (screens_ && train_ && !train_->screen_batches().empty()) {
            // Not laid out before the pass (no before_main_pass this frame):
            // straight onto the panel.
            if (!screens_set_) set_screens();
            screens_->render(pass, frame.projection * frame.view, frame.eye);
        }
        screens_set_ = false;
    }

    // This frame's cab screens to the renderer: the supersampled ones blend
    // premultiplied textures (renderer::cab_screens_supersampled).
    void set_screens() {
        screens_set_ = true;
        const bool supersampled = renderer::cab_screens_supersampled();
        std::vector<renderer::ScreenQuadVertex> vertices;
        std::vector<renderer::ScreenQuadDraw> draws;
        std::vector<renderer::ScreenSurface> surfaces;
        for (const auto& batch : train_->screen_batches()) {
            const auto texture = screen_texture(batch.texture, supersampled && batch.blend);
            if (texture == 0 || batch.vertices.empty()) continue;
            draws.push_back({texture, batch.blend, static_cast<std::uint32_t>(vertices.size()),
                             static_cast<std::uint32_t>(batch.vertices.size()), batch.screen});
            for (const auto& v : batch.vertices) vertices.push_back({v.position, v.uv, v.colour, v.local});
        }
        for (const auto& plane : train_->screen_planes()) {
            surfaces.push_back({plane.top_left, plane.top_right, plane.bottom_right, glm::length(plane.top_right - plane.top_left),
                                glm::length(plane.bottom_right - plane.top_right)});
        }
        screens_->set(std::move(vertices), std::move(draws), std::move(surfaces));
    }
    bool screens_set_ = false; // set_screens() this frame

    [[nodiscard]] WGPUQuerySet occlusion_query_set() const override { return probe_ ? probe_->query_set() : nullptr; }
    [[nodiscard]] bool seated_in_cab() const override { return train_ && train_->seated_in_cab(); }
    [[nodiscard]] bool has_train_panel() const override { return true; }
    void draw_train_panel() const override {
#ifdef OPENRTS_HAS_IMGUI
        if (train_) {
            train_->draw_panel();
        } else {
            ImGui::TextDisabled("%s", tr("No train yet."));
        }
#endif
    }

    // openRTS's own sky's clouds for this frame's view (half its size); the
    // cab screens' images (supersampled).
    void before_main_pass(const WGPUCommandEncoder encoder, const FrameView& frame) override {
        if (screens_ && train_ && !train_->screen_batches().empty()) {
            set_screens();
            screens_->prepare(encoder);
        }
        if (native_sky_on()) {
            native_sky_->render_clouds(encoder, frame.projection, frame.view, static_cast<std::uint32_t>(frame.viewport_width),
                                       static_cast<std::uint32_t>(frame.viewport_height));
        }
    }

    void after_main_pass(const WGPUCommandEncoder encoder) override {
        if (probe_) probe_->resolve(encoder);
        // openRTS's own sky's atmosphere, for the next frame.
        if (native_sky_on()) native_sky_->render_table(encoder);
        // The sky the openRTS water reflects, and the sky's light is made
        // from (PBR stage 2, the window's SkyLight), for the next frame.
        const bool water_sky = terrain_ && terrain_->draws_native_water();
        if (sky_renderer_ && sky_state_ && (water_sky || renderer::frame_sky_light())) {
            if (native_sky_on()) {
                sky_renderer_->render_reflection_probe(
                    encoder, *sky_state_,
                    [&](const WGPURenderPassEncoder face, const glm::mat4& projection, const glm::mat4& view, const std::uint32_t slot) {
                        sky_renderer_->render_satellites(face, projection, view, *sky_state_, slot);
                        native_sky_->render(face, projection, view, slot);
                    });
            } else {
                sky_renderer_->render_reflection_probe(encoder, *sky_state_);
            }
        }
    }

    void frame_submitted() override {
        if (probe_) probe_->submitted();
    }

    [[nodiscard]] bool keyboard_captured() const override { return train_ && train_->keyboard_captured(); }
    [[nodiscard]] bool control_in_hand() const override { return train_ && train_->control_in_hand(); }
    [[nodiscard]] bool engineer_on_foot() const override { return train_ && train_->engineer_on_foot(); }
    [[nodiscard]] bool engineer_action_at_hand() const override { return train_ && train_->engineer_action_at_hand(); }
    [[nodiscard]] bool takes_scroll() const override { return train_ && train_->viewing_inside(); }
    [[nodiscard]] std::optional<double> take_time_of_day() override {
        return net_ ? net_->take_time_of_day() : std::nullopt;
    }

    // --- saved games (docs/saves.md) ---------------------------------------
    // One text file: a header (route, scenario, clock), then parts after
    // "@@ <part>" lines: the junctions, the trains (TrainView::save_game),
    // the scenario (ActivityView::save).
    [[nodiscard]] std::filesystem::path saves_folder() const override {
        if (!train_ || net_) return {};
        return user_data_directory() / "saves" / std::string(route_.filename());
    }
    [[nodiscard]] std::vector<std::string> saved_games() const override {
        std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(saves_folder(), error)) {
            if (entry.path().extension() != ".save") continue;
            const auto stem = entry.path().stem().u8string();
            found.emplace_back(entry.last_write_time(error), std::string(stem.begin(), stem.end()));
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::string> names;
        for (auto& [_, name] : found) names.push_back(std::move(name));
        return names;
    }
    [[nodiscard]] std::string save_game(const std::string& name) override {
        if (net_) return tr("saving is not available in a network game yet");
        if (!train_ || !track_) return tr("there is no game to save");
        if (train_->loading_game()) return tr("the saved game is still being put back");
        std::string clean;
        for (const char c : name) clean += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
        if (clean.empty()) return tr("give the save a name");
        std::ostringstream out;
        out << std::setprecision(17);
        out << "# openRTS saved game\nversion=1\nroute=" << route_.filename() << '\n';
        if (activity_) out << "activity=" << activity_->activity().name << "\nactivity_file=" << content_.activity_name() << '\n';
        out << "time=" << last_clock_.time_of_day_exact << "\nseason=" << static_cast<int>(last_clock_.season) << "\nweather="
            << static_cast<int>(last_clock_.weather) << "\nstorm=" << (last_clock_.storm ? 1 : 0) << '\n';
        // The dynamic weather's clock and seed: it goes on as it was.
        if (dynamic_weather_) out << "weather_cycle=" << weather_cycle().time << ' ' << weather_cycle().seed << '\n';
        out << "@@ junctions\n";
        const auto& network = track_->network();
        for (std::uint32_t node = 0; node < network.node_count(); ++node) {
            if (const auto* junction = network.junction(node)) out << "junction=" << node << ' ' << junction->selected << '\n';
        }
        out << "@@ trains\n" << train_->save_game();
        if (activity_) out << "@@ activity\n" << activity_->save();
        std::error_code fs_error;
        std::filesystem::create_directories(saves_folder(), fs_error);
        const auto file = saves_folder() / std::filesystem::path(std::u8string(clean.begin(), clean.end())).replace_extension(".save");
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << out.str();
        if (!output) return tr("the save could not be written");
        std::cout << "saved game: " << file.string() << std::endl;
        return {};
    }
    [[nodiscard]] std::string load_game(const std::filesystem::path& file) override {
        if (net_) return tr("loading is not available in a network game");
        if (!train_ || !track_) return tr("there is no game to load into");
        std::ifstream input(file, std::ios::binary);
        if (!input) return tr("the save could not be read");
        std::map<std::string, std::string> header;
        std::map<std::string, std::string> parts;
        std::string part;
        for (std::string line; std::getline(input, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("@@ ")) {
                part = line.substr(3);
                continue;
            }
            if (part.empty()) {
                const auto equals = line.find('=');
                if (equals != std::string::npos && !line.starts_with('#')) header[line.substr(0, equals)] = line.substr(equals + 1);
            } else {
                parts[part] += line + '\n';
            }
        }
        if (header["route"] != std::string(route_.filename())) return trf("the save is from another route (%s)", header["route"].c_str());
        const std::string scenario = activity_ ? activity_->activity().name : std::string();
        if (header["activity"] != scenario) {
            return header["activity"].empty() ? std::string(tr("the save is from a free run: start the route without a scenario"))
                                              : trf("the save is from the scenario \"%s\": start it first", header["activity"].c_str());
        }
        auto& network = track_->network();
        std::istringstream junctions(parts["junctions"]);
        std::size_t thrown = 0;
        for (std::string line; std::getline(junctions, line);) {
            std::uint32_t node{}, selected{};
            if (!line.starts_with("junction=")) continue;
            std::istringstream fields(line.substr(9));
            if (!(fields >> node >> selected)) continue;
            const auto* junction = network.junction(node);
            if (junction && junction->selected != selected && network.throw_junction(node, selected)) ++thrown;
        }
        if (const auto error = train_->load_game(parts["trains"]); !error.empty()) return error;
        if (activity_ && parts.contains("activity")) activity_->load(parts["activity"]);
        if (const auto cycle = header.find("weather_cycle"); cycle != header.end()) {
            std::istringstream fields(cycle->second);
            double time = 0.0;
            std::uint64_t seed = 0;
            if (fields >> time >> seed) {
                auto& weather = weather_cycle();
                weather.time = time;
                weather.seed = seed;
                weather.forced.reset();
                weather.shown_valid = false;
                weather_started_ = true;
            }
        }
        saved_clock_ = SavedClock{.time_of_day = std::atof(header["time"].c_str()),
                                  .season = std::atoi(header["season"].c_str()),
                                  .weather = std::atoi(header["weather"].c_str()),
                                  .storm = header["storm"] == "1"};
        std::cout << "saved game: " << file.string() << " put back (" << thrown << " junctions thrown)" << std::endl;
        return {};
    }
    [[nodiscard]] std::optional<SavedClock> take_saved_clock() override { return std::exchange(saved_clock_, std::nullopt); }
    void close_activity_message() override {
        if (activity_) activity_->close_message();
    }
    void report_activity() override {
        if (!activity_) return;
        const auto* menu = activity_->activity().menu(activity_->main_menu());
        if (!menu || menu->items.empty() || reported_.contains(menu->id)) return;
        reported_.insert(menu->id);
        std::cout << "activity: reported " << menu->name << ": " << menu->items.front().label << std::endl;
        activity_->choose(menu->id, menu->items.front().id);
    }
    // A server's pause; a scenario's modal message, till its OK.
    [[nodiscard]] bool paused() const override { return (net_ && net_->paused()) || (activity_ && activity_->holds()); }
    [[nodiscard]] bool wants_close() const override { return leave_; }
    [[nodiscard]] int close_code() const override { return leave_ ? exit_code::server_gone : exit_code::success; }
    [[nodiscard]] SceneLoading loading() const override { return loading_; }
    // The Discord profile: what he does (loading, at the console, on foot,
    // driving which engine, looking round), and the route with the kind of
    // game (a scenario's name; a network game: nothing of the server's).
    [[nodiscard]] std::optional<ScenePresence> presence() const override {
        ScenePresence presence;
        const std::string route(route_.filename());
        const auto engine = train_ ? train_->driven_name() : std::string{};
        if (!loading_.done) {
            presence.details = tr("Loading");
        } else if (content_.console()) {
            presence.details = tr("At the dispatcher's console");
        } else if (train_ && train_->on_foot()) {
            presence.details = tr("On foot");
        } else if (train_ && train_->in_passenger_seat()) {
            presence.details = tr("Rides as a passenger");
        } else if (!engine.empty()) {
            presence.details = trf("Drives %s", engine.c_str());
        } else {
            presence.details = tr("Looks round the route");
        }
        if (content_.connection()) {
            presence.state = trf("%s: a network game", route.c_str());
            // The server lets the others join from Discord: its address (or
            // ours, not this computer's own), the party by it.
            if (const auto join = net_ && !content_.console() ? net_->discord_join() : std::nullopt) {
                std::string address = join->address.empty() ? content_.server_address() : join->address;
                const bool local = address.starts_with("127.") || address.starts_with("localhost") || address.starts_with("[::1]") ||
                                   address.starts_with("::1");
                if (!address.empty() && !local) {
                    if (address.find(':') == std::string::npos) address += ":800";
                    std::uint32_t hash = 2166136261u;
                    for (const auto c : address) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
                    char party[32];
                    std::snprintf(party, sizeof party, "openrts-%08x", hash);
                    // The server's count of its players and its places
                    // (none: no limit, the party shown without them).
                    presence.party_id = party;
                    presence.party_max = join->places;
                    presence.party_size = (std::max)(join->players, 1);
                    presence.join_secret = "openrts1 " + address;
                }
            }
        } else if (content_.activity() && !content_.activity()->name.empty()) {
            presence.state = trf("%s: scenario \"%s\"", route.c_str(), content_.activity()->name.c_str());
        } else {
            presence.state = trf("Route %s", route.c_str());
        }
        return presence;
    }

    [[nodiscard]] std::string world_report() const override {
        std::string report = "water " + std::to_string(terrain_ ? terrain_->water_quads() : 0) + " quads";
        if (terrain_ && terrain_->draws_native_water()) {
            report += " (openRTS water: " + std::to_string(terrain_->water_surface_draws()) + " tiles drawn, sky probe "
                + (sky_renderer_ && sky_renderer_->reflection_probe() ? "on" : "off") + ")";
        }
        std::size_t forests = 0, crossings = 0, pickups = 0;
        for (const auto& instance : streamer_->visible_instances()) {
            forests += instance.kind == formats::WorldObjectKind::forest ? 1 : 0;
            crossings += instance.kind == formats::WorldObjectKind::level_crossing ? 1 : 0;
            pickups += instance.kind == formats::WorldObjectKind::pickup ? 1 : 0;
        }
        const auto trees = renderer::forest_tree_counts();
        report += ", forests " + std::to_string(forests) + " (trees " + std::to_string(trees.planted) + ", kept off tracks and roads "
            + std::to_string(trees.cleared) + "), level crossings " + std::to_string(crossings) + " ("
            + std::to_string(crossings_ ? crossings_->closed() : 0) + " closed), pickups " + std::to_string(pickups) + ", car spawners "
            + std::to_string(roads_ ? roads_->spawners() : 0) + ", cars " + std::to_string(roads_ ? roads_->cars() : 0) + ", "
            + (precipitation_ && precipitation_->snow() ? "snowflakes " : "raindrops ") + std::to_string(precipitation_ ? precipitation_->drops() : 0)
            + ", smoke puffs " + std::to_string(smoke_ ? smoke_->particles() : 0);
        const auto streamed = streamer_->stats();
        report += "; terrain tiles " + std::to_string(terrain_ ? terrain_->loaded_tiles() : 0) + ", world tiles " + std::to_string(streamed.loaded_tiles) + " of "
            + std::to_string(streamed.requested_tiles) + ", instances " + std::to_string(streamed.visible_instances);
        if (roads_) report += std::string(1, '\n') + "road traffic" + roads_->report(crossings_.get());
        if (train_ && train_->placed()) report += std::string(1, '\n') + "trains: " + train_->systems_report();
        return report;
    }
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> cab_switch_state(const std::uint32_t id) const override {
        return train_ ? train_->cab_switch_state(id) : std::nullopt;
    }
    void set_cab_switch(const std::uint32_t id, const std::uint32_t state) override {
        if (train_) train_->set_cab_switch(id, state);
    }
    [[nodiscard]] std::optional<glm::mat4> train_car_transform(const std::size_t car) const override {
        return train_ && train_->placed() ? train_->car_transform(car) : std::nullopt;
    }
    void enter_cab() override {
        if (train_ && train_->placed()) train_->enter_cab();
    }
    void choose_car_menu_item(const int id) override {
        if (train_) train_->choose_car_menu_item(id);
    }
    void uncouple_driven(const std::size_t index) override {
        if (train_) train_->uncouple_driven(index);
    }
    void couple_driven(const bool front) override {
        if (train_) train_->couple_driven(front);
    }
    void drive_unit(const std::size_t index) override {
        if (train_) train_->drive_unit(index);
    }
    void stand_at_passage(const std::size_t index) override {
        if (train_) train_->stand_at_passage(index);
    }
    void stand_at_hand_switch() override {
        if (train_) train_->stand_at_hand_switch();
    }
    void report_hand_switch() const override {
        if (train_) train_->report_hand_switch();
    }
    void stand_at_cab_door(const std::size_t index) override {
        if (train_) train_->stand_at_cab_door(index);
    }
    [[nodiscard]] std::string car_menu_report() const override { return train_ ? train_->car_menu_report() : std::string{}; }
    [[nodiscard]] bool escape_taken() const override {
        return (train_ && train_->car_menu_open()) || (track_control_ && track_control_->is_open() && track_control_->placing());
    }
    [[nodiscard]] std::string inspector_report() const override { return inspector_.report(inspector_world()); }
    [[nodiscard]] std::string tape_report() const override { return tape_view_.report(); }
    [[nodiscard]] std::string timetable_report() const override { return train_ ? train_->timetable_report() : std::string(); }
    [[nodiscard]] std::optional<renderer::ShadowFocus> shadow_focus() const override {
        return train_ ? train_->cab_shadow_focus() : std::nullopt;
    }
    [[nodiscard]] std::optional<renderer::LightShaftRoom> light_shaft_room() const override {
        return train_ ? train_->light_shaft_room() : std::nullopt;
    }
    // In flight the lamps' shadows go where the camera looks: along the view
    // to the ground (up to 200 m; looking up or level, 40 m), the sphere over
    // that stretch from the camera, bigger the farther. Low over a station it
    // takes the near lamps and the far ones; from above it lies on the ground.
    // In the cab: the cab's own focus.
    [[nodiscard]] std::optional<renderer::ShadowFocus> lamp_shadow_focus(const ViewerCamera& camera) const override {
        if (!train_ || train_->cab_shadow_focus()) {
            lamp_focus_.reset();
            return std::nullopt;
        }
        const glm::vec3 eye = camera.eye();
        const glm::vec3 forward = camera.forward();
        // How far the view reaches: to the ground it meets within 200 m
        // looking down, 40 m looking up, the two blended across the horizon
        // (until 2026-11-21 it jumped from 200 to 40 there, the sphere with
        // it, and a lamp's shadow came and went as the camera tipped a hair,
        // testers).
        float reach = 40.0f;
        if (forward.y < 0.05f) {
            float down = 200.0f;
            for (float t = 2.0f; t <= 200.0f; t += (std::max)(2.0f, t * 0.03f)) {
                const glm::vec3 at = eye + forward * t;
                if (const auto ground = train_->ground_at(at); ground && at.y <= *ground) {
                    down = t;
                    break;
                }
            }
            const float up = std::clamp((forward.y + 0.05f) / 0.1f, 0.0f, 1.0f);
            reach = down + (40.0f - down) * up;
        }
        renderer::ShadowFocus target;
        target.centre = eye + forward * (0.6f * reach);
        target.radius = (std::max)(25.0f, 0.5f * reach + 15.0f);
        // Eased over a quarter of a second: turning the head moves the
        // sphere the lamps' shadows cover, it does not jump; a camera put
        // somewhere else takes it at once.
        const auto now = std::chrono::steady_clock::now();
        const float dt = lamp_focus_clock_ ? std::chrono::duration<float>(now - *lamp_focus_clock_).count() : 0.0f;
        lamp_focus_clock_ = now;
        if (!lamp_focus_ || glm::distance(lamp_focus_->centre, target.centre) > 2.0f * target.radius) {
            lamp_focus_ = target;
        } else {
            const float k = 1.0f - std::exp(-std::clamp(dt, 0.0f, 0.5f) / 0.25f);
            lamp_focus_->centre += (target.centre - lamp_focus_->centre) * k;
            lamp_focus_->radius += (target.radius - lamp_focus_->radius) * k;
        }
        return lamp_focus_;
    }
    // The lamps' shadows' sphere as it eases (lamp_shadow_focus).
    mutable std::optional<renderer::ShadowFocus> lamp_focus_;
    mutable std::optional<std::chrono::steady_clock::time_point> lamp_focus_clock_;

    void draw_overlay([[maybe_unused]] const ViewerCamera& camera) const override {
#ifdef OPENRTS_HAS_IMGUI
        const auto position = renderer::route_position(camera.eye(), origin_);
        ImGui::Text(tr("Local XYZ: %.1f, %.1f, %.1f"), position.x, position.y, position.z);
        ImGui::Text(tr("Tile X/Z: %d, %d"), position.tile_x, position.tile_z);
        if (sky_state_) {
            ImGui::Text(tr("Sky: %s%s"), environment_name_.c_str(), sky_state_->night ? " (night)" : "");
            if (!sky_state_->satellites.empty() && sky_state_->satellites.front().visible) {
                const auto& sun = sky_state_->satellites.front();
                ImGui::Text(tr("Sun: azimuth %.1f, elevation %.1f"), sun.azimuth_degrees, sun.elevation_degrees);
            }
        } else {
            ImGui::TextUnformatted(tr("Sky: no environment"));
        }
        if (terrain_) ImGui::Text(tr("Terrain: %zu tiles, %zu loading"), terrain_->loaded_tiles(), terrain_->pending_jobs());
        ImGui::Text(tr("Lights: %zu scenery lamps lit nearby, %zu in the rings, %zu in the nearest one (of 9)"), scenery_lamps_lit_,
                    light_rings_.placed(), light_rings_.lights[0].size());
        ImGui::Checkbox(tr("Distance and frustum culling"), &culling_);
        // The world's extras (remembered): road traffic, rain and snow, diesel smoke.
        if (ImGui::Checkbox(tr("Cars on the roads"), &effects_.traffic)) remember("world_traffic", effects_.traffic ? "1" : "0");
        ImGui::SameLine();
        if (ImGui::Checkbox(tr("Rain and snow"), &effects_.precipitation)) remember("world_precipitation", effects_.precipitation ? "1" : "0");
        ImGui::SameLine();
        if (ImGui::Checkbox(tr("Diesel smoke"), &effects_.smoke)) remember("world_smoke", effects_.smoke ? "1" : "0");
        ImGui::TextDisabled(tr("Water %zu patches, cars %zu, %s %zu, smoke puffs %zu"), terrain_ ? terrain_->water_quads() : std::size_t{0},
                            roads_ ? roads_->cars() : std::size_t{0}, precipitation_ && precipitation_->snow() ? tr("snowflakes") : tr("raindrops"),
                            precipitation_ ? precipitation_->drops() : std::size_t{0}, smoke_ ? smoke_->particles() : std::size_t{0});

        const auto stream = streamer_->stats();
        const auto models = library_->stats();
        ImGui::Text(tr("Tiles: %zu/%zu  |  Instances: %zu"), stream.loaded_tiles, stream.requested_tiles,
                    stream.visible_instances);
        ImGui::Text(tr("Drawn: %zu within %.0f m"), drawn_instances_, draw_distance);
        if (world_sounds_) {
            const auto sounds = world_sounds_->stats();
            ImGui::Text(tr("World sounds: %zu sources (%zu active, %zu voices), %zu scripts, %zu clips"), sounds.sources,
                        sounds.active, sounds.voices, sounds.scripts, sounds.clips);
        }
        ImGui::Text(tr("Models: %zu GPU, %zu CPU queued, %zu pending"), models.models, models.queued_uploads,
                    stream.pending_model_assets);
        ImGui::Checkbox(tr("Labels: junctions and signals (L)"), &labels_);
        // Walking is the player's (with a train or without): on foot from
        // where the camera is; off, the free camera.
        if (train_) {
            bool walking = train_->walking();
            if (ImGui::Checkbox(tr(wasd_walking() ? "On foot (arrows or W A S D walk, Enter at a cab door gets in)" : "On foot (arrows walk, Enter at a cab door gets in)"), &walking)) {
                train_->set_walking(walking, last_eye_);
            }
        }
        draw_scene_marks();
        {
            const auto world = inspector_world();
            inspector_.draw(world);
            inspector_.add_selection_lines(world, pending_lines_);
        }
        if (train_) train_->add_placement_lines(pending_lines_);
        if (track_control_) {
            bool open = track_control_->is_open();
            if (ImGui::Checkbox(tr("Track control (Ctrl+Alt+F7)"), &open)) track_control_->toggle();
            ImGui::SameLine();
        }
        ImGui::Checkbox(tr("Names (F7)"), &place_names_);
        ImGui::SameLine();
        ImGui::Checkbox(tr("Signal names (Ctrl+F7)"), &signal_names_);
        {
            bool tape_open = tape_view_.is_open();
            if (ImGui::Checkbox(tr("Speed recorder tape (Ctrl+F9)"), &tape_open)) tape_view_.toggle();
        }
        if (track_) track_->draw_overlay();
        if (train_) train_->draw_overlay();
        if (signals_) signals_->draw_overlay();
        if (track_control_ && train_) {
            track_control_->set_viewer(camera.eye(), camera.forward(), origin_);
            track_control_->draw(*train_, signals_ ? signals_->system() : nullptr);
        }
        if (net_ && net_->down()) draw_server_gone();
        if (net_ && net_->console() && train_) console_panel_.draw(*net_, *train_, track_control_.get(), *content_.vfs());
        if (net_ && !net_->console() && train_) {
            if (draw_players_section(*net_, *train_, camera.eye(), origin_, player_names_)) {
                remember("player_names", player_names_ ? "1" : "0");
            }
            draw_helper_windows(*net_);
        }
        {
            // The tape window: the file rolling now, and the RTS installation's tapes.
            static const std::filesystem::path none;
            tape_view_.draw(train_ && train_->tape_rolling() ? train_->tape_path() : none, content_.root());
        }
        if (train_) train_->draw_hud();
        if (activity_) activity_->draw();
        content_.draw_overlay(library_.get());
#endif
    }

private:
    static constexpr int tile_radius = 1;

#ifdef OPENRTS_HAS_IMGUI
    // The server of the network game is gone (it stopped, or the connection
    // broke): a window says so in the middle of the screen; the game goes
    // back to the menu in 15 s, or at once, or stays to look round the world.
    void draw_server_gone() const {
        const double now = ImGui::GetTime();
        if (!server_gone_at_) {
            server_gone_at_ = now;
            std::cout << "status: server_gone" << std::endl;
        }
        if (stay_) return;
        constexpr double wait = 15.0;
        const double left = wait - (now - *server_gone_at_);
        if (left <= 0.0) leave_ = true;
        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin(tr("The server is gone###server_gone"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(tr("The connection to the server is lost: it stopped, or the network broke."));
        ImGui::TextUnformatted(tr("The network game is over."));
        ImGui::Spacing();
        if (ImGui::Button(trf("To the menu (in %.0f s)", (std::max)(0.0, std::ceil(left))).c_str())) leave_ = true;
        ImGui::SameLine();
        if (ImGui::Button(tr("Stay and look round"))) stay_ = true;
        ImGui::End();
    }
#endif
    mutable std::optional<double> server_gone_at_;
    mutable bool stay_ = false;
    mutable bool leave_ = false;

    static constexpr int terrain_radius = 2;
    // The nearest the loaded terrain ends (m): its tiles round the eye's.
    static constexpr float land_end = static_cast<float>(terrain_radius) * 2048.0f;

    // Terrain height at the route start (the scene origin), 0 without terrain.
    [[nodiscard]] float start_ground_height() const {
        const auto tile = formats::load_terrain_tile(*content_.vfs(), child(route_, "TILES"), origin_.tile_x, origin_.tile_z);
        if (!tile.ok()) return 0.0f;
        const auto half_extent = static_cast<double>(tile.sample_count) * tile.sample_size * 0.5;
        const auto last = static_cast<double>(tile.sample_count - 1);
        const auto column = std::clamp(std::round((origin_.x + half_extent) / tile.sample_size), 0.0, last);
        const auto row = std::clamp(std::round((half_extent - origin_.z) / tile.sample_size), 0.0, last);
        return tile.height(static_cast<std::uint32_t>(row), static_cast<std::uint32_t>(column)) - static_cast<float>(origin_.y);
    }

    // Switches the environment file and terrain textures when the season or
    // weather changes, as RTS selects them from the .trk Environment block.
    void apply_conditions(const renderer::rts::Season season, const renderer::rts::Weather weather) {
        if (conditions_ && conditions_->first == season && conditions_->second == weather) return;
        conditions_ = std::pair{season, weather};
        if (terrain_) terrain_->set_conditions(season, weather);

        const auto* entry = find_environment(renderer::rts::environment_condition(season, weather));
        if (!entry) entry = environments_.empty() ? nullptr : &environments_.front();
        environment_name_ = entry ? entry->file : std::string{};
        environment_ = nullptr;
        sky_simulation_.reset();
        sky_state_.reset();
        if (!entry) return;

        const auto envfiles = child(route_, "ENVFILES");
        auto [it, inserted] = loaded_environments_.try_emplace(entry->file);
        if (inserted) {
            it->second = formats::load_environment(*content_.vfs(), child(envfiles, entry->file));
            if (!it->second.ok()) std::cout << "environment '" << entry->file << "' not loaded: " << it->second.error << '\n';
        }
        if (!it->second.ok()) return;

        environment_ = &it->second;
        sky_simulation_.emplace(*environment_);
        if (terrain_) {
            // The environment's water over the water patches (RTS 0x4A2144).
            terrain_->set_water(environment_->water ? &*environment_->water : nullptr,
                                {child(envfiles, "TEXTURES"), *core::VirtualPath::parse("GLOBAL/TEXTURES")});
        }
        if (precipitation_) {
            precipitation_->set_environment(environment_->precipitation ? &*environment_->precipitation : nullptr, environment_->wind,
                                            {child(envfiles, "TEXTURES"), *core::VirtualPath::parse("GLOBAL/TEXTURES")});
        }
        if (smoke_) smoke_->set_wind(environment_->wind);
        if (sky_renderer_) {
            sky_renderer_->load(*environment_, *content_.vfs(),
                                {child(envfiles, "TEXTURES"), *core::VirtualPath::parse("GLOBAL/TEXTURES")});
        }
    }

    [[nodiscard]] const formats::RouteEnvironment* find_environment(const std::string_view condition) const {
        const auto it = std::find_if(environments_.begin(), environments_.end(), [&](const auto& entry) {
            return entry.condition.size() == condition.size()
                && std::equal(entry.condition.begin(), entry.condition.end(), condition.begin(), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                   });
        });
        return it == environments_.end() ? nullptr : &*it;
    }
    static constexpr float draw_distance = 1800.0f;

    // Horizontal-only distance test, matching a ground-level view range.
    [[nodiscard]] static bool beyond_draw_distance(const glm::vec3& center, const float radius, const glm::vec3& eye) {
        const auto dx = center.x - eye.x;
        const auto dz = center.z - eye.z;
        const auto limit = draw_distance + radius;
        return dx * dx + dz * dz > limit * limit;
    }

    ContentView content_;
    core::VirtualPath route_;
    formats::RoutePosition origin_;
    std::vector<formats::RouteEnvironment> environments_;
    std::map<std::string, formats::EnvironmentDefinition> loaded_environments_;
    std::optional<std::pair<renderer::rts::Season, renderer::rts::Weather>> conditions_;
    const formats::EnvironmentDefinition* environment_ = nullptr;
    std::string environment_name_;
    std::unique_ptr<renderer::TerrainRenderer> terrain_;
    audio::AudioEngine* audio_ = nullptr;
    std::unique_ptr<audio::WorldSounds> world_sounds_;
    // Debug switch in the overlay; the overlay callback is const.
    mutable bool culling_ = true;

    // Floating origin: positions are floats relative to origin_, which at
    // 100+ km (Kuban: 132 km from RouteStart) step by centimetres and make
    // everything shake. When the camera is more than 1 km from the origin,
    // the origin moves under it (whole metres, same height) and every part
    // of the scene follows.
    // The rain or the snow heard (RTS 0x55859C: the environment's sound, else
    // rain_ex.sms / snow_ex.sms), while it falls.
    // In the cab RTS plays the outside sound too, through the room's doors
    // and windows: rain_in / snow_in list only CabCam and PassengerCam, which
    // RTS starts for the player's train's objects, never for the weather's
    // (0x44AB7C), so they stay silent (docs/sound.md).
    void update_weather_sound(const float elapsed, const glm::vec3& eye, const float gain) {
        const bool falling = audio_ && precipitation_ && precipitation_->drops() > 0;
        const bool snow = precipitation_ && precipitation_->snow();
        if (weather_sound_ && (!falling || snow != weather_snow_)) {
            weather_sound_->stop(*audio_);
            weather_sound_.reset();
        }
        if (!falling) return;
        if (!weather_sound_) {
            weather_snow_ = snow;
            const std::string name = snow ? "snow_ex.sms" : "rain_ex.sms";
            std::vector<core::VirtualPath> candidates{child(child(route_, "SOUND"), name)};
            if (const auto global = core::VirtualPath::parse("SOUND/" + name)) candidates.push_back(*global);
            for (const auto& candidate : candidates) {
                if (!content_.vfs()->exists(candidate)) continue;
                auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(*content_.vfs(), candidate));
                const auto* group = script->ok() ? script->select_group(formats::SoundOwner::environment) : nullptr;
                if (!group) continue;
                if (!weather_bank_) weather_bank_ = std::make_unique<audio::SoundBank>(content_.vfs());
                auto lookup = [this, candidate](const std::string& file) -> std::shared_ptr<const audio::AudioClip> {
                    const auto found = audio::find_sound_file(*content_.vfs(), candidate, file);
                    return found ? weather_bank_->get(*found) : nullptr;
                };
                weather_script_ = std::move(script);
                weather_sound_ = std::make_unique<audio::SoundInstance>(weather_script_, *group, formats::SoundOwner::environment,
                                                                        std::move(lookup), 7u);
                break;
            }
        }
        if (!weather_sound_) return;
        audio::SoundInputs inputs{.camera = audio::SoundCamera::external, .position = eye};
        // The dynamic weather's light rain is quieter, its heavy one louder.
        inputs.gain = gain * (dynamic_weather_ ? std::clamp(0.3f + 0.8f * weather_cycle().shown.precipitation, 0.3f, 1.1f) : 1.0f);
        weather_sound_->update(elapsed, inputs, *audio_);
    }

    void rebase_near(ViewerCamera& camera) {
        constexpr float rebase_distance = 1000.0f;
        const auto eye = camera.eye();
        if (std::abs(eye.x) < rebase_distance && std::abs(eye.z) < rebase_distance) return;
        const glm::vec3 shift{std::round(eye.x), 0.0f, std::round(eye.z)};
        origin_ = renderer::route_position(shift, origin_);
        camera.position -= shift;
        camera.target -= shift;
        if (streamer_) streamer_->set_origin(origin_);
        if (terrain_) terrain_->set_origin(origin_);
        if (track_) track_->set_origin(origin_);
        if (train_) train_->set_origin(origin_);
        if (signals_) signals_->set_origin(origin_);
        if (roads_) roads_->set_origin(origin_);
        if (smoke_) smoke_->set_origin_shift(shift);
        if (world_sounds_) world_sounds_->set_origin(origin_);
    }
    mutable bool labels_ = false;
    glm::mat4 last_view_projection_{1.0f};
    glm::vec3 last_eye_{};
    float last_viewport_height_ = 0.0f; // pixels
    float last_vertical_fov_ = 0.0f;    // radians

    // RTS turns a dragged control's cursor move into frames per pixel of its
    // own window (0x437E64): the same move looks the same in RTS when it is
    // the same angle of view. This many RTS pixels make one of ours: RTS's
    // window height over twice the tangent of half its vertical FOV (its
    // config: FOV, WindowHeight; FullScreen: our height), over ours.
    [[nodiscard]] float rts_pixels_per_pixel() const {
        if (last_viewport_height_ <= 0.0f || last_vertical_fov_ <= 0.0f) return 1.0f;
        const auto& config = content_.rts_config();
        const float fov = rts_config_value(config, "fov").value_or(45.0f) * 3.14159265f / 180.0f;
        const bool fullscreen = rts_config_value(config, "fullscreen").value_or(1.0f) != 0.0f;
        const float height = fullscreen ? last_viewport_height_ : rts_config_value(config, "windowheight").value_or(768.0f);
        if (fov <= 0.0f || height <= 0.0f) return 1.0f;
        const float rts = height / (2.0f * std::tan(fov * 0.5f));
        const float ours = last_viewport_height_ / (2.0f * std::tan(last_vertical_fov_ * 0.5f));
        return ours > 0.0f ? rts / ours : 1.0f;
    }
    // Helper lines in the scene pass, and labels hidden behind the scene.
    std::unique_ptr<renderer::WideLineRenderer> wide_lines_;
    std::unique_ptr<renderer::ScreenQuadRenderer> screens_;
    std::unique_ptr<renderer::GlowSpriteRenderer> train_glows_; // the train's lamps
    std::unordered_map<std::string, std::uint32_t> train_glow_textures_;
    // This frame's further lights of the shaders: the train's beams and the
    // scenery's lamps, the nearest nine (RTS Shader.NumLights 10, the sun first).
    renderer::LightRings light_rings_;

    // A scenery lamp (ESD_LightSources) as a light of lights.glsl: light space
    // z towards PointsTo, x along the shape's x; a cone (LIGHT_CONE) spreads
    // Radius at the PointsTo plane, a sphere or a box (neither) takes Radius
    // as its half sizes.
    [[nodiscard]] static renderer::ModelExtraLight scenery_light(const formats::ShapeLight& lamp, const glm::mat4& transform) {
        const glm::vec3 from{lamp.position[0], lamp.position[1], lamp.position[2]};
        glm::vec3 to{lamp.points_to[0], lamp.points_to[1], lamp.points_to[2]};
        if (glm::distance(from, to) < 1e-3f) to = from - glm::vec3{0.0f, 1.0f, 0.0f};
        const float reach = glm::distance(from, to);
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
        const glm::mat4 world = transform * local;
        renderer::ModelExtraLight light;
        const bool cone = lamp.flags.find("light_cone") != std::string::npos;
        light.kind = cone ? 4.0f : lamp.flags.find("light_sphere") != std::string::npos ? 2.0f : 3.0f;
        const float rx = (std::max)(lamp.radius[0], 0.01f);
        const float rz = (std::max)(lamp.radius[1] > 0.0f ? lamp.radius[1] : lamp.radius[0], 0.01f);
        light.size = cone ? glm::vec4{rx / reach, rz / reach, reach, lamp.source_radius} : glm::vec4{rx, rz, reach, lamp.source_radius};
        light.to_light = glm::inverse(world);
        light.direction = glm::normalize(glm::vec3{world[2]});
        light.spot_exponent = std::clamp(lamp.spot_exponent, 0.0f, 1.0f) * 128.0f;
        light.attenuation = glm::vec4{lamp.attenuation[0], lamp.attenuation[1], lamp.attenuation[2], 0.0f};
        const std::uint32_t c = lamp.colour;
        light.diffuse = glm::vec4{static_cast<float>((c >> 16) & 0xFFu) / 255.0f, static_cast<float>((c >> 8) & 0xFFu) / 255.0f,
                                  static_cast<float>(c & 0xFFu) / 255.0f, 1.0f};
        return light;
    }

    // Whether a scenery lamp burns (rts1.exe 0x4B0284): RUNNING burns;
    // NIGHT_AUTO burns at night or, with AutoWhenWorldLit, while the world is
    // at most that lit (SkyState::world_lit); NIGHT_ONLY never by day; a lamp
    // with none of them does not burn.
    [[nodiscard]] static bool scenery_lamp_lit(const formats::ShapeLight& lamp, const bool night, const float world_lit) {
        const auto has = [&](const char* flag) { return lamp.flags.find(flag) != std::string::npos; };
        bool lit = has("running");
        if (has("night_auto")) lit = lamp.auto_when_world_lit > 0.0f ? world_lit <= lamp.auto_when_world_lit : night;
        if (has("night_only") && !night) lit = false;
        return lit;
    }

    // The sphere a scenery lamp's light is taken to fill (0x408574): its
    // larger Radius plus LightFade (sic), times the attenuation's start and
    // length shares plus 0.1, at least 1.1.
    [[nodiscard]] static float scenery_lamp_reach(const formats::ShapeLight& lamp) {
        const float rx = lamp.radius[0] != 0.0f ? std::abs(lamp.radius[0]) : 1.0f;
        const float rz = lamp.radius[1] != 0.0f ? std::abs(lamp.radius[1]) : rx;
        return ((std::max)(rx, rz) + lamp.light_fade) * (std::max)(1.1f, lamp.attenuation[1] + lamp.attenuation[2] + 0.1f);
    }

    // The train's beams, then the lamps of the loaded scenery, into RTS's
    // rings of further lights (renderer::LightRings, rts1.exe 0x408574): a
    // scenery lamp takes part when its object is within MaxLightDistance
    // (ViewDistance without it) and its own Distance, it burns, and its reach
    // is in view (the last frame's).
    // A scenery lamp's fade (rts1.exe 0x4B0284, the record's +52 level and
    // its fading bits): the level moves by the frame's milliseconds over
    // LightFade's first value (the main loop's time step, 8..1000 ms, so the
    // fade takes LightFade milliseconds), up while the lamp burns, down while
    // it does not; without LightFade it follows at once. The record is the
    // shape's, shared by its instances, each of which steps it again.
    struct LampFade {
        float level = 0.0f;
        std::uint8_t state = 0; // 1 fading in, 2 fading out, 4 fully on (RTS's bits)
    };
    std::unordered_map<std::string, std::vector<LampFade>> lamp_fades_;

    static void step_lamp(LampFade& fade, const bool lit, const float fade_time, const float step_ms) {
        if (fade_time == 0.0f) {
            fade.level = lit ? 1.0f : 0.0f;
            return;
        }
        if (lit) {
            if ((fade.state & 4u) == 0) fade.state |= 1u;
            fade.state &= static_cast<std::uint8_t>(~2u);
        } else {
            if ((fade.state & 4u) != 0) fade.state |= 2u;
            fade.state &= static_cast<std::uint8_t>(~1u);
        }
        if ((fade.state & 1u) != 0) {
            fade.level += step_ms / fade_time;
            if (fade.level >= 1.0f) {
                fade.level = 1.0f;
                fade.state = static_cast<std::uint8_t>((fade.state & ~3u) | 4u);
            }
        } else if ((fade.state & 2u) != 0) {
            fade.level -= step_ms / fade_time;
            if (fade.level <= 0.0f) {
                fade.level = 0.0f;
                fade.state = static_cast<std::uint8_t>(fade.state & ~6u);
            }
        }
    }

    void collect_lights(const glm::vec3& eye, const float elapsed) {
        if (view_distance_ <= 0.0f) {
            view_distance_ = rts_config_value(content_.rts_config(), "viewdistance").value_or(1000.0f);
            max_light_distance_ = rts_config_value(content_.rts_config(), "maxlightdistance").value_or(0.0f);
        }
        const float rts_limit = max_light_distance_ > 0.0f ? max_light_distance_ : view_distance_;
        // The experimental path's light range (panel): farther lamps, each
        // lamp's own Distance stretched in the same proportion.
        const bool wider = experimental_ && renderer::light_range() > 0.0f && rts_limit > 0.0f;
        const float object_limit = wider ? renderer::light_range() : rts_limit;
        const float distance_scale = wider ? renderer::light_range() / rts_limit : 1.0f;
        light_rings_.reset(eye, max_light_distance_, view_distance_);
        const auto frustum = renderer::Frustum::from_view_projection(last_view_projection_);
        if (flashlight_) {
            // RTS's flashlight (0x409A2C, Num2; its record 0x5E03D4, set at
            // 0x405F15, flashlight.* settings at 0x4070FB): a spotlight at the
            // eye along the view, the first light of the frame.
            renderer::ModelExtraLight light;
            light.diffuse = glm::vec4{0.643f, 0.4745f, 0.1725f, 1.0f};
            glm::vec3 z = glm::length(flashlight_forward_) > 1e-4f ? glm::normalize(flashlight_forward_) : glm::vec3{0.0f, 0.0f, -1.0f};
            glm::vec3 x = glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, z);
            x = glm::length(x) > 1e-4f ? glm::normalize(x) : glm::vec3{1.0f, 0.0f, 0.0f};
            const glm::vec3 y = glm::cross(z, x);
            glm::mat4 world{1.0f};
            world[0] = glm::vec4{x, 0.0f};
            world[1] = glm::vec4{y, 0.0f};
            world[2] = glm::vec4{z, 0.0f};
            world[3] = glm::vec4{eye, 1.0f};
            light.to_light = glm::inverse(world);
            light.direction = z;
            light.size = glm::vec4{0.436f, 0.233f, 50.0f, 0.0f};
            light.kind = 1.0f;
            light.spot_exponent = 90.0f;
            light.attenuation = glm::vec4{2.0f, 0.2f, 0.8f, 0.0f};
            light_rings_.add(light, eye, light.size.z);
        }
        if (train_) {
            // The experimental path stretches their reach (panel); the shape
            // of the beam and its fall-off keep their proportions.
            const float reach = experimental_ ? (std::max)(renderer::headlight_reach(), 0.1f) : 1.0f;
            for (auto beam : train_->light_beams()) {
                beam.size.z *= reach;
                light_rings_.add(beam, glm::vec3{glm::inverse(beam.to_light)[3]}, (std::max)(beam.size.z, 1.0f));
            }
        }
        const bool night = sky_state_ && sky_state_->night;
        const float world_lit = sky_state_ ? sky_state_->world_lit : 1.0f;
        // RTS's frame step in milliseconds, as its main loop clamps it.
        const float step_ms = std::clamp(elapsed * 1000.0f, 8.0f, 1000.0f);
        std::size_t lamps = 0;
        if (streamer_ && library_) {
            const auto& visible = streamer_->visible_instances();
            for (std::size_t index = 0; index < visible.size(); ++index) {
                const auto& instance = visible[index];
                const auto* model = library_->find(instance.source_path);
                if (!model || model->lights().empty()) continue;
                const auto centre = glm::vec3{instance.transform * glm::vec4{model->bounds().center, 1.0f}};
                // The experimental path from the shape's near side, not its
                // middle: a platform 200 m long keeps its far lamps (each
                // lamp's Distance is then its own, below).
                const float scale = glm::length(glm::vec3{instance.transform[0]});
                const float object_distance = experimental_
                    ? (std::max)(glm::distance(eye, centre) - model->bounds().radius * scale, 0.0f)
                    : glm::distance(eye, centre);
                if (object_distance > object_limit) continue;
                auto& fades = lamp_fades_[instance.source_path.key()];
                fades.resize(model->lights().size());
                for (std::size_t k = 0; k < model->lights().size(); ++k) {
                    const auto& lamp = model->lights()[k];
                    if (lamp.distance > 0.0f && object_distance > lamp.distance * distance_scale) continue;
                    auto& fade = fades[k];
                    const bool lit = scenery_lamp_lit(lamp, night, world_lit);
                    step_lamp(fade, lit, lamp.light_fade, step_ms);
                    // Ringed while it burns or is still fading (0x408574).
                    if (!lit && (fade.state & 3u) == 0) continue;
                    const glm::vec3 at{instance.transform * glm::vec4{lamp.position[0], lamp.position[1], lamp.position[2], 1.0f}};
                    const float reach = scenery_lamp_reach(lamp);
                    // A row of lamps (LightArray): the RTS path takes it as
                    // one light whose nearest lamp lights each point, as RTS's
                    // shader does; its ring by the row's middle and length.
                    // The experimental path takes every lamp as a light of its
                    // own: its clusters and its shadows are each lamp's.
                    // Seen or not by the whole row (a platform's 200 m: from
                    // its far end the first lamp is out of view while the
                    // others light the platform), each lamp on its own on the
                    // experimental path, its Distance by its own place too.
                    const glm::vec3 step{instance.transform * glm::vec4{lamp.array_step[0], lamp.array_step[1], lamp.array_step[2], 0.0f}};
                    const auto row = lamp.array_count >= 2 ? lamp.array_count : 1u;
                    const float half_row = glm::length(step) * static_cast<float>(row - 1) * 0.5f;
                    const glm::vec3 middle = at + step * (static_cast<float>(row - 1) * 0.5f);
                    if (!frustum.intersects_sphere(middle, reach + half_row)) continue;
                    auto light = scenery_light(lamp, instance.transform);
                    light.owner = static_cast<std::uint32_t>(index + 1); // its instance's tag (submit)
                    if (fade.level < 1.0f) {
                        // RTS's packing (0x4AEBEC) scales red and alpha by the
                        // level, green by it twice and blue not at all; the
                        // experimental path scales the colour.
                        auto& d = light.diffuse;
                        if (experimental_) {
                            d *= fade.level;
                        } else {
                            d.r *= fade.level;
                            d.g *= fade.level * fade.level;
                            d.a *= fade.level;
                        }
                    }
                    if (row == 1) {
                        light_rings_.add(light, at, reach);
                    } else if (!experimental_) {
                        const glm::vec3 local_step{light.to_light * glm::vec4{step, 0.0f}};
                        light.array_count = row;
                        light.array_step = glm::vec4{local_step, glm::dot(local_step, local_step)};
                        light_rings_.add(light, middle, reach + half_row);
                    } else {
                        for (std::uint32_t n = 0; n < row; ++n) {
                            const glm::vec3 shift = step * static_cast<float>(n);
                            const glm::vec3 place = at + shift;
                            if (lamp.distance > 0.0f && glm::distance(eye, place) > lamp.distance * distance_scale) continue;
                            if (!frustum.intersects_sphere(place, reach)) continue;
                            auto one = light;
                            one.to_light = light.to_light * glm::translate(glm::mat4{1.0f}, -shift);
                            light_rings_.add(one, place, reach);
                        }
                    }
                    ++lamps;
                }
            }
        }
        scenery_lamps_lit_ = lamps;
    }
    std::size_t scenery_lamps_lit_ = 0;
    // submit(): each visible instance's model and whether it is drawn.
    struct Pick {
        enum Kind : std::uint8_t { skip, draw, shadow };
        renderer::RenderModel* model = nullptr;
        Kind kind = skip;
    };
    std::vector<Pick> picks_;
    std::vector<renderer::RenderModel*> model_cache_;
    std::uint64_t model_cache_generation_ = ~std::uint64_t{0};
    // The world's models no loaded tile around has placed for half a minute
    // leave the library (and the card), unless a train or the road traffic
    // uses them: they stayed for the whole run, so the memory grew all along
    // the line (testers 2026-11-18, a multiplayer event coming).
    static constexpr auto world_model_linger = std::chrono::seconds(30);
    std::chrono::steady_clock::time_point world_models_checked_{};
    std::optional<std::uint32_t> texture_limit_; // renderer::texture_size_limit() at the last update
    std::size_t world_models_let_go_ = 0;
    void let_go_world_models() {
        const auto now = std::chrono::steady_clock::now();
        if (!streamer_ || !library_ || now - world_models_checked_ < std::chrono::seconds(2)) return;
        world_models_checked_ = now;
        const auto released = streamer_->take_released_models(world_model_linger);
        if (released.empty()) return;
        const auto trains = train_ ? train_->shapes_in_use() : std::unordered_set<std::string>{};
        std::size_t let_go = 0;
        for (const auto& path : released) {
            const auto key = path.key();
            if (trains.contains(key) || (roads_ && roads_->holds_shape(key))) continue;
            library_->remove(path);
            ++let_go;
        }
        world_models_let_go_ += let_go;
        if (let_go > 0)
            std::cout << "world: " << let_go << " models of tiles left behind let go (" << world_models_let_go_ << " in all; the route keeps "
                      << streamer_->held_models() << ", the library " << library_->stats().models << ")" << std::endl;
    }
    bool experimental_ = false; // the renderer's shading path, from the last frame
    float max_light_distance_ = 0.0f; // RTS config MaxLightDistance, m (0: none)
    float view_distance_ = 0.0f;      // RTS config ViewDistance, m (RTS: 1000 without it)

    // A lamp's LightTexture for the glows (0: the built-in round glow).
    std::uint32_t glow_texture(const core::VirtualPath& path) {
        const auto it = train_glow_textures_.find(path.key());
        if (it != train_glow_textures_.end()) return it->second;
        std::uint32_t id = 0;
        const auto decoded = formats::decode_ace_rgba(*content_.vfs(), path);
        if (decoded.ok()) {
            const auto& image = *decoded.image;
            id = train_glows_->add_texture({.width = image.width, .height = image.height, .pixels = &image.pixels, .mips = &image.mips});
        }
        train_glow_textures_.emplace(path.key(), id);
        return id;
    }
    std::unordered_map<std::string, std::uint32_t> screen_textures_; // ACE path key (+ "|p") -> texture (0: failed)

    // A cab screen texture, decoded the first time it is asked for;
    // `premultiplied` as the supersampled screens blend it.
    std::uint32_t screen_texture(const core::VirtualPath& path, const bool premultiplied) {
        const auto key = premultiplied ? path.key() + "|p" : path.key();
        const auto it = screen_textures_.find(key);
        if (it != screen_textures_.end()) return it->second;
        std::uint32_t id = 0;
        const auto decoded = formats::decode_ace_rgba(*content_.vfs(), path);
        if (decoded.ok()) {
            const auto& image = *decoded.image;
            id = screens_->add_texture({.width = image.width, .height = image.height, .pixels = &image.pixels, .mips = &image.mips},
                                       premultiplied);
        } else {
            std::cout << "cab screen texture " << path.str() << ": " << decoded.error << '\n';
        }
        screen_textures_.emplace(key, id);
        return id;
    }
    std::unique_ptr<renderer::OcclusionProbe> probe_;
    mutable std::vector<renderer::WideLine> pending_lines_;
    // A request of the probe: its tag, the labels tested first, then this
    // many points of the sky.
    struct ProbeFrame {
        std::uint64_t tag = 0;
        std::vector<std::size_t> labels;
        std::size_t sky = 0;
        std::size_t sky_light = 0; // ways tested for the sky's light, two points each, after the sky's
        std::vector<std::uint8_t> sky_light_bins; // each way's bin round the eye (open_bins_)
        glm::vec3 eye{};                          // where the eye was
    };
    mutable std::deque<ProbeFrame> probe_frames_;
    mutable std::vector<renderer::OcclusionProbe::Point> label_probe_points_; // this frame's labels to test
    mutable std::vector<std::size_t> label_probe_keys_;
    mutable std::unordered_set<std::size_t> hidden_labels_;
    mutable std::uint64_t probe_tag_ = 0;
    static constexpr int sky_probe_columns = 6;
    static constexpr int sky_probe_rows = 4;
    double spawn_check_time_ = 0.0;
    bool spawn_check_done_ = false;
    void spawn_check(const SceneClock& clock, const ViewerCamera& camera) {
        static const std::string consist = [] {
#ifdef _WIN32
            char* value = nullptr;
            std::size_t size = 0;
            std::string text;
            if (_dupenv_s(&value, &size, "OPENRTS_SPAWN_CHECK") == 0 && value) text = value;
            std::free(value);
            return text;
#else
            const char* value = std::getenv("OPENRTS_SPAWN_CHECK");
            return std::string(value ? value : "");
#endif
        }();
        if (consist.empty() || !train_ || !track_ || !loading_.done) return;
        spawn_check_time_ += clock.elapsed;
        if (!spawn_check_done_ && spawn_check_time_ > 3.0) {
            spawn_check_done_ = true;
            const auto& network = track_->network();
            simulation::TrackLocation where{};
            for (std::uint32_t node = 1; node < network.node_count(); ++node) {
                const auto* n = network.node(node);
                if (n && n->kind == formats::TrackNodeKind::vector && n->length > 600.0) {
                    where = {node, n->length * 0.5, true};
                    break;
                }
            }
            const auto error = train_->spawn(consist, where, TrainView::Role::standing, std::nullopt, -1);
            std::cout << "spawn check: " << consist << " at node " << where.node << ": " << (error.empty() ? "put down" : error) << std::endl;
        }
        if (spawn_check_done_ && spawn_check_time_ < 9.5 && std::fmod(spawn_check_time_, 1.0) < clock.elapsed) {
            std::cout << "spawn check: " << spawn_check_time_ << " s: keyboard captured " << keyboard_captured() << ", on foot "
                      << engineer_on_foot() << ", walking " << train_->walking() << ", camera fly " << camera.fly << " at "
                      << camera.position.x << " " << camera.position.y << " " << camera.position.z << std::endl;
        }
    }
    // A thunderstorm this frame, and its lightning (update_lightning).
    bool storm_ = false;
    // The dynamic weather runs (weather_cycle.hpp: on, and not a network
    // game, whose weather is the server's); begun from the weather chosen.
    bool dynamic_weather_ = false;
    bool weather_started_ = false;
    std::uint32_t weather_serial_ = 0; // the server's weather last taken (NetSession::weather_serial)
    // The weather this frame from the dynamic weather: its clock on by the
    // game's, the mix shown eased towards the cycle's (a setting changed by
    // hand comes over some 20 game seconds, also with the clock stopped).
    // It starts from the weather chosen (the start menu's, a scenario's),
    // held a spell, then the cycle's.
    void apply_dynamic_weather(SceneClock& clock) {
        auto& cycle = weather_cycle();
        // OPENRTS_WEATHER_CHECK set: on for this run, 600 times as fast,
        // what it shows logged every game hour.
        static const bool check = [] {
#ifdef _WIN32
            char* value = nullptr;
            std::size_t size = 0;
            const bool set = _dupenv_s(&value, &size, "OPENRTS_WEATHER_CHECK") == 0 && value != nullptr;
            std::free(value);
            return set;
#else
            return std::getenv("OPENRTS_WEATHER_CHECK") != nullptr;
#endif
        }();
        if (check) cycle.settings.dynamic = true;
        // A network game: the server's weather (openrts_weather), one that
        // stands or its dynamic one, the same for everyone by its seed and
        // clock (2026-11-21: the server could not change it while it ran).
        if (net_ && net_->server_weather()) {
            const auto& w = *net_->server_weather();
            if (!w.dynamic) {
                dynamic_weather_ = false;
                cycle.shown_valid = false;
                weather_started_ = false;
                clock.weather = w.weather == 1 ? renderer::rts::Weather::snow
                              : w.weather >= 2 ? renderer::rts::Weather::rain
                                               : renderer::rts::Weather::clear;
                clock.storm = w.weather == 3;
                return;
            }
            cycle.settings.dynamic = true;
            cycle.settings.by_season = w.by_season != 0;
            cycle.settings.climate = {.cloudiness = w.cloudiness, .wet = w.wet, .storms = w.storms};
            cycle.settings.spell_minutes = w.spell_minutes;
            cycle.settings.change_minutes = w.change_minutes;
            cycle.seed = w.seed;
            cycle.forced.reset();
            if (net_->weather_serial() != weather_serial_) {
                weather_serial_ = net_->weather_serial();
                cycle.time = w.clock;
            }
            weather_started_ = true; // from the cycle's own, not a weather chosen here
            dynamic_weather_ = true;
        } else {
            dynamic_weather_ = cycle.settings.dynamic && !content_.connection();
        }
        if (!dynamic_weather_) {
            cycle.shown_valid = false;
            weather_started_ = false;
            return;
        }
        if (!weather_started_) {
            weather_started_ = true;
            WeatherMix chosen;
            chosen.snow = clock.season == renderer::rts::Season::winter;
            if (clock.weather == renderer::rts::Weather::rain || clock.weather == renderer::rts::Weather::snow) {
                chosen.snow = clock.weather == renderer::rts::Weather::snow;
                chosen.cloud = clock.storm ? 0.97f : chosen.snow ? 0.85f : 0.92f;
                chosen.precipitation = clock.storm ? 0.85f : 0.6f;
                chosen.storm = clock.storm ? 0.9f : 0.0f;
            }
            cycle.shown = chosen;
            cycle.shown_valid = true;
            cycle.force(chosen);
        }
        const double before = cycle.time;
        const float pace = (std::max)(clock.time_speed, 0.0f) * (check ? 600.0f : 1.0f);
        cycle.time += static_cast<double>(clock.elapsed) * pace;
        if (check && std::floor(cycle.time / 3600.0) != std::floor(before / 3600.0))
            std::cout << "weather check: hour " << std::floor(cycle.time / 3600.0) << ": clouds " << cycle.shown.cloud << ", precipitation "
                      << cycle.shown.precipitation << ", storm " << cycle.shown.storm << ", rts weather " << static_cast<int>(clock.weather)
                      << (clock.storm ? " (storm)" : "") << ", drops " << (precipitation_ ? precipitation_->drops() : 0) << std::endl;
        const auto target = cycle.now(clock.season);
        if (!cycle.shown_valid) {
            cycle.shown = target;
            cycle.shown_valid = true;
        } else {
            const float k = 1.0f - std::exp(-clock.elapsed * (std::max)(pace, 1.0f) / 20.0f);
            cycle.shown = blend(cycle.shown, target, k);
            // Gone: none (not a trace of it for ever).
            if (cycle.shown.precipitation < 1e-4f) cycle.shown.precipitation = 0.0f;
            if (cycle.shown.storm < 1e-4f) cycle.shown.storm = 0.0f;
        }
        clock.weather = rts_weather(cycle.shown);
        clock.storm = storming(cycle.shown);
    }
    float sky_open_seen_ = 1.0f; // the view's sky share the probe last saw (0..1, a quarter or more: 1)
    float sky_open_ = 1.0f;      // that, eased
    // The sky's openness round the eye for its light (PBR stage 2): the
    // probe's last answer, and eased.
    static constexpr int sky_light_columns = 8;
    static constexpr int sky_light_rows = 6;
    float sky_light_seen_ = 1.0f;
    float sky_light_open_ = 1.0f;
    // What the probe last saw each way round the eye in the world (azimuth
    // by height above the horizon): the share of its ways out that reached
    // the sky, and where the eye was then. The openness is their mean, those
    // seen from further off weighing less, so turning the head leaves it as
    // it is (it took the view's ways alone until 2026-11-17: in a cab the
    // sky's light dimmed looking at a dark corner and rose at the windows,
    // as an auto exposure turned the wrong way round, testers).
    static constexpr int open_azimuths = 12;
    static constexpr int open_heights = 3;
    struct OpenBin {
        bool known = false;
        float open = 1.0f;
        glm::vec3 where{};
    };
    std::array<OpenBin, open_azimuths * open_heights> open_bins_{};
    std::unique_ptr<renderer::LightningRenderer> lightning_;
    std::unique_ptr<ThunderView> thunder_;
    std::vector<renderer::LightningStrike> test_strikes_; // the panel's, alight
    std::uint64_t test_strikes_made_ = 0;
    std::unordered_map<std::uint64_t, std::vector<renderer::LightningSegment>> lightning_channels_;
    glm::vec4 flash_light_{0.0f};       // ModelLighting::flash
    std::uint64_t sky_flash_id_ = 0;    // NativeSkyFrame's flash
    glm::vec3 sky_flash_at_{0.0f};
    float sky_flash_ = 0.0f;
    // OPENRTS_LIGHTNING_TRACE: each strike's brightest light and pieces.
    std::unordered_map<std::uint64_t, std::pair<float, std::size_t>> lightning_traced_;
    float lightning_traced_peak_ = 0.0f;

    // Service labels over the 3D view (behind the panels), from the last
    // submitted frame's camera.
    // Service labels (L), signal names (Ctrl+F7), platform and siding names
    // (F7) and the lines tying signals and names to their track.
    void draw_scene_marks() const {
        std::vector<WorldLabel> labels;
        std::vector<WorldLine> lines;
        if (labels_) {
            constexpr float range = 600.0f;
            if (track_) track_->collect_labels(labels, last_eye_, range);
            if (signals_) signals_->collect_labels(labels, last_eye_, range);
        }
        if (signal_names_ && signals_) signals_->collect_names(labels, last_eye_);
        if ((signal_names_ || labels_) && signals_) signals_->collect_links(lines, last_eye_, labels_ ? 600.0f : 150.0f);
        if (place_names_ && track_) track_->collect_places(labels, lines, last_eye_, 2000.0f);
        if (place_names_ && train_) train_->collect_car_numbers(labels, last_eye_, 60.0f);
        if (train_) train_->collect_cab_labels(labels, last_eye_, cab_labels_);
        // The other players of a network game, where their cameras are.
        if (net_ && !net_->console() && player_names_) collect_player_labels(*net_, labels, last_eye_, origin_, 3000.0f);
        draw_world_lines(lines);
        draw_world_labels(std::move(labels));
    }

    // Lines of the scene: drawn in this frame's scene pass when the wide-line
    // renderer is there (hidden behind the models), else over the 3D view.
    void draw_world_lines(const std::vector<WorldLine>& lines) const {
        if (wide_lines_) {
            // This frame's lines replace the last ones: a frame that is not
            // drawn (a minimised window, no surface texture) must not pile them up.
            pending_lines_.clear();
            for (const auto& line : lines) pending_lines_.push_back({line.a, line.b, line.colour, line.thickness});
            return;
        }
#ifdef OPENRTS_HAS_IMGUI
        const auto size = ImGui::GetIO().DisplaySize;
        auto* list = ImGui::GetBackgroundDrawList();
        constexpr float near_w = 0.1f;
        for (const auto& line : lines) {
            auto a = last_view_projection_ * glm::vec4{line.a, 1.0f};
            auto b = last_view_projection_ * glm::vec4{line.b, 1.0f};
            if (a.w < near_w && b.w < near_w) continue;
            if (a.w < near_w) a = a + (b - a) * ((near_w - a.w) / (b.w - a.w));
            if (b.w < near_w) b = b + (a - b) * ((near_w - b.w) / (a.w - b.w));
            const auto screen = [&](const glm::vec4& c) {
                return ImVec2{(c.x / c.w * 0.5f + 0.5f) * size.x, (0.5f - c.y / c.w * 0.5f) * size.y};
            };
            const auto sa = screen(a), sb = screen(b);
            if ((sa.x < 0 && sb.x < 0) || (sa.y < 0 && sb.y < 0) || (sa.x > size.x && sb.x > size.x) || (sa.y > size.y && sb.y > size.y)) continue;
            list->AddLine(sa, sb, line.colour, line.thickness);
        }
#else
        (void)lines;
#endif
    }

    // Labels at points of the scene, the far ones first (near ones on top);
    // the background follows the text's alpha.
    void draw_world_labels(std::vector<WorldLabel> labels) const {
#ifdef OPENRTS_HAS_IMGUI
        const auto size = ImGui::GetIO().DisplaySize;
        auto* list = ImGui::GetBackgroundDrawList();
        // An earlier frame's occlusion tests (take_probe_results): a label is
        // hidden when no part of a small square at its depth passed against
        // the solid scene.
        std::vector<renderer::OcclusionProbe::Point> probe_points;
        std::vector<std::size_t> probe_labels;
        const auto label_key = [](const WorldLabel& label) {
            if (label.key != 0) return label.key;
            std::size_t key = std::hash<std::string>{}(label.text);
            for (const float v : {label.position.x, label.position.y, label.position.z}) {
                key = key * 1000003u ^ std::hash<long long>{}(static_cast<long long>(std::floor(v * 4.0f)));
            }
            return key;
        };
        // Far labels first, so near ones are drawn on top.
        std::sort(labels.begin(), labels.end(), [&](const WorldLabel& a, const WorldLabel& b) {
            return glm::distance(a.position, last_eye_) > glm::distance(b.position, last_eye_);
        });
        for (const auto& label : labels) {
            const auto clip = last_view_projection_ * glm::vec4{label.position, 1.0f};
            if (clip.w <= 0.1f) continue;
            const auto ndc = glm::vec3{clip} / clip.w;
            if (ndc.x < -1.2f || ndc.x > 1.2f || ndc.y < -1.2f || ndc.y > 1.2f || ndc.z < 0.0f || ndc.z > 1.0f) continue;
            const ImVec2 anchor{(ndc.x * 0.5f + 0.5f) * size.x, (0.5f - ndc.y * 0.5f) * size.y};
            if (label.occludable && probe_) {
                const auto key = label_key(label);
                if (anchor.x >= 0.0f && anchor.y >= 0.0f && anchor.x < size.x && anchor.y < size.y &&
                    probe_points.size() < renderer::OcclusionProbe::max_points) {
                    probe_points.push_back({.ndc = {ndc.x, ndc.y}, .depth = ndc.z});
                    probe_labels.push_back(key);
                }
                if (hidden_labels_.contains(key)) continue;
            }
            const auto text_size = ImGui::CalcTextSize(label.text.c_str());
            const ImVec2 corner{anchor.x - text_size.x * 0.5f, anchor.y - text_size.y};
            const auto alpha = static_cast<std::uint32_t>(((label.colour >> 24) & 0xffu) * 0xb0u / 0xffu);
            list->AddRectFilled({corner.x - 3.0f, corner.y - 2.0f}, {corner.x + text_size.x + 3.0f, corner.y + text_size.y + 2.0f},
                                alpha << 24, 3.0f);
            list->AddText(corner, label.colour, label.text.c_str());
        }
        // Tested in this frame's scene pass (draw_before_translucent).
        if (probe_) {
            label_probe_points_ = std::move(probe_points);
            label_probe_keys_ = std::move(probe_labels);
        }
#endif
    }
    std::optional<renderer::rts::SkySimulation> sky_simulation_;
    std::optional<renderer::rts::SkyState> sky_state_;
    std::unique_ptr<renderer::SkyRenderer> sky_renderer_;
    std::unique_ptr<renderer::NativeSky> native_sky_; // openRTS's own sky (renderer::native_sky())
    [[nodiscard]] bool native_sky_on() const { return native_sky_ && sky_state_ && renderer::native_sky(); }
    std::unique_ptr<renderer::RouteStreamer> streamer_;
    std::unique_ptr<renderer::ModelLibrary> library_;
    float forest_distance_ = 0.0f; // the forests' clearance they were built with
    // RTS's flashlight (Num2 / Shift+`): off at the start, as RTS's (0x4044B9).
    bool flashlight_ = false;
    glm::vec3 flashlight_forward_{0.0f, 0.0f, -1.0f};
    // F12: the map's objects and their properties.
    mutable Inspector inspector_;
    // Ctrl+F9: the tapes drawn as a speed recorder's paper.
    mutable TapeView tape_view_;
    [[nodiscard]] InspectorWorld inspector_world() const {
        return {.instances = streamer_ ? &streamer_->visible_instances() : nullptr,
                .library = library_.get(),
                .vfs = content_.vfs().get(),
                .origin = origin_,
                .trains = train_.get(),
                .root = content_.root()};
    }
    // The start's loading screen (update_loading).
    static constexpr double loading_limit = 120.0;
    SceneLoading loading_;
    std::chrono::steady_clock::time_point loading_since_{};
    int loading_quiet_ = 0;
    std::size_t loading_peak_ = 0;
    double loading_report_ = 0.0; // s into the loading of its last progress line
    std::optional<TrackView> track_;
    std::unique_ptr<TrainView> train_;
    std::unique_ptr<ActivityView> activity_; // a scenario (--activity)
    std::set<std::int32_t> reported_;        // --capture-report: the menus picked from
    std::unique_ptr<NetSession> net_; // --mp-connect: the server's game
    SceneClock last_clock_;                // the last update's (a save takes it)
    bool start_game_loaded_ = false;        // --load's save put back
    std::optional<SavedClock> saved_clock_; // a loaded game's, for the window's clock
#ifdef OPENRTS_HAS_IMGUI
    mutable ConsolePanel console_panel_; // --mp-console: the dispatcher's window
#endif
    std::unique_ptr<TrackControl> track_control_;
    mutable bool signal_names_ = false; // Ctrl+F7
    mutable bool place_names_ = false;  // F7
    bool signal_names_set_ = false;     // the F4 panel has the signals' names
    // The other players' names over the world (a network game).
    mutable bool player_names_ = remembered("player_names").value_or("1") != "0";
    mutable bool cab_labels_ = false;   // Alt held in the 3-D cab
    std::unique_ptr<SignalView> signals_;
    bool snapshot_signals_ = false; // the snapshot's aspects wait for the signals to load
    std::unique_ptr<CrossingView> crossings_;
    std::unique_ptr<RoadTraffic> roads_;
    std::unique_ptr<PrecipitationView> precipitation_;
    std::unique_ptr<SmokeView> smoke_;
    std::unique_ptr<audio::SoundBank> weather_bank_;
    std::shared_ptr<const formats::SoundScript> weather_script_;
    std::unique_ptr<audio::SoundInstance> weather_sound_;
    bool weather_snow_ = false;
    // The world's extras the viewer panel turns on and off (remembered).
    struct WorldEffects {
        bool traffic = true;
        bool precipitation = true;
        bool smoke = true;
    };
    mutable WorldEffects effects_ = [] {
        WorldEffects effects;
        effects.traffic = remembered("world_traffic").value_or("1") != "0";
        effects.precipitation = remembered("world_precipitation").value_or("1") != "0";
        effects.smoke = remembered("world_smoke").value_or("1") != "0";
        return effects;
    }();
    std::size_t drawn_instances_ = 0;
};

} // namespace

SceneLoadResult load_shape_scene(SceneContent content, const core::VirtualPath& shape) {
    auto asset = renderer::load_model_asset(*content.vfs, shape, {});
    if (!asset.ok()) return {.error = "Shape load failed: " + asset.error};
    return {.scene = std::make_unique<ShapeScene>(std::move(content), std::move(asset))};
}

SceneLoadResult load_route_scene(SceneContent content, const core::VirtualPath& route) {
    const auto definition = formats::load_route_definition(*content.vfs, route);
    if (!definition.ok() || !definition.start) {
        return {.error = "Route load failed: "
                    + (definition.error.empty() ? std::string("RouteStart missing.") : definition.error)};
    }
    std::string track_error;
    auto network = simulation::TrackNetwork::load(*content.vfs, route, &track_error);
    if (!network) std::cout << "track network not loaded: " << track_error << '\n';
    return {.scene = std::make_unique<RouteScene>(std::move(content), route, *definition.start, definition.environments,
                                                  std::move(network))};
}

} // namespace openrts::viewer
#endif
