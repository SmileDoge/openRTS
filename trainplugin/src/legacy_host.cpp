#include <openrts/trainplugin/legacy_host.hpp>

#include <openrts/core/profile.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <atomic>
#include <cstring>

namespace openrts::trainplugin {

using namespace dllhost;

std::u16string utf8_to_utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t code = c;
        std::size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            code = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            code = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            code = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
            n = 2;
        }
        i += n;
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}

std::string utf16_to_utf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        std::uint32_t code = s[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

std::string LegacyHost::take_messages() { return std::exchange(messages_, {}); }

#ifdef _WIN32

std::filesystem::path LegacyHost::default_executable() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    std::filesystem::path self(std::wstring(buffer, n));
    return self.parent_path() / L"openrts-dllhost32.exe";
}

std::unique_ptr<LegacyHost> LegacyHost::start(std::string* error, const std::filesystem::path& executable) {
    auto fail = [&](const std::string& why) -> std::unique_ptr<LegacyHost> {
        if (error) *error = why;
        return nullptr;
    };
    const auto exe = executable.empty() ? default_executable() : executable;
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) return fail("openrts-dllhost32.exe not found: " + exe.string());

    static std::atomic<unsigned> counter{0};
    const std::wstring name = L"Local\\openrts-dllhost-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(counter++);
    std::unique_ptr<LegacyHost> host(new LegacyHost());
    host->mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, shared_size, name.c_str());
    if (!host->mapping_) return fail("CreateFileMapping failed");
    host->view_ = static_cast<std::uint8_t*>(MapViewOfFile(host->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, shared_size));
    host->request_ = CreateEventW(nullptr, FALSE, FALSE, (name + L".req").c_str());
    host->reply_ = CreateEventW(nullptr, FALSE, FALSE, (name + L".rep").c_str());
    if (!host->view_ || !host->request_ || !host->reply_) return fail("shared memory setup failed");

    std::wstring command = L"\"" + exe.wstring() + L"\" --serve " + name + L" --parent " +
                           std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // The DLLs run with their own folder as current directory only through
    // their absolute paths; the host starts in the game's directory.
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        return fail("cannot start openrts-dllhost32.exe (error " + std::to_string(GetLastError()) + ")");
    }
    CloseHandle(pi.hThread);
    host->process_ = pi.hProcess;
    host->alive_ = true;

    std::uint32_t version = 0, input_size = 0, output_size = 0, switch_size = 0, display_size = 0, signal_size = 0,
                  limit_size = 0;
    const bool ok = host->call(
        Command::hello, [](Writer&) {},
        [&](Reader& r) {
            version = r.get<std::uint32_t>();
            input_size = r.get<std::uint32_t>();
            output_size = r.get<std::uint32_t>();
            switch_size = r.get<std::uint32_t>();
            display_size = r.get<std::uint32_t>();
            signal_size = r.get<std::uint32_t>();
            limit_size = r.get<std::uint32_t>();
        },
        10000);
    if (!ok) return fail("dllhost: " + host->error_);
    if (version != protocol_version || input_size != sizeof(CarInput) || output_size != sizeof(EngineOutput) ||
        switch_size != sizeof(SwitchState) || display_size != sizeof(DisplayState) ||
        signal_size != sizeof(SignalInput) || limit_size != sizeof(SpeedLimitInput)) {
        host->kill("protocol layout differs between the game and openrts-dllhost32.exe (rebuild both)");
        return fail(host->error_);
    }
    return host;
}

LegacyHost::~LegacyHost() {
    if (alive_) {
        call(Command::shutdown, [](Writer&) {}, [](Reader&) {}, 2000);
        if (process_ && WaitForSingleObject(process_, 2000) == WAIT_TIMEOUT) TerminateProcess(process_, 1);
    }
    if (view_) UnmapViewOfFile(view_);
    for (void* h : {mapping_, request_, reply_, process_})
        if (h) CloseHandle(h);
}

void LegacyHost::kill(const std::string& reason) {
    error_ = reason;
    alive_ = false;
    if (process_) TerminateProcess(process_, 1);
}

template <class Write, class Read>
bool LegacyHost::call(Command command, Write&& write, Read&& read, std::uint32_t timeout_ms) {
    if (!alive_) return false;
    core::SequentialZone part;
    part.open("DLL: write the request");
    auto* header = reinterpret_cast<Header*>(view_);
    std::uint8_t* payload = view_ + payload_offset;
    Writer w(payload, payload_capacity);
    write(w);
    if (w.overflow()) {
        error_ = "request too large";
        return false;
    }
    header->magic = protocol_magic;
    header->version = protocol_version;
    header->command = static_cast<std::uint32_t>(command);
    header->status = 0;
    header->request_size = w.size();
    header->reply_size = 0;
    header->sequence = ++sequence_;
    header->message[0] = 0;
    // The host's own time (the DLLs' calls) and two thread switches.
    part.open("DLL: waiting for the host");
    SetEvent(request_);
    HANDLE waits[2] = {reply_, process_};
    const DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
    part.open("DLL: read the reply");
    if (r == WAIT_OBJECT_0 + 1) {
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        alive_ = false;
        error_ = command == Command::shutdown ? std::string{} : "openrts-dllhost32 ended (exit code " + std::to_string(code) + ")";
        return command == Command::shutdown;
    }
    if (r != WAIT_OBJECT_0) {
        kill("openrts-dllhost32 did not answer in " + std::to_string(timeout_ms) + " ms (a DLL hung?)");
        return false;
    }
    std::size_t n = 0;
    while (n < std::size(header->message) && header->message[n]) ++n;
    if (n) messages_ += utf16_to_utf8({header->message, n});
    Reader rd(payload, (std::min)(header->reply_size, payload_capacity));
    read(rd);
    if (header->status != 0) {
        error_ = "host status " + std::to_string(header->status);
        return false;
    }
    if (rd.failed()) {
        error_ = "malformed reply";
        return false;
    }
    return true;
}

#else

std::filesystem::path LegacyHost::default_executable() { return {}; }
std::unique_ptr<LegacyHost> LegacyHost::start(std::string* error, const std::filesystem::path&) {
    if (error) *error = "legacy RTS DLLs need Windows";
    return nullptr;
}
LegacyHost::~LegacyHost() = default;
void LegacyHost::kill(const std::string& reason) { error_ = reason; }
template <class Write, class Read>
bool LegacyHost::call(Command, Write&&, Read&&, std::uint32_t) {
    return false;
}

#endif

bool LegacyHost::setup(const SetupRequest& request, std::vector<ModuleReport>& modules) {
    return call(
        Command::setup, [&](Writer& w) { write(w, request); }, [&](Reader& r) { read_list(r, modules); }, 20000);
}

bool LegacyHost::enter(const EnterRequest& request, StepReply& reply) {
    return call(
        Command::enter, [&](Writer& w) { write(w, request); }, [&](Reader& r) { read(r, reply); }, 20000);
}

bool LegacyHost::reconsist(const ReconsistRequest& request, StepReply& reply) {
    return call(
        Command::reconsist, [&](Writer& w) { write(w, request); }, [&](Reader& r) { read(r, reply); }, 20000);
}

bool LegacyHost::step(const StepRequest& request, StepReply& reply) {
    return call(
        Command::step, [&](Writer& w) { write(w, request); }, [&](Reader& r) { read(r, reply); }, step_timeout_);
}

bool LegacyHost::can_switch(const CanSwitchRequest& request, bool& allowed) {
    return call(
        Command::can_switch, [&](Writer& w) { w.put(request); },
        [&](Reader& r) { allowed = r.get<std::uint8_t>() != 0; }, step_timeout_);
}

bool LegacyHost::drag_switch(const DragSwitchRequest& request, bool& moved, SwitchState& state) {
    return call(
        Command::drag_switch, [&](Writer& w) { w.put(request); },
        [&](Reader& r) {
            moved = r.get<std::uint8_t>() != 0;
            state = r.get<SwitchState>();
        },
        step_timeout_);
}

} // namespace openrts::trainplugin
