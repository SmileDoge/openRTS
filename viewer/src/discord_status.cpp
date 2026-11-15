#include "discord_status.hpp"

#include <ctime>
#include <iostream>
#include <string>

#include <openrts/core/core.hpp>

#include "ui_text.hpp"
#include "viewer_scene.hpp"
#include "viewer_state.hpp"

namespace openrts::viewer {
namespace {

std::optional<bool> enabled_setting;

// The project's page, as the menu's link.
constexpr const char* project_page = "https://github.com/SmileDoge/openRTS";

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

void DiscordStatus::update(const ViewerScene& scene) {
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
        presence_ = std::make_unique<platform::DiscordPresence>(platform::DiscordPresence::Options{.application_id = std::move(id)});
    }
    const auto what = scene.presence();
    if (what != shown_) std::cout << "discord: " << (what ? what->details + " | " + what->state : std::string("nothing")) << std::endl;
    shown_ = what;
    if (what) {
        platform::DiscordActivity activity;
        activity.details = what->details;
        activity.state = what->state;
        activity.large_image = "logo";
        activity.large_text = "openRTS " + core::version_text();
        activity.start = start_;
        activity.buttons = {{tr("openRTS on GitHub"), project_page}};
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
    error_logged_ = status.error;
}

} // namespace openrts::viewer
