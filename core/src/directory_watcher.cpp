#include "directory_watcher.hpp"

#include <atomic>
#include <span>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <map>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace openrts::core::detail {
namespace {

#ifdef _WIN32

// Network drives do not deliver reliable change notifications.
[[nodiscard]] bool is_remote_drive(const std::filesystem::path& directory) {
    const auto root = directory.root_path().wstring();
    if (root.empty()) return false;
    if (root.rfind(L"\\\\", 0) == 0) return true; // UNC path
    return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

// Overlapped ReadDirectoryChangesW: the first request is made in the
// constructor, so a change right after it is caught (Windows queues a
// directory's changes from its first request on; the thread making the first
// one itself missed what came before it ran, a file written right after
// start_watching on CI, 2026-11-10).
class WindowsWatcher final : public DirectoryWatcher {
public:
    WindowsWatcher(HANDLE directory, Sink sink) : directory_(directory), sink_(std::move(sink)) {
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped_.hEvent || !stop_ || !request()) {
            healthy_ = false;
            return;
        }
        thread_ = std::thread([this] { run(); });
    }

    ~WindowsWatcher() override {
        if (stop_) SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
        // A request still pending is cancelled and waited for before its
        // buffer goes.
        if (pending_) {
            CancelIoEx(directory_, &overlapped_);
            DWORD bytes = 0;
            GetOverlappedResult(directory_, &overlapped_, &bytes, TRUE);
        }
        if (overlapped_.hEvent) CloseHandle(overlapped_.hEvent);
        if (stop_) CloseHandle(stop_);
        CloseHandle(directory_);
    }

    [[nodiscard]] bool healthy() const override { return healthy_; }

private:
    bool request() {
        constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
            | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
        ResetEvent(overlapped_.hEvent);
        pending_ = ReadDirectoryChangesW(directory_, buffer_.data(), static_cast<DWORD>(buffer_.size()), TRUE, filter, nullptr,
                                         &overlapped_, nullptr)
            != FALSE;
        return pending_;
    }

    void run() {
        const HANDLE events[2] = {stop_, overlapped_.hEvent};
        while (true) {
            const auto woke = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (woke != WAIT_OBJECT_0 + 1) return; // stopping (or the wait failed)
            DWORD bytes = 0;
            const auto ok = GetOverlappedResult(directory_, &overlapped_, &bytes, FALSE);
            pending_ = false;
            if (!ok) {
                if (GetLastError() == ERROR_NOTIFY_ENUM_DIR) {
                    sink_({.overflow = true});
                } else {
                    healthy_ = false; // folder removed, handle invalid, ...
                    return;
                }
            } else if (bytes == 0) { // buffer overflow: events were dropped
                sink_({.overflow = true});
            } else {
                std::size_t offset = 0;
                while (true) {
                    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer_.data() + offset);
                    const std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    sink_({.relative = std::filesystem::path(name)});
                    if (info->NextEntryOffset == 0) break;
                    offset += info->NextEntryOffset;
                }
            }
            if (!request()) {
                healthy_ = false;
                return;
            }
        }
    }

    HANDLE directory_;
    Sink sink_;
    // DWORD-aligned, as ReadDirectoryChangesW wants.
    std::vector<DWORD> buffer_storage_ = std::vector<DWORD>(64 * 1024 / sizeof(DWORD));
    std::span<std::byte> buffer_{reinterpret_cast<std::byte*>(buffer_storage_.data()), 64 * 1024};
    OVERLAPPED overlapped_{};
    HANDLE stop_ = nullptr;
    std::atomic<bool> pending_{false};
    std::atomic<bool> healthy_{true};
    std::thread thread_;
};

#elif defined(__linux__)

// inotify is not recursive: every directory gets its own watch, and new
// directories are added as they appear.
class InotifyWatcher final : public DirectoryWatcher {
public:
    InotifyWatcher(int fd, std::filesystem::path root, Sink sink)
        : fd_(fd), root_(std::move(root)), sink_(std::move(sink)) {
        add_tree(root_);
        thread_ = std::thread([this] { run(); });
    }

    ~InotifyWatcher() override {
        stopping_ = true;
        if (thread_.joinable()) thread_.join();
        close(fd_);
    }

    [[nodiscard]] bool healthy() const override { return healthy_; }

private:
    static constexpr std::uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM
        | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;

    void add_tree(const std::filesystem::path& directory) {
        add_watch(directory);
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(
                 directory, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_directory(error)) add_watch(it->path());
        }
    }

    void add_watch(const std::filesystem::path& directory) {
        const auto wd = inotify_add_watch(fd_, directory.c_str(), mask);
        if (wd >= 0) directories_[wd] = directory.lexically_relative(root_);
    }

    void run() {
        std::vector<char> buffer(64 * 1024);
        while (!stopping_) {
            pollfd descriptor{.fd = fd_, .events = POLLIN, .revents = 0};
            const auto ready = poll(&descriptor, 1, 200);
            if (ready <= 0) continue;
            const auto length = ::read(fd_, buffer.data(), buffer.size());
            if (length <= 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                healthy_ = false;
                return;
            }
            for (ssize_t offset = 0; offset < length;) {
                const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
                if (event->mask & IN_Q_OVERFLOW) {
                    sink_({.overflow = true});
                    continue;
                }
                const auto directory = directories_.find(event->wd);
                if (directory == directories_.end()) continue;
                if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) && directory->second.empty()) {
                    healthy_ = false; // the watched root itself went away
                    return;
                }
                auto relative = directory->second;
                if (event->len > 0) relative /= event->name;
                if ((event->mask & (IN_CREATE | IN_MOVED_TO)) && (event->mask & IN_ISDIR)) add_tree(root_ / relative);
                sink_({.relative = relative});
            }
        }
    }

    int fd_;
    std::filesystem::path root_;
    Sink sink_;
    std::map<int, std::filesystem::path> directories_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> healthy_{true};
    std::thread thread_;
};

#endif

} // namespace

std::unique_ptr<DirectoryWatcher> DirectoryWatcher::create(const std::filesystem::path& directory, Sink sink,
                                                           std::string& error) {
#ifdef _WIN32
    if (is_remote_drive(directory)) {
        error = "network drive: polled";
        return nullptr;
    }
    const auto handle = CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "cannot open directory for watching";
        return nullptr;
    }
    return std::make_unique<WindowsWatcher>(handle, std::move(sink));
#elif defined(__linux__)
    const auto fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        error = "inotify unavailable";
        return nullptr;
    }
    return std::make_unique<InotifyWatcher>(fd, directory, std::move(sink));
#else
    (void)directory;
    (void)sink;
    error = "no native watcher on this platform: polled";
    return nullptr;
#endif
}

} // namespace openrts::core::detail
