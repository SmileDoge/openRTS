#include <openrts/assets/content_resolver.hpp>

#include <openrts/core/installation.hpp>

namespace openrts::assets {
namespace {

[[nodiscard]] std::shared_ptr<const core::Vfs> mount(const ContentRoots& roots) {
    auto vfs = std::make_shared<core::Vfs>();
    core::mount_installation(*vfs, roots.installation_root, core::load_installation_config(roots.installation_root));
    return vfs;
}

// Search folder as a virtual path ("ROUTES/<route>/SHAPES").
[[nodiscard]] std::optional<core::VirtualPath> folder(const std::string_view a, const std::string_view b = {},
                                                      const std::string_view c = {}, const std::string_view d = {}) {
    std::string text(a);
    for (const auto part : {b, c, d}) {
        if (part.empty()) continue;
        text += '/';
        text += part;
    }
    return core::VirtualPath::parse(text);
}

} // namespace

const char* to_string(const AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::shape:
        return "shape";
    case AssetKind::texture:
        return "texture";
    case AssetKind::locomotive_library:
        return "locomotive-library";
    case AssetKind::train_asset:
        return "train-asset";
    }
    return "unknown";
}

ContentResolver::ContentResolver(std::shared_ptr<const core::Vfs> vfs) : vfs_(std::move(vfs)) {}

ContentResolver::ContentResolver(const ContentRoots& roots) : vfs_(mount(roots)) {}

ResolutionResult ContentResolver::resolve(const AssetKind kind,
                                          const std::string_view requested_path,
                                          const AssetContext& context) const {
    ResolutionResult result{.requested_path = std::string(requested_path), .kind = kind};
    if (!core::VirtualPath::parse(requested_path)) return result; // rooted

    std::vector<std::optional<core::VirtualPath>> folders;
    switch (kind) {
    case AssetKind::shape:
        if (context.route_name) folders.push_back(folder("ROUTES", *context.route_name, "SHAPES"));
        folders.push_back(folder("GLOBAL", "SHAPES"));
        break;
    case AssetKind::texture:
        if (context.route_name) folders.push_back(folder("ROUTES", *context.route_name, "TEXTURES"));
        folders.push_back(folder("GLOBAL", "TEXTURES"));
        break;
    case AssetKind::locomotive_library:
        if (context.trainset_name) folders.push_back(folder("TRAINS", "TRAINSET", *context.trainset_name));
        folders.push_back(folder("GLOBAL", "LIB"));
        break;
    case AssetKind::train_asset:
        if (context.trainset_name) folders.push_back(folder("TRAINS", "TRAINSET", *context.trainset_name));
        break;
    }

    for (const auto& directory : folders) {
        if (!directory) continue;
        const auto candidate = directory->resolve(requested_path);
        if (!candidate) continue;
        const bool found = vfs_->exists(*candidate);
        result.attempts.push_back({.candidate = *candidate, .found = found});
        if (found) {
            result.resolved_path = *candidate;
            return result;
        }
    }
    return result;
}

ResolutionResult ContentResolver::resolve_relative_to_directory(const AssetKind kind,
                                                                const core::VirtualPath& directory,
                                                                const std::string_view requested_path) const {
    ResolutionResult result{.requested_path = std::string(requested_path), .kind = kind};
    const auto candidate = directory.resolve(requested_path);
    if (!candidate) return result;
    const bool found = vfs_->exists(*candidate);
    result.attempts.push_back({.candidate = *candidate, .found = found});
    if (found) result.resolved_path = *candidate;
    return result;
}

} // namespace openrts::assets
