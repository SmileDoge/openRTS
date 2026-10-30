#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace openrts::core::detail {

// One notification from a watched directory tree. `relative` is the changed
// path below the watched root; `overflow` means events were lost and the whole
// tree must be compared with disk.
struct WatchEvent {
    std::filesystem::path relative;
    bool overflow = false;
};

// Native recursive directory watcher (ReadDirectoryChangesW / inotify). Events
// are delivered on the watcher's own thread.
class DirectoryWatcher {
public:
    using Sink = std::function<void(const WatchEvent&)>;

    virtual ~DirectoryWatcher() = default;
    // False once the watch broke (handle error, folder removed); the owner
    // re-creates it and rescans, since events may have been missed.
    [[nodiscard]] virtual bool healthy() const = 0;

    // Returns nullptr when native watching is unavailable for `directory`
    // (unsupported platform, network drive); the caller then polls.
    [[nodiscard]] static std::unique_ptr<DirectoryWatcher> create(const std::filesystem::path& directory, Sink sink,
                                                                  std::string& error);
};

} // namespace openrts::core::detail
