#include "discord_status.hpp"

#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <openrts/core/core.hpp>

#include "ui_text.hpp"
#include "viewer_scene.hpp"
#include "viewer_state.hpp"

namespace openrts::viewer {
namespace {

std::optional<bool> enabled_setting;

// The project's page, as the menu's link.
constexpr const char* project_page = "https://github.com/davydov-dt/DTRE";
// Discord's large activity image: the DTRE logo from this repository.
constexpr const char* logo_url = "https://raw.githubusercontent.com/davydov-dt/DTRE/v.1.0.0-devel/resources/logo.png";

[[nodiscard]] std::string application_id() {
    if (const auto id = remembered("discord_application_id"); id && !id->empty()) return *id;
    return std::string(platform::discord_application_id);
}

} // namespace

bool discord_presence_enabled() {
    if (!enabled_setting) enabled_setting = remembered("discord_presence").value_or("1") != "0";
    return *enabled_setting;
}

void set_discord_presence_enabled(const bool on) {
    enabled_setting = on;
    remember("discord_presence", on ? "1" : "0");
}

DiscordStatus::DiscordStatus(const bool allowed) : allowed_(allowed), start_(static_cast<std::int64_t>(std::time(nullptr))) {}

DiscordStatus::~DiscordStatus() = default;

namespace {

// This program, for Discord to start it for a join.
[[nodiscard]] std::filesystem::path this_program() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
#else
    return {};
#endif
}

} // namespace

void DiscordStatus::update(ViewerScene& scene) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_) return;
    next_ = now + std::chrono::seconds(1);
    if (!allowed_ || !discord_presence_enabled()) {
        if (presence_) {
            presence_.reset(); // the pipe closed: Discord clears it
            std::cout << "discord: presence off" << std::endl;
        }
        ready_logged_ = accepted_logged_ = false;
        return;
    }
    if (!presence_) {
        auto id = application_id();
        if (id.empty()) {
            allowed_ = false; // no application of openRTS's yet: nothing to show as
            return;
        }
        // The start menu takes the games joined from Discord (and is what
        // Discord starts for one); the game says yes to "Ask to Join".
        const bool menu = scene.wants_discord_joins();
        if (menu) {
            std::string error;
            if (!platform::register_discord_launch(id, this_program(), error)) std::cout << "discord: not registered to be started: " << error << std::endl;
        }
        presence_ = std::make_unique<platform::DiscordPresence>(
            platform::DiscordPresence::Options{.application_id = std::move(id), .join_events = menu, .accept_join_requests = !menu});
    }
    for (const auto& secret : presence_->take_joins()) {
        std::cout << "discord: joined from Discord" << std::endl;
        scene.discord_join(secret);
    }
    const auto what = scene.presence();
    if (what != shown_) {
        std::cout << "discord: " << (what ? what->details + " | " + what->state : std::string("nothing"))
                  << (what && !what->join_secret.empty()
                          ? " (others may join: " + std::to_string(what->party_size) +
                                (what->party_max > 0 ? " of " + std::to_string(what->party_max) : std::string(" players")) + ")"
                          : std::string())
                  << std::endl;
    }
    shown_ = what;
    if (what) {
        platform::DiscordActivity activity;
        activity.details = what->details;
        activity.state = what->state;
        activity.large_image = logo_url;
        activity.large_text = "DTRE " + core::version_text();
        activity.start = start_;
        activity.buttons = {{tr("DTRE on GitHub"), project_page}};
        activity.party_id = what->party_id;
        activity.party_size = what->party_size;
        activity.party_max = what->party_max;
        activity.join_secret = what->join_secret;
        presence_->set(std::move(activity));
    } else {
        presence_->set(std::nullopt);
    }
    const auto status = presence_->status();
    if (status.ready && !ready_logged_) std::cout << "discord: connected" << std::endl;
    ready_logged_ = status.ready;
    if (status.accepted > 0 && !accepted_logged_) std::cout << "discord: presence shown" << std::endl;
    accepted_logged_ = status.accepted > 0;
    if (!status.error.empty() && status.error != error_logged_) std::cout << "discord: " << status.error << std::endl;
    if (status.join_requests != requests_logged_) std::cout << "discord: an \"Ask to Join\" answered yes" << std::endl;
    if (status.subscribed != subscribed_logged_) std::cout << "discord: join events subscribed (" << status.subscribed << ")" << std::endl;
    subscribed_logged_ = status.subscribed;
    requests_logged_ = status.join_requests;
    error_logged_ = status.error;
}

} // namespace openrts::viewer
