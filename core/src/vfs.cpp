#include <openrts/core/vfs.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <openrts/core/zip_archive.hpp>

#include "directory_watcher.hpp"

namespace openrts::core {
namespace fs = std::filesystem;

const char* to_string(const VfsLayer layer) noexcept {
    switch (layer) {
    case VfsLayer::base_pack: return "base";
    case VfsLayer::folder: return "folder";
    case VfsLayer::patch_pack: return "patch";
    case VfsLayer::mod: return "mod";
    }
    return "unknown";
}

namespace {

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

[[nodiscard]] std::int64_t file_time(const fs::file_time_type time) {
    return static_cast<std::int64_t>(time.time_since_epoch().count());
}

struct FileStat {
    std::uint64_t size{};
    std::int64_t mtime{};
    friend bool operator==(const FileStat&, const FileStat&) = default;
};

[[nodiscard]] std::optional<FileStat> stat_file(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) return std::nullopt;
    const auto size = fs::file_size(path, error);
    if (error) return std::nullopt;
    const auto time = fs::last_write_time(path, error);
    if (error) return std::nullopt;
    return FileStat{size, file_time(time)};
}

// One file provided by a source.
struct SourceFile {
    std::string path;        // virtual path, as spelled
    std::string location;    // folder: path below the source root; archive: member name
    std::uint64_t size{};
    std::int64_t mtime{};    // folder files
    std::uint32_t crc{};     // archive members
    std::uint32_t zip_index{};

    [[nodiscard]] bool same_content(const SourceFile& other) const noexcept {
        return size == other.size && mtime == other.mtime && crc == other.crc;
    }
};

using FileTable = std::unordered_map<std::string, SourceFile>; // key -> file

// Immutable snapshot of one mounted folder or archive. A rescan or remount
// creates a new Source; readers holding an index keep the old one alive.
struct Source {
    std::size_t mount_id{};
    std::string name;
    VfsLayer layer{};
    int priority{};
    std::uint64_t order{};
    bool archive = false;
    fs::path location;
    VirtualPath mount_point;
    FileTable files;
    std::optional<ZipReader> zip;
    FileStat archive_stat;
    std::string error;

    [[nodiscard]] std::optional<std::string> read(const SourceFile& file, std::string& error_text) const {
        if (archive) {
            if (!zip || file.zip_index >= zip->entries().size()) {
                error_text = "Archive entry unavailable.";
                return std::nullopt;
            }
            return zip->read(zip->entries()[file.zip_index], error_text);
        }
        std::ifstream input(location / fs::path(std::u8string(file.location.begin(), file.location.end())),
                            std::ios::binary);
        if (!input) {
            error_text = "Cannot open " + name + "/" + file.location;
            return std::nullopt;
        }
        return std::string(std::istreambuf_iterator<char>(input), {});
    }
};

[[nodiscard]] bool higher_priority(const Source& a, const Source& b) {
    if (a.layer != b.layer) return a.layer > b.layer;
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.order > b.order;
}

void add_folder_file(FileTable& table, const VirtualPath& mount_point, const fs::path& root, const fs::path& file,
                     const std::uint64_t size, const std::int64_t mtime) {
    const auto relative = file.lexically_relative(root);
    const auto path = mount_point.resolve_filesystem(relative);
    if (!path || path->empty()) return;
    table[path->key()] = SourceFile{
        .path = path->str(),
        .location = utf8(relative),
        .size = size,
        .mtime = mtime,
    };
}

// Adds every file below `directory` (inclusive) to `table`.
void scan_tree(FileTable& table, const VirtualPath& mount_point, const fs::path& root, const fs::path& directory) {
    std::error_code error;
    for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || entry_error) continue;
        const auto size = it->file_size(entry_error);
        if (entry_error) continue;
        const auto time = it->last_write_time(entry_error);
        if (entry_error) continue;
        add_folder_file(table, mount_point, root, it->path(), size, file_time(time));
    }
}

[[nodiscard]] std::shared_ptr<Source> scan_folder(const Source& base) {
    auto source = std::make_shared<Source>(base);
    source->files.clear();
    source->error.clear();
    std::error_code error;
    if (!fs::is_directory(source->location, error)) {
        source->error = "folder not found";
        return source;
    }
    scan_tree(source->files, source->mount_point, source->location, source->location);
    return source;
}

// Applies changes below `changed` relative paths to a copy of `base`.
[[nodiscard]] std::shared_ptr<Source> update_folder(const Source& base, const std::set<fs::path>& changed) {
    auto source = std::make_shared<Source>(base);
    for (const auto& relative : changed) {
        const auto path = source->mount_point.resolve_filesystem(relative);
        if (!path) continue;
        std::erase_if(source->files, [&](const auto& item) {
            const auto key = std::string_view(item.first);
            return key == path->key()
                || (key.size() > path->key().size() && key.starts_with(path->key()) && key[path->key().size()] == '/');
        });
        const auto absolute = source->location / relative;
        std::error_code error;
        if (fs::is_directory(absolute, error)) {
            scan_tree(source->files, source->mount_point, source->location, absolute);
        } else if (const auto stat = stat_file(absolute)) {
            add_folder_file(source->files, source->mount_point, source->location, absolute, stat->size, stat->mtime);
        }
    }
    return source;
}

[[nodiscard]] std::optional<std::string> member_text(const ZipReader& zip, const std::string_view name) {
    for (const auto& entry : zip.entries()) {
        if (fold_case(entry.name) == name) {
            std::string error;
            return zip.read(entry, error);
        }
    }
    return std::nullopt;
}

// `Priority = n` from the pack manifest, if any.
[[nodiscard]] std::optional<int> manifest_priority(const ZipReader& zip) {
    const auto text = member_text(zip, "openrts-pack.ini");
    if (!text) return std::nullopt;
    std::size_t at = 0;
    while (at < text->size()) {
        auto end = text->find('\n', at);
        if (end == std::string::npos) end = text->size();
        auto line = std::string_view(*text).substr(at, end - at);
        at = end + 1;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        auto key = line.substr(0, equals);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.remove_prefix(1);
        if (fold_case(key) != "priority") continue;
        try {
            return std::stoi(std::string(line.substr(equals + 1)));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::shared_ptr<Source> open_archive(const Source& base) {
    auto source = std::make_shared<Source>(base);
    source->files.clear();
    source->zip.reset();
    source->error.clear();
    const auto stat = stat_file(source->location);
    if (!stat) {
        source->error = "archive not found";
        return source;
    }
    std::string error;
    auto zip = ZipReader::open(source->location, error);
    if (!zip) {
        source->error = error;
        return source;
    }
    source->archive_stat = *stat;
    if (const auto priority = manifest_priority(*zip)) source->priority = *priority;
    for (std::uint32_t i = 0; i < zip->entries().size(); ++i) {
        const auto& entry = zip->entries()[i];
        if (entry.directory()) continue;
        const auto path = source->mount_point.resolve(std::string_view(entry.name));
        if (!path || path->empty()) continue;
        source->files[path->key()] = SourceFile{
            .path = path->str(),
            .location = entry.name,
            .size = entry.size,
            .crc = entry.crc32,
            .zip_index = i,
        };
    }
    source->zip = std::move(zip);
    return source;
}

class MemoryStream final : public VfsStream {
public:
    explicit MemoryStream(std::string data) : data_(std::move(data)) {}
    [[nodiscard]] std::uint64_t size() const override { return data_.size(); }
    [[nodiscard]] std::uint64_t position() const override { return at_; }
    bool seek(const std::uint64_t position) override {
        if (position > data_.size()) return false;
        at_ = static_cast<std::size_t>(position);
        return true;
    }
    std::size_t read(void* buffer, const std::size_t count) override {
        const auto n = (std::min)(count, data_.size() - at_);
        std::memcpy(buffer, data_.data() + at_, n);
        at_ += n;
        return n;
    }

private:
    std::string data_;
    std::size_t at_ = 0;
};

// A byte range of a file on disk: a folder file, or a stored archive member.
class FileRangeStream final : public VfsStream {
public:
    FileRangeStream(std::ifstream file, const std::uint64_t begin, const std::uint64_t size)
        : file_(std::move(file)), begin_(begin), size_(size) {
        file_.seekg(static_cast<std::streamoff>(begin_));
    }
    [[nodiscard]] std::uint64_t size() const override { return size_; }
    [[nodiscard]] std::uint64_t position() const override { return at_; }
    bool seek(const std::uint64_t position) override {
        if (position > size_) return false;
        at_ = position;
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(begin_ + at_));
        return static_cast<bool>(file_);
    }
    std::size_t read(void* buffer, const std::size_t count) override {
        const auto n = static_cast<std::size_t>((std::min<std::uint64_t>)(count, size_ - at_));
        if (n == 0) return 0;
        file_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(n));
        const auto got = static_cast<std::size_t>(file_.gcount());
        at_ += got;
        return got;
    }

private:
    std::ifstream file_;
    std::uint64_t begin_;
    std::uint64_t size_;
    std::uint64_t at_ = 0;
};

} // namespace

// One index generation: the effective file of every path, the providers of
// overridden paths and the directory tree. Immutable once published.
class VfsIndex {
public:
    struct Hit {
        const Source* source = nullptr;
        const SourceFile* file = nullptr;
    };
    struct Directory {
        std::string path;
        std::vector<std::string> children; // child keys
    };

    std::uint64_t generation{};
    std::vector<std::shared_ptr<const Source>> sources; // lowest priority first
    std::unordered_map<std::string, Hit> files;
    std::unordered_map<std::string, std::vector<const Source*>> providers; // only overridden paths
    std::unordered_map<std::string, Directory> directories;

    [[nodiscard]] const Hit* find(const VirtualPath& path) const {
        const auto it = files.find(path.key());
        return it == files.end() ? nullptr : &it->second;
    }
};

namespace {

[[nodiscard]] std::shared_ptr<const VfsIndex> build(std::vector<std::shared_ptr<const Source>> sources,
                                                    const std::uint64_t generation) {
    auto index = std::make_shared<VfsIndex>();
    index->generation = generation;
    std::stable_sort(sources.begin(), sources.end(),
                     [](const auto& a, const auto& b) { return higher_priority(*b, *a); });
    index->sources = std::move(sources);

    std::size_t total = 0;
    for (const auto& source : index->sources) total += source->files.size();
    index->files.reserve(total);

    for (const auto& source : index->sources) {
        for (const auto& [key, file] : source->files) {
            auto [it, inserted] = index->files.try_emplace(key, VfsIndex::Hit{source.get(), &file});
            if (inserted) continue;
            auto& providers = index->providers[key];
            if (providers.empty()) providers.push_back(it->second.source);
            providers.push_back(source.get());
            it->second = {source.get(), &file};
        }
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> children;
    index->directories[""] = {};
    for (const auto& [key, hit] : index->files) {
        std::string child = key;
        std::string_view spelled = hit.file->path;
        while (true) {
            const auto slash = child.rfind('/');
            const auto parent = slash == std::string::npos ? std::string() : child.substr(0, slash);
            auto& set = children[parent];
            const bool fresh = set.insert(child).second;
            auto [dir, created] = index->directories.try_emplace(parent);
            if (created) {
                const auto spelled_slash = spelled.rfind('/');
                dir->second.path = std::string(spelled.substr(0, spelled_slash == std::string_view::npos ? 0 : spelled_slash));
            }
            if (!fresh || parent.empty()) break;
            child = parent;
            const auto spelled_slash = spelled.rfind('/');
            spelled = spelled.substr(0, spelled_slash == std::string_view::npos ? 0 : spelled_slash);
        }
    }
    for (auto& [key, set] : children) {
        auto& dir = index->directories[key];
        dir.children.assign(set.begin(), set.end());
        std::sort(dir.children.begin(), dir.children.end());
    }
    return index;
}

// Paths whose effective version differs between two generations, limited to
// `candidates` (the keys touched by the sources that changed).
[[nodiscard]] std::vector<VirtualPath> diff(const VfsIndex* before, const VfsIndex& after,
                                            const std::unordered_set<std::string>& candidates) {
    std::vector<VirtualPath> changed;
    for (const auto& key : candidates) {
        const VfsIndex::Hit* old_hit = nullptr;
        if (before) {
            if (const auto it = before->files.find(key); it != before->files.end()) old_hit = &it->second;
        }
        const auto it = after.files.find(key);
        const VfsIndex::Hit* new_hit = it == after.files.end() ? nullptr : &it->second;
        const bool same = (old_hit == nullptr) == (new_hit == nullptr)
            && (!old_hit
                || (old_hit->source->mount_id == new_hit->source->mount_id
                    && old_hit->file->same_content(*new_hit->file)));
        if (same) continue;
        const auto& spelled = new_hit ? new_hit->file->path : old_hit->file->path;
        if (auto path = VirtualPath::parse(spelled)) changed.push_back(std::move(*path));
    }
    std::sort(changed.begin(), changed.end());
    return changed;
}

[[nodiscard]] std::string hex_hash(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (const auto c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i, hash >>= 4) result[static_cast<std::size_t>(i)] = digits[hash & 0xF];
    return result;
}

} // namespace

struct Vfs::Impl {
    struct Mount {
        std::size_t id{};
        std::shared_ptr<const Source> source;
        std::optional<std::size_t> pack_directory; // auto-mounted by this pack directory
        // Pending work for the background thread.
        bool full_rescan = false;
        std::set<fs::path> changed;
        bool archive_check = false;
        std::optional<FileStat> archive_seen; // stat at the previous check, for stability
        bool polled = false;                  // no native watcher
    };
    struct PackDirectory {
        fs::path directory;
        VfsLayer layer{};
        bool dirty = false;
        bool polled = false;
    };
    struct Watch {
        enum class Kind { folder, pack_directory, archive_parent } kind{};
        std::size_t target{}; // mount id or pack directory index
        fs::path directory;
        std::unique_ptr<detail::DirectoryWatcher> watcher;
    };
    struct RawEvent {
        std::size_t watch{};
        detail::WatchEvent event;
    };

    // Mounts and rebuilds; `work` serialises the watcher thread with
    // rescan_all() and mounting.
    mutable std::mutex work;
    std::vector<Mount> mounts;
    std::vector<PackDirectory> pack_directories;
    std::size_t next_mount_id = 1;
    std::uint64_t next_order = 0;
    std::uint64_t next_generation = 1;

    std::atomic<std::shared_ptr<const VfsIndex>> index{std::make_shared<VfsIndex>()};

    std::mutex changes_mutex;
    std::vector<VirtualPath> changes;

    // Watching.
    VfsWatchOptions watch_options;
    std::vector<Watch> watches;
    std::mutex events_mutex;
    std::condition_variable events_signal;
    std::vector<RawEvent> events;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<bool> watching{false};
    std::atomic<std::size_t> rescans{0};
    std::atomic<std::size_t> lost_event_rescans{0};

    [[nodiscard]] std::shared_ptr<const VfsIndex> snapshot() const { return index.load(); }

    [[nodiscard]] Mount* find_mount(const std::size_t id) {
        const auto it = std::find_if(mounts.begin(), mounts.end(), [&](const Mount& m) { return m.id == id; });
        return it == mounts.end() ? nullptr : &*it;
    }

    std::size_t add_mount(const fs::path& location, VfsMountOptions options, const bool archive,
                          std::optional<std::size_t> pack_directory = std::nullopt) {
        Source source;
        source.mount_id = next_mount_id++;
        source.name = options.name.empty() ? utf8(location.filename()) : options.name;
        source.layer = options.layer;
        source.priority = options.priority;
        source.order = next_order++;
        source.archive = archive;
        source.location = location;
        source.mount_point = options.mount_point;
        mounts.push_back({
            .id = source.mount_id,
            .source = archive ? open_archive(source) : scan_folder(source),
            .pack_directory = pack_directory,
        });
        return source.mount_id;
    }

    // Mounts new archives of a pack directory and drops vanished ones.
    void sync_pack_directory(const std::size_t index_of_directory) {
        const auto& directory = pack_directories[index_of_directory];
        std::set<fs::path> present;
        std::error_code error;
        for (fs::directory_iterator it(directory.directory, error), end; !error && it != end; it.increment(error)) {
            std::error_code entry_error;
            if (!it->is_regular_file(entry_error)) continue;
            if (fold_case(utf8(it->path().extension())) != ".zip") continue;
            present.insert(it->path());
        }
        std::erase_if(mounts, [&](const Mount& mount) {
            return mount.pack_directory == index_of_directory && !present.contains(mount.source->location);
        });
        for (const auto& archive : present) {
            const bool known = std::any_of(mounts.begin(), mounts.end(), [&](const Mount& mount) {
                return mount.pack_directory == index_of_directory && mount.source->location == archive;
            });
            if (known) continue;
            add_mount(archive, {.layer = directory.layer}, true, index_of_directory);
        }
    }

    // Builds and publishes a new generation; returns the changed paths, which
    // are also queued for poll_changes(). `touched` are keys of sources that
    // changed (old and new contents).
    std::vector<VirtualPath> publish(const std::unordered_set<std::string>& touched) {
        std::vector<std::shared_ptr<const Source>> sources;
        sources.reserve(mounts.size());
        for (const auto& mount : mounts) sources.push_back(mount.source);
        const auto before = snapshot();
        auto after = build(std::move(sources), next_generation++);
        auto changed = diff(before.get(), *after, touched);
        index.store(std::move(after));
        if (!changed.empty()) {
            const std::scoped_lock lock(changes_mutex);
            changes.insert(changes.end(), changed.begin(), changed.end());
        }
        return changed;
    }

    static void touch(std::unordered_set<std::string>& touched, const Source& source) {
        for (const auto& [key, _] : source.files) touched.insert(key);
    }

    // Applies all pending mount work (caller holds `work`). `force` skips the
    // archive stability check (rescan_all).
    std::vector<VirtualPath> process(const bool force) {
        std::unordered_set<std::string> touched;
        bool any = false;

        // Pack directories first: they add and remove mounts.
        for (std::size_t i = 0; i < pack_directories.size(); ++i) {
            if (!pack_directories[i].dirty) continue;
            pack_directories[i].dirty = false;
            std::vector<std::shared_ptr<const Source>> old;
            for (const auto& mount : mounts) {
                if (mount.pack_directory == i) old.push_back(mount.source);
            }
            sync_pack_directory(i);
            for (const auto& source : old) {
                const bool still = std::any_of(mounts.begin(), mounts.end(), [&](const Mount& m) { return m.source == source; });
                if (!still) {
                    touch(touched, *source);
                    any = true;
                }
            }
            for (auto& mount : mounts) {
                if (mount.pack_directory != i) continue;
                const bool fresh = std::none_of(old.begin(), old.end(), [&](const auto& s) { return s == mount.source; });
                if (fresh) {
                    touch(touched, *mount.source);
                    any = true;
                } else {
                    mount.archive_check = true;
                }
            }
        }

        for (auto& mount : mounts) {
            const auto& source = *mount.source;
            std::shared_ptr<const Source> next;
            if (source.archive && mount.archive_check) {
                const auto stat = stat_file(source.location);
                if (!force && stat && mount.archive_seen != stat) {
                    // Still being written or just changed: check again after
                    // the next quiet period.
                    mount.archive_seen = stat;
                    continue;
                }
                mount.archive_check = false;
                mount.archive_seen.reset();
                if (stat && *stat == source.archive_stat && source.error.empty()) continue;
                auto reopened = open_archive(source);
                if (!reopened->error.empty() && stat) {
                    // Corrupt or half-written: keep the old mount.
                    auto kept = std::make_shared<Source>(source);
                    kept->error = reopened->error;
                    next = std::move(kept);
                } else {
                    next = std::move(reopened);
                }
                ++rescans;
            } else if (!source.archive && mount.full_rescan) {
                next = scan_folder(source);
                ++rescans;
            } else if (!source.archive && !mount.changed.empty()) {
                next = update_folder(source, mount.changed);
            }
            mount.full_rescan = false;
            mount.changed.clear();
            if (!next) continue;
            touch(touched, source);
            touch(touched, *next);
            mount.source = std::move(next);
            any = true;
        }
        if (!any) return {};
        return publish(touched);
    }

    [[nodiscard]] bool pending_archive_checks() const {
        return std::any_of(mounts.begin(), mounts.end(), [](const Mount& m) { return m.archive_check; });
    }

    void mark_everything(const bool lost_events) {
        for (auto& mount : mounts) {
            if (mount.source->archive) {
                mount.archive_check = true;
            } else {
                mount.full_rescan = true;
            }
        }
        for (auto& directory : pack_directories) directory.dirty = true;
        if (lost_events) ++lost_event_rescans;
    }

    void route(const RawEvent& raw) {
        if (raw.watch >= watches.size()) return;
        const auto& watch = watches[raw.watch];
        switch (watch.kind) {
        case Watch::Kind::folder:
            if (auto* mount = find_mount(watch.target)) {
                if (raw.event.overflow) {
                    mount->full_rescan = true;
                    ++lost_event_rescans;
                } else {
                    mount->changed.insert(raw.event.relative);
                }
            }
            break;
        case Watch::Kind::pack_directory:
            if (watch.target < pack_directories.size()) pack_directories[watch.target].dirty = true;
            break;
        case Watch::Kind::archive_parent:
            if (auto* mount = find_mount(watch.target)) {
                const auto name = fold_case(utf8(raw.event.relative.filename()));
                if (raw.event.overflow || name == fold_case(utf8(mount->source->location.filename()))) {
                    mount->archive_check = true;
                }
            }
            break;
        }
    }

    void create_watches() {
        watches.clear();
        const auto add = [&](Watch::Kind kind, const std::size_t target, const fs::path& directory) {
            const auto slot = watches.size();
            std::string error;
            auto watcher = detail::DirectoryWatcher::create(
                directory,
                [this, slot](const detail::WatchEvent& event) {
                    {
                        const std::scoped_lock lock(events_mutex);
                        events.push_back({slot, event});
                    }
                    events_signal.notify_one();
                },
                error);
            const bool polled = watcher == nullptr;
            watches.push_back({kind, target, directory, std::move(watcher)});
            return polled;
        };
        for (auto& mount : mounts) {
            if (mount.pack_directory) continue; // covered by the directory watch
            if (mount.source->archive) {
                mount.polled = add(Watch::Kind::archive_parent, mount.id, mount.source->location.parent_path());
            } else {
                mount.polled = add(Watch::Kind::folder, mount.id, mount.source->location);
            }
        }
        for (std::size_t i = 0; i < pack_directories.size(); ++i) {
            pack_directories[i].polled = add(Watch::Kind::pack_directory, i, pack_directories[i].directory);
        }
    }

    void run() {
        using clock = std::chrono::steady_clock;
        auto last_event = clock::now();
        auto last_reconcile = clock::now();
        auto last_poll = clock::now();
        bool pending = false;

        while (!stopping) {
            std::vector<RawEvent> batch;
            {
                std::unique_lock lock(events_mutex);
                events_signal.wait_for(lock, std::chrono::milliseconds(100),
                                       [&] { return stopping || !events.empty(); });
                batch.swap(events);
            }
            if (stopping) return;
            const auto now = clock::now();

            const std::scoped_lock lock(work);
            if (!batch.empty()) {
                for (const auto& raw : batch) route(raw);
                last_event = now;
                pending = true;
            }

            // A watch that broke may have missed events: re-arm it and rescan.
            for (auto& watch : watches) {
                if (!watch.watcher || watch.watcher->healthy()) continue;
                std::string error;
                const auto slot = static_cast<std::size_t>(&watch - watches.data());
                watch.watcher = detail::DirectoryWatcher::create(
                    watch.directory,
                    [this, slot](const detail::WatchEvent& event) {
                        {
                            const std::scoped_lock events_lock(events_mutex);
                            events.push_back({slot, event});
                        }
                        events_signal.notify_one();
                    },
                    error);
                route({slot, {.overflow = true}});
                last_event = now;
                pending = true;
            }

            if (now - last_poll >= watch_options.poll_interval) {
                last_poll = now;
                for (auto& mount : mounts) {
                    if (!mount.polled) continue;
                    if (mount.source->archive) {
                        mount.archive_check = true;
                    } else {
                        mount.full_rescan = true;
                    }
                    pending = true;
                }
                for (auto& directory : pack_directories) {
                    if (directory.polled) {
                        directory.dirty = true;
                        pending = true;
                    }
                }
            }
            if (watch_options.reconcile_interval.count() > 0 && now - last_reconcile >= watch_options.reconcile_interval) {
                last_reconcile = now;
                mark_everything(false);
                pending = true;
            }

            if (pending && now - last_event >= watch_options.debounce) {
                process(false);
                pending = pending_archive_checks();
                if (pending) last_event = now; // re-check archive stability later
            }
        }
    }
};

Vfs::Vfs() : impl_(std::make_unique<Impl>()) {}

Vfs::~Vfs() {
    stop_watching();
}

std::size_t Vfs::mount_directory(const fs::path& root, VfsMountOptions options) {
    const std::scoped_lock lock(impl_->work);
    return impl_->add_mount(root, std::move(options), false);
}

std::size_t Vfs::mount_zip(const fs::path& archive, VfsMountOptions options) {
    const std::scoped_lock lock(impl_->work);
    return impl_->add_mount(archive, std::move(options), true);
}

void Vfs::mount_pack_directory(const fs::path& directory, const VfsLayer layer) {
    const std::scoped_lock lock(impl_->work);
    impl_->pack_directories.push_back({.directory = directory, .layer = layer});
    impl_->sync_pack_directory(impl_->pack_directories.size() - 1);
}

void Vfs::build_index() {
    const std::scoped_lock lock(impl_->work);
    impl_->publish({}); // the initial build reports no changes
}

bool Vfs::exists(const VirtualPath& path) const {
    return impl_->snapshot()->find(path) != nullptr;
}

bool Vfs::is_directory(const VirtualPath& path) const {
    const auto index = impl_->snapshot();
    return index->directories.contains(path.key());
}

std::optional<std::string> Vfs::read(const VirtualPath& path, std::string* error) const {
    const auto index = impl_->snapshot();
    const auto* hit = index->find(path);
    std::string message;
    if (!hit) {
        if (error) *error = "Not found: " + path.str();
        return std::nullopt;
    }
    auto data = hit->source->read(*hit->file, message);
    if (!data && error) *error = message;
    return data;
}

std::unique_ptr<VfsStream> Vfs::open(const VirtualPath& path, std::string* error) const {
    const auto index = impl_->snapshot();
    const auto* hit = index->find(path);
    if (!hit) {
        if (error) *error = "Not found: " + path.str();
        return nullptr;
    }
    const auto& source = *hit->source;
    std::string message;
    if (!source.archive) {
        const auto file = source.location / fs::path(std::u8string(hit->file->location.begin(), hit->file->location.end()));
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            if (error) *error = "Cannot open " + utf8(file);
            return nullptr;
        }
        return std::make_unique<FileRangeStream>(std::move(input), 0, hit->file->size);
    }
    const auto& entry = source.zip->entries()[hit->file->zip_index];
    if (static_cast<ZipMethod>(entry.method) == ZipMethod::stored) {
        if (const auto offset = source.zip->data_offset(entry, message)) {
            std::ifstream input(source.location, std::ios::binary);
            if (input) return std::make_unique<FileRangeStream>(std::move(input), *offset, entry.size);
        }
    } else if (auto data = source.read(*hit->file, message)) {
        return std::make_unique<MemoryStream>(std::move(*data));
    }
    if (error) *error = message;
    return nullptr;
}

std::vector<VfsEntry> Vfs::list(const VirtualPath& directory) const {
    const auto index = impl_->snapshot();
    std::vector<VfsEntry> result;
    const auto dir = index->directories.find(directory.key());
    if (dir == index->directories.end()) return result;
    for (const auto& child : dir->second.children) {
        if (const auto file = index->files.find(child); file != index->files.end()) {
            if (auto path = VirtualPath::parse(file->second.file->path)) {
                result.push_back({.path = std::move(*path), .directory = false, .size = file->second.file->size});
            }
        } else if (const auto sub = index->directories.find(child); sub != index->directories.end()) {
            if (auto path = VirtualPath::parse(sub->second.path)) result.push_back({.path = std::move(*path), .directory = true});
        }
    }
    return result;
}

std::optional<VfsOrigin> Vfs::origin(const VirtualPath& path) const {
    const auto index = impl_->snapshot();
    const auto* hit = index->find(path);
    if (!hit) return std::nullopt;
    const auto& source = *hit->source;
    VfsOrigin result{
        .source_id = source.mount_id,
        .source_name = source.name,
        .layer = source.layer,
        .archive = source.archive,
        .location = source.location,
        .size = hit->file->size,
    };
    if (source.archive) {
        result.member = hit->file->location;
    } else {
        result.location = source.location / fs::path(std::u8string(hit->file->location.begin(), hit->file->location.end()));
    }
    return result;
}

std::vector<VfsConflict> Vfs::conflicts() const {
    const auto index = impl_->snapshot();
    std::vector<VfsConflict> result;
    for (const auto& [key, providers] : index->providers) {
        const auto& file = index->files.at(key);
        auto path = VirtualPath::parse(file.file->path);
        if (!path) continue;
        VfsConflict conflict{.path = std::move(*path)};
        for (const auto* source : providers) {
            conflict.sources.push_back(std::string(to_string(source->layer)) + ":" + source->name);
        }
        result.push_back(std::move(conflict));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return result;
}

std::vector<VfsSourceInfo> Vfs::sources() const {
    const auto index = impl_->snapshot();
    std::vector<VfsSourceInfo> result;
    for (const auto& source : index->sources) {
        result.push_back({
            .id = source->mount_id,
            .name = source->name,
            .layer = source->layer,
            .priority = source->priority,
            .archive = source->archive,
            .location = source->location,
            .mount_point = source->mount_point,
            .files = source->files.size(),
            .error = source->error,
        });
    }
    return result;
}

VfsStats Vfs::stats() const {
    const auto index = impl_->snapshot();
    return {
        .generation = index->generation,
        .sources = index->sources.size(),
        .files = index->files.size(),
        .directories = index->directories.size(),
        .conflicts = index->providers.size(),
        .rescans = impl_->rescans,
        .lost_event_rescans = impl_->lost_event_rescans,
        .watching = impl_->watching,
    };
}

std::uint64_t Vfs::generation() const {
    return impl_->snapshot()->generation;
}

std::optional<fs::path> Vfs::real_path(const VirtualPath& path, const fs::path& cache_root, std::string* error) const {
    const auto index = impl_->snapshot();
    const auto* hit = index->find(path);
    if (!hit) {
        if (error) *error = "Not found: " + path.str();
        return std::nullopt;
    }
    const auto& source = *hit->source;
    if (!source.archive) {
        return source.location / fs::path(std::u8string(hit->file->location.begin(), hit->file->location.end()));
    }

    const auto stamp = utf8(source.location) + "|" + std::to_string(source.archive_stat.size) + "|"
        + std::to_string(source.archive_stat.mtime);
    const auto target = cache_root / "extracted" / hex_hash(stamp) / path.to_filesystem();
    if (const auto existing = stat_file(target); existing && existing->size == hit->file->size) return target;

    std::string message;
    const auto data = source.read(*hit->file, message);
    if (!data) {
        if (error) *error = message;
        return std::nullopt;
    }
    std::error_code fs_error;
    fs::create_directories(target.parent_path(), fs_error);
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(data->data(), static_cast<std::streamsize>(data->size()));
        if (!out) {
            if (error) *error = "Cannot write " + utf8(temporary);
            return std::nullopt;
        }
    }
    fs::rename(temporary, target, fs_error);
    if (fs_error) {
        if (error) *error = fs_error.message();
        return std::nullopt;
    }
    return target;
}

void Vfs::start_watching(const VfsWatchOptions options) {
    if (impl_->watching) return;
    {
        const std::scoped_lock lock(impl_->work);
        impl_->watch_options = options;
        impl_->stopping = false;
        impl_->create_watches();
    }
    impl_->watching = true;
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
}

void Vfs::stop_watching() {
    if (!impl_->watching) return;
    impl_->stopping = true;
    impl_->events_signal.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    const std::scoped_lock lock(impl_->work);
    impl_->watches.clear();
    impl_->watching = false;
}

std::vector<VirtualPath> Vfs::poll_changes() {
    std::vector<VirtualPath> result;
    {
        const std::scoped_lock lock(impl_->changes_mutex);
        result.swap(impl_->changes);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<VirtualPath> Vfs::rescan_all() {
    const std::scoped_lock lock(impl_->work);
    impl_->mark_everything(true);
    return impl_->process(true);
}

} // namespace openrts::core
