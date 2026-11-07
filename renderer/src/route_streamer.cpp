#include <openrts/renderer/route_streamer.hpp>

#include <openrts/core/profile.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <set>
#include <utility>

#include <openrts/assets/content_resolver.hpp>
#include <openrts/renderer/route_space.hpp>

#include "forest_shape.hpp"
#include "transfer_shape.hpp"

namespace openrts::renderer {
namespace {

using TileKey = std::pair<std::int32_t, std::int32_t>;

constexpr std::size_t max_concurrent_model_tasks = 4;

template <typename T>
[[nodiscard]] bool is_ready(const std::future<T>& task) {
    using namespace std::chrono_literals;
    return task.valid() && task.wait_for(0ms) == std::future_status::ready;
}

struct TileLoadResult {
    std::vector<StreamedInstance> instances;
    std::string error;
};

} // namespace

struct RouteStreamer::Impl {
    struct TileState {
        bool desired{};
        bool loaded{};
        std::future<TileLoadResult> task;
        std::vector<StreamedInstance> instances;
    };

    std::shared_ptr<const core::Vfs> vfs;
    assets::ContentResolver resolver;
    core::VirtualPath route;
    std::string route_name;
    std::vector<core::VirtualPath> texture_directories;
    TextureVariant variant;
    formats::RoutePosition origin;
    int radius{};
    std::map<TileKey, core::VirtualPath> world_files;
    std::map<TileKey, TileState> tiles;
    std::map<std::string, std::future<StreamedModelAsset>> model_tasks;
    std::set<std::string> requested_models;
    std::deque<core::VirtualPath> pending_models;
    std::vector<StreamedModelAsset> ready_models;
    // visible_instances() cache and the tiles it was built from.
    mutable std::vector<StreamedInstance> visible;
    mutable std::vector<TileKey> visible_tiles;
    mutable bool visible_stale = true;
    mutable std::uint64_t visible_generation = 0;

    explicit Impl(std::shared_ptr<const core::Vfs> shared) : vfs(shared), resolver(std::move(shared)) {}

    void index_world_files() {
        const auto world = route.resolve(std::string_view("WORLD"));
        if (!world) return;
        for (const auto& entry : vfs->list(*world)) {
            if (entry.directory) continue;
            if (const auto coordinate = formats::parse_world_tile_name(entry.path.filename())) {
                world_files.emplace(*coordinate, entry.path);
            }
        }
    }

    // Worker-thread job: read a world file and resolve the shapes it places.
    [[nodiscard]] TileLoadResult load_tile(const core::VirtualPath& file) const {
        TileLoadResult result;
        const auto tile = formats::load_world_tile(*vfs, file);
        if (!tile.ok()) {
            result.error = tile.error;
            return result;
        }

        const assets::AssetContext context{.route_name = route_name};
        for (const auto& object : tile.objects) {
            std::optional<core::VirtualPath> source;
            if (object.kind == formats::WorldObjectKind::transfer) {
                // A ground texture: its own shape, built from the world file
                // and the terrain (transfer_shape.hpp).
                source = transfer_shape_path(file, object.uid);
            } else if (object.kind == formats::WorldObjectKind::forest) {
                // Trees RTS builds itself (forest_shape.hpp); an incomplete
                // forest is not drawn, as in RTS.
                if (object.forest_complete) source = forest_shape_path(file, object.uid);
            } else if (object.kind == formats::WorldObjectKind::car_spawner) {
                // Road traffic: no shape of its own; listed (an empty path)
                // for the viewer's RoadTraffic to find it.
                source = core::VirtualPath{};
            } else {
                const auto resolved = resolver.resolve(assets::AssetKind::shape, object.shape_file.generic_string(), context);
                if (resolved.found()) source = *resolved.resolved_path;
            }
            if (!source) continue;
            result.instances.push_back({
                .source_path = *source,
                .position = object.position,
                .transform = placement_transform(object, origin),
                .kind = object.kind,
                .world_tile_x = tile.tile_x,
                .world_tile_z = tile.tile_z,
                .uid = object.uid,
                .collide_flags = object.collide_flags,
                .static_flags = object.static_flags,
            });
        }
        return result;
    }

    // The instance's translation relative to the current origin.
    void relocate(StreamedInstance& instance) const {
        constexpr double tile_size = formats::route_tile_size;
        const auto& p = instance.position;
        instance.transform[3] = glm::vec4{
            static_cast<float>((p.tile_x - origin.tile_x) * tile_size + p.x - origin.x),
            static_cast<float>(p.y - origin.y),
            static_cast<float>((p.tile_z - origin.tile_z) * tile_size + p.z - origin.z),
            1.0f,
        };
    }

    void request_model(const core::VirtualPath& shape) {
        if (shape.empty()) return; // a car spawner
        if (requested_models.insert(shape.key()).second) pending_models.push_back(shape);
    }

    void collect_loaded_tiles() {
        for (auto& [_, state] : tiles) {
            if (!is_ready(state.task)) continue;

            const auto result = state.task.get();
            state.loaded = true;
            if (!result.error.empty()) continue;

            state.instances = result.instances;
            // The tile may have been read before an origin change.
            for (auto& instance : state.instances) relocate(instance);
            visible_stale = true;
            for (const auto& instance : state.instances) request_model(instance.source_path);
        }
    }

    void collect_loaded_models() {
        for (auto it = model_tasks.begin(); it != model_tasks.end();) {
            if (!is_ready(it->second)) {
                ++it;
                continue;
            }

            auto asset = it->second.get();
            if (asset.ok()) ready_models.push_back(std::move(asset));
            it = model_tasks.erase(it);
        }
    }

    void start_model_tasks() {
        while (model_tasks.size() < max_concurrent_model_tasks && !pending_models.empty()) {
            auto path = std::move(pending_models.front());
            pending_models.pop_front();
            auto key = path.key();
            auto task = std::async(std::launch::async, [vfs = vfs, path, directories = texture_directories, variant = variant] {
                return load_model_asset(*vfs, path, directories, nullptr, variant);
            });
            model_tasks.emplace(std::move(key), std::move(task));
        }
    }

    void evict_unwanted_tiles() {
        std::erase_if(tiles, [](const auto& entry) {
            const auto& state = entry.second;
            return !state.desired && state.loaded && !state.task.valid();
        });
    }
};

void RouteStreamer::set_texture_variant(const TextureVariant& variant) { impl_->variant = variant; }

RouteStreamer::RouteStreamer(std::shared_ptr<const core::Vfs> vfs,
                             core::VirtualPath route,
                             const formats::RoutePosition origin,
                             const int radius)
    : impl_(std::make_unique<Impl>(std::move(vfs))) {
    impl_->route = std::move(route);
    impl_->route_name = std::string(impl_->route.filename());
    // Objects placed in a route take textures from the route first, including
    // global track shapes.
    if (auto textures = impl_->route.resolve(std::string_view("TEXTURES"))) {
        impl_->texture_directories.push_back(std::move(*textures));
    }
    impl_->origin = origin;
    impl_->radius = (std::max)(radius, 0);
    impl_->index_world_files();
}

RouteStreamer::~RouteStreamer() = default;
RouteStreamer::RouteStreamer(RouteStreamer&&) noexcept = default;
RouteStreamer& RouteStreamer::operator=(RouteStreamer&&) noexcept = default;

void RouteStreamer::set_origin(const formats::RoutePosition& origin) {
    impl_->origin = origin;
    for (auto& [_, state] : impl_->tiles) {
        for (auto& instance : state.instances) impl_->relocate(instance);
    }
    impl_->visible_stale = true;
}

void RouteStreamer::update(const formats::RoutePosition& camera) {
    OPENRTS_ZONE("route streaming");
    for (auto& [_, state] : impl_->tiles) state.desired = false;

    for (int z = -impl_->radius; z <= impl_->radius; ++z) {
        for (int x = -impl_->radius; x <= impl_->radius; ++x) {
            const TileKey key{camera.tile_x + x, camera.tile_z + z};
            const auto file = impl_->world_files.find(key);
            if (file == impl_->world_files.end()) continue;

            auto [tile, inserted] = impl_->tiles.try_emplace(key);
            tile->second.desired = true;
            if (inserted) {
                tile->second.task = std::async(std::launch::async, [impl = impl_.get(), path = file->second] {
                    return impl->load_tile(path);
                });
            }
        }
    }
}

void RouteStreamer::poll() {
    impl_->collect_loaded_tiles();
    impl_->collect_loaded_models();
    impl_->start_model_tasks();
    impl_->evict_unwanted_tiles();
}

std::vector<StreamedModelAsset> RouteStreamer::take_ready_models() {
    return std::exchange(impl_->ready_models, {});
}

const std::vector<StreamedInstance>& RouteStreamer::visible_instances() const {
    auto& impl = *impl_;
    std::vector<TileKey> tiles;
    for (const auto& [key, state] : impl.tiles) {
        if (state.desired && state.loaded) tiles.push_back(key);
    }
    if (impl.visible_stale || tiles != impl.visible_tiles) {
        impl.visible.clear();
        for (const auto& key : tiles) {
            const auto& instances = impl.tiles.at(key).instances;
            impl.visible.insert(impl.visible.end(), instances.begin(), instances.end());
        }
        impl.visible_tiles = std::move(tiles);
        impl.visible_stale = false;
        ++impl.visible_generation;
    }
    return impl.visible;
}

std::uint64_t RouteStreamer::visible_generation() const noexcept {
    return impl_->visible_generation;
}

RouteStreamerStats RouteStreamer::stats() const {
    RouteStreamerStats result;
    result.pending_model_assets = impl_->model_tasks.size() + impl_->pending_models.size();
    result.ready_model_assets = impl_->ready_models.size();
    for (const auto& [_, state] : impl_->tiles) {
        if (state.desired) ++result.requested_tiles;
        if (state.loaded) ++result.loaded_tiles;
        if (state.desired && state.loaded) result.visible_instances += state.instances.size();
    }
    return result;
}

const std::vector<core::VirtualPath>& RouteStreamer::texture_directories() const noexcept {
    return impl_->texture_directories;
}

} // namespace openrts::renderer
