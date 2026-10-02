#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
    std::filesystem::path candidate;
    bool found = false;
};

struct ResolutionResult {
    std::filesystem::path requested_path;
    AssetKind kind{};
    std::optional<std::filesystem::path> resolved_path;
    std::vector<ResolutionAttempt> attempts;

    [[nodiscard]] bool found() const noexcept { return resolved_path.has_value(); }
};

// Resolves an asset against known MSTS/RTS content roots without writing to
// them. Absolute paths and parent-directory traversal are rejected.
class ContentResolver {
public:
    explicit ContentResolver(ContentRoots roots);

    [[nodiscard]] ResolutionResult resolve(
        AssetKind kind,
        const std::filesystem::path& requested_path,
        const AssetContext& context = {}) const;

    [[nodiscard]] ResolutionResult resolve_relative_to_directory(
        AssetKind kind,
        const std::filesystem::path& directory,
        const std::filesystem::path& requested_path) const;

private:
    ContentRoots roots_;
};

} // namespace openrts::assets
