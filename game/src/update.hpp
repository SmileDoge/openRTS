#pragma once

// The start menu's updates: the latest release of openRTS on GitHub (not the
// nightly pre-release), its archive downloaded, checked (SHA-256) and laid
// over the program's folder, then the menu started again.
//
// The folder's files the archive has are replaced: each old one is first
// renamed to <name>.old (Windows lets a running exe or a loaded DLL be
// renamed, not overwritten) and deleted at the next start. Files the archive
// does not have (portable.txt, a user's own) are left alone; the settings
// live in %LOCALAPPDATA%\openRTS anyway.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace openrts::update {

// Where the releases come from.
inline constexpr std::string_view latest_release_url = "https://api.github.com/repos/SmileDoge/openRTS/releases/latest";
// The archive CI makes: openRTS-windows-x64-<tag>.zip.
inline constexpr std::string_view archive_prefix = "openRTS-windows-x64";

struct Version {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
};
// "v0.5.1", "0.5.1" (a suffix after the numbers is ignored).
[[nodiscard]] std::optional<Version> parse_version(std::string_view text);

struct Release {
    std::string tag;           // v0.5.2
    std::string title;         // openRTS v0.5.2
    std::string page;          // the release's page
    std::string summary;       // the first paragraph of its "What's new"
    std::string archive_name;  // openRTS-windows-x64-v0.5.2.zip
    std::string archive_url;
    std::uint64_t archive_size = 0;
    std::string sha256;        // lowercase hex: the asset's digest, else empty
    std::string sha256_url;    // the archive's .sha256 asset, else empty
};
// GitHub's release JSON (REST: releases/latest). Without a Windows archive
// (the release made, its asset not uploaded yet) it is an error.
[[nodiscard]] std::optional<Release> parse_release(std::string_view json, std::string& error);
// The first paragraph after "### Что нового / What's new" in a release's text
// (.github/scripts/release-notes.sh puts the CHANGELOG section there).
[[nodiscard]] std::string release_summary(std::string_view body);

// Lays the archive over `folder`. CI's archive has one folder inside it; its
// files are taken relative to it. Every file is unpacked next to its place
// first (<name>.new, its CRC checked), and only then are the old ones renamed
// to <name>.old and the new ones put in; on a failure the old ones go back.
// The .old files are added to `old_list` (one path a line, UTF-8). An archive
// without openrts_game.exe, or with a path leaving the folder, is refused.
[[nodiscard]] bool install_archive(const std::filesystem::path& archive, const std::filesystem::path& folder,
                                   const std::filesystem::path& old_list, std::string& error);
// Deletes the files `old_list` names; those still in use stay listed.
void remove_old_files(const std::filesystem::path& old_list);
// A file can be made in `folder` (not Program Files without the rights).
[[nodiscard]] bool folder_writable(const std::filesystem::path& folder);
// A build folder of the sources (a CMakeCache.txt up to three levels above):
// told of a release, never replaced by it.
[[nodiscard]] bool development_build(const std::filesystem::path& folder);

// The network (Windows: WinHTTP, the system's proxy; elsewhere they fail).
// HTTPS only; redirects are followed.
[[nodiscard]] std::optional<std::string> http_get(const std::string& url, std::string& error);
// `done` counts the bytes written; `cancel` stops it (the file removed).
[[nodiscard]] bool download(const std::string& url, const std::filesystem::path& file, std::atomic<std::uint64_t>& done,
                            const std::atomic<bool>& cancel, std::string& error);
// Lowercase hex; empty when the file cannot be read.
[[nodiscard]] std::string sha256_of_file(const std::filesystem::path& file);
// Starts `exe` with `arguments` (the caller then closes).
bool start_process(const std::filesystem::path& exe, const std::string& arguments);

// The menu's side: a check and an update on a thread of their own, read by
// the menu each frame.
class Updater {
public:
    enum class Stage {
        idle,
        checking,
        up_to_date,  // the latest release is not newer
        available,   // release() is newer than this build
        downloading, // progress()
        installing,
        installed,   // the menu should start again
        failed,      // error()
    };
    struct Status {
        Stage stage = Stage::idle;
        std::optional<Release> release;
        std::string error;  // failed: what failed, in English (the menu's tr() key)
        std::string detail; // and why (the system's message, an HTTP code, a file)
        std::uint64_t done = 0, total = 0; // downloading
    };

    Updater() = default;
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Asks GitHub for the latest release, against `current` (core::version()).
    void check(std::string current);
    // Downloads release() into `work` (a folder of its own), checks it and
    // lays it over `folder`; the .old files go to `old_list`.
    void install(std::filesystem::path folder, std::filesystem::path work, std::filesystem::path old_list);
    void cancel() noexcept { cancel_ = true; }
    [[nodiscard]] Status status() const;
    [[nodiscard]] bool busy() const;

private:
    void join();
    void set_stage(Stage stage, std::string error = {}, std::string detail = {});

    mutable std::mutex mutex_;
    Status status_;
    std::thread worker_;
    std::atomic<std::uint64_t> done_{0};
    std::atomic<bool> cancel_{false};
};

} // namespace openrts::update
