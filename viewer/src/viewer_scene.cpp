#include "viewer_scene.hpp"

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>

#include <glm/common.hpp>
#include <glm/vec4.hpp>

#include <openrts/formats/environment.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/terrain.hpp>
#include <openrts/audio/world_sounds.hpp>
#include <openrts/renderer/frustum.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/route_space.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/renderer/sky_renderer.hpp>
#include <openrts/renderer/terrain_renderer.hpp>
#include <openrts/simulation/track_network.hpp>

#include "track_view.hpp"
#include "train_view.hpp"

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

using renderer::ModelRenderer;
using renderer::RenderModel;

[[nodiscard]] core::VirtualPath child(const core::VirtualPath& directory, const std::string_view name) {
    return directory.resolve(name).value_or(directory);
}

// Content state shared by both scenes: VFS change polling for hot reload and
// the overlay block describing sources, generations and reloads.
class ContentView {
public:
    explicit ContentView(SceneContent content) : content_(std::move(content)) {}

    [[nodiscard]] const std::shared_ptr<core::Vfs>& vfs() const noexcept { return content_.vfs; }
    [[nodiscard]] const std::string& consist() const noexcept { return content_.consist; }
    [[nodiscard]] const std::string& path() const noexcept { return content_.path; }

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
        if (!ImGui::CollapsingHeader("Content", ImGuiTreeNodeFlags_DefaultOpen)) return;
        const auto stats = content_.vfs->stats();
        ImGui::Text("VFS: %zu sources, %zu files, generation %llu", stats.sources, stats.files,
                    static_cast<unsigned long long>(stats.generation));
        ImGui::Text("Overrides: %zu  |  rescans: %zu (lost events: %zu)", stats.conflicts, stats.rescans,
                    stats.lost_event_rescans);
        ImGui::Text("Hot reload: %s  |  last change batch: %zu file(s)", content_.watching ? "on" : "off", last_changes_);
        if (library) {
            const auto models = library->stats();
            ImGui::Text("Reloaded: %zu models, %zu textures in %zu batches%s", models.reloaded_models,
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
        ImGui::Text("XYZ: %.1f, %.1f, %.1f", eye.x, eye.y, eye.z);
        if (auto* model = const_cast<ShapeScene*>(this)->library_->find(path_); model && model->animation().frame_count > 0) {
            ImGui::Text("Animation frame %.2f of %u (Up/Down)", frame_, model->animation().frame_count);
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
        streamer_ = std::make_unique<renderer::RouteStreamer>(content_.vfs(), route_, origin_, tile_radius);
        streamer_->update(origin_);
        library_ = std::make_unique<renderer::ModelLibrary>(gpu.models, content_.vfs(), streamer_->texture_directories());

        sky_renderer_ = std::make_unique<renderer::SkyRenderer>(gpu.device, gpu.target_format);
        if (!sky_renderer_->ready()) sky_renderer_.reset();
        terrain_ = std::make_unique<renderer::TerrainRenderer>(gpu.device, gpu.target_format, content_.vfs(), route_,
                                                               origin_, terrain_radius);
        if (!terrain_->ready()) terrain_.reset();
        if (track_) track_->attach(gpu.device, gpu.target_format);
        audio_ = gpu.audio;
        if (audio_) world_sounds_ = std::make_unique<audio::WorldSounds>(content_.vfs(), route_, origin_);

        // Start at the route start, a little above the ground, looking along it.
        camera.distance = 1000.0f;
        camera.position = glm::vec3{0.0f, start_ground_height() + 30.0f, 0.0f};
        camera.pitch = 0.08f;
        camera.fly = true;
        if (track_) {
            train_ = std::make_unique<TrainView>(content_.vfs(), route_, origin_, track_->network());
            if (!content_.consist().empty()) {
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
        return {};
    }

    void detach() override {
        if (world_sounds_ && audio_) world_sounds_->stop(*audio_);
        if (train_ && audio_) train_->stop_sound(*audio_);
        train_.reset();
        world_sounds_.reset();
        audio_ = nullptr;
        if (track_) track_->detach();
        terrain_.reset();
        sky_renderer_.reset();
        library_.reset();
        streamer_.reset();
    }

    void update(ModelRenderer&, ViewerCamera& camera, const SceneClock& clock, const SceneInput& input) override {
        if (track_) {
            track_->update(input, clock.elapsed);
            if (input.throw_junction) {
                // The first facing junction ahead of the train, else the
                // junction nearest to the camera.
                if (const auto ahead = train_ ? train_->ahead() : std::nullopt) {
                    track_->throw_junction_ahead(*ahead);
                } else {
                    track_->throw_nearest_junction(camera.eye());
                }
            }
        }
        if (train_) {
            if (input.place_train && train_->loaded()) place_train(camera);
            train_->update(input, camera, clock.elapsed, *library_, audio_);
        }
        const auto camera_position = renderer::route_position(camera.eye(), origin_);
        apply_conditions(clock.season, clock.weather);
        if (terrain_) terrain_->update(camera_position);
        if (sky_simulation_) {
            sky_state_ = sky_simulation_->update(clock.time_of_day, clock.elapsed, camera_position.tile_z, clock.season);
        }

        streamer_->update(camera_position);
        streamer_->poll();
        for (auto& asset : streamer_->take_ready_models()) library_->add(std::move(asset));

        // GPU uploads stay on the render thread; one model per frame keeps the
        // frame time bounded while a tile fills in. Hot-reload batches are
        // published here as a whole.
        content_.forward_changes(*library_);
        library_->update();

        // The free camera is an external camera for sound activation.
        if (world_sounds_ && audio_) {
            world_sounds_->update(clock.elapsed, camera.eye(), audio::SoundCamera::external, *audio_);
        }
    }

    [[nodiscard]] SceneEnvironment environment() const override {
        if (!sky_state_) return {};
        // The sky's fog block is shared by every RTS shader (terrain, shapes).
        auto lighting = sky_state_->lighting;
        lighting.fog_colour = sky_state_->fog_colour;
        lighting.fog_start = sky_state_->fog_start;
        lighting.fog_end = sky_state_->fog_end;
        return {
            .lighting = lighting,
            .clear_colour = glm::vec4{glm::vec3{sky_state_->clear_colour}, 1.0f},
            .time_of_day_applies = true,
        };
    }

    // Sky first (no depth test), then terrain; models follow.
    void draw_background(const WGPURenderPassEncoder pass, const FrameView& frame) override {
        if (sky_renderer_ && sky_state_) sky_renderer_->render(pass, frame.projection, frame.view, *sky_state_);
        if (terrain_) terrain_->render(pass, frame.projection * frame.view, environment().lighting);
    }

    void submit(ModelRenderer& renderer, const FrameView& frame) override {
        const auto frustum = renderer::Frustum::from_view_projection(frame.projection * frame.view);

        drawn_instances_ = 0;
        for (const auto& instance : streamer_->visible_instances()) {
            auto* model = library_->find(instance.source_path);
            if (!model) continue;

            const auto bounds = model->bounds();
            const auto center = glm::vec3{instance.transform * glm::vec4{bounds.center, 1.0f}};
            if (culling_ && beyond_draw_distance(center, bounds.radius, frame.eye)) continue;
            if (culling_ && !frustum.intersects_sphere(center, bounds.radius)) continue;

            // Switch shapes follow their junction; other shapes stay at rest.
            if (const auto blade = track_ ? track_->switch_frame(instance) : std::nullopt) {
                renderer.submit(*model, instance.transform, *blade);
            } else {
                renderer.submit(*model, instance.transform);
            }
            ++drawn_instances_;
        }
        if (train_) train_->submit(renderer, *library_);
    }

    [[nodiscard]] std::optional<formats::RoutePosition> origin() const override { return origin_; }

    void draw_foreground(const WGPURenderPassEncoder pass, const FrameView& frame) override {
        if (track_) track_->draw(pass, frame.projection * frame.view, train_ && train_->placed() ? train_->head_position() : frame.eye);
    }

    void draw_overlay([[maybe_unused]] const ViewerCamera& camera) const override {
#ifdef OPENRTS_HAS_IMGUI
        const auto position = renderer::route_position(camera.eye(), origin_);
        ImGui::Text("Local XYZ: %.1f, %.1f, %.1f", position.x, position.y, position.z);
        ImGui::Text("Tile X/Z: %d, %d", position.tile_x, position.tile_z);
        if (sky_state_) {
            ImGui::Text("Sky: %s%s", environment_name_.c_str(), sky_state_->night ? " (night)" : "");
            if (!sky_state_->satellites.empty() && sky_state_->satellites.front().visible) {
                const auto& sun = sky_state_->satellites.front();
                ImGui::Text("Sun: azimuth %.1f, elevation %.1f", sun.azimuth_degrees, sun.elevation_degrees);
            }
        } else {
            ImGui::TextUnformatted("Sky: no environment");
        }
        if (terrain_) ImGui::Text("Terrain: %zu tiles, %zu loading", terrain_->loaded_tiles(), terrain_->pending_jobs());
        ImGui::Checkbox("Distance and frustum culling", &culling_);

        const auto stream = streamer_->stats();
        const auto models = library_->stats();
        ImGui::Text("Tiles: %zu/%zu  |  Instances: %zu", stream.loaded_tiles, stream.requested_tiles,
                    stream.visible_instances);
        ImGui::Text("Drawn: %zu within %.0f m", drawn_instances_, draw_distance);
        if (world_sounds_) {
            const auto sounds = world_sounds_->stats();
            ImGui::Text("World sounds: %zu sources (%zu active, %zu voices), %zu scripts, %zu clips", sounds.sources,
                        sounds.active, sounds.voices, sounds.scripts, sounds.clips);
        }
        ImGui::Text("Models: %zu GPU, %zu CPU queued, %zu pending", models.models, models.queued_uploads,
                    stream.pending_model_assets);
        if (track_) track_->draw_overlay();
        if (train_) train_->draw_overlay();
        content_.draw_overlay(library_.get());
#endif
    }

private:
    static constexpr int tile_radius = 1;

    // Front of the train on the track nearest to the camera; the other way
    // if the track ends behind.
    void place_train(ViewerCamera& camera) {
        const auto spot = track_->nearest_track(camera.eye());
        if (!spot) {
            std::cout << "train: no track near the camera\n";
            return;
        }
        auto error = train_->place(spot->node(), spot->offset(), spot->forward());
        if (!error.empty()) error = train_->place(spot->node(), spot->offset(), !spot->forward());
        if (!error.empty()) {
            std::cout << "train: " << error << '\n';
            return;
        }
        std::cout << "train " << train_->consist_name() << " placed on node " << spot->node() << '\n';
    }

    static constexpr int terrain_radius = 2;

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
    std::optional<renderer::rts::SkySimulation> sky_simulation_;
    std::optional<renderer::rts::SkyState> sky_state_;
    std::unique_ptr<renderer::SkyRenderer> sky_renderer_;
    std::unique_ptr<renderer::RouteStreamer> streamer_;
    std::unique_ptr<renderer::ModelLibrary> library_;
    std::optional<TrackView> track_;
    std::unique_ptr<TrainView> train_;
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
