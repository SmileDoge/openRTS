#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace openrts::viewer {

// A saved viewer position ("debug view"): what is open, where the camera is
// and the conditions, so the same picture can be reopened with --view.
// Positions are absolute route coordinates (tile + metres), independent of
// the scene origin. Plain `key = value` text.
struct DebugView {
    std::string root;         // installation root
    std::string mode;         // "route" or "shape"
    std::string content;      // virtual path, e.g. ROUTES/Kuban_RTS
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    double x{}, y{}, z{};     // camera eye
    double target_x{}, target_y{}, target_z{}; // orbit target (same tile frame)
    float yaw{};
    float pitch{};
    float distance{};
    bool fly = true;
    std::int32_t time_of_day{}; // seconds
    std::int32_t season{};
    std::int32_t weather{};
    bool experimental = false;
};

[[nodiscard]] std::optional<DebugView> load_debug_view(const std::filesystem::path& file, std::string& error);
bool save_debug_view(const std::filesystem::path& file, const DebugView& view, std::string& error);

// `debug_views/` in user_data_directory(); `--view last` means last.view there.
[[nodiscard]] std::filesystem::path debug_view_directory();
[[nodiscard]] std::filesystem::path resolve_debug_view(const std::string& argument);

} // namespace openrts::viewer
