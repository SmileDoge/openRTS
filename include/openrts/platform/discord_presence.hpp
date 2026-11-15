#pragma once

// What the player's Discord profile shows while openRTS runs ("Rich
// Presence"): the Discord client on this computer is told over its local
// IPC pipe (\\.\pipe\discord-ipc-0..9; the protocol of discord-rpc: frames of
// an opcode, a length and JSON, a handshake with the application's id, then
// SET_ACTIVITY commands). Nothing is sent anywhere else, and without the
// client running nothing happens (it is looked for again now and then).
//
// A thread of its own does the talking: set() only hands over what to show.
// Discord takes at most 5 changes in 20 s, so a change waits for the last
// one to be 4 s old; the latest wins. The activity goes when this object
// does (Discord clears it when the pipe closes) or with set(std::nullopt).

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openrts::platform {

// The Discord application openRTS shows as ("Играет в openRTS"): its id
// from discord.com/developers. Empty: no presence at all.
// viewer_state.ini's "discord_application_id" overrides it (a test).
inline constexpr std::string_view discord_application_id = "1571494138317111337";

struct DiscordActivity {
    std::string details;     // the first line under the game's name
    std::string state;       // the second line
    std::string large_image; // an art asset's key of the application, or an https URL
    std::string large_text;  // the image's hover text
    std::int64_t start = 0;  // Unix time, seconds: "00:12 elapsed"; 0 none
    struct Button {
        std::string label; // at most 32 characters
        std::string url;
        bool operator==(const Button&) const = default;
    };
    std::vector<Button> buttons; // at most 2 (others see them, not the player)

    bool operator==(const DiscordActivity&) const = default;
};

// The SET_ACTIVITY command's JSON for process `pid` (no activity: cleared).
// Texts cut to Discord's 128 characters (on a character's boundary), and
// one shorter than 2 left out (Discord refuses it).
[[nodiscard]] std::string discord_activity_command(const std::optional<DiscordActivity>& activity, std::uint32_t pid,
                                                   std::uint64_t nonce);

class DiscordPresence {
public:
    struct Options {
        std::string application_id;
        // The pipes tried, with 0..9 after it.
        std::string pipe_prefix = R"(\\.\pipe\discord-ipc-)";
        std::chrono::milliseconds retry{15000};       // between tries to find the client
        std::chrono::milliseconds min_interval{4000}; // between two changes
    };
    explicit DiscordPresence(Options options);
    ~DiscordPresence();
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    // What to show from now on (none: nothing).
    void set(std::optional<DiscordActivity> activity);

    struct Status {
        bool connected = false; // the pipe is open
        bool ready = false;     // the client accepted the handshake
        std::string error;      // the client's last refusal (an ERROR event, a CLOSE)
        std::uint64_t sent = 0; // SET_ACTIVITY commands written
        std::uint64_t accepted = 0; // and answered without an error
    };
    [[nodiscard]] Status status() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace openrts::platform
