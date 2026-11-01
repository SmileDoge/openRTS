#include "file_dialog.hpp"

#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace openrts::viewer {

std::optional<std::filesystem::path> open_file_dialog(const std::filesystem::path& folder, const std::wstring_view description,
                                                      const std::wstring_view pattern) {
#ifdef _WIN32
    // Filter: "description\0pattern\0All files\0*.*\0\0".
    std::wstring filter(description);
    filter.push_back(L'\0');
    filter.append(pattern);
    filter.push_back(L'\0');
    filter.append(L"All files");
    filter.push_back(L'\0');
    filter.append(L"*.*");
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    std::vector<wchar_t> file(32768, L'\0');
    const std::wstring initial = folder.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    // OFN_NOCHANGEDIR: the viewer keeps paths relative to its working directory.
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path(file.data());
#else
    (void)folder;
    (void)description;
    (void)pattern;
    return std::nullopt;
#endif
}

} // namespace openrts::viewer
