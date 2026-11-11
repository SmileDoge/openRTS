#include "file_dialog.hpp"

#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shobjidl.h>
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

std::optional<std::filesystem::path> open_folder_dialog(const std::filesystem::path& folder, const std::wstring_view title) {
#ifdef _WIN32
    // The Vista dialog in folder mode (COM, single-threaded on this thread
    // for the call unless the thread has COM already).
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::optional<std::filesystem::path> result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        if (!title.empty()) dialog->SetTitle(std::wstring(title).c_str());
        std::error_code error;
        if (!folder.empty() && std::filesystem::is_directory(folder, error)) {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(folder.wstring().c_str(), nullptr, IID_PPV_ARGS(&start)))) {
                dialog->SetFolder(start);
                start->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
#else
    (void)folder;
    (void)title;
    return std::nullopt;
#endif
}

} // namespace openrts::viewer
