// openrts-ace: ACE textures (MSTS / RTS) to PNG and back.
//
//   openrts-ace <file.ace>                     PNG beside it, opened in the image viewer
//                                              (what a double click does once registered)
//   openrts-ace to-png <file.ace | folder> [--out <folder>] [--mips] [--recursive]
//   openrts-ace to-ace <file.png | folder> [--out <folder>] [--format dxt1|rgb|rgb-mask|rgba|auto]
//                                          [--no-mips] [--no-zlib] [--recursive]
//   openrts-ace <file.png | folder>            to-ace, asking for the format
//   openrts-ace --register | --unregister      .ace opened with it; "to ACE" on PNGs and folders,
//                                              "to PNG" on ACEs and folders (this user only)
//
// An .ace replaced by to-ace is kept once as <name>.ace.bak.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/ace_writer.hpp>
#include <openrts/formats/png_file.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace {

namespace fs = std::filesystem;
using openrts::formats::AceFormat;

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] fs::path from_utf8(const std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

[[nodiscard]] std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

[[nodiscard]] bool has_extension(const fs::path& path, const std::string_view extension) {
    return lower(utf8(path.extension())) == extension;
}

bool write_file(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

[[nodiscard]] const char* format_name(const AceFormat format) {
    switch (format) {
    case AceFormat::dxt1: return "DXT1";
    case AceFormat::rgb: return "RGB";
    case AceFormat::rgb_mask: return "RGB + маска";
    case AceFormat::rgba: return "RGBA";
    }
    return "";
}

[[nodiscard]] std::optional<std::optional<AceFormat>> parse_format(const std::string& text) {
    const auto word = lower(text);
    if (word == "dxt1") return std::optional<AceFormat>{AceFormat::dxt1};
    if (word == "rgb") return std::optional<AceFormat>{AceFormat::rgb};
    if (word == "rgb-mask" || word == "mask") return std::optional<AceFormat>{AceFormat::rgb_mask};
    if (word == "rgba") return std::optional<AceFormat>{AceFormat::rgba};
    if (word == "auto") return std::optional<AceFormat>{};
    return std::nullopt;
}

struct Options {
    std::optional<fs::path> out;
    bool mips = false;      // to-png: the mipmap levels too
    bool recursive = false;
    bool format_given = false;
    std::optional<AceFormat> format; // none: auto
    openrts::formats::AceWriteOptions write;
};

// The files of a folder (or the file itself) with that extension.
[[nodiscard]] std::vector<fs::path> inputs(const fs::path& path, const std::string_view extension, const bool recursive) {
    std::vector<fs::path> files;
    std::error_code error;
    if (fs::is_directory(path, error)) {
        const auto add = [&](const fs::directory_entry& entry) {
            if (entry.is_regular_file(error) && has_extension(entry.path(), extension)) files.push_back(entry.path());
        };
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(path, error)) add(entry);
        } else {
            for (const auto& entry : fs::directory_iterator(path, error)) add(entry);
        }
        std::sort(files.begin(), files.end());
    } else if (has_extension(path, extension)) {
        files.push_back(path);
    }
    return files;
}

[[nodiscard]] fs::path output_for(const fs::path& input, const Options& options, const std::string_view extension) {
    auto name = input.filename();
    name.replace_extension(fs::path(std::string(extension)));
    return options.out ? *options.out / name : input.parent_path() / name;
}

// One ACE as PNG (and its file's mipmap levels with --mips): the PNG's path.
std::optional<fs::path> ace_to_png(const fs::path& input, const Options& options) {
    const auto decoded = openrts::formats::decode_structured_ace_rgba_path(input);
    if (!decoded.ok()) {
        std::cout << utf8(input) << ": " << decoded.error << '\n';
        return std::nullopt;
    }
    const auto& image = *decoded.image;
    auto target = output_for(input, options, ".png");
    if (!write_file(target, openrts::formats::encode_png(image.width, image.height, image.pixels))) {
        // A folder that cannot be written to (the game's own): the temp folder.
        std::error_code error;
        const auto spare = fs::temp_directory_path(error) / "openRTS ace";
        fs::create_directories(spare, error);
        target = spare / target.filename();
        if (!write_file(target, openrts::formats::encode_png(image.width, image.height, image.pixels))) {
            std::cout << utf8(input) << ": не удалось записать PNG\n";
            return std::nullopt;
        }
    }
    std::cout << utf8(input) << " -> " << utf8(target) << " (" << image.width << " x " << image.height << ")\n";
    if (options.mips) {
        auto w = image.width, h = image.height;
        for (std::size_t k = 0; k < image.mips.size(); ++k) {
            w = (std::max)(w / 2, 1u);
            h = (std::max)(h / 2, 1u);
            auto name = target.stem();
            name += "_mip" + std::to_string(k + 1) + ".png";
            write_file(target.parent_path() / name, openrts::formats::encode_png(w, h, image.mips[k]));
        }
        std::cout << "  мипмапы: " << image.mips.size() << (image.mips_from_file ? " (из файла)" : " (построены)") << '\n';
    }
    return target;
}

bool png_to_ace(const fs::path& input, const Options& options) {
    const auto decoded = openrts::formats::decode_png_path(input);
    if (!decoded.ok()) {
        std::cout << utf8(input) << ": " << decoded.error << '\n';
        return false;
    }
    const auto& image = *decoded.image;
    auto write = options.write;
    write.format = options.format ? *options.format : openrts::formats::suggested_ace_format(image.pixels);
    const auto encoded = openrts::formats::encode_ace(image.width, image.height, image.pixels, write);
    if (!encoded.ok()) {
        std::cout << utf8(input) << ": " << encoded.error << '\n';
        return false;
    }
    const auto target = output_for(input, options, ".ace");
    std::error_code error;
    // The texture it replaces, kept once.
    if (fs::exists(target, error)) {
        auto backup = target;
        backup += ".bak";
        if (!fs::exists(backup, error)) fs::copy_file(target, backup, error);
    }
    if (!write_file(target, encoded.bytes)) {
        std::cout << utf8(target) << ": не удалось записать\n";
        return false;
    }
    std::cout << utf8(input) << " -> " << utf8(target) << " (" << format_name(write.format) << ", " << image.width << " x "
              << image.height << ", " << encoded.bytes.size() / 1024 << " КБ)\n";
    for (const auto& note : encoded.notes) {
        // The writer's notes (English), as the player reads them.
        if (note.rfind("not a power of two", 0) == 0) std::cout << "  размер не степень двойки: без мипмапов (RTS построит их сам)\n";
        else if (note.rfind("RGB: its transparency", 0) == 0) std::cout << "  RGB: прозрачность отброшена\n";
        else std::cout << "  " << note << '\n';
    }
    return true;
}

// Asked in the console: the format for to-ace (auto by the image).
[[nodiscard]] std::optional<AceFormat> ask_format(const std::string& what) {
    std::cout << "Формат ACE для " << what << ":\n"
              << "  1 - DXT1: сжатие, в 4-6 раз меньше; прозрачность только 0/1 (маска)\n"
              << "  2 - RGB без сжатия, без прозрачности\n"
              << "  3 - RGB + маска: без потерь цвета, прозрачность 0/1\n"
              << "  4 - RGBA: без сжатия, полная прозрачность (стёкла, дым)\n"
              << "  Enter - сам: DXT1, а если есть полупрозрачное - RGBA\n> " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    if (line == "1") return AceFormat::dxt1;
    if (line == "2") return AceFormat::rgb;
    if (line == "3") return AceFormat::rgb_mask;
    if (line == "4") return AceFormat::rgba;
    return std::nullopt;
}

#ifdef _WIN32
[[nodiscard]] std::wstring widen(const std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

bool set_key(const std::wstring& key, const std::wstring& name, const std::wstring& value) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const bool ok = RegSetValueExW(handle, name.empty() ? nullptr : name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes)
                    == ERROR_SUCCESS;
    RegCloseKey(handle);
    return ok;
}

// The keys this user's registration makes (HKEY_CURRENT_USER\Software\Classes).
constexpr const wchar_t* prog_id = L"openRTS.ace";
constexpr const wchar_t* classes = L"Software\\Classes\\";

[[nodiscard]] std::optional<std::wstring> get_key(const std::wstring& key, const std::wstring& name) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return std::nullopt;
    wchar_t value[512]{};
    DWORD size = sizeof(value) - sizeof(wchar_t);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(handle, name.empty() ? nullptr : name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(value), &size)
                        == ERROR_SUCCESS
                    && type == REG_SZ;
    RegCloseKey(handle);
    if (!ok) return std::nullopt;
    return std::wstring(value);
}

bool register_types(const fs::path& exe) {
    const std::wstring program = L"\"" + exe.wstring() + L"\"";
    const std::wstring root = std::wstring(classes) + prog_id;
    bool ok = set_key(root, L"", L"Текстура ACE (MSTS / RTS)");
    ok &= set_key(root + L"\\DefaultIcon", L"", exe.wstring() + L",0");
    ok &= set_key(root + L"\\shell\\open", L"", L"Открыть как PNG");
    ok &= set_key(root + L"\\shell\\open\\command", L"", program + L" \"%1\"");
    ok &= set_key(root + L"\\shell\\openrts.topng", L"", L"Преобразовать в PNG (openRTS)");
    ok &= set_key(root + L"\\shell\\openrts.topng\\command", L"", program + L" to-png \"%1\"");
    // Another program this user had for .ace: kept, given back by --unregister.
    const std::wstring ace = std::wstring(classes) + L".ace";
    if (const auto before = get_key(ace, L""); before && *before != prog_id && !before->empty())
        ok &= set_key(ace, L"openRTS.previous", *before);
    ok &= set_key(ace, L"", prog_id);
    ok &= set_key(std::wstring(classes) + L".ace\\OpenWithProgids", prog_id, L"");
    const std::wstring png = std::wstring(classes) + L"SystemFileAssociations\\.png\\shell\\openrts.toace";
    ok &= set_key(png, L"", L"Преобразовать в ACE (openRTS)");
    ok &= set_key(png + L"\\command", L"", program + L" to-ace \"%1\"");
    const std::wstring folder = std::wstring(classes) + L"Directory\\shell\\";
    ok &= set_key(folder + L"openrts.toace", L"", L"Все PNG папки в ACE (openRTS)");
    ok &= set_key(folder + L"openrts.toace\\command", L"", program + L" to-ace \"%1\"");
    ok &= set_key(folder + L"openrts.topng", L"", L"Все ACE папки в PNG (openRTS)");
    ok &= set_key(folder + L"openrts.topng\\command", L"", program + L" to-png \"%1\"");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

void unregister_types() {
    const auto remove = [](const std::wstring& key) { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); };
    remove(std::wstring(classes) + prog_id);
    remove(std::wstring(classes) + L"SystemFileAssociations\\.png\\shell\\openrts.toace");
    remove(std::wstring(classes) + L"Directory\\shell\\openrts.toace");
    remove(std::wstring(classes) + L"Directory\\shell\\openrts.topng");
    // .ace pointed at us only: then it is let go.
    HKEY handle = nullptr;
    const std::wstring ace = std::wstring(classes) + L".ace";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ace.c_str(), 0, KEY_READ | KEY_WRITE, &handle) == ERROR_SUCCESS) {
        wchar_t value[64]{};
        DWORD size = sizeof(value);
        if (RegQueryValueExW(handle, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS
            && std::wstring(value) == prog_id) {
            // The program it had before, back; else no default.
            wchar_t previous[512]{};
            DWORD previous_size = sizeof(previous) - sizeof(wchar_t);
            if (RegQueryValueExW(handle, L"openRTS.previous", nullptr, nullptr, reinterpret_cast<BYTE*>(previous), &previous_size) == ERROR_SUCCESS) {
                const std::wstring text(previous);
                RegSetValueExW(handle, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                               static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
            } else {
                RegDeleteValueW(handle, nullptr);
            }
        }
        RegDeleteValueW(handle, L"openRTS.previous");
        HKEY with = nullptr;
        if (RegOpenKeyExW(handle, L"OpenWithProgids", 0, KEY_WRITE, &with) == ERROR_SUCCESS) {
            RegDeleteValueW(with, prog_id);
            RegCloseKey(with);
        }
        RegCloseKey(handle);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Started from Explorer (a double click, the context menu): the console is
// this program's own and closes with it, so it waits for Enter first.
[[nodiscard]] bool own_console() {
    DWORD processes[2];
    return GetConsoleProcessList(processes, 2) <= 1;
}
#endif

bool no_wait = false; // --no-wait: never waits for Enter (the start menu runs it unseen)

void wait_if_own_console() {
#ifdef _WIN32
    if (no_wait || !own_console()) return;
    std::cout << "\nНажмите Enter, чтобы закрыть." << std::flush;
    std::string line;
    std::getline(std::cin, line);
#endif
}

void usage() {
    std::cout << "openrts-ace: текстуры ACE (MSTS / RTS) в PNG и обратно\n\n"
                 "  openrts-ace <файл.ace>                 PNG рядом и открыть его\n"
                 "  openrts-ace to-png <файл.ace | папка> [--out <папка>] [--mips] [--recursive]\n"
                 "  openrts-ace to-ace <файл.png | папка> [--out <папка>] [--format dxt1|rgb|rgb-mask|rgba|auto]\n"
                 "                                         [--no-mips] [--no-zlib] [--recursive]\n"
                 "  openrts-ace <файл.png | папка>         в ACE, спросив формат\n"
                 "  openrts-ace --register | --unregister  открывать .ace этой программой, пункты в меню\n"
                 "                                         PNG и папок (только для этого пользователя;\n"
                 "                                         прежняя программа для .ace возвращается при снятии)\n"
                 "  --no-wait                              не ждать Enter в конце\n\n"
                 "Заменённый файл .ace сохраняется один раз как <имя>.ace.bak.\n";
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // The arguments as UTF-16 (a path with Cyrillic in it), taken to UTF-8.
    std::vector<std::string> args;
    {
        int count = 0;
        wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
        for (int i = 1; i < count; ++i) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
            args.push_back(std::move(text));
        }
        LocalFree(wide);
    }
    wchar_t exe_buffer[MAX_PATH * 4];
    const fs::path exe(std::wstring(exe_buffer, GetModuleFileNameW(nullptr, exe_buffer, static_cast<DWORD>(std::size(exe_buffer)))));
#else
    std::vector<std::string> args(argv + 1, argv + argc);
    const fs::path exe = fs::absolute(argv[0]);
#endif
    (void)argc;
    (void)argv;
    if (const auto it = std::find(args.begin(), args.end(), "--no-wait"); it != args.end()) {
        no_wait = true;
        args.erase(it);
    }
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        usage();
        wait_if_own_console();
        return args.empty() ? 1 : 0;
    }
#ifdef _WIN32
    if (args[0] == "--register") {
        const bool ok = register_types(exe);
        std::cout << (ok ? "Готово: .ace открывается openrts-ace, в меню PNG и папок есть пункты openRTS.\n"
                            "Если Windows всё равно открывает .ace другой программой: \"Открыть с помощью\" -> openrts-ace, \"Всегда\".\n"
                         : "Не все ключи реестра удалось записать.\n");
        wait_if_own_console();
        return ok ? 0 : 1;
    }
    if (args[0] == "--unregister") {
        unregister_types();
        std::cout << "Связь .ace с openrts-ace и пункты меню убраны.\n";
        wait_if_own_console();
        return 0;
    }
#endif

    // The command, then the paths and the options.
    std::string command;
    std::size_t first = 0;
    if (args[0] == "to-png" || args[0] == "to-ace") {
        command = args[0];
        first = 1;
    }
    Options options;
    std::vector<fs::path> paths;
    for (std::size_t i = first; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--out" && i + 1 < args.size()) {
            options.out = from_utf8(args[++i]);
        } else if (a == "--mips") {
            options.mips = true;
        } else if (a == "--recursive") {
            options.recursive = true;
        } else if (a == "--no-mips") {
            options.write.mipmaps = false;
        } else if (a == "--no-zlib") {
            options.write.zlib = false;
        } else if (a == "--format" && i + 1 < args.size()) {
            const auto format = parse_format(args[++i]);
            if (!format) {
                std::cout << "Неизвестный формат: " << args[i] << " (dxt1, rgb, rgb-mask, rgba, auto)\n";
                return 1;
            }
            options.format = *format;
            options.format_given = true;
        } else {
            paths.push_back(from_utf8(a));
        }
    }
    if (paths.empty()) {
        usage();
        return 1;
    }
    if (options.out) {
        std::error_code error;
        fs::create_directories(*options.out, error);
    }

    // No command: an .ace is shown (to PNG and opened), a PNG or a folder
    // goes to ACE.
    const bool double_click = command.empty() && paths.size() == 1 && has_extension(paths.front(), ".ace");
    if (command.empty()) {
        std::error_code error;
        const bool folder = fs::is_directory(paths.front(), error);
        command = has_extension(paths.front(), ".ace") ? "to-png" : (folder || has_extension(paths.front(), ".png")) ? "to-ace" : "";
        if (command.empty()) {
            usage();
            return 1;
        }
    }

    int done = 0, failed = 0;
    if (command == "to-png") {
        std::optional<fs::path> last;
        for (const auto& path : paths) {
            for (const auto& file : inputs(path, ".ace", options.recursive)) {
                if (auto png = ace_to_png(file, options)) {
                    last = png;
                    ++done;
                } else {
                    ++failed;
                }
            }
        }
#ifdef _WIN32
        if (double_click) {
            if (last) {
                ShellExecuteW(nullptr, L"open", last->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            MessageBoxW(nullptr, widen("Не удалось прочитать " + utf8(paths.front().filename())).c_str(), L"openrts-ace", MB_ICONWARNING);
            return 1;
        }
#else
        (void)double_click;
#endif
    } else {
        std::vector<fs::path> files;
        for (const auto& path : paths) {
            const auto found = inputs(path, ".png", options.recursive);
            files.insert(files.end(), found.begin(), found.end());
        }
        if (files.empty()) {
            std::cout << "PNG не найдено.\n";
            wait_if_own_console();
            return 1;
        }
        if (!options.format_given) {
            options.format = ask_format(files.size() == 1 ? utf8(files.front().filename()) : std::to_string(files.size()) + " PNG");
        }
        for (const auto& file : files) (png_to_ace(file, options) ? done : failed) += 1;
    }
    std::cout << "Готово: " << done << (failed ? ", с ошибкой: " + std::to_string(failed) : std::string()) << '\n';
    wait_if_own_console();
    return failed == 0 ? 0 : 1;
}
