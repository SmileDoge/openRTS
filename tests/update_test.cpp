// The start menu's updates (game/src/update): versions, GitHub's release
// JSON, the release text's summary, and laying an archive over a folder (old
// files renamed aside and deleted later, other files kept, a path leaving
// the folder refused, nothing changed on a failure). No network.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <openrts/core/zip_archive.hpp>

#include "update.hpp"

namespace {

namespace fs = std::filesystem;
using namespace openrts::update;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void write_file(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

[[nodiscard]] std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_zip(const fs::path& archive, const std::vector<std::pair<std::string, std::string>>& files) {
    fs::create_directories(archive.parent_path());
    openrts::core::ZipWriter writer(archive);
    for (const auto& [name, data] : files) writer.add(name, data, {});
    check(writer.finish(), "the test archive is written");
}

void versions() {
    const auto v = parse_version("v0.5.1");
    check(v && v->major == 0 && v->minor == 5 && v->patch == 1, "v0.5.1 is read");
    check(parse_version("0.5") == Version{0, 5, 0}, "0.5 is 0.5.0");
    check(parse_version("1.2.3-rc1") == Version{1, 2, 3}, "a suffix is ignored");
    check(!parse_version("nightly"), "nightly is no version");
    check(!parse_version("5"), "one number is no version");
    check(*parse_version("v0.5.2") > *parse_version("0.5.1"), "0.5.2 is newer than 0.5.1");
    check(*parse_version("v0.10.0") > *parse_version("0.9.9"), "numbers, not text: 0.10 after 0.9");
    check(!(*parse_version("v0.5.1") > *parse_version("0.5.1")), "the same version is not newer");
}

void releases() {
    // As GitHub answers (shortened), the body with CRLF, a Cyrillic escape
    // and a surrogate pair.
    const std::string json = R"({
  "url": "https://api.github.com/repos/SmileDoge/openRTS/releases/1",
  "html_url": "https://github.com/SmileDoge/openRTS/releases/tag/v0.5.2",
  "tag_name": "v0.5.2",
  "name": "openRTS v0.5.2",
  "draft": false,
  "prerelease": false,
  "body": "Сборка openRTS.\r\n\r\n### Что нового / What's new\r\n\r\nОблака \u043e\u0442\u0431\u0440\u0430\u0441\u044b\u0432\u0430\u044e\u0442\r\nтени \ud83d\ude82 \"везде\".\r\n\r\n### Небо\r\n\r\n- пункт\r\n",
  "assets": [
    {"name": "openRTS-windows-x64-v0.5.2.zip.sha256", "size": 99,
     "browser_download_url": "https://github.com/SmileDoge/openRTS/releases/download/v0.5.2/openRTS-windows-x64-v0.5.2.zip.sha256"},
    {"name": "openRTS-windows-x64-v0.5.2.zip", "size": 52428800, "digest": "sha256:ABCDEF0123",
     "browser_download_url": "https://github.com/SmileDoge/openRTS/releases/download/v0.5.2/openRTS-windows-x64-v0.5.2.zip",
     "uploader": {"login": "github-actions[bot]", "id": 41898282}, "download_count": 0, "content_type": "application/zip"}
  ]
})";
    std::string error;
    const auto release = parse_release(json, error);
    check(release.has_value(), "the release is read: " + error);
    if (!release) return;
    check(release->tag == "v0.5.2", "its tag");
    check(release->title == "openRTS v0.5.2", "its title");
    check(release->page == "https://github.com/SmileDoge/openRTS/releases/tag/v0.5.2", "its page");
    check(release->archive_name == "openRTS-windows-x64-v0.5.2.zip", "the archive, not the .sha256 beside it");
    check(release->archive_size == 52428800u, "the archive's size");
    check(release->sha256 == "abcdef0123", "the digest, lowercase, without sha256:");
    check(release->sha256_url.ends_with(".zip.sha256"), "the .sha256 asset");
    check(release->summary == "Облака отбрасывают тени \xF0\x9F\x9A\x82 \"везде\".", "the first paragraph of What's new, joined: " + release->summary);

    check(!parse_release(R"({"tag_name": "v0.5.2", "assets": []})", error), "no archive yet: no release to update to");
    check(!parse_release(R"({"tag_name": "v0.5.2", "assets": [{"name": "openRTS-windows-x64-v0.5.2.zip", "browser_download_url": "http://x/y.zip"}]})", error),
          "a plain http archive is refused");
    check(!parse_release("{\"tag_name\": \"v0.5.2\"", error), "broken JSON");
    check(!parse_release("[1, 2]", error), "not an object");
    check(release_summary("no such section") .empty(), "a text without What's new has no summary");
}

void installing(const fs::path& work) {
    const auto folder = work / "game";
    const auto old_list = work / "data" / "old_files.txt";
    write_file(folder / "openrts_game.exe", "old menu");
    write_file(folder / "openrts_viewer.exe", "old game");
    write_file(folder / "tools" / "openrts-check.exe", "old tool");
    write_file(folder / "portable.txt", "");
    write_file(folder / "my_notes.txt", "mine");
    write_zip(work / "release.zip", {{"openRTS-v9.9.9/openrts_game.exe", "new menu"},
                                     {"openRTS-v9.9.9/openrts_viewer.exe", "new game"},
                                     {"openRTS-v9.9.9/tools/openrts-check.exe", "new tool"},
                                     {"openRTS-v9.9.9/tools/new-tool.exe", "another"},
                                     {"openRTS-v9.9.9/README.md", "readme"}});
    std::string error;
    check(install_archive(work / "release.zip", folder, old_list, error), "the archive is laid over the folder: " + error);
    check(read_file(folder / "openrts_game.exe") == "new menu", "the menu replaced");
    check(read_file(folder / "openrts_viewer.exe") == "new game", "the game replaced");
    check(read_file(folder / "tools" / "openrts-check.exe") == "new tool", "a tool replaced");
    check(read_file(folder / "tools" / "new-tool.exe") == "another", "a new tool added");
    check(read_file(folder / "README.md") == "readme", "a new file added");
    check(fs::exists(folder / "portable.txt") && read_file(folder / "my_notes.txt") == "mine", "files not in the archive are kept");
    check(read_file(folder / "openrts_game.exe.old") == "old menu", "the old menu renamed aside");
    check(!fs::exists(folder / "openrts_game.exe.new"), "no .new left");
    const auto listed = read_file(old_list);
    check(listed.find("openrts_game.exe.old") != std::string::npos && listed.find("openrts-check.exe.old") != std::string::npos,
          "the .old files listed");
    check(listed.find("README.md.old") == std::string::npos, "a file that was not there has no .old");
    remove_old_files(old_list);
    check(!fs::exists(folder / "openrts_game.exe.old") && !fs::exists(folder / "tools" / "openrts-check.exe.old"), "the .old files deleted");
    check(!fs::exists(old_list), "the list gone with them");

    // A second update over the first: .old names free again.
    write_zip(work / "second.zip", {{"openRTS-v9.9.10/openrts_game.exe", "newer menu"}});
    check(install_archive(work / "second.zip", folder, old_list, error), "a second update: " + error);
    check(read_file(folder / "openrts_game.exe") == "newer menu" && read_file(folder / "openrts_game.exe.old") == "new menu",
          "the second update's old menu aside");
    remove_old_files(old_list);

    // Refused archives leave the folder as it was.
    write_zip(work / "escape.zip", {{"openRTS-v9.9.9/openrts_game.exe", "bad"}, {"openRTS-v9.9.9/../evil.txt", "bad"}});
    check(!install_archive(work / "escape.zip", folder, old_list, error), "a path leaving the folder is refused");
    check(read_file(folder / "openrts_game.exe") == "newer menu" && !fs::exists(work / "evil.txt"), "nothing changed by it");
    write_zip(work / "absolute.zip", {{"openrts_game.exe", "bad"}, {"C:/evil.txt", "bad"}});
    check(!install_archive(work / "absolute.zip", folder, old_list, error), "a drive path is refused");
    write_zip(work / "other.zip", {{"something/readme.txt", "not ours"}});
    check(!install_archive(work / "other.zip", folder, old_list, error), "an archive without openrts_game.exe is refused");

    // A file in the archive where the folder has a file of that name as a
    // folder's: unpacking fails before anything is replaced.
    write_file(folder / "blocked", "a file");
    write_zip(work / "blocked.zip", {{"top/openrts_game.exe", "blocked menu"}, {"top/blocked/inside.txt", "x"}});
    check(!install_archive(work / "blocked.zip", folder, old_list, error), "a folder that cannot be made fails the update");
    check(read_file(folder / "openrts_game.exe") == "newer menu", "and the menu is the one before");
    check(!fs::exists(folder / "openrts_game.exe.new"), "its unpacked files removed");
}

void folders(const fs::path& work) {
    check(folder_writable(work), "the test folder is writable");
    write_file(work / "dev" / "CMakeCache.txt", "");
    fs::create_directories(work / "dev" / "game" / "Release");
    check(development_build(work / "dev" / "game" / "Release"), "a CMake build folder is a development build");
    check(!development_build(work / "game"), "an unpacked release is not");
}

} // namespace

int main() {
    const auto work = fs::temp_directory_path() / "openrts_update_test";
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(work);
    versions();
    releases();
    installing(work);
    folders(work);
    fs::remove_all(work, error);
    if (failures == 0) std::cout << "update: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
