#include <openrts/assets/content_resolver.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace openrts::assets {
namespace {

[[nodiscard]] std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) {
        return false;
    }

    return std::none_of(path.begin(), path.end(), [](const auto& component) {
        return component == "..";
    });
}

[[nodiscard]] std::optional<std::filesystem::path> find_case_insensitive(
    const std::filesystem::path& root,
    const std::filesystem::path& relative_path) {
    std::error_code error;
    auto current = root;

    for (const auto& component : relative_path) {
        const auto direct = current / component;
        if (std::filesystem::exists(direct, error) && !error) {
            current = direct;
            continue;
        }

        error.clear();
        if (!std::filesystem::is_directory(current, error) || error) {
            return std::nullopt;
        }

        const auto wanted = lowercase(component.string());
        std::optional<std::filesystem::path> match;
        for (std::filesystem::directory_iterator iterator(current, error), end;
             !error && iterator != end;
             iterator.increment(error)) {
            if (lowercase(iterator->path().filename().string()) == wanted) {
                match = iterator->path();
                break;
            }
        }

        if (error || !match) {
            return std::nullopt;
        }
        current = *match;
    }

    return current;
}

void add_candidate(std::vector<std::filesystem::path>& candidates,
                   const std::filesystem::path& root,
                   const std::filesystem::path& requested_path) {
    candidates.push_back(root / requested_path);
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

ContentResolver::ContentResolver(ContentRoots roots) : roots_(std::move(roots)) {}

ResolutionResult ContentResolver::resolve(const AssetKind kind,
                                          const std::filesystem::path& requested_path,
                                          const AssetContext& context) const {
    ResolutionResult result{.requested_path = requested_path, .kind = kind};
    if (!is_safe_relative_path(requested_path)) {
        return result;
    }

    std::vector<std::filesystem::path> candidates;
    const auto& root = roots_.installation_root;
    switch (kind) {
    case AssetKind::shape:
        if (context.route_name) {
            add_candidate(candidates, root / "ROUTES" / *context.route_name / "SHAPES", requested_path);
        }
        add_candidate(candidates, root / "GLOBAL" / "SHAPES", requested_path);
        break;
    case AssetKind::texture:
        if (context.route_name) {
            add_candidate(candidates, root / "ROUTES" / *context.route_name / "TEXTURES", requested_path);
        }
        add_candidate(candidates, root / "GLOBAL" / "TEXTURES", requested_path);
        break;
    case AssetKind::locomotive_library:
        if (context.trainset_name) {
            add_candidate(candidates, root / "TRAINS" / "TRAINSET" / *context.trainset_name, requested_path);
        }
        add_candidate(candidates, root / "GLOBAL" / "LIB", requested_path);
        break;
    case AssetKind::train_asset:
        if (context.trainset_name) {
            add_candidate(candidates, root / "TRAINS" / "TRAINSET" / *context.trainset_name, requested_path);
        }
        break;
    }

    for (const auto& candidate : candidates) {
        const auto parent = candidate.parent_path();
        const auto match = find_case_insensitive(parent, candidate.filename());
        result.attempts.push_back({.candidate = candidate, .found = match.has_value()});
        if (match) {
            result.resolved_path = *match;
            return result;
        }
    }

    return result;
}

ResolutionResult ContentResolver::resolve_relative_to_directory(
    const AssetKind kind,
    const std::filesystem::path& directory,
    const std::filesystem::path& requested_path) const {
    ResolutionResult result{.requested_path = requested_path, .kind = kind};
    if (!is_safe_relative_path(requested_path)) return result;

    const auto match = find_case_insensitive(directory, requested_path);
    result.attempts.push_back({.candidate = directory / requested_path, .found = match.has_value()});
    if (match) result.resolved_path = *match;
    return result;
}

} // namespace openrts::assets
