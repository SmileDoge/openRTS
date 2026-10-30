#include <openrts/core/installation.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <string_view>

namespace openrts::core {
namespace fs = std::filesystem;
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::vector<std::string> split_list(const std::string_view text) {
    std::vector<std::string> result;
    std::size_t at = 0;
    while (at <= text.size()) {
        auto end = text.find(',', at);
        if (end == std::string_view::npos) end = text.size();
        if (const auto item = trim(text.substr(at, end - at)); !item.empty()) result.emplace_back(item);
        at = end + 1;
    }
    return result;
}

[[nodiscard]] bool is_true(const std::string_view value) {
    const auto folded = fold_case(trim(value));
    return folded == "1" || folded == "true" || folded == "yes" || folded == "on";
}

// Finds `name` in `directory` ignoring case (content written on Windows).
[[nodiscard]] std::optional<fs::path> find_child(const fs::path& directory, const std::string_view name) {
    const auto direct = directory / fs::path(std::u8string(name.begin(), name.end()));
    std::error_code error;
    if (fs::exists(direct, error)) return direct;
    const auto wanted = fold_case(name);
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto spelled = it->path().filename().u8string();
        if (fold_case(std::string_view(reinterpret_cast<const char*>(spelled.data()), spelled.size())) == wanted) {
            return it->path();
        }
    }
    return std::nullopt;
}

} // namespace

InstallationConfig load_installation_config(const fs::path& root) {
    InstallationConfig config;
    std::ifstream input(root / "openrts.ini", std::ios::binary);
    if (!input) return config;

    std::string section;
    std::map<int, std::string> ordered_mods;
    int unnumbered = 1'000'000;
    std::string line;
    bool first = true;
    while (std::getline(input, line)) {
        std::string_view text(line);
        if (first && text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        first = false;
        if (const auto comment = text.find(';'); comment != std::string_view::npos) text = text.substr(0, comment);
        text = trim(text);
        if (text.empty()) continue;
        if (text.front() == '[' && text.back() == ']') {
            section = fold_case(trim(text.substr(1, text.size() - 2)));
            continue;
        }
        const auto equals = text.find('=');
        if (equals == std::string_view::npos) {
            if (section == "mods") ordered_mods[unnumbered++] = std::string(text);
            continue;
        }
        const auto key = fold_case(trim(text.substr(0, equals)));
        const auto value = trim(text.substr(equals + 1));
        if (section == "vfs") {
            if (key == "folders") config.folders = split_list(value);
            if (key == "hotreload") config.hot_reload = is_true(value);
            if (key == "reconcileseconds") {
                try {
                    config.reconcile_seconds = std::stoi(std::string(value));
                } catch (...) {
                }
            }
        } else if (section == "mods" && !value.empty()) {
            int order = unnumbered++;
            try {
                order = std::stoi(std::string(key));
            } catch (...) {
            }
            ordered_mods[order] = std::string(value);
        }
    }
    for (const auto& [_, mod] : ordered_mods) config.mods.push_back(mod);
    return config;
}

void mount_installation(Vfs& vfs, const fs::path& root, const InstallationConfig& config) {
    if (const auto packs = find_child(root, "PACKS")) {
        if (const auto base = find_child(*packs, "base")) vfs.mount_pack_directory(*base, VfsLayer::base_pack);
    }
    for (const auto& folder : config.folders) {
        const auto directory = find_child(root, folder);
        if (!directory) continue;
        const auto spelled = directory->filename().u8string();
        const auto point = VirtualPath::parse(std::string_view(reinterpret_cast<const char*>(spelled.data()), spelled.size()));
        vfs.mount_directory(*directory, {.layer = VfsLayer::folder, .mount_point = point.value_or(VirtualPath{})});
    }
    if (const auto packs = find_child(root, "PACKS")) {
        if (const auto patch = find_child(*packs, "patch")) vfs.mount_pack_directory(*patch, VfsLayer::patch_pack);
    }
    if (const auto mods = find_child(root, "MODS_OPENRTS")) {
        int order = 0;
        for (const auto& name : config.mods) {
            const auto location = find_child(*mods, name);
            ++order;
            if (!location) continue;
            std::error_code error;
            const VfsMountOptions options{.layer = VfsLayer::mod, .priority = order, .name = name};
            if (fs::is_directory(*location, error)) {
                vfs.mount_directory(*location, options);
            } else {
                vfs.mount_zip(*location, options);
            }
        }
    }
    vfs.build_index();
}

std::optional<fs::path> find_installation_root(const fs::path& inside) {
    std::error_code error;
    auto current = fs::weakly_canonical(fs::absolute(inside, error), error);
    if (error) current = inside;
    while (!current.empty()) {
        for (const auto* marker : {"ROUTES", "TRAINS", "GLOBAL", "PACKS"}) {
            if (fs::is_directory(current / marker, error)) return current;
        }
        if (current == current.parent_path()) break;
        current = current.parent_path();
    }
    return std::nullopt;
}

} // namespace openrts::core
