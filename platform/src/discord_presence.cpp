#include <openrts/platform/discord_presence.hpp>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace openrts::platform {
namespace {

// The IPC frames' opcodes (discord-rpc's Opcode).
enum : std::uint32_t { op_handshake = 0, op_frame = 1, op_close = 2, op_ping = 3, op_pong = 4 };

// Discord's limit for a text, in characters.
constexpr std::size_t text_limit = 128;
// A frame longer than this is not Discord's (the pipe is dropped).
constexpr std::uint32_t frame_limit = 1u << 20;

// `text` cut to `limit` characters of UTF-8 (at a character's start).
[[nodiscard]] std::string_view cut_utf8(const std::string_view text, const std::size_t limit) {
    std::size_t characters = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u) continue; // a continuation byte
        if (characters == limit) return text.substr(0, i);
        ++characters;
    }
    return text;
}

[[nodiscard]] std::size_t utf8_length(const std::string_view text) {
    std::size_t characters = 0;
    for (const char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) ++characters;
    return characters;
}

void append_json_string(std::string& out, const std::string_view text) {
    out += '"';
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (byte < 0x20u) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[byte >> 4];
            out += hex[byte & 15u];
        } else {
            out += c;
        }
    }
    out += '"';
}

// `"key":"text"` (comma first unless the object has just begun), when the
// text is long enough for Discord; true when written.
bool append_text_field(std::string& out, const char* key, const std::string_view text) {
    const auto cut = cut_utf8(text, text_limit);
    if (utf8_length(cut) < 2) return false;
    if (out.back() != '{') out += ',';
    append_json_string(out, key);
    out += ':';
    append_json_string(out, cut);
    return true;
}

// The string value of the last "key" in a reply (Discord's replies are
// flat enough: "evt" and "cmd" come once, after "data"), unescaped; none
// when it is not a string (null).
[[nodiscard]] std::optional<std::string> json_string_field(const std::string_view json, const std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    const auto at = json.rfind(quoted);
    if (at == std::string_view::npos) return std::nullopt;
    auto i = at + quoted.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == ':' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
    if (i >= json.size() || json[i] != '"') return std::nullopt;
    std::string value;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            ++i;
            value += json[i] == 'n' ? '\n' : json[i];
        } else {
            value += json[i];
        }
    }
    return value;
}

[[nodiscard]] std::uint32_t current_pid() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

// One connection to the client's pipe. Windows only for now (elsewhere
// Discord listens on a Unix socket; not done: openRTS is built for Windows).
class Pipe {
public:
    Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() { close(); }

    [[nodiscard]] bool open() const noexcept {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return false;
#endif
    }
    [[nodiscard]] bool broken() const noexcept { return broken_; }

    bool connect(const std::string& prefix) {
        close();
#ifdef _WIN32
        for (int i = 0; i < 10; ++i) {
            const std::string name = prefix + std::to_string(i);
            const std::wstring wide(name.begin(), name.end()); // the names are ASCII
            const HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                handle_ = handle;
                broken_ = false;
                return true;
            }
        }
#else
        (void)prefix;
#endif
        return false;
    }

    void close() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#endif
        broken_ = false;
    }

    bool write_frame(const std::uint32_t opcode, const std::string_view json) {
#ifdef _WIN32
        if (!open()) return false;
        std::string frame(8 + json.size(), '\0');
        const std::uint32_t header[2] = {opcode, static_cast<std::uint32_t>(json.size())};
        std::memcpy(frame.data(), header, 8);
        std::memcpy(frame.data() + 8, json.data(), json.size());
        std::size_t done = 0;
        while (done < frame.size()) {
            DWORD wrote = 0;
            if (!WriteFile(handle_, frame.data() + done, static_cast<DWORD>(frame.size() - done), &wrote, nullptr) || wrote == 0) {
                broken_ = true;
                return false;
            }
            done += wrote;
        }
        return true;
#else
        (void)opcode;
        (void)json;
        return false;
#endif
    }

    // A whole frame once it has come (none yet: nothing; an error marks
    // the pipe broken).
    std::optional<std::pair<std::uint32_t, std::string>> read_frame() {
#ifdef _WIN32
        if (!open() || broken_) return std::nullopt;
        DWORD available = 0;
        std::uint32_t header[2] = {};
        DWORD peeked = 0;
        if (!PeekNamedPipe(handle_, header, 8, &peeked, &available, nullptr)) {
            broken_ = true;
            return std::nullopt;
        }
        if (peeked < 8) return std::nullopt;
        if (header[1] > frame_limit) {
            broken_ = true;
            return std::nullopt;
        }
        if (available < 8u + header[1]) return std::nullopt;
        std::string data(8u + header[1], '\0');
        std::size_t done = 0;
        while (done < data.size()) {
            DWORD got = 0;
            if (!ReadFile(handle_, data.data() + done, static_cast<DWORD>(data.size() - done), &got, nullptr) || got == 0) {
                broken_ = true;
                return std::nullopt;
            }
            done += got;
        }
        return std::pair{header[0], data.substr(8)};
#else
        return std::nullopt;
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
    bool broken_ = false;
};

} // namespace

std::string discord_activity_command(const std::optional<DiscordActivity>& activity, const std::uint32_t pid,
                                     const std::uint64_t nonce) {
    std::string out = R"({"cmd":"SET_ACTIVITY","args":{"pid":)" + std::to_string(pid);
    if (activity) {
        const auto& a = *activity;
        out += R"(,"activity":{)";
        append_text_field(out, "details", a.details);
        append_text_field(out, "state", a.state);
        if (a.start > 0) {
            if (out.back() != '{') out += ',';
            out += R"("timestamps":{"start":)" + std::to_string(a.start) + "}";
        }
        if (!a.large_image.empty()) {
            if (out.back() != '{') out += ',';
            out += R"("assets":{)";
            append_json_string(out, "large_image");
            out += ':';
            append_json_string(out, a.large_image);
            append_text_field(out, "large_text", a.large_text);
            out += '}';
        }
        std::string buttons;
        std::size_t count = 0;
        for (const auto& button : a.buttons) {
            if (count == 2 || button.url.empty() || button.label.empty()) continue;
            buttons += count++ == 0 ? "{" : ",{";
            append_json_string(buttons, "label");
            buttons += ':';
            append_json_string(buttons, cut_utf8(button.label, 32));
            buttons += ',';
            append_json_string(buttons, "url");
            buttons += ':';
            append_json_string(buttons, button.url);
            buttons += '}';
        }
        if (count > 0) {
            if (out.back() != '{') out += ',';
            out += R"("buttons":[)" + buttons + "]";
        }
        if (out.back() != '{') out += ',';
        out += R"("instance":false})";
    }
    out += R"(},"nonce":")" + std::to_string(nonce) + "\"}";
    return out;
}

struct DiscordPresence::State {
    Options options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool kicked = false;
    std::optional<DiscordActivity> wanted;
    std::uint64_t wanted_version = 0;
    Status status;
    std::thread thread;

    void run();
};

void DiscordPresence::State::run() {
    using clock = std::chrono::steady_clock;
    Pipe pipe;
    bool ready = false;
    std::uint64_t sent_version = 0; // 0: nothing (as Discord starts)
    std::uint64_t nonce = 0;
    auto next_try = clock::now();
    auto last_send = clock::now() - options.min_interval;
    const auto pid = current_pid();
    const auto handshake = R"({"v":1,"client_id":)" + [&] {
        std::string id;
        append_json_string(id, options.application_id);
        return id;
    }() + "}";
    const auto lost = [&](std::string error) {
        pipe.close();
        ready = false;
        sent_version = 0;
        next_try = clock::now() + options.retry;
        const std::lock_guard lock(mutex);
        status.connected = status.ready = false;
        if (!error.empty()) status.error = std::move(error);
    };
    while (true) {
        {
            const std::lock_guard lock(mutex);
            if (stop) break;
        }
        const auto now = clock::now();
        if (!pipe.open() && now >= next_try) {
            if (pipe.connect(options.pipe_prefix) && pipe.write_frame(op_handshake, handshake)) {
                const std::lock_guard lock(mutex);
                status.connected = true;
            } else {
                pipe.close();
                next_try = now + options.retry;
            }
        }
        while (pipe.open()) {
            auto frame = pipe.read_frame();
            if (!frame) break;
            const auto& [opcode, json] = *frame;
            if (opcode == op_frame) {
                const auto evt = json_string_field(json, "evt");
                const auto cmd = json_string_field(json, "cmd");
                const std::lock_guard lock(mutex);
                if (evt && *evt == "READY") {
                    ready = status.ready = true;
                    status.error.clear();
                } else if (evt && *evt == "ERROR") {
                    status.error = json_string_field(json, "message").value_or("error");
                } else if (cmd && *cmd == "SET_ACTIVITY") {
                    ++status.accepted;
                }
            } else if (opcode == op_ping) {
                pipe.write_frame(op_pong, json);
            } else if (opcode == op_close) {
                lost(json_string_field(json, "message").value_or("closed"));
            }
        }
        if (pipe.broken()) lost({});
        if (ready && pipe.open() && clock::now() - last_send >= options.min_interval) {
            std::optional<std::string> command;
            std::uint64_t version = 0;
            {
                const std::lock_guard lock(mutex);
                if (wanted_version != sent_version) {
                    command = discord_activity_command(wanted, pid, ++nonce);
                    version = wanted_version;
                }
            }
            if (command) {
                if (pipe.write_frame(op_frame, *command)) {
                    sent_version = version;
                    last_send = clock::now();
                    const std::lock_guard lock(mutex);
                    ++status.sent;
                } else {
                    lost({});
                }
            }
        }
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return stop || kicked; });
        kicked = false;
    }
    // Going: the activity cleared in so many words, and its answer awaited
    // a moment, before the pipe closes. Discord clears it when the pipe goes
    // too, but not always at once (testers, 2026-11-15: the game closed, its
    // line still in the profile).
    if (ready && pipe.open() && sent_version != 0) {
        if (pipe.write_frame(op_frame, discord_activity_command(std::nullopt, pid, ++nonce))) {
            const auto until = clock::now() + std::chrono::milliseconds(300);
            bool answered = false;
            while (!answered && !pipe.broken() && clock::now() < until) {
                while (auto frame = pipe.read_frame()) {
                    if (frame->first == op_frame) answered = true;
                }
                if (!answered) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (answered) {
                const std::lock_guard lock(mutex);
                ++status.sent;
            }
        }
    }
}

DiscordPresence::DiscordPresence(Options options) : state_(std::make_unique<State>()) {
    state_->options = std::move(options);
    if (state_->options.application_id.empty()) return;
    state_->thread = std::thread([state = state_.get()] { state->run(); });
}

DiscordPresence::~DiscordPresence() {
    {
        const std::lock_guard lock(state_->mutex);
        state_->stop = true;
    }
    state_->wake.notify_all();
    if (state_->thread.joinable()) state_->thread.join();
}

void DiscordPresence::set(std::optional<DiscordActivity> activity) {
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->wanted == activity) return;
        state_->wanted = std::move(activity);
        ++state_->wanted_version;
        state_->kicked = true;
    }
    state_->wake.notify_all();
}

DiscordPresence::Status DiscordPresence::status() const {
    const std::lock_guard lock(state_->mutex);
    return state_->status;
}

} // namespace openrts::platform
