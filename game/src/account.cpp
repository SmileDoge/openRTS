#include "account.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <winhttp.h>
#endif

namespace openrts::game {
namespace {

namespace fs = std::filesystem;

constexpr const char* kSite = "https://dark-team.ru";

#ifdef _WIN32

struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

[[nodiscard]] std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

[[nodiscard]] std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

[[nodiscard]] std::string read_registry(const HKEY root, const wchar_t* key_path, const wchar_t* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0) {
        RegCloseKey(key);
        return {};
    }
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(text.data()), &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return narrow(text);
}

[[nodiscard]] std::string sha256_hex(const std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD object_size = 0, result = 0;
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result, 0);
    std::vector<unsigned char> object(object_size);
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) >= 0) {
            std::array<unsigned char, 32> digest{};
            if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
                char text[65]{};
                for (std::size_t i = 0; i < digest.size(); ++i)
                    std::snprintf(text + i * 2, sizeof(text) - i * 2, "%02x", digest[i]);
                out = text;
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return out;
}

[[nodiscard]] std::string system_disk_serial() {
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)))) return {};
    wchar_t root[4]{windows[0], L':', L'\\', L'\0'};
    DWORD serial = 0;
    if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) return {};
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08lx", static_cast<unsigned long>(serial));
    return text;
}

[[nodiscard]] std::string first_mac() {
    ULONG size = 0;
    if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW) return {};
    std::vector<unsigned char> buffer(size);
    auto* info = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
    if (GetAdaptersInfo(info, &size) != ERROR_SUCCESS) return {};
    for (auto* adapter = info; adapter; adapter = adapter->Next) {
        if (adapter->AddressLength != 6) continue;
        const bool empty = std::all_of(adapter->Address, adapter->Address + 6, [](const unsigned char c) { return c == 0; });
        if (empty) continue;
        char text[18]{};
        std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", adapter->Address[0], adapter->Address[1],
                      adapter->Address[2], adapter->Address[3], adapter->Address[4], adapter->Address[5]);
        return text;
    }
    return {};
}

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
};

[[nodiscard]] HttpResponse post_json(const std::string& url, const std::string& body, const std::string& bearer = {}) {
    HttpResponse response;
    const auto address = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts)) {
        response.error = "Не удалось разобрать адрес сайта.";
        return response;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    InternetHandle session;
    session.handle = WinHttpOpen(L"DTRE/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) {
        response.error = "Нет доступа к WinHTTP.";
        return response;
    }
    WinHttpSetTimeouts(session.handle, 5000, 5000, 10000, 15000);
    InternetHandle connection;
    connection.handle = WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0);
    if (!connection.handle) {
        response.error = "Не удалось соединиться с сайтом.";
        return response;
    }
    InternetHandle request;
    request.handle = WinHttpOpenRequest(connection.handle, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request.handle) {
        response.error = "Не удалось создать запрос.";
        return response;
    }
    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!bearer.empty()) headers += L"Authorization: Bearer " + wide(bearer) + L"\r\n";
    if (!WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) {
        response.error = "Сайт не ответил.";
        return response;
    }
    if (!WinHttpReceiveResponse(request.handle, nullptr)) {
        response.error = "Сайт не ответил.";
        return response;
    }
    DWORD status = 0, length = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request.handle, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, chunk.data(), available, &read) || read == 0) break;
        response.body.append(chunk.data(), read);
    }
    return response;
}

#else

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
};

[[nodiscard]] HttpResponse post_json(const std::string&, const std::string&, const std::string& = {}) {
    return {.error = "Аккаунт доступен только в Windows."};
}

[[nodiscard]] std::string sha256_hex(std::string_view) { return {}; }

#endif

[[nodiscard]] std::string json_escape(const std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

void append_utf8(std::string& out, const unsigned int code) {
    if (code <= 0x7F) {
        out += static_cast<char>(code);
    } else if (code <= 0x7FF) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code <= 0xFFFF) {
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

[[nodiscard]] std::string json_value(const std::string_view body, const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t at = body.find(needle);
    if (at == std::string_view::npos) return {};
    at = body.find(':', at + needle.size());
    if (at == std::string_view::npos) return {};
    ++at;
    while (at < body.size() && (body[at] == ' ' || body[at] == '\t' || body[at] == '\r' || body[at] == '\n')) ++at;
    if (at >= body.size()) return {};
    if (body[at] != '"') {
        const auto end = body.find_first_of(",}] \r\n", at);
        return std::string(body.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
    }
    ++at;
    std::string out;
    while (at < body.size()) {
        const char c = body[at++];
        if (c == '"') break;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (at >= body.size()) break;
        const char escaped = body[at++];
        switch (escaped) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            if (at + 4 > body.size()) break;
            unsigned int code = 0;
            for (int i = 0; i < 4; ++i) {
                const char digit = body[at++];
                code <<= 4;
                if (digit >= '0' && digit <= '9') code |= static_cast<unsigned int>(digit - '0');
                else if (digit >= 'a' && digit <= 'f') code |= static_cast<unsigned int>(digit - 'a' + 10);
                else if (digit >= 'A' && digit <= 'F') code |= static_cast<unsigned int>(digit - 'A' + 10);
            }
            append_utf8(out, code);
            break;
        }
        default: out += escaped; break;
        }
    }
    return out;
}

[[nodiscard]] std::string error_of(const HttpResponse& response) {
    if (!response.error.empty()) return response.error;
    const auto error = json_value(response.body, "error");
    if (!error.empty()) return error;
    if (response.status == 401) return "Неверный email или пароль.";
    if (response.status == 403) return "Доступ запрещён.";
    if (response.status == 429) return "Слишком много попыток. Подождите немного.";
    return "Сайт ответил ошибкой " + std::to_string(response.status) + ".";
}

[[nodiscard]] std::string body_with(const std::string& key, const std::string& value) {
    return "{\"" + key + "\":\"" + json_escape(value) + "\"}";
}

} // namespace

std::string account_token_file() {
#ifdef _WIN32
    wchar_t local[MAX_PATH * 2]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
    fs::path folder = length > 0 && length < std::size(local) ? fs::path(local) : fs::temp_directory_path();
    folder /= L"openRTS";
    return (folder / L"account.ini").string();
#else
    return (fs::temp_directory_path() / "openRTS" / "account.ini").string();
#endif
}

bool load_account(std::string& token, AccountUser& user) {
    std::ifstream file(account_token_file());
    if (!file) return false;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto at = line.find('=');
        if (at == std::string::npos) continue;
        const auto key = line.substr(0, at);
        const auto value = line.substr(at + 1);
        if (key == "token") token = value;
        else if (key == "username") user.username = value;
        else if (key == "email") user.email = value;
        else if (key == "role") user.role = value;
        else if (key == "position") user.position = value;
    }
    return !token.empty();
}

void save_account(const std::string& token, const AccountUser& user) {
    const fs::path path = account_token_file();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::trunc);
    if (!file) return;
    file << "token=" << token << '\n';
    file << "username=" << user.username << '\n';
    file << "email=" << user.email << '\n';
    file << "role=" << user.role << '\n';
    file << "position=" << user.position << '\n';
}

void clear_account() {
    std::error_code error;
    fs::remove(account_token_file(), error);
}

HardwareInfo hardware_info() {
    HardwareInfo info;
#ifdef _WIN32
    const auto machine = read_registry(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid");
    info.disk_serial = system_disk_serial();
    info.mac_address = first_mac();
    info.cpu_name = read_registry(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    info.motherboard = read_registry(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardProduct");
    if (info.motherboard.empty())
        info.motherboard = read_registry(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName");
    info.gpu_name = read_registry(HKEY_LOCAL_MACHINE,
                                  L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
                                  L"DriverDesc");
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) info.ram_total_mb = static_cast<int>(memory.ullTotalPhys / (1024ull * 1024ull));

    std::string material = machine;
    material += '|';
    material += info.disk_serial;
    if (material.size() <= 1) material = info.mac_address;
    if (material.empty()) {
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD size = static_cast<DWORD>(std::size(computer));
        if (GetComputerNameW(computer, &size)) material = narrow(std::wstring(computer, size));
    }
    info.hwid = sha256_hex(material);
    if (info.hwid.empty()) info.hwid = machine;
#endif
    return info;
}

AccountResult account_login(const std::string& email, const std::string& password, const std::string& hwid) {
    AccountResult result;
    const auto response = post_json(std::string(kSite) + "/api/launcher/login",
                                    "{\"email\":\"" + json_escape(email) + "\",\"password\":\"" + json_escape(password) +
                                        "\",\"hwid\":\"" + json_escape(hwid) + "\"}");
    result.status = response.status;
    if (response.status == 200 || response.status == 201) {
        result.token = json_value(response.body, "token");
        result.user.username = json_value(response.body, "username");
        result.user.email = json_value(response.body, "email");
        result.user.role = json_value(response.body, "role");
        result.user.position = json_value(response.body, "position");
        result.ok = !result.token.empty();
        if (!result.ok) result.error = "Сайт не выдал ключ входа.";
    } else {
        result.error = error_of(response);
    }
    return result;
}

AccountResult account_validate(const std::string& token) {
    AccountResult result;
    const auto response = post_json(std::string(kSite) + "/api/launcher/validate-token", body_with("token", token));
    result.status = response.status;
    if (response.status == 200 && json_value(response.body, "valid") == "true") {
        result.ok = true;
        result.token = token;
        result.user.username = json_value(response.body, "username");
        result.user.email = json_value(response.body, "email");
        result.user.role = json_value(response.body, "role");
        result.user.position = json_value(response.body, "position");
    } else {
        result.error = error_of(response);
        if (response.status == 401 || response.status == 403) result.error = "Вход больше не действует. Войдите снова.";
        else if (response.status == 200) result.error = "Срок входа истёк.";
    }
    return result;
}

void account_logout(const std::string& token) {
    if (!token.empty()) (void)post_json(std::string(kSite) + "/api/launcher/logout", "{}", token);
}

bool account_hwid_banned(const std::string& hwid, std::string& reason) {
    const auto response = post_json(std::string(kSite) + "/api/launcher/hwid-check", body_with("hwid", hwid));
    if (response.status == 403) {
        reason = json_value(response.body, "reason");
        if (reason.empty()) reason = json_value(response.body, "error");
        return true;
    }
    return false;
}

void account_report_hwid(const std::string& token, const HardwareInfo& info) {
    if (token.empty()) return;
    const std::string body = "{\"hwid_hash\":\"" + json_escape(info.hwid) + "\",\"motherboard\":\"" +
                             json_escape(info.motherboard) + "\",\"disk_serial\":\"" + json_escape(info.disk_serial) +
                             "\",\"mac_address\":\"" + json_escape(info.mac_address) + "\",\"cpu_name\":\"" +
                             json_escape(info.cpu_name) + "\",\"ram_total\":" + std::to_string(info.ram_total_mb) +
                             ",\"gpu_name\":\"" + json_escape(info.gpu_name) + "\"}";
    (void)post_json(std::string(kSite) + "/api/launcher/hwid-report", body, token);
}

} // namespace openrts::game
