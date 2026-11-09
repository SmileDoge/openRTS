#include "update.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <functional>
#include <utility>

#include <openrts/core/zip_archive.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#endif

namespace openrts::update {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

[[nodiscard]] bool starts_with(const std::string_view text, const std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool ends_with(const std::string_view text, const std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] fs::path from_utf8(const std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

// A small JSON reader: what GitHub's release answer needs (objects, arrays,
// strings with their escapes, numbers, true / false / null).
struct Json {
    enum class Kind { null, boolean, number, string, array, object };
    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    [[nodiscard]] const Json* get(const std::string_view key) const {
        for (const auto& [name, value] : members)
            if (name == key) return &value;
        return nullptr;
    }
    [[nodiscard]] std::string string_of(const std::string_view key) const {
        const auto* value = get(key);
        return value && value->kind == Kind::string ? value->text : std::string{};
    }
    [[nodiscard]] double number_of(const std::string_view key) const {
        const auto* value = get(key);
        return value && value->kind == Kind::number ? value->number : 0.0;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<Json> document() {
        auto value = read_value(0);
        skip_space();
        if (!value || at_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void skip_space() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_;
    }

    [[nodiscard]] bool word(const std::string_view expected) {
        if (text_.substr(at_, expected.size()) != expected) return false;
        at_ += expected.size();
        return true;
    }

    [[nodiscard]] bool hex4(unsigned& value) {
        if (at_ + 4 > text_.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void append_utf8(std::string& out, const unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    [[nodiscard]] bool read_string(std::string& out) {
        ++at_; // the opening quote
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) return false;
            switch (const char e = text_[at_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code = 0;
                if (!hex4(code)) return false;
                if (code >= 0xD800 && code < 0xDC00) {
                    // A surrogate pair: the low half follows as \uXXXX.
                    unsigned low = 0;
                    if (at_ + 1 < text_.size() && text_[at_] == '\\' && text_[at_ + 1] == 'u') {
                        at_ += 2;
                        if (!hex4(low)) return false;
                    }
                    code = low >= 0xDC00 && low < 0xE000 ? 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00) : 0xFFFD;
                } else if (code >= 0xDC00 && code < 0xE000) {
                    code = 0xFFFD;
                }
                append_utf8(out, code);
                break;
            }
            default: (void)e; return false;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<Json> read_value(const int depth) {
        if (depth > 64) return std::nullopt;
        skip_space();
        if (at_ >= text_.size()) return std::nullopt;
        Json value;
        const char c = text_[at_];
        if (c == '{') {
            ++at_;
            value.kind = Json::Kind::object;
            skip_space();
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return value;
            }
            for (;;) {
                skip_space();
                if (at_ >= text_.size() || text_[at_] != '"') return std::nullopt;
                std::string key;
                if (!read_string(key)) return std::nullopt;
                skip_space();
                if (at_ >= text_.size() || text_[at_] != ':') return std::nullopt;
                ++at_;
                auto member = read_value(depth + 1);
                if (!member) return std::nullopt;
                value.members.emplace_back(std::move(key), std::move(*member));
                skip_space();
                if (at_ < text_.size() && text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == '}') {
                    ++at_;
                    return value;
                }
                return std::nullopt;
            }
        }
        if (c == '[') {
            ++at_;
            value.kind = Json::Kind::array;
            skip_space();
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return value;
            }
            for (;;) {
                auto item = read_value(depth + 1);
                if (!item) return std::nullopt;
                value.items.push_back(std::move(*item));
                skip_space();
                if (at_ < text_.size() && text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == ']') {
                    ++at_;
                    return value;
                }
                return std::nullopt;
            }
        }
        if (c == '"') {
            value.kind = Json::Kind::string;
            if (!read_string(value.text)) return std::nullopt;
            return value;
        }
        if (word("true")) {
            value.kind = Json::Kind::boolean;
            value.boolean = true;
            return value;
        }
        if (word("false")) {
            value.kind = Json::Kind::boolean;
            return value;
        }
        if (word("null")) return value;
        const auto start = at_;
        while (at_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[at_])) || text_[at_] == '-' || text_[at_] == '+' ||
                                      text_[at_] == '.' || text_[at_] == 'e' || text_[at_] == 'E'))
            ++at_;
        if (at_ == start) return std::nullopt;
        value.kind = Json::Kind::number;
        const auto [end, failure] = std::from_chars(text_.data() + start, text_.data() + at_, value.number);
        if (failure != std::errc{} || end != text_.data() + at_) return std::nullopt;
        return value;
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

// A path inside the archive's folder: no root, no drive, no "..".
[[nodiscard]] bool safe_relative(const std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos)
        return false;
    std::size_t from = 0;
    while (from <= name.size()) {
        auto end = name.find('/', from);
        if (end == std::string_view::npos) end = name.size();
        const auto part = name.substr(from, end - from);
        if (part.empty() || part == "." || part == "..") return false;
        from = end + 1;
    }
    return true;
}

[[nodiscard]] fs::path with_suffix(const fs::path& file, const std::string_view suffix) {
    auto name = file.filename().u8string();
    name.append(suffix.begin(), suffix.end());
    return file.parent_path() / fs::path(name);
}

[[nodiscard]] std::string message_of(const std::error_code& error) { return error.message(); }

} // namespace

std::optional<Version> parse_version(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    Version version;
    int* parts[] = {&version.major, &version.minor, &version.patch};
    int read = 0;
    const char* at = text.data();
    const char* end = text.data() + text.size();
    for (; read < 3; ++read) {
        const auto [next, failure] = std::from_chars(at, end, *parts[read]);
        if (failure != std::errc{}) break;
        at = next;
        if (at == end || *at != '.') {
            ++read;
            break;
        }
        ++at;
    }
    if (read < 2) return std::nullopt;
    return version;
}

std::string release_summary(const std::string_view body) {
    std::string text(body);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::vector<std::string_view> lines;
    for (std::size_t from = 0; from <= text.size();) {
        auto end = text.find('\n', from);
        if (end == std::string::npos) end = text.size();
        lines.emplace_back(std::string_view(text).substr(from, end - from));
        from = end + 1;
    }
    std::size_t i = 0;
    while (i < lines.size() && !(starts_with(lines[i], "### ") && lines[i].find("What's new") != std::string_view::npos)) ++i;
    if (i == lines.size()) return {};
    ++i;
    while (i < lines.size() && lines[i].find_first_not_of(" \t") == std::string_view::npos) ++i;
    std::string summary;
    for (; i < lines.size(); ++i) {
        const auto line = lines[i];
        if (line.find_first_not_of(" \t") == std::string_view::npos || starts_with(line, "#")) break;
        const auto first = line.find_first_not_of(" \t");
        const auto last = line.find_last_not_of(" \t");
        if (!summary.empty()) summary += ' ';
        summary += line.substr(first, last - first + 1);
    }
    return summary;
}

std::optional<Release> parse_release(const std::string_view json, std::string& error) {
    const auto document = JsonReader(json).document();
    if (!document || document->kind != Json::Kind::object) {
        error = "not a release's JSON";
        return std::nullopt;
    }
    Release release;
    release.tag = document->string_of("tag_name");
    release.title = document->string_of("name");
    release.page = document->string_of("html_url");
    release.summary = release_summary(document->string_of("body"));
    if (release.tag.empty()) {
        error = "no tag_name";
        return std::nullopt;
    }
    const auto* assets = document->get("assets");
    if (assets && assets->kind == Json::Kind::array) {
        for (const auto& asset : assets->items) {
            const auto name = asset.string_of("name");
            if (!starts_with(name, archive_prefix) || !ends_with(lower(name), ".zip")) continue;
            release.archive_name = name;
            release.archive_url = asset.string_of("browser_download_url");
            release.archive_size = static_cast<std::uint64_t>((std::max)(asset.number_of("size"), 0.0));
            if (const auto digest = asset.string_of("digest"); starts_with(digest, "sha256:")) release.sha256 = lower(digest.substr(7));
            break;
        }
        for (const auto& asset : assets->items) {
            if (!release.archive_name.empty() && asset.string_of("name") == release.archive_name + ".sha256")
                release.sha256_url = asset.string_of("browser_download_url");
        }
    }
    if (release.archive_url.empty() || !starts_with(release.archive_url, "https://")) {
        error = "no Windows archive in the release";
        return std::nullopt;
    }
    if (!release.sha256_url.empty() && !starts_with(release.sha256_url, "https://")) release.sha256_url.clear();
    return release;
}

bool install_archive(const fs::path& archive, const fs::path& folder, const fs::path& old_list, std::string& error) {
    auto reader = core::ZipReader::open(archive, error);
    if (!reader) return false;
    const auto& entries = reader->entries();
    // CI's one folder inside the archive: taken off every name.
    std::string prefix;
    if (!entries.empty()) {
        const auto slash = entries.front().name.find('/');
        if (slash != std::string::npos) {
            prefix = entries.front().name.substr(0, slash + 1);
            for (const auto& entry : entries) {
                if (!starts_with(entry.name, prefix)) {
                    prefix.clear();
                    break;
                }
            }
        }
    }
    struct Item {
        const core::ZipEntry* entry = nullptr;
        fs::path target, staged, old;
        bool moved_old = false, placed = false;
    };
    std::vector<Item> items;
    bool has_menu = false;
    for (const auto& entry : entries) {
        if (entry.directory()) continue;
        const auto name = std::string_view(entry.name).substr(prefix.size());
        if (!safe_relative(name)) {
            error = "a path outside the folder in the archive: " + entry.name;
            return false;
        }
        if (lower(std::string(name)) == "openrts_game.exe") has_menu = true;
        Item item;
        item.entry = &entry;
        item.target = folder / from_utf8(name);
        item.staged = with_suffix(item.target, ".new");
        items.push_back(std::move(item));
    }
    if (!has_menu) {
        error = "not an openRTS archive (no openrts_game.exe)";
        return false;
    }

    std::error_code fs_error;
    const auto drop_staged = [&] {
        for (const auto& item : items) fs::remove(item.staged, fs_error);
    };
    // Unpacked next to their places first: the folder is not touched until
    // every file is there.
    for (auto& item : items) {
        fs::create_directories(item.target.parent_path(), fs_error);
        if (fs_error) {
            error = "cannot make the folder " + utf8(item.target.parent_path()) + ": " + message_of(fs_error);
            drop_staged();
            return false;
        }
        const auto data = reader->read(*item.entry, error);
        if (!data) {
            error = item.entry->name + ": " + error;
            drop_staged();
            return false;
        }
        std::ofstream out(item.staged, std::ios::binary | std::ios::trunc);
        out.write(data->data(), static_cast<std::streamsize>(data->size()));
        out.close();
        if (!out) {
            error = "cannot write " + utf8(item.staged);
            drop_staged();
            return false;
        }
    }

    // The old files aside, the new ones in their places.
    const auto roll_back = [&] {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            std::error_code ignored;
            if (it->placed) fs::remove(it->target, ignored);
            if (it->moved_old) fs::rename(it->old, it->target, ignored);
        }
        drop_staged();
    };
    for (auto& item : items) {
        if (fs::exists(item.target, fs_error)) {
            // An .old left from before (still in use then) keeps its name.
            item.old = with_suffix(item.target, ".old");
            for (int n = 1; n < 10 && fs::exists(item.old, fs_error); ++n) {
                fs::remove(item.old, fs_error);
                if (fs::exists(item.old, fs_error)) item.old = with_suffix(item.target, ".old" + std::to_string(n));
            }
            fs::rename(item.target, item.old, fs_error);
            if (fs_error) {
                error = "cannot rename " + utf8(item.target) + ": " + message_of(fs_error);
                roll_back();
                return false;
            }
            item.moved_old = true;
        }
        fs::rename(item.staged, item.target, fs_error);
        if (fs_error) {
            error = "cannot put " + utf8(item.target) + " in place: " + message_of(fs_error);
            roll_back();
            return false;
        }
        item.placed = true;
    }

    // The old files are deleted at the next start (the running menu's
    // cannot be now).
    fs::create_directories(old_list.parent_path(), fs_error);
    std::ofstream list(old_list, std::ios::binary | std::ios::app);
    for (const auto& item : items)
        if (item.moved_old) list << utf8(item.old) << '\n';
    error.clear();
    return true;
}

void remove_old_files(const fs::path& old_list) {
    std::ifstream in(old_list, std::ios::binary);
    if (!in) return;
    std::vector<std::string> kept;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::error_code error;
        const auto file = from_utf8(line);
        fs::remove_all(file, error);
        if (fs::exists(file, error)) kept.push_back(line);
    }
    in.close();
    std::error_code error;
    if (kept.empty()) {
        fs::remove(old_list, error);
        return;
    }
    std::ofstream out(old_list, std::ios::binary | std::ios::trunc);
    for (const auto& file : kept) out << file << '\n';
}

bool folder_writable(const fs::path& folder) {
    const auto probe = folder / ".openrts_write_test";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "openRTS";
        if (!out) return false;
    }
    std::error_code error;
    fs::remove(probe, error);
    return true;
}

bool development_build(const fs::path& folder) {
    std::error_code error;
    auto at = folder;
    for (int up = 0; up < 4 && !at.empty(); ++up) {
        if (fs::exists(at / "CMakeCache.txt", error)) return true;
        const auto parent = at.parent_path();
        if (parent == at) break;
        at = parent;
    }
    return false;
}

#ifdef _WIN32
namespace {

[[nodiscard]] std::wstring widen(const std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

[[nodiscard]] std::string narrow(const std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// WinHTTP's own message for its error (in the system's language), else the number.
[[nodiscard]] std::string winhttp_message(const DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length > 0 && buffer) {
        text = narrow(std::wstring_view(buffer, length));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) text.pop_back();
    }
    if (buffer) LocalFree(buffer);
    return text.empty() ? "error " + std::to_string(code) : text + " (" + std::to_string(code) + ")";
}

struct Internet {
    HINTERNET handle = nullptr;
    explicit Internet(HINTERNET h) : handle(h) {}
    ~Internet() {
        if (handle) WinHttpCloseHandle(handle);
    }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
};

// One GET; `sink` takes the body as it comes (false: stop).
bool get(const std::string& url, const std::function<bool(const char*, std::size_t)>& sink, std::string& error) {
    if (!starts_with(url, "https://")) {
        error = "not an https address: " + url;
        return false;
    }
    const auto wide = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
        error = winhttp_message(GetLastError());
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    // The system's proxy (Windows 8.1 on), else the older default.
    Internet session(WinHttpOpen(L"openRTS-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle)
        session.handle = WinHttpOpen(L"openRTS-updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) {
        error = winhttp_message(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.handle, 10000, 10000, 15000, 30000);
    Internet connection(WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0));
    Internet request(connection.handle ? WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                       : nullptr);
    if (!request.handle) {
        error = winhttp_message(GetLastError());
        return false;
    }
    // GitHub's API asks for its own type; the archive itself is a plain download.
    const bool api = host == L"api.github.com";
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.handle, api ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, api ? static_cast<DWORD>(-1L) : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        error = winhttp_message(GetLastError());
        return false;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            error = winhttp_message(GetLastError());
            return false;
        }
        if (read == 0) return true;
        if (!sink(buffer.data(), read)) {
            error = "cancelled";
            return false;
        }
    }
}

} // namespace

std::optional<std::string> http_get(const std::string& url, std::string& error) {
    std::string body;
    constexpr std::size_t most = 8u << 20; // a release's JSON is a few kB
    const bool ok = get(url, [&](const char* data, const std::size_t size) {
        if (body.size() + size > most) return false;
        body.append(data, size);
        return true;
    }, error);
    if (!ok) return std::nullopt;
    return body;
}

bool download(const std::string& url, const fs::path& file, std::atomic<std::uint64_t>& done, const std::atomic<bool>& cancel,
              std::string& error) {
    done = 0;
    bool ok = false;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + utf8(file);
            return false;
        }
        ok = get(url, [&](const char* data, const std::size_t size) {
            if (cancel) return false;
            out.write(data, static_cast<std::streamsize>(size));
            done += size;
            return static_cast<bool>(out);
        }, error);
        out.close();
        if (ok && !out) {
            error = "cannot write " + utf8(file);
            ok = false;
        }
    }
    if (!ok) {
        std::error_code ignored;
        fs::remove(file, ignored);
    }
    return ok;
}

std::string sha256_of_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<char> buffer(1u << 20);
        bool ok = true;
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto got = in.gcount();
            if (got <= 0) break;
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(got), 0) < 0) {
                ok = false;
                break;
            }
        }
        unsigned char digest[32]{};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
            char hex[3];
            for (const auto byte : digest) {
                std::snprintf(hex, sizeof(hex), "%02x", byte);
                result += hex;
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

bool start_process(const fs::path& exe, const std::string& arguments) {
    std::wstring line = L"\"" + exe.wstring() + L"\"";
    if (!arguments.empty()) line += L" " + widen(arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const auto folder = exe.parent_path().wstring();
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &info)) return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

#else

std::optional<std::string> http_get(const std::string&, std::string& error) {
    error = "updates are made on Windows only";
    return std::nullopt;
}

bool download(const std::string&, const fs::path&, std::atomic<std::uint64_t>&, const std::atomic<bool>&, std::string& error) {
    error = "updates are made on Windows only";
    return false;
}

std::string sha256_of_file(const fs::path&) { return {}; }

bool start_process(const fs::path&, const std::string&) { return false; }

#endif

Updater::~Updater() {
    cancel_ = true;
    join();
}

void Updater::join() {
    if (worker_.joinable()) worker_.join();
}

void Updater::set_stage(const Stage stage, std::string error, std::string detail) {
    const std::lock_guard lock(mutex_);
    status_.stage = stage;
    status_.error = std::move(error);
    status_.detail = std::move(detail);
}

Updater::Status Updater::status() const {
    const std::lock_guard lock(mutex_);
    auto status = status_;
    status.done = done_;
    return status;
}

bool Updater::busy() const {
    const std::lock_guard lock(mutex_);
    return status_.stage == Stage::checking || status_.stage == Stage::downloading || status_.stage == Stage::installing;
}

void Updater::check(std::string current) {
    if (busy()) return;
    join();
    {
        const std::lock_guard lock(mutex_);
        status_ = {};
        status_.stage = Stage::checking;
    }
    worker_ = std::thread([this, current = std::move(current)] {
        std::string detail;
        const auto body = http_get(std::string(latest_release_url), detail);
        if (!body) {
            set_stage(Stage::failed, "Could not ask GitHub for the latest version", detail);
            return;
        }
        auto release = parse_release(*body, detail);
        if (!release) {
            set_stage(Stage::failed, "Could not read the latest release", detail);
            return;
        }
        const auto latest = parse_version(release->tag);
        const auto mine = parse_version(current);
        const std::lock_guard lock(mutex_);
        status_.release = std::move(*release);
        status_.stage = latest && mine && *latest > *mine ? Stage::available : Stage::up_to_date;
    });
}

void Updater::install(fs::path folder, fs::path work, fs::path old_list) {
    if (busy()) return;
    const auto now = status();
    if (!now.release) return;
    join();
    cancel_ = false;
    done_ = 0;
    {
        const std::lock_guard lock(mutex_);
        status_.stage = Stage::downloading;
        status_.error.clear();
        status_.detail.clear();
        status_.total = now.release->archive_size;
    }
    worker_ = std::thread([this, release = *now.release, folder = std::move(folder), work = std::move(work), old_list = std::move(old_list)] {
        std::error_code fs_error;
        fs::create_directories(work, fs_error);
        const auto archive = work / from_utf8(release.archive_name);
        std::string detail;
        if (!download(release.archive_url, archive, done_, cancel_, detail)) {
            if (cancel_) set_stage(Stage::available);
            else set_stage(Stage::failed, "The download failed", detail);
            return;
        }
        set_stage(Stage::installing);
        const auto size = fs::file_size(archive, fs_error);
        if (release.archive_size != 0 && (fs_error || size != release.archive_size)) {
            fs::remove(archive, fs_error);
            set_stage(Stage::failed, "The download is incomplete");
            return;
        }
        // The archive's SHA-256: GitHub's digest of the asset, else the
        // .sha256 file CI publishes beside it.
        auto expected = release.sha256;
        if (expected.empty() && !release.sha256_url.empty()) {
            if (const auto text = http_get(release.sha256_url, detail)) expected = lower(text->substr(0, text->find_first_of(" \t\r\n")));
        }
        if (!expected.empty() && sha256_of_file(archive) != expected) {
            fs::remove(archive, fs_error);
            set_stage(Stage::failed, "The download is damaged (its SHA-256 does not match)");
            return;
        }
        if (!install_archive(archive, folder, old_list, detail)) {
            set_stage(Stage::failed, "The update could not be installed", detail);
            return;
        }
        fs::remove(archive, fs_error);
        set_stage(Stage::installed);
    });
}

} // namespace openrts::update
