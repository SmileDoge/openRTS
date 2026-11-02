// openrts-dllhost32: runs legacy RTS locomotive DLLs (32-bit) for openRTS.
//
//   openrts-dllhost32 --probe <dll | folder>... [--rts <RTS folder>]
//       loads each DLL (folders recursively), prints its DLLVersion answers
//       against the host's layouts and whether InitRTSLibrary accepts the
//       host; calls nothing else in the DLL.
//   openrts-dllhost32 --serve <name> --parent <pid>
//       serves the game over shared memory (include/openrts/dllhost/protocol.hpp)
//       until the game says shutdown or its process ends.

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include "host.hpp"

using namespace openrts::dllhost;

namespace {

DWORD call_init(abi::InitRTSLibraryFn init, Module* module, bool& accepted) {
    __try {
        accepted = init(GetModuleHandleW(nullptr), module);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

int probe(const std::vector<std::wstring>& targets, const std::wstring& rts_root) {
    if (!rts_root.empty()) SetDllDirectoryW(rts_root.c_str());
    std::vector<std::wstring> files;
    for (const auto& t : targets) {
        std::error_code ec;
        if (std::filesystem::is_directory(t, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(t, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec) && _wcsicmp(it->path().extension().c_str(), L".dll") == 0)
                    files.push_back(it->path().wstring());
            }
        } else {
            files.push_back(t);
        }
    }
    Host host; // the host functions answer InitRTSLibrary's lookups
    int ok = 0, refused = 0, not_rts = 0;
    for (const auto& path : files) {
        Module m;
        std::wstring exports;
        wprintf(L"%ls\n", path.c_str());
        if (!load_module(m, path, &exports)) {
            wprintf(L"  refused: %ls\n", m.error.c_str());
            if (m.error.find(L"not an RTS DLL") != std::wstring::npos)
                ++not_rts;
            else
                ++refused;
            continue;
        }
        wprintf(L"  exports: %ls\n", exports.c_str());
        wprintf(L"  TS_VERSION %u, adapter compliance %u, stack %u\n", m.version[abi::v_version],
                m.version[abi::v_adapter_compliance], m.version[abi::v_stack_size]);
        const auto mismatch = layout_mismatch(m);
        const auto refusal = choose_adapters(m);
        if (mismatch.empty()) {
            wprintf(L"  layout: matches the host\n");
        } else if (refusal.empty()) {
            wprintf(L"  layout: TS_VERSION %u, through the adapters (loco %u, cabin %u, signals %u)\n",
                    m.version[abi::v_version], m.loco_variant, m.cab_variant, m.signals_variant);
        } else {
            wprintf(L"  layout: refused: %ls\n", refusal.c_str());
        }
        auto* init = reinterpret_cast<abi::InitRTSLibraryFn>(GetProcAddress(m.handle, "_InitRTSLibrary"));
        bool accepted = false;
        const DWORD crash = call_init(init, &m, accepted);
        if (crash)
            wprintf(L"  InitRTSLibrary: crashed 0x%08X\n", crash);
        else
            wprintf(L"  InitRTSLibrary: %ls\n", accepted ? L"ok" : L"returned false");
        if (refusal.empty() && accepted && !crash)
            ++ok;
        else
            ++refused;
    }
    wprintf(L"\n%zu DLL(s): %d accepted, %d refused, %d not RTS DLLs\n", files.size(), ok, refused, not_rts);
    return refused ? 2 : 0;
}

struct Shared {
    HANDLE mapping = nullptr;
    std::uint8_t* view = nullptr;
    HANDLE request = nullptr;
    HANDLE reply = nullptr;
};

void set_message(Header& h, const std::wstring& text) {
    const std::size_t n = (std::min<std::size_t>)(text.size(), std::size(h.message) - 1);
    std::memcpy(h.message, text.data(), n * sizeof(char16_t));
    h.message[n] = 0;
}

std::u16string u16(const std::wstring& s) { return {reinterpret_cast<const char16_t*>(s.data()), s.size()}; }

int serve(const std::wstring& name, DWORD parent_pid) {
    Shared s;
    s.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!s.mapping) return 3;
    s.view = static_cast<std::uint8_t*>(MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, 0, shared_size));
    s.request = OpenEventW(EVENT_ALL_ACCESS, FALSE, (name + L".req").c_str());
    s.reply = OpenEventW(EVENT_ALL_ACCESS, FALSE, (name + L".rep").c_str());
    HANDLE parent = parent_pid ? OpenProcess(SYNCHRONIZE, FALSE, parent_pid) : nullptr;
    if (!s.view || !s.request || !s.reply) return 3;
    // A DLL's own error boxes would block the game: no dialogs for missing files.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    Host host;
    auto* header = reinterpret_cast<Header*>(s.view);
    std::uint8_t* payload = s.view + payload_offset;
    for (;;) {
        HANDLE waits[2] = {s.request, parent};
        const DWORD r = WaitForMultipleObjects(parent ? 2 : 1, waits, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0) break; // the game ended
        if (header->magic != protocol_magic || header->version != protocol_version) {
            header->status = -100;
            set_message(*header, L"protocol mismatch");
            header->reply_size = 0;
            SetEvent(s.reply);
            continue;
        }
        const auto command = static_cast<Command>(header->command);
        Reader in(payload, (std::min)(header->request_size, payload_capacity));
        // The request is parsed before the reply overwrites the payload.
        int status = 0;
        std::vector<std::uint8_t> out_buffer(payload_capacity);
        Writer out(out_buffer.data(), payload_capacity);
        switch (command) {
        case Command::hello: {
            out.put(protocol_version);
            out.put(static_cast<std::uint32_t>(sizeof(CarInput)));
            out.put(static_cast<std::uint32_t>(sizeof(EngineOutput)));
            out.put(static_cast<std::uint32_t>(sizeof(SwitchState)));
            out.put(static_cast<std::uint32_t>(sizeof(DisplayState)));
            out.put(static_cast<std::uint32_t>(sizeof(SignalInput)));
            out.put(static_cast<std::uint32_t>(sizeof(SpeedLimitInput)));
            out.put(GetCurrentProcessId());
            break;
        }
        case Command::setup: {
            SetupRequest request;
            read(in, request);
            std::vector<ModuleReport> modules;
            if (in.failed() || !host.setup(request, modules)) status = -1;
            write_list(out, modules);
            break;
        }
        case Command::enter: {
            EnterRequest request;
            read(in, request);
            StepReply reply;
            if (in.failed() || !host.enter(request, request.cars, reply)) status = -1;
            write(out, reply);
            break;
        }
        case Command::step: {
            StepRequest request;
            read(in, request);
            StepReply reply;
            if (in.failed() || !host.step(request, reply)) status = -1;
            write(out, reply);
            break;
        }
        case Command::can_switch: {
            const auto request = in.get<CanSwitchRequest>();
            bool allowed = false;
            if (in.failed() || !host.can_switch(request, allowed)) status = -1;
            out.put(static_cast<std::uint8_t>(allowed));
            break;
        }
        case Command::drag_switch: {
            const auto request = in.get<DragSwitchRequest>();
            bool moved = false;
            if (in.failed() || !host.drag_switch(request, moved)) status = -1;
            out.put(static_cast<std::uint8_t>(moved));
            out.put(host.switch_state(request.car, request.id)); // seen at once, not a step later
            break;
        }
        case Command::shutdown:
            header->status = 0;
            header->reply_size = 0;
            set_message(*header, host.take_log());
            SetEvent(s.reply);
            return 0;
        default:
            status = -2;
            break;
        }
        if (out.overflow()) status = -3;
        std::memcpy(payload, out_buffer.data(), out.size());
        header->reply_size = out.size();
        header->status = status;
        set_message(*header, host.take_log());
        SetEvent(s.reply);
    }
    return 0;
}

void usage() {
    wprintf(L"openrts-dllhost32 --probe <dll|folder>... [--rts <RTS folder>]\n"
            L"openrts-dllhost32 --serve <name> --parent <pid>\n");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 1;
    }
    std::wstring rts_root;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == L"--rts") {
            rts_root = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            break;
        }
    }
    if (args[0] == L"--probe" && args.size() > 1) return probe({args.begin() + 1, args.end()}, rts_root);
    if (args[0] == L"--serve" && args.size() > 1) {
        DWORD parent = 0;
        for (std::size_t i = 2; i + 1 < args.size(); ++i)
            if (args[i] == L"--parent") parent = static_cast<DWORD>(std::wcstoul(args[i + 1].c_str(), nullptr, 10));
        return serve(args[1], parent);
    }
    usage();
    return 1;
}
