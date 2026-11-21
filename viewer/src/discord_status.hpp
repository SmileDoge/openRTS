#pragma once

// The game and the start menu in the player's Discord profile
// (platform::DiscordPresence): "Играет в openRTS", what the scene says it is
// doing (ViewerScene::presence), the time since the window opened, the
// version, a button to the releases. On unless switched off in the menu's
// settings ("discord_presence" in viewer_state.ini); never for a capture or
// with --no-discord.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <openrts/platform/discord_presence.hpp>

#include "viewer_scene.hpp"

namespace openrts::viewer {

// The setting: as remembered at the start, changed by the menu's checkbox.
[[nodiscard]] bool discord_presence_enabled();
void set_discord_presence_enabled(bool on);

class DiscordStatus {
public:
    // `allowed`: false for a capture or --no-discord (nothing then at all).
    explicit DiscordStatus(bool allowed);
    ~DiscordStatus();
    DiscordStatus(const DiscordStatus&) = delete;
    DiscordStatus& operator=(const DiscordStatus&) = delete;

    // Every frame; asks the scene about once a second, and hands it the games
    // joined from Discord (the start menu).
    void update(ViewerScene& scene);

private:
    bool allowed_ = false;
    std::int64_t start_ = 0; // Unix seconds, the window's opening
    std::chrono::steady_clock::time_point next_{};
    std::unique_ptr<platform::DiscordPresence> presence_;
    bool ready_logged_ = false;
    bool accepted_logged_ = false;
    std::uint64_t requests_logged_ = 0;
    std::uint64_t subscribed_logged_ = 0;
    std::optional<ScenePresence> shown_; // the scene's last, for the log
    std::string error_logged_;
};

} // namespace openrts::viewer
