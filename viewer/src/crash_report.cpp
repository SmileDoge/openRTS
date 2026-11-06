#include "crash_report.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>

namespace openrts::viewer {
namespace {

// ntdll's record of the modules unloaded lately (RtlGetUnloadEventTrace).
struct UnloadEvent {
    void* base;
    SIZE_T size;
    ULONG sequence;
    ULONG time_stamp;
    ULONG check_sum;
    WCHAR name[32];
    ULONG version[2];
};

// The module a code address was in when it is gone now.
void report_unloaded(const DWORD64 address) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using Get = UnloadEvent* (NTAPI*)();
    const auto get = ntdll ? reinterpret_cast<Get>(GetProcAddress(ntdll, "RtlGetUnloadEventTrace")) : nullptr;
    const UnloadEvent* events = get ? get() : nullptr;
    if (!events) return;
    for (int i = 0; i < 64; ++i) {
        const auto& e = events[i];
        if (!e.base) continue;
        const auto base = reinterpret_cast<DWORD64>(e.base);
        if (address >= base && address < base + e.size) {
            std::fprintf(stderr, "  in %ls, unloaded before the crash (+0x%llx)\n", e.name,
                         static_cast<unsigned long long>(address - base));
        }
    }
}

// Return addresses on the stack into loaded modules (when the frames do not
// unwind: code of a module that is gone).
void scan_stack(const HANDLE process, const DWORD64 rsp) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    int shown = 0;
    for (int k = 0; k < 2048 && shown < 24; ++k) {
        DWORD64 value = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<const void*>(rsp + 8ull * k), &value, sizeof value, &got)) break;
        const auto base = SymGetModuleBase64(process, value);
        if (!base) continue;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        if (!SymFromAddr(process, value, &displacement, symbol)) continue;
        char module[MAX_PATH] = "?";
        GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module, MAX_PATH);
        const char* file_name = module;
        for (const char* p = module; *p; ++p)
            if (*p == '\\' || *p == '/') file_name = p + 1;
        std::fprintf(stderr, "  stack +0x%x: %s +0x%llx [%s]\n", k * 8, symbol->Name, static_cast<unsigned long long>(displacement),
                     file_name);
        ++shown;
    }
}

LONG WINAPI report(EXCEPTION_POINTERS* info) {
    static bool reported = false;
    if (reported) return EXCEPTION_CONTINUE_SEARCH;
    reported = true;
    const auto* record = info->ExceptionRecord;
    std::fprintf(stderr, "openRTS crashed: exception 0x%08lX at %p\n", static_cast<unsigned long>(record->ExceptionCode),
                 record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        std::fprintf(stderr, "  %s address %p\n", record->ExceptionInformation[0] ? "writing" : "reading",
                     reinterpret_cast<void*>(record->ExceptionInformation[1]));
    }
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    if (!SymGetModuleBase64(process, info->ContextRecord->Rip)) {
        report_unloaded(info->ContextRecord->Rip);
        scan_stack(process, info->ContextRecord->Rsp);
    }
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    for (int depth = 0; depth < 48; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const char* name = SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : "?";
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD column = 0;
        char module[MAX_PATH] = "?";
        if (const auto base = SymGetModuleBase64(process, address)) {
            GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module, MAX_PATH);
        }
        const char* file_name = module;
        for (const char* p = module; *p; ++p)
            if (*p == '\\' || *p == '/') file_name = p + 1;
        if (SymGetLineFromAddr64(process, address, &column, &line)) {
            std::fprintf(stderr, "  #%02d %s (%s:%lu) [%s]\n", depth, name, line.FileName, static_cast<unsigned long>(line.LineNumber),
                         file_name);
        } else {
            std::fprintf(stderr, "  #%02d %s +0x%llx [%s]\n", depth, name, static_cast<unsigned long long>(displacement), file_name);
        }
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install_crash_report() {
    SetUnhandledExceptionFilter(report);
}

} // namespace openrts::viewer

#else

namespace openrts::viewer {
void install_crash_report() {}
} // namespace openrts::viewer

#endif
