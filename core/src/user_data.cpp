#include <openrts/core/user_data.hpp>

#include <openrts/core/installation.hpp>

#include <cstdlib>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace openrts::core {

namespace fs = std::filesystem;

fs::path per_user_directory() {
#ifdef _WIN32
    std::wstring local(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (length > 0 && length < local.size()) return fs::path(local.substr(0, length)) / L"openRTS";
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) return fs::path(data) / "openRTS";
    if (const char* home = std::getenv("HOME"); home && *home) return fs::path(home) / ".local" / "share" / "openRTS";
#endif
    return {};
}

fs::path find_user_data_directory() {
    std::error_code error;
    const auto program = program_directory();
    if (!program.empty() && fs::exists(program / "portable.txt", error)) return program;
    if (const auto user = per_user_directory(); !user.empty()) {
        fs::create_directories(user, error);
        if (fs::is_directory(user, error)) return user;
    }
    return program.empty() ? fs::current_path(error) : program;
}

} // namespace openrts::core
