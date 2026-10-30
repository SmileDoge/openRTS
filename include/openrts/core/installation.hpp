#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::core {

// Settings from `<installation>/openrts.ini` (all optional):
//
//   [Vfs]
//   Folders = ROUTES, TRAINS, GLOBAL, SOUND, GUI   ; content folders on disk
//   HotReload = 1
//   ReconcileSeconds = 180
//   [Mods]
//   1 = HD_Kuban_Textures        ; MODS_OPENRTS/HD_Kuban_Textures/
//   2 = VL80k_Sounds.zip         ; MODS_OPENRTS/VL80k_Sounds.zip
struct InstallationConfig {
    std::vector<std::string> folders{"ROUTES", "TRAINS", "GLOBAL", "SOUND", "GUI"};
    std::vector<std::string> mods; // load order, later wins
    std::optional<bool> hot_reload;
    int reconcile_seconds = 180;
};

[[nodiscard]] InstallationConfig load_installation_config(const std::filesystem::path& root);

// Mounts an installation in the order of docs/vfs.md: PACKS/base, content
// folders, PACKS/patch, then the mods in load order; builds the index.
void mount_installation(Vfs& vfs, const std::filesystem::path& root, const InstallationConfig& config);

// The installation a file or folder belongs to: the nearest ancestor holding
// ROUTES, TRAINS, GLOBAL or PACKS.
[[nodiscard]] std::optional<std::filesystem::path> find_installation_root(const std::filesystem::path& inside);

} // namespace openrts::core
