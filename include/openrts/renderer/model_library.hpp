#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_renderer.hpp>

namespace openrts::renderer {

struct ModelLibraryStats {
    std::size_t models{};
    std::size_t queued_uploads{};
    bool reloading = false;
    std::size_t reload_batches{};
    std::size_t reloaded_models{};
    std::size_t reloaded_textures{};
    std::vector<std::string> errors; // of the last reload batch
};

// GPU models by shape path, with hot reload (docs/vfs.md):
//  - notify_changes() takes the VFS change list; every model whose shape,
//    .sd or any texture candidate changed is reloaded on a worker thread;
//  - when the whole batch is ready, changed textures are uploaded once and all
//    affected models are swapped to their new generation in the same frame;
//  - a file that fails to load keeps the previous generation and is reported;
//  - replaced generations go to the renderer's retire queue.
class ModelLibrary {
public:
    ModelLibrary(ModelRenderer& renderer,
                 std::shared_ptr<const core::Vfs> vfs,
                 std::vector<core::VirtualPath> texture_directories = {});
    ~ModelLibrary();
    ModelLibrary(const ModelLibrary&) = delete;
    ModelLibrary& operator=(const ModelLibrary&) = delete;

    // Queues a streamed model for upload.
    void add(StreamedModelAsset&& asset);
    [[nodiscard]] RenderModel* find(const core::VirtualPath& shape);
    void notify_changes(const std::vector<core::VirtualPath>& changed);
    // Night, season and weather for ESD_Alternative_Texture: every model whose
    // images then come from other subfolders is reloaded on a worker thread
    // (RTS reloads the route's textures when night falls); models loaded
    // under another variant are reloaded when they arrive.
    void set_texture_variant(const TextureVariant& variant);
    // Publishes a finished reload batch, starts the next one and uploads up
    // to `uploads` queued models. Call once per frame on the render thread.
    void update(std::size_t uploads = 1);
    // Retires every model (scene teardown).
    void clear();
    [[nodiscard]] ModelLibraryStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
