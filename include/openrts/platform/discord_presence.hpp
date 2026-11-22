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
//
// Joining (2026-11-21, a try of discord-rpc's way): an activity with a party
// and a join secret gives the others "Ask to Join" / "Join"; Discord starts
// the game registered for the application (register_discord_launch) at the
// one who joins and hands it the secret in an ACTIVITY_JOIN event. The
// player's own "Ask to Join" requests (ACTIVITY_JOIN_REQUEST) are answered
// yes while the activity has a secret.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openrts::platform {

// The Discord application DTRE shows as ("Играет в DTRE"): its id from
// discord.com/developers. Empty: no presence at all.
// viewer_state.ini's "discord_application_id" overrides it (a test).
inline constexpr std::string_view discord_application_id = "1574133159162028213";

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
    std::vector<Button> buttons; // at most 2 (others see them, not the player); none with a join secret
    // A game others may join: the party (the same id for everyone in it,
    // how many are in it of how many; party_max 0: no limit, no size shown)
    // and the secret Discord hands the one who joins (at most 128
    // characters). Empty: none.
    std::string party_id;
    int party_size = 0;
    int party_max = 0;
    std::string join_secret;

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
        // ACTIVITY_JOIN subscribed: the secrets of games joined from Discord
        // come in take_joins() (the start menu).
        bool join_events = false;
        // ACTIVITY_JOIN_REQUEST subscribed and answered yes while the
        // activity has a join secret (the game).
        bool accept_join_requests = false;
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
        std::uint64_t subscribed = 0;    // SUBSCRIBE commands answered without an error
        std::uint64_t join_requests = 0; // "Ask to Join" requests answered yes
        std::uint64_t joins = 0;         // ACTIVITY_JOIN events
    };
    [[nodiscard]] Status status() const;
    // The join secrets Discord handed over since the last call (join_events).
    [[nodiscard]] std::vector<std::string> take_joins();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Discord's way to start this game for a join: the URL scheme
// discord-<application id> under HKEY_CURRENT_USER\Software\Classes
// running `exe` (as discord-rpc's Discord_Register). Windows only; false
// and `error` when it could not be written.
bool register_discord_launch(std::string_view application_id, const std::filesystem::path& exe, std::string& error);

} // namespace openrts::platform
