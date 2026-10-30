#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/virtual_path.hpp>

namespace openrts::core {

// Mount layers, lowest priority first. A path provided by several sources is
// taken from the highest layer; inside a layer from the highest priority, then
// the latest mount.
enum class VfsLayer : std::uint8_t {
    base_pack = 0, // PACKS/base/*.zip
    folder = 1,    // ROUTES, TRAINS, GLOBAL, ... on disk
    patch_pack = 2, // PACKS/patch/*.zip
    mod = 3,       // MODS_OPENRTS/<mod> or <mod>.zip, in load order
};

[[nodiscard]] const char* to_string(VfsLayer layer) noexcept;

struct VfsMountOptions {
    VfsLayer layer = VfsLayer::folder;
    // Where the source appears in the namespace (`ROUTES` for a folder
    // holding routes); empty mounts at the root.
    VirtualPath mount_point;
    int priority = 0;
    std::string name; // shown in diagnostics; defaults to the file name
};

struct VfsSourceInfo {
    std::size_t id{};
    std::string name;
    VfsLayer layer{};
    int priority{};
    bool archive = false;
    std::filesystem::path location;
    VirtualPath mount_point;
    std::size_t files{};
    std::string error; // last mount/rescan error; the previous contents stay
};

struct VfsEntry {
    VirtualPath path;
    bool directory = false;
    std::uint64_t size{};
};

// Where the effective version of a path comes from.
struct VfsOrigin {
    std::size_t source_id{};
    std::string source_name;
    VfsLayer layer{};
    bool archive = false;
    std::filesystem::path location; // the file, or the archive holding it
    std::string member;             // entry name inside the archive
    std::uint64_t size{};
};

// A path provided by more than one source; `sources` lowest priority first,
// the last one wins.
struct VfsConflict {
    VirtualPath path;
    std::vector<std::string> sources;
};

// Sequential reader over one file (the whole decompressed file for
// compressed archive members).
class VfsStream {
public:
    virtual ~VfsStream() = default;
    [[nodiscard]] virtual std::uint64_t size() const = 0;
    [[nodiscard]] virtual std::uint64_t position() const = 0;
    virtual bool seek(std::uint64_t position) = 0;
    // Reads up to `count` bytes; returns the number read (0 at the end).
    virtual std::size_t read(void* buffer, std::size_t count) = 0;
};

struct VfsWatchOptions {
    // Quiet time before a burst of events is processed.
    std::chrono::milliseconds debounce{300};
    // Full comparison of every source with disk, in case events were lost.
    // Zero disables it.
    std::chrono::seconds reconcile_interval{180};
    // Interval for sources that cannot be watched (network drives, platforms
    // without native notifications).
    std::chrono::seconds poll_interval{5};
};

struct VfsStats {
    std::uint64_t generation{};
    std::size_t sources{};
    std::size_t files{};
    std::size_t directories{};
    std::size_t conflicts{};
    std::size_t rescans{};       // sources rescanned since start
    std::size_t lost_event_rescans{}; // of which caused by overflow or broken watches
    bool watching = false;
};

class VfsIndex;

// Virtual file system over folders and ZIP archives (see docs/vfs.md).
// Lookups use an immutable index generation that is swapped atomically when
// sources change, so concurrent readers always see a consistent view.
class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    // Mounting; the index is rebuilt by build_index().
    std::size_t mount_directory(const std::filesystem::path& root, VfsMountOptions options = {});
    std::size_t mount_zip(const std::filesystem::path& archive, VfsMountOptions options = {});
    // Mounts every *.zip in `directory` with `layer`, and keeps following the
    // directory: archives added, replaced or removed there are (re)mounted.
    void mount_pack_directory(const std::filesystem::path& directory, VfsLayer layer);
    void build_index();

    [[nodiscard]] bool exists(const VirtualPath& path) const;
    [[nodiscard]] bool is_directory(const VirtualPath& path) const;
    [[nodiscard]] std::optional<std::string> read(const VirtualPath& path, std::string* error = nullptr) const;
    [[nodiscard]] std::unique_ptr<VfsStream> open(const VirtualPath& path, std::string* error = nullptr) const;
    // Files and sub-directories directly inside `directory`, merged over all
    // sources, sorted by key.
    [[nodiscard]] std::vector<VfsEntry> list(const VirtualPath& directory) const;
    [[nodiscard]] std::optional<VfsOrigin> origin(const VirtualPath& path) const;
    [[nodiscard]] std::vector<VfsConflict> conflicts() const;
    [[nodiscard]] std::vector<VfsSourceInfo> sources() const;
    [[nodiscard]] VfsStats stats() const;
    [[nodiscard]] std::uint64_t generation() const;

    // A real file for code that needs one (legacy DLLs): folder files are
    // returned as they are, archive members are copied to
    // `cache_root/extracted/<archive hash>/<path>` on first use.
    [[nodiscard]] std::optional<std::filesystem::path> real_path(const VirtualPath& path,
                                                                 const std::filesystem::path& cache_root,
                                                                 std::string* error = nullptr) const;

    // Hot reload: watches every source; changes are applied by a background
    // thread that publishes a new index generation.
    void start_watching(VfsWatchOptions options = {});
    void stop_watching();
    // Virtual paths whose effective content changed since the last call,
    // reported after the generation containing them was published.
    [[nodiscard]] std::vector<VirtualPath> poll_changes();
    // Marks every source dirty and rescans it now (what the watcher does when
    // events were lost); returns the changed paths, which are also queued for
    // poll_changes().
    std::vector<VirtualPath> rescan_all();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::core
