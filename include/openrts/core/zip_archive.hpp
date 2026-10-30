#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openrts::core {

// ZIP compression methods openRTS reads and writes.
enum class ZipMethod : std::uint16_t {
    stored = 0,
    deflate = 8,
    zstd = 93, // Zstandard, APPNOTE 6.3.7
};

[[nodiscard]] const char* to_string(ZipMethod method) noexcept;
[[nodiscard]] std::optional<ZipMethod> parse_zip_method(std::string_view name) noexcept;

struct ZipEntry {
    std::string name; // UTF-8, '/' separators as stored
    std::uint16_t method{};
    std::uint16_t flags{};
    std::uint32_t crc32{};
    std::uint32_t dos_time{}; // DOS date << 16 | DOS time
    std::uint64_t compressed_size{};
    std::uint64_t size{};
    std::uint64_t local_header_offset{};
    [[nodiscard]] bool directory() const noexcept { return !name.empty() && name.back() == '/'; }
};

// Read-only view of a ZIP archive: the central directory is parsed once;
// every read opens the file on its own, so reads are thread-safe and the
// archive file is never kept locked.
class ZipReader {
public:
    [[nodiscard]] static std::optional<ZipReader> open(const std::filesystem::path& archive, std::string& error);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return entries_; }

    // Decompressed contents; the CRC-32 is verified.
    [[nodiscard]] std::optional<std::string> read(const ZipEntry& entry, std::string& error) const;
    // Stored (compressed) bytes of an entry, as in the archive.
    [[nodiscard]] std::optional<std::string> read_raw(const ZipEntry& entry, std::string& error) const;
    // Offset of the entry data inside the archive file.
    [[nodiscard]] std::optional<std::uint64_t> data_offset(const ZipEntry& entry, std::string& error) const;

private:
    std::filesystem::path path_;
    std::vector<ZipEntry> entries_;
};

struct ZipWriteOptions {
    ZipMethod method = ZipMethod::deflate;
    int level = 0; // 0 = method default
};

// Writes a ZIP archive to `<path>.tmp` and renames it over `path` on finish(),
// so readers (and hot reload) never see a half-written archive.
class ZipWriter {
public:
    explicit ZipWriter(std::filesystem::path path);
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // Compresses `data` with `method`; stores it when compression does not
    // help.
    bool add(std::string_view name, std::string_view data, ZipWriteOptions options, std::uint32_t dos_time = 0);
    // Adds data that is already compressed with `method` (for conversions).
    bool add_precompressed(std::string_view name,
                           std::string_view compressed,
                           ZipMethod method,
                           std::uint32_t crc32,
                           std::uint64_t size,
                           std::uint32_t dos_time = 0);
    // Forces ZIP64 records even for small archives (tests).
    void force_zip64(bool value) noexcept { force_zip64_ = value; }
    bool finish();

private:
    bool write_entry(std::string_view name, std::string_view payload, std::uint16_t method, std::uint32_t crc32,
                     std::uint64_t size, std::uint32_t dos_time);

    std::filesystem::path path_;
    std::filesystem::path temporary_;
    std::ofstream out_;
    std::vector<ZipEntry> written_;
    std::uint64_t offset_ = 0;
    bool force_zip64_ = false;
    bool finished_ = false;
    std::string error_;
};

[[nodiscard]] std::uint32_t crc32_of(std::string_view data) noexcept;
// Compresses with deflate (raw, as in ZIP) or zstd; nullopt on failure.
[[nodiscard]] std::optional<std::string> compress_bytes(std::string_view data, ZipMethod method, int level);
[[nodiscard]] std::optional<std::string> decompress_bytes(std::string_view data, ZipMethod method,
                                                          std::uint64_t size, std::string& error);
// DOS timestamp of a file time (local time), for archive entries.
[[nodiscard]] std::uint32_t dos_time_of(std::filesystem::file_time_type time);

} // namespace openrts::core
