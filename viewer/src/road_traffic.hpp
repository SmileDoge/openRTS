#pragma once

// Road traffic (world CarSpawner objects with the route's carspawn.dat): cars
// sent along the road database between two road items, as rts1.exe runs them
// (0x493FB8 each frame, 0x494B28 sends one, 0x49396C its speed): they keep
// their distance, slow in curves and stop before a closed level crossing
// (CrossingView). See docs/world-objects.md.

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/car_spawn.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/frustum.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/route_streamer.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_geometry.hpp>
#include <openrts/simulation/track_traveller.hpp>

namespace openrts::viewer {

class CrossingView;

class RoadTraffic {
public:
    RoadTraffic(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, formats::RoutePosition origin);
    ~RoadTraffic();
    RoadTraffic(const RoadTraffic&) = delete;
    RoadTraffic& operator=(const RoadTraffic&) = delete;

    [[nodiscard]] bool ready() const noexcept { return network_ != nullptr && !types_.types.empty(); }
    void set_origin(const formats::RoutePosition& origin) { origin_ = origin; }
    // Each frame: the spawners among the streamed objects send cars and move them.
    void update(float elapsed, const std::vector<renderer::StreamedInstance>& visible, const CrossingView* crossings,
                renderer::ModelLibrary& library);
    void submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library, const renderer::Frustum* frustum,
                const glm::vec3& eye, float draw_distance);
    [[nodiscard]] std::size_t cars() const noexcept;
    [[nodiscard]] std::size_t spawners() const noexcept { return spawners_.size(); }

private:
    using Key = std::tuple<std::int32_t, std::int32_t, std::uint32_t>;
    using TileKey = std::pair<std::int32_t, std::int32_t>;
    struct Car {
        std::size_t type{};
        double along{};     // the front, m from the spawner's start
        float speed{};      // m/s
        float target{};     // CarAvSpeed, at most the type's CarMaxVelocity
        float length{};     // m (the model's length once loaded)
        float wheels{};     // wheel frame, 0..20
    };
    struct Curve {
        double start{}, end{};
        float limit{}; // m/s
    };
    struct Crossing {
        double at{};
        std::uint32_t item{};
    };
    struct Spawner {
        formats::WorldObjectPlacement object;
        std::optional<simulation::TrackTraveller> start; // facing the end
        double end = 0.0;                                // m to the end item
        std::vector<Curve> curves;
        std::vector<Crossing> crossings;
        std::vector<Car> cars; // [0] the furthest along
        float timer = 0.0f;
        float interval = 0.0f;
        std::vector<std::uint32_t> sent; // per type
        bool seen = false;
        bool failed = false;
    };
    struct TypeModel {
        std::optional<core::VirtualPath> shape;
        bool requested = false;
        std::future<renderer::StreamedModelAsset> loading;
    };

    void prepare(Spawner& spawner);
    void send(Spawner& spawner, renderer::ModelLibrary& library);
    void move(Spawner& spawner, float elapsed, const CrossingView* crossings);
    [[nodiscard]] const formats::WorldObjectPlacement* world_object(const Key& key);
    [[nodiscard]] float car_length(std::size_t type, renderer::ModelLibrary& library);
    void request_model(std::size_t type, renderer::ModelLibrary& library);
    [[nodiscard]] std::optional<glm::mat4> place(const Spawner& spawner, const Car& car) const;

    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    formats::RoutePosition origin_;
    std::unique_ptr<simulation::TrackNetwork> network_;
    formats::CarSpawnList types_;
    std::vector<TypeModel> models_;
    std::map<Key, Spawner> spawners_;
    std::map<TileKey, std::future<formats::WorldTile>> loading_;
    std::map<TileKey, formats::WorldTile> tiles_;
    std::minstd_rand random_{20261103u};
};

} // namespace openrts::viewer
