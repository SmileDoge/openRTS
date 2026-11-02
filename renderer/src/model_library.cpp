#include <openrts/renderer/model_library.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace openrts::renderer {
namespace {

struct ReloadBatch {
    std::vector<StreamedModelAsset> assets;
    std::unordered_set<std::string> changed_keys;
};

} // namespace

struct ModelLibrary::Impl {
    ModelRenderer& renderer;
    std::shared_ptr<const core::Vfs> vfs;
    std::vector<core::VirtualPath> texture_directories;

    std::unordered_map<std::string, RenderModel> models;                         // shape key -> model
    std::unordered_map<std::string, std::unordered_set<std::string>> dependents; // file key -> shape keys
    std::deque<StreamedModelAsset> uploads;
    std::vector<core::VirtualPath> pending_changes;
    TextureVariant variant;
    std::vector<core::VirtualPath> variant_reloads; // shapes whose texture folders changed
    std::future<ReloadBatch> job;
    ModelLibraryStats statistics;

    Impl(ModelRenderer& r, std::shared_ptr<const core::Vfs> v, std::vector<core::VirtualPath> directories)
        : renderer(r), vfs(std::move(v)), texture_directories(std::move(directories)) {}

    ~Impl() {
        if (job.valid()) job.wait();
    }

    void index(const RenderModel& model) {
        for (const auto& key : model.dependency_keys()) dependents[key].insert(model.source().key());
    }

    void unindex(const RenderModel& model) {
        for (const auto& key : model.dependency_keys()) {
            const auto it = dependents.find(key);
            if (it == dependents.end()) continue;
            it->second.erase(model.source().key());
            if (it->second.empty()) dependents.erase(it);
        }
    }

    void start_batch() {
        std::unordered_set<std::string> changed;
        for (const auto& path : pending_changes) changed.insert(path.key());
        pending_changes.clear();

        std::vector<core::VirtualPath> affected;
        std::unordered_set<std::string> seen;
        const auto affect = [&](const core::VirtualPath& shape) {
            if (seen.insert(shape.key()).second) affected.push_back(shape);
        };
        for (const auto& shape : variant_reloads) affect(shape);
        variant_reloads.clear();
        for (const auto& key : changed) {
            if (const auto it = models.find(key); it != models.end()) affect(it->second.source());
            if (const auto it = dependents.find(key); it != dependents.end()) {
                for (const auto& shape : it->second) {
                    if (const auto model = models.find(shape); model != models.end()) affect(model->second.source());
                }
            }
        }
        // Queued uploads were loaded before the change: reload them too.
        for (auto it = uploads.begin(); it != uploads.end();) {
            const auto keys = it->dependency_keys();
            const bool stale = std::any_of(keys.begin(), keys.end(), [&](const auto& k) { return changed.contains(k); });
            if (stale) {
                affect(it->source_path);
                it = uploads.erase(it);
            } else {
                ++it;
            }
        }
        if (affected.empty()) return;

        statistics.reloading = true;
        job = std::async(std::launch::async, [vfs = vfs, directories = texture_directories, variant = variant,
                                              affected = std::move(affected), changed = std::move(changed)] {
            ReloadBatch batch{.changed_keys = changed};
            TextureDecodeCache cache; // a texture shared by many models is decoded once
            for (const auto& shape : affected) batch.assets.push_back(load_model_asset(*vfs, shape, directories, &cache, variant));
            return batch;
        });
    }

    // Applies a finished batch atomically: all new textures first, then every
    // model swap, all before the next frame is recorded.
    void apply(ReloadBatch batch) {
        statistics.reloading = false;
        statistics.errors.clear();

        std::vector<std::shared_ptr<const void>> pinned; // new textures until models hold them
        std::unordered_set<std::string> uploaded;
        for (const auto& asset : batch.assets) {
            for (const auto& texture : asset.textures) {
                if (!texture.source_path || !batch.changed_keys.contains(texture.source_path->key())) continue;
                if (!uploaded.insert(texture.source_path->key()).second) continue;
                if (!texture.image) {
                    statistics.errors.push_back(texture.source_path->str() + ": " + texture.error + " (previous kept)");
                    continue;
                }
                if (auto handle = renderer.replace_texture(*texture.source_path, *texture.image)) {
                    pinned.push_back(std::move(handle));
                    ++statistics.reloaded_textures;
                }
            }
        }

        std::vector<std::pair<std::string, RenderModel>> next;
        for (auto& asset : batch.assets) {
            const auto path = asset.source_path;
            if (!asset.ok()) {
                statistics.errors.push_back(path.str() + ": " + asset.error + " (previous kept)");
                continue;
            }
            auto model = renderer.create_model(std::move(asset));
            if (!model) {
                statistics.errors.push_back(path.str() + ": GPU model creation failed (previous kept)");
                continue;
            }
            next.emplace_back(path.key(), std::move(model));
        }

        for (auto& [key, model] : next) {
            auto [slot, inserted] = models.try_emplace(key);
            if (!inserted) unindex(slot->second);
            renderer.replace(slot->second, std::move(model));
            index(slot->second);
            ++statistics.reloaded_models;
        }
        ++statistics.reload_batches;
        for (const auto& error : statistics.errors) std::cout << "hot reload: " << error << '\n';
        std::cout << "hot reload: " << next.size() << " model(s), " << pinned.size() << " texture(s)" << std::endl;
    }
};

ModelLibrary::ModelLibrary(ModelRenderer& renderer,
                           std::shared_ptr<const core::Vfs> vfs,
                           std::vector<core::VirtualPath> texture_directories)
    : impl_(std::make_unique<Impl>(renderer, std::move(vfs), std::move(texture_directories))) {}

ModelLibrary::~ModelLibrary() {
    clear();
}

void ModelLibrary::add(StreamedModelAsset&& asset) {
    impl_->uploads.push_back(std::move(asset));
}

RenderModel* ModelLibrary::find(const core::VirtualPath& shape) {
    const auto it = impl_->models.find(shape.key());
    return it == impl_->models.end() ? nullptr : &it->second;
}

void ModelLibrary::notify_changes(const std::vector<core::VirtualPath>& changed) {
    impl_->pending_changes.insert(impl_->pending_changes.end(), changed.begin(), changed.end());
}

void ModelLibrary::set_texture_variant(const TextureVariant& variant) {
    auto& impl = *impl_;
    if (impl.variant == variant) return;
    impl.variant = variant;
    const auto before = impl.variant_reloads.size();
    for (const auto& [_, model] : impl.models) {
        if (model.alternative_texture() == 0) continue;
        if (texture_variant_key(model.alternative_texture(), model.fullbright_images(), variant) != model.texture_key()) {
            impl.variant_reloads.push_back(model.source());
        }
    }
    std::cout << "textures: " << (variant.night ? "night" : "day") << ", season " << static_cast<int>(variant.season) << ", weather "
              << static_cast<int>(variant.weather) << ": " << impl.variant_reloads.size() - before << " model(s) to reload" << std::endl;
}

void ModelLibrary::update(const std::size_t uploads) {
    auto& impl = *impl_;
    using namespace std::chrono_literals;
    if (impl.job.valid() && impl.job.wait_for(0ms) == std::future_status::ready) impl.apply(impl.job.get());
    if (!impl.job.valid() && (!impl.pending_changes.empty() || !impl.variant_reloads.empty())) impl.start_batch();

    for (std::size_t i = 0; i < uploads && !impl.uploads.empty(); ++i) {
        auto asset = std::move(impl.uploads.front());
        impl.uploads.pop_front();
        const auto key = asset.source_path.key();
        if (impl.models.contains(key)) continue;
        // Loaded under another night / season: shown now, reloaded for this one.
        if (asset.alternative_texture != 0
            && texture_variant_key(asset.alternative_texture, asset.fullbright_images, impl.variant) != asset.texture_key) {
            impl.variant_reloads.push_back(asset.source_path);
        }
        if (auto model = impl.renderer.create_model(std::move(asset))) {
            impl.index(model);
            impl.models.emplace(key, std::move(model));
        }
    }
}

void ModelLibrary::clear() {
    if (impl_->job.valid()) impl_->job.wait();
    for (auto& [_, model] : impl_->models) impl_->renderer.retire(std::move(model));
    impl_->models.clear();
    impl_->dependents.clear();
    impl_->uploads.clear();
}

ModelLibraryStats ModelLibrary::stats() const {
    auto result = impl_->statistics;
    result.models = impl_->models.size();
    result.queued_uploads = impl_->uploads.size();
    return result;
}

} // namespace openrts::renderer
#endif
