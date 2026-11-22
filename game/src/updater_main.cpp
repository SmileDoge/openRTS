// openrts-updater: the start menu's update without the menu or the game
// (update.hpp), for a dedicated server's computer and for anyone who would
// rather not open the game (the organiser, 2026-11-21): the latest release
// on GitHub against this build, its archive downloaded, checked (size,
// SHA-256) and laid over the program's folder, the old files renamed to
// *.old and deleted at the next start (the menu's list, so either deletes
// them).
//
// A plain Windows window by default (no GPU needed: a server's computer
// often has none worth the name): what is installed, what is new, a
// progress bar, "Обновить", "Страница релиза", "Запустить openRTS".
// Without a window, for a script or the task scheduler:
//
//   openrts-updater --check | --yes [--folder <dir>]
//
// --check: only tells (exit 0: up to date, 3: a newer release); --yes:
// installs a newer one without a question; --folder: another openRTS folder
// than the updater's own (also with the window). The lines go to the
// console it was started from ("start /wait" in a .bat waits for it).
// Exit codes: 0 done or up to date, 1 failed, 2 usage, 3 (--check) newer.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#ifdef _MSC_VER
// The system's look for the buttons and the progress bar (Common Controls 6).
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#endif

#include <openrts/core/core.hpp>
#include <openrts/core/installation.hpp>
#include <openrts/core/user_data.hpp>

#include "update.hpp"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace update = openrts::update;
using Stage = update::Updater::Stage;

constexpr const char* usage =
    "openrts-updater [--check | --yes] [--folder <папка>]\n"
    "  без параметров: окно - проверить последний релиз openRTS и установить его\n"
    "  --check     без окна: только проверить (код выхода 0 - последняя версия, 3 - есть новее)\n"
    "  --yes       без окна: установить новую версию без вопроса (сервер, планировщик, скрипт)\n"
    "  --folder    папка openRTS (по умолчанию - папка самого openrts-updater.exe)\n";

struct Options {
    bool check_only = false, yes = false;
    fs::path folder;
};

// The update's errors (update.cpp, English: the menu's tr() keys) in Russian.
std::string russian(const std::string& error) {
    static const std::pair<const char*, const char*> known[] = {
        {"Could not ask GitHub for the latest version", "не удалось спросить у GitHub последнюю версию"},
        {"Could not read the latest release", "не удалось прочитать последний релиз"},
        {"The download failed", "загрузка не удалась"},
        {"The download is incomplete", "файл загрузился не полностью"},
        {"The download is damaged (its SHA-256 does not match)", "файл повреждён (SHA-256 не совпадает)"},
        {"The update could not be installed", "обновление не удалось установить"},
    };
    for (const auto& [en, ru] : known)
        if (error == en) return ru;
    return error;
}

std::string failure(const update::Updater::Status& status) {
    return russian(status.error) + (status.detail.empty() ? std::string() : ": " + status.detail);
}

std::string megabytes(const std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", static_cast<double>(bytes) / 1048576.0);
    return text;
}

// openRTS's programs running from `folder`: they go on in the old version
// until started again (Windows lets their files be renamed, not replaced).
std::vector<std::string> running_from(const fs::path& folder) {
    std::vector<std::string> out;
#ifdef _WIN32
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    std::error_code error;
    const auto here = fs::weakly_canonical(folder, error);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        std::wstring name = entry.szExeFile;
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        if (name.rfind(L"openrts", 0) != 0) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t path[MAX_PATH * 4]{};
        DWORD size = static_cast<DWORD>(std::size(path));
        const bool got = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
        CloseHandle(process);
        if (!got || fs::weakly_canonical(fs::path(path).parent_path(), error) != here) continue;
        const auto utf8 = fs::path(entry.szExeFile).u8string();
        out.push_back(std::string(utf8.begin(), utf8.end()));
    }
    CloseHandle(snapshot);
#else
    (void)folder;
#endif
    return out;
}

// This build's version; OPENRTS_UPDATER_AS another (a check of the update
// on a folder of its own: an older version is offered the latest).
std::string current_version() {
#ifdef _WIN32
    wchar_t as[64]{};
    if (const DWORD n = GetEnvironmentVariableW(L"OPENRTS_UPDATER_AS", as, 64); n > 0 && n < 64) {
        const auto utf8 = fs::path(std::wstring(as, n)).u8string();
        return std::string(utf8.begin(), utf8.end());
    }
#else
    if (const char* as = std::getenv("OPENRTS_UPDATER_AS"); as && *as) return as;
#endif
    return std::string(openrts::core::version());
}

// A version as the releases' tags write it: "v0.33.0" (core::version()
// gives "0.33.0").
std::string tag_of(const std::string& version) {
    return !version.empty() && version[0] != 'v' && version[0] != 'V' ? "v" + version : version;
}

fs::path work_folder() { return openrts::core::find_user_data_directory() / "update"; }
fs::path old_list() { return work_folder() / "old_files.txt"; }

// ---- Without a window ------------------------------------------------------

int run_headless(const Options& options) {
    const auto current = current_version();
    std::cout << "openRTS: обновление. Папка: " << options.folder.string() << "\nЭта версия: " << tag_of(current) << "\n";
    update::remove_old_files(old_list());
    update::Updater updater;
    updater.check(current);
    while (updater.busy()) std::this_thread::sleep_for(100ms);
    auto status = updater.status();
    if (!status.release) {
        std::cout << "Не удалось проверить обновление: " << failure(status) << "\n";
        return 1;
    }
    const auto& release = *status.release;
    std::cout << "Последний релиз: " << release.tag << " (" << megabytes(release.archive_size) << " МБ)\n";
    if (status.stage != Stage::available) {
        std::cout << "Обновлять нечего: установлена последняя версия.\n";
        return 0;
    }
    std::cout << release.title << "\n" << release.summary << "\nПодробнее: " << release.page << "\n";
    if (options.check_only) return 3;
    if (update::development_build(options.folder)) {
        std::cout << "Это папка сборки из исходников: её обновляет сборка, а не релиз.\n";
        return 1;
    }
    if (!update::folder_writable(options.folder)) {
        std::cout << "В папку нельзя записывать: запустите от имени администратора или перенесите openRTS из Program Files.\n";
        return 1;
    }
    for (const auto& name : running_from(options.folder))
        std::cout << "Запущен " << name << ": он продолжит работать прежней версией, пока его не перезапустить.\n";
    updater.install(options.folder, work_folder(), old_list());
    std::uint64_t shown = 0;
    while (updater.busy()) {
        std::this_thread::sleep_for(200ms);
        status = updater.status();
        if (status.stage == Stage::downloading && status.done >= shown + (8u << 20)) {
            shown = status.done;
            std::cout << "Загрузка: " << megabytes(status.done) << " из " << megabytes(status.total) << " МБ\n";
        }
    }
    status = updater.status();
    if (status.stage != Stage::installed) {
        std::cout << "Обновление не удалось: " << failure(status) << "\nПрежние файлы на месте.\n";
        return 1;
    }
    std::cout << "Готово: " << tag_of(current) << " -> " << release.tag << ". Старые файлы (*.old) удалятся при следующем запуске.\n";
    return 0;
}

#ifdef _WIN32

// ---- The window --------------------------------------------------------------

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// An edit control's lines end in "\r\n".
std::wstring edit_text(const std::string& text) {
    std::wstring out;
    for (const wchar_t c : wide(text)) {
        if (c == L'\r') continue;
        if (c == L'\n') out += L'\r';
        out += c;
    }
    return out;
}

enum Control : int { id_update = 101, id_page, id_run, id_close };

struct Window {
    Options options;
    std::string current;
    update::Updater updater;
    HWND window{}, status_text{}, notes{}, progress{}, update_button{}, page_button{}, run_button{}, close_button{};
    HFONT font{}, bold{};
    int dpi = 96;
    Stage shown = Stage::idle;
    std::uint64_t shown_done = ~std::uint64_t{0};

    [[nodiscard]] int px(const int dip) const { return MulDiv(dip, dpi, 96); }

    void set(HWND control, const std::string& text) const { SetWindowTextW(control, wide(text).c_str()); }

    void layout() const {
        RECT client{};
        GetClientRect(window, &client);
        const int w = client.right - client.left, h = client.bottom - client.top;
        const int m = px(14), button_h = px(30);
        MoveWindow(status_text, m, m, w - 2 * m, px(58), TRUE);
        const int notes_top = m + px(64);
        const int buttons_top = h - m - button_h;
        const int progress_top = buttons_top - px(12) - px(16);
        MoveWindow(notes, m, notes_top, w - 2 * m, progress_top - px(10) - notes_top, TRUE);
        MoveWindow(progress, m, progress_top, w - 2 * m, px(16), TRUE);
        MoveWindow(update_button, m, buttons_top, px(150), button_h, TRUE);
        MoveWindow(run_button, m, buttons_top, px(150), button_h, TRUE);
        MoveWindow(page_button, m + px(160), buttons_top, px(150), button_h, TRUE);
        MoveWindow(close_button, w - m - px(110), buttons_top, px(110), button_h, TRUE);
    }

    void check() {
        shown = Stage::idle;
        updater.check(current);
        refresh();
    }

    // The text and the buttons as the update stands.
    void refresh() {
        const auto status = updater.status();
        const bool same = status.stage == shown && (status.stage != Stage::downloading || status.done == shown_done);
        if (same) return;
        shown = status.stage;
        shown_done = status.done;
        const auto& release = status.release;
        std::string head = "Эта версия: " + tag_of(current) + "\n";
        std::string body;
        bool can_update = false, can_page = release.has_value(), can_run = false;
        int bar = 0;
        switch (status.stage) {
        case Stage::idle:
        case Stage::checking:
            head += "Проверяю, есть ли новая версия...";
            break;
        case Stage::up_to_date:
            head += "Установлена последняя версия" + (release ? " (" + release->tag + ")." : std::string("."));
            can_run = true;
            break;
        case Stage::available: {
            head += "Доступна новая версия: " + release->tag + " (" + megabytes(release->archive_size) + " МБ).";
            body = release->title + "\n\n" + release->summary + "\n\nПолный список изменений — на странице релиза.";
            can_update = true;
            std::string why;
            if (update::development_build(options.folder)) {
                why = "Это папка сборки из исходников: её обновляет сборка, а не релиз.";
            } else if (!update::folder_writable(options.folder)) {
                why = "В папку нельзя записывать: запустите openrts-updater от имени администратора или перенесите openRTS из Program Files.";
            }
            if (!why.empty()) {
                can_update = false;
                body = why + "\n\n" + body;
            }
            if (const auto running = running_from(options.folder); can_update && !running.empty()) {
                std::string list;
                for (const auto& name : running) list += (list.empty() ? "" : ", ") + name;
                body = "Сейчас запущено: " + list +
                       ". Обновить можно и так, но они продолжат работать прежней версией, пока их не перезапустить "
                       "(сервер: расстановка autosave сохранит поезда).\n\n" +
                       body;
            }
            break;
        }
        case Stage::downloading:
            head += "Загрузка " + (release ? release->tag : std::string()) + ": " + megabytes(status.done) + " из " + megabytes(status.total) + " МБ";
            bar = status.total > 0 ? static_cast<int>(status.done * 1000 / status.total) : 0;
            break;
        case Stage::installing:
            head += "Устанавливаю...";
            bar = 1000;
            break;
        case Stage::installed:
            head = "Готово: " + tag_of(current) + " → " + (release ? release->tag : std::string()) + ".\nСтарые файлы (*.old) удалятся при следующем запуске.";
            if (release) body = release->title + "\n\n" + release->summary;
            body += std::string(body.empty() ? "" : "\n\n") +
                    "Если из этой папки был запущен сервер или игра, перезапустите их: до этого они работают прежней версией.";
            bar = 1000;
            can_run = true;
            break;
        case Stage::failed:
            head += "Ошибка: " + failure(status);
            body = release ? "Прежние файлы на месте. Можно попробовать ещё раз или скачать архив со страницы релиза."
                           : "Проверьте подключение к интернету и попробуйте ещё раз.";
            can_update = true; // again: the install, or the check
            break;
        }
        set(status_text, head);
        SetWindowTextW(notes, edit_text(body).c_str());
        SendMessageW(progress, PBM_SETPOS, static_cast<WPARAM>(bar), 0);
        set(update_button, status.stage == Stage::failed ? (release ? "Повторить" : "Проверить снова") : "Обновить");
        ShowWindow(update_button, can_run ? SW_HIDE : SW_SHOW);
        ShowWindow(run_button, can_run ? SW_SHOW : SW_HIDE);
        EnableWindow(update_button, can_update);
        EnableWindow(page_button, can_page);
        EnableWindow(run_button, can_run && fs::exists(options.folder / "openrts_game.exe"));
        EnableWindow(close_button, status.stage != Stage::installing);
    }

    void command(const int id) {
        const auto status = updater.status();
        switch (id) {
        case id_update:
            if (updater.busy()) return;
            if (status.stage == Stage::failed && !status.release) check();
            else updater.install(options.folder, work_folder(), old_list());
            refresh();
            break;
        case id_page:
            if (status.release) ShellExecuteW(window, L"open", wide(status.release->page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case id_run:
            update::start_process(options.folder / "openrts_game.exe", "");
            DestroyWindow(window);
            break;
        case id_close:
            DestroyWindow(window);
            break;
        default:
            break;
        }
    }
};

Window* window_of(HWND hwnd) { return reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); }

LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* w = window_of(hwnd);
    switch (message) {
    case WM_NCCREATE: {
        auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
        break;
    }
    case WM_SIZE:
        if (w && w->status_text) w->layout();
        return 0;
    case WM_GETMINMAXINFO:
        if (w) {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {w->px(460), w->px(300)};
        }
        return 0;
    case WM_TIMER:
        if (w) w->refresh();
        return 0;
    case WM_COMMAND:
        if (w && HIWORD(wparam) == BN_CLICKED) w->command(LOWORD(wparam));
        return 0;
    case WM_CTLCOLORSTATIC:
        // The read-only text on the window's own colour.
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_CLOSE:
        if (w && w->updater.status().stage == Stage::installing) return 0; // not halfway through the files
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int run_window(HINSTANCE instance, const Options& options) {
    using SetContext = BOOL(WINAPI*)(HANDLE);
    if (const auto user32 = GetModuleHandleW(L"user32.dll")) {
        if (const auto set_context = reinterpret_cast<SetContext>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))))
            set_context(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    update::remove_old_files(old_list());

    Window w;
    w.options = options;
    w.current = current_version();
    WNDCLASSEXW cls{sizeof cls};
    cls.lpfnWndProc = procedure;
    cls.hInstance = instance;
    cls.hIcon = LoadIconW(instance, L"GLFW_ICON");
    cls.hIconSm = cls.hIcon;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    cls.lpszClassName = L"openrts_updater";
    RegisterClassExW(&cls);
    const HDC screen = GetDC(nullptr);
    w.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT frame{0, 0, w.px(600), w.px(380)};
    AdjustWindowRect(&frame, style, FALSE);
    w.window = CreateWindowExW(0, cls.lpszClassName, wide("openRTS — обновление").c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                               frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, &w);
    if (!w.window) return 1;
    if (const auto user32 = GetModuleHandleW(L"user32.dll")) {
        using GetDpi = UINT(WINAPI*)(HWND);
        if (const auto get_dpi = reinterpret_cast<GetDpi>(reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow"))))
            w.dpi = static_cast<int>(get_dpi(w.window));
    }
    // The system's message font (Segoe UI), at the window's DPI.
    NONCLIENTMETRICSW metrics{sizeof metrics};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
    LOGFONTW face = metrics.lfMessageFont;
    face.lfHeight = -MulDiv(10, w.dpi, 72);
    w.font = CreateFontIndirectW(&face);
    face.lfWeight = FW_SEMIBOLD;
    w.bold = CreateFontIndirectW(&face);
    const auto make = [&](const wchar_t* cls_name, const DWORD control_style, const int id, const char* text, HFONT f) {
        HWND control = CreateWindowExW(0, cls_name, wide(text).c_str(), WS_CHILD | WS_VISIBLE | control_style, 0, 0, 10, 10, w.window,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return control;
    };
    w.status_text = make(L"STATIC", SS_LEFT, 0, "", w.bold);
    w.notes = make(L"EDIT", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, "", w.font);
    w.progress = make(PROGRESS_CLASSW, 0, 0, "", w.font);
    SendMessageW(w.progress, PBM_SETRANGE32, 0, 1000);
    w.update_button = make(L"BUTTON", BS_DEFPUSHBUTTON | WS_TABSTOP, id_update, "Обновить", w.font);
    w.run_button = make(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, id_run, "Запустить openRTS", w.font);
    w.page_button = make(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, id_page, "Страница релиза", w.font);
    w.close_button = make(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, id_close, "Закрыть", w.font);
    w.layout();
    w.check();
    SetTimer(w.window, 1, 100, nullptr);
    ShowWindow(w.window, SW_SHOWNORMAL);
    UpdateWindow(w.window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w.window, &message)) continue; // Tab between the buttons
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    w.updater.cancel();
    DeleteObject(w.font);
    DeleteObject(w.bold);
    const auto stage = w.updater.status().stage;
    return stage == Stage::failed ? 1 : 0;
}

#endif

// The arguments; false: not ours (the usage shown).
bool parse(const std::vector<std::string>& args, Options& options) {
    options.folder = openrts::core::program_directory();
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--check") {
            options.check_only = true;
        } else if (args[i] == "--yes" || args[i] == "-y") {
            options.yes = true;
        } else if (args[i] == "--folder" && i + 1 < args.size()) {
            const auto& text = args[++i];
            options.folder = fs::path(std::u8string(text.begin(), text.end()));
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    // The arguments as UTF-8 (a folder with Cyrillic in its name).
    std::vector<std::string> args;
    int count = 0;
    if (wchar_t** list = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) {
            const auto utf8 = fs::path(list[i]).u8string();
            args.emplace_back(utf8.begin(), utf8.end());
        }
        LocalFree(list);
    }
    Options options;
    const bool ours = parse(args, options);
    if (ours && !options.check_only && !options.yes) return run_window(instance, options);
    // Without a window: the lines where its output was sent (a file, a
    // pipe), else to the console it was started from.
    const HANDLE given = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool redirected = given && given != INVALID_HANDLE_VALUE && GetFileType(given) != FILE_TYPE_UNKNOWN;
    if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
        SetConsoleOutputCP(CP_UTF8);
        std::cout << "\n";
    }
    if (!ours) {
        std::cout << usage;
        return 2;
    }
    return run_headless(options);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
    Options options;
    if (!parse(args, options) || (!options.check_only && !options.yes)) {
        std::cout << usage;
        return 2;
    }
    return run_headless(options);
}
#endif
