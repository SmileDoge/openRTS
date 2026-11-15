// Discord's Rich Presence (platform/discord_presence): the SET_ACTIVITY JSON,
// and on Windows a talk with a stand-in for the Discord client on a pipe of
// its own: the handshake, an activity once READY, the latest of quick
// changes only, PING answered, and after the client's CLOSE the pipe found
// again and the activity sent once more.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <openrts/platform/discord_presence.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

[[nodiscard]] bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void test_command() {
    using openrts::platform::DiscordActivity;
    using openrts::platform::discord_activity_command;
    check(discord_activity_command(std::nullopt, 42, 7) == R"({"cmd":"SET_ACTIVITY","args":{"pid":42},"nonce":"7"})",
          "no activity: the command clears it");

    DiscordActivity a;
    a.details = "Ведёт \"ВЛ80с\"\\606\n";
    a.state = "x"; // too short for Discord: left out
    a.large_image = "logo";
    a.large_text = "openRTS 0.13.0";
    a.start = 1763200000;
    a.buttons = {{"openRTS на GitHub", "https://github.com/SmileDoge/openRTS"}, {"", "https://x"}, {"Два", "https://y"}, {"Три", "https://z"}};
    const auto json = discord_activity_command(a, 1, 2);
    check(has(json, R"("details":"Ведёт \"ВЛ80с\"\\606\n")"), "quotes, backslashes and new lines escaped: " + json);
    check(!has(json, R"("state")"), "a text of one character is left out");
    check(has(json, R"("timestamps":{"start":1763200000})"), "the start time");
    check(has(json, R"("assets":{"large_image":"logo","large_text":"openRTS 0.13.0"})"), "the image and its text");
    check(has(json, R"("buttons":[{"label":"openRTS на GitHub","url":"https://github.com/SmileDoge/openRTS"},{"label":"Два","url":"https://y"}])"),
          "two buttons, one without a label skipped: " + json);
    check(has(json, R"("instance":false}},"nonce":"2"})"), "the end");

    DiscordActivity long_text;
    for (int i = 0; i < 130; ++i) long_text.details += "ж"; // two bytes each
    const auto cut = discord_activity_command(long_text, 1, 3);
    std::string expected = R"("details":")";
    for (int i = 0; i < 128; ++i) expected += "ж";
    expected += '"';
    check(has(cut, expected), "a long text cut to 128 characters, not bytes");
}

#ifdef _WIN32
// The stand-in client: one pipe instance, served again after a CLOSE.
class FakeClient {
public:
    explicit FakeClient(const std::string& name) {
        const std::wstring wide(name.begin(), name.end());
        pipe_ = CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0,
                                 nullptr);
    }
    ~FakeClient() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    }
    [[nodiscard]] bool ok() const { return pipe_ != INVALID_HANDLE_VALUE; }
    bool accept() {
        const BOOL connected = ConnectNamedPipe(pipe_, nullptr);
        return connected || GetLastError() == ERROR_PIPE_CONNECTED;
    }
    void drop() {
        FlushFileBuffers(pipe_);
        DisconnectNamedPipe(pipe_);
    }
    bool send(const std::uint32_t opcode, const std::string& json) {
        std::string frame(8 + json.size(), '\0');
        const std::uint32_t header[2] = {opcode, static_cast<std::uint32_t>(json.size())};
        std::memcpy(frame.data(), header, 8);
        std::memcpy(frame.data() + 8, json.data(), json.size());
        DWORD wrote = 0;
        return WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &wrote, nullptr) && wrote == frame.size();
    }
    std::optional<std::pair<std::uint32_t, std::string>> receive() {
        std::uint32_t header[2] = {};
        if (!read_all(header, 8)) return std::nullopt;
        std::string json(header[1], '\0');
        if (header[1] > 0 && !read_all(json.data(), header[1])) return std::nullopt;
        return std::pair{header[0], json};
    }
    // Is a frame there (peeked) within `wait`?
    bool frame_within(const std::chrono::milliseconds wait) {
        const auto until = std::chrono::steady_clock::now() + wait;
        while (std::chrono::steady_clock::now() < until) {
            DWORD available = 0;
            if (PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) && available >= 8) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

private:
    bool read_all(void* to, const std::size_t size) {
        auto* bytes = static_cast<char*>(to);
        std::size_t done = 0;
        while (done < size) {
            DWORD got = 0;
            if (!ReadFile(pipe_, bytes + done, static_cast<DWORD>(size - done), &got, nullptr) || got == 0) return false;
            done += got;
        }
        return true;
    }
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

void test_talk() {
    using namespace std::chrono_literals;
    using openrts::platform::DiscordActivity;
    using openrts::platform::DiscordPresence;
    const std::string prefix = R"(\\.\pipe\openrts-test-discord-)" + std::to_string(GetCurrentProcessId()) + "-";
    FakeClient client(prefix + "0");
    check(client.ok(), "the stand-in's pipe");
    if (!client.ok()) return;

    constexpr std::string_view ready = R"({"cmd":"DISPATCH","data":{"v":1,"config":{},"user":{"id":"1","username":"t"}},"evt":"READY","nonce":null})";
    auto owned = std::make_unique<DiscordPresence>(
        DiscordPresence::Options{.application_id = "123456789012345678", .pipe_prefix = prefix, .retry = 100ms, .min_interval = 300ms});
    auto& presence = *owned;
    DiscordActivity first;
    first.details = "Ведёт VL80s-606";
    first.state = "Маршрут Lviv";
    presence.set(first);

    check(client.accept(), "the presence connects");
    auto handshake = client.receive();
    check(handshake && handshake->first == 0 && handshake->second == R"({"v":1,"client_id":"123456789012345678"})",
          "the handshake: " + (handshake ? handshake->second : std::string("none")));
    check(!client.frame_within(400ms), "nothing before READY");
    client.send(1, std::string(ready));
    auto activity = client.receive();
    check(activity && activity->first == 1 && has(activity->second, R"("details":"Ведёт VL80s-606","state":"Маршрут Lviv")")
              && has(activity->second, R"("pid":)" + std::to_string(GetCurrentProcessId())),
          "the activity after READY: " + (activity ? activity->second : std::string("none")));
    client.send(1, R"({"cmd":"SET_ACTIVITY","data":{"details":"x"},"evt":null,"nonce":"1"})");

    // Two changes at once: only the second goes, once the first is 300 ms old.
    DiscordActivity second = first;
    second.details = "Пешком";
    DiscordActivity third = first;
    third.details = "Сценарий";
    const auto changed = std::chrono::steady_clock::now();
    presence.set(second);
    presence.set(third);
    activity = client.receive();
    const auto waited = std::chrono::steady_clock::now() - changed;
    check(activity && has(activity->second, R"("details":"Сценарий")"), "the latest change: " + (activity ? activity->second : std::string("none")));
    check(waited >= 150ms, "not at once after the last one");
    check(!client.frame_within(500ms), "the change in between is not sent");
    presence.set(third); // the same again: nothing
    check(!client.frame_within(400ms), "the same activity is not sent again");

    client.send(3, R"({"n":5})");
    auto pong = client.receive();
    check(pong && pong->first == 4 && pong->second == R"({"n":5})", "PING answered with PONG");

    // The client closes: the presence comes back and says it all again.
    client.send(2, R"({"code":4000,"message":"Invalid Client ID"})");
    for (int i = 0; i < 50 && presence.status().connected; ++i) std::this_thread::sleep_for(20ms);
    check(!presence.status().connected && presence.status().error == "Invalid Client ID", "the client's CLOSE: " + presence.status().error);
    client.drop();
    check(client.accept(), "found again");
    handshake = client.receive();
    check(handshake && handshake->first == 0, "a new handshake");
    client.send(1, std::string(ready));
    activity = client.receive();
    check(activity && has(activity->second, R"("details":"Сценарий")"), "the activity again after the new READY");
    const auto status = presence.status();
    check(status.ready && status.error.empty() && status.sent == 3 && status.accepted == 1,
          "status: sent " + std::to_string(status.sent) + ", accepted " + std::to_string(status.accepted));

    // Going: the activity cleared before the pipe closes.
    owned.reset();
    const auto cleared = client.receive();
    check(cleared && cleared->first == 1 && has(cleared->second, R"("args":{"pid":)") && !has(cleared->second, "activity"),
          "the activity cleared on the way out: " + (cleared ? cleared->second : std::string("none")));
}
#endif

} // namespace

int main() {
    // A test that hangs on a pipe fails instead.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::cerr << "FAILED: timed out\n";
        std::_Exit(1);
    }).detach();
    test_command();
#ifdef _WIN32
    test_talk();
#endif
    if (failures == 0) std::cout << "discord presence: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
