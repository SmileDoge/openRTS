#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <future>
#include <memory>
#include <optional>
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
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train.hpp>

#include "camera.hpp"
#include "viewer_scene.hpp"

namespace openrts::viewer {

// A consist from the content on the route: simulated (simulation::Train on a
// fixed step), driven with the keyboard, drawn with posed bogies and wheels,
// and heard through its cars' sound scripts.
class TrainView {
public:
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
    [[nodiscard]] bool loaded() const noexcept { return train_ != nullptr; }
    [[nodiscard]] bool placed() const noexcept { return train_ && train_->placed(); }
    [[nodiscard]] const std::string& consist_name() const noexcept { return consist_; }

    void update(const SceneInput& input, ViewerCamera& camera, float elapsed, renderer::ModelLibrary& library,
                audio::AudioEngine* sound);
    void submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library);
    void draw_overlay();
    void stop_sound(audio::AudioEngine& sound);
    [[nodiscard]] bool following() const noexcept { return follow_; }
    [[nodiscard]] const simulation::Train* train() const noexcept { return train_.get(); }
    // A traveller at the end of the train that leads (by the motion, else
    // the reverser), facing away from the train.
    [[nodiscard]] std::optional<simulation::TrackTraveller> ahead() const;
    [[nodiscard]] glm::vec3 head_position() const;

private:
    struct Car;
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
    void step_train();
    void update_sound(float elapsed, const ViewerCamera& camera, audio::AudioEngine& sound);
    void list_consists();
    [[nodiscard]] float alpha() const noexcept;
    [[nodiscard]] Pose pose(std::size_t car) const;
    [[nodiscard]] glm::vec3 local(const simulation::TrackPoint& point) const;
    [[nodiscard]] glm::vec3 local(const formats::RoutePosition& position) const;

    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    formats::RoutePosition origin_;
    simulation::TrackNetwork* network_ = nullptr;
    std::string consist_;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock_;
    std::vector<Car> cars_;
    std::unique_ptr<simulation::Train> train_;
    std::shared_ptr<simulation::DriverHandles> driver_;
    std::vector<simulation::CarPlacement> previous_, current_;
    std::vector<double> previous_travelled_, current_travelled_;
    double accumulator_{};
    bool follow_ = true;
    std::size_t joints_ = 0, junctions_ = 0, coupler_breaks_ = 0, derailments_ = 0;
    bool end_of_track_ = false;
    std::unique_ptr<audio::SoundBank> bank_;
    int substeps_ = 1;

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
    std::string message_;
};

} // namespace openrts::viewer
#endif
