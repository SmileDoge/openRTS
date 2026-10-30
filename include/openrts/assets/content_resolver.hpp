#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::assets {

enum class AssetKind {
    shape,
    texture,
    locomotive_library,
    train_asset,
};

[[nodiscard]] const char* to_string(AssetKind kind) noexcept;

struct ContentRoots {
    std::filesystem::path installation_root;
};

struct AssetContext {
    std::optional<std::string> route_name;
    std::optional<std::string> trainset_name;
};

struct ResolutionAttempt {
    core::VirtualPath candidate;
    bool found = false;
};

struct ResolutionResult {
    std::string requested_path;
    AssetKind kind{};
    std::optional<core::VirtualPath> resolved_path;
    std::vector<ResolutionAttempt> attempts;

    [[nodiscard]] bool found() const noexcept { return resolved_path.has_value(); }
};

// Resolves an asset name the way RTS searches for it (route folder first, then
// GLOBAL; trainset folders for train assets) over the content VFS, so every
// candidate may come from a folder, a pack or a mod. Names may use `\` and
// climb with `..`; absolute paths are refused.
class ContentResolver {
public:
    explicit ContentResolver(std::shared_ptr<const core::Vfs> vfs);
    // Mounts the installation (openrts.ini, packs, folders, mods) itself.
    explicit ContentResolver(const ContentRoots& roots);

    [[nodiscard]] ResolutionResult resolve(AssetKind kind,
                                           std::string_view requested_path,
                                           const AssetContext& context = {}) const;

    // A name relative to the directory of the file that references it (shape
    // image tables, trainset aliases).
    [[nodiscard]] ResolutionResult resolve_relative_to_directory(AssetKind kind,
                                                                 const core::VirtualPath& directory,
                                                                 std::string_view requested_path) const;

    [[nodiscard]] const core::Vfs& vfs() const noexcept { return *vfs_; }
    [[nodiscard]] const std::shared_ptr<const core::Vfs>& shared_vfs() const noexcept { return vfs_; }

private:
    std::shared_ptr<const core::Vfs> vfs_;
};

} // namespace openrts::assets
