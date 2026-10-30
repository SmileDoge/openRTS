#include <openrts/core/zip_archive.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <limits>
#include <system_error>

#include <zlib.h>
#include <zstd.h>

namespace openrts::core {
namespace {

constexpr std::uint32_t local_header_signature = 0x04034b50;
constexpr std::uint32_t central_header_signature = 0x02014b50;
constexpr std::uint32_t end_signature = 0x06054b50;
constexpr std::uint32_t zip64_end_signature = 0x06064b50;
constexpr std::uint32_t zip64_locator_signature = 0x07064b50;
constexpr std::uint16_t zip64_extra_id = 0x0001;
constexpr std::uint16_t unicode_path_extra_id = 0x7075;
constexpr std::uint16_t utf8_flag = 0x0800;
constexpr std::uint32_t max32 = 0xFFFFFFFFu;

[[nodiscard]] std::uint16_t u16(const std::string_view bytes, const std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at])
                                      | (static_cast<unsigned char>(bytes[at + 1]) << 8));
}

[[nodiscard]] std::uint32_t u32(const std::string_view bytes, const std::size_t at) {
    return static_cast<std::uint32_t>(u16(bytes, at)) | (static_cast<std::uint32_t>(u16(bytes, at + 2)) << 16);
}

[[nodiscard]] std::uint64_t u64(const std::string_view bytes, const std::size_t at) {
    return static_cast<std::uint64_t>(u32(bytes, at)) | (static_cast<std::uint64_t>(u32(bytes, at + 4)) << 32);
}

void put16(std::string& out, const std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>(value >> 8));
}

void put32(std::string& out, const std::uint32_t value) {
    put16(out, static_cast<std::uint16_t>(value & 0xFFFF));
    put16(out, static_cast<std::uint16_t>(value >> 16));
}

void put64(std::string& out, const std::uint64_t value) {
    put32(out, static_cast<std::uint32_t>(value & max32));
    put32(out, static_cast<std::uint32_t>(value >> 32));
}

// Code page 866 upper half: Windows Explorer stores Cyrillic names in the
// OEM code page when it does not set the UTF-8 flag.
constexpr std::array<char16_t, 128> cp866 = {
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
    0x0401, 0x0451, 0x0404, 0x0454, 0x0407, 0x0457, 0x040E, 0x045E,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x2116, 0x00A4, 0x25A0, 0x00A0,
};

[[nodiscard]] std::string cp866_to_utf8(const std::string_view text) {
    std::string result;
    for (const auto c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const char16_t code = byte < 0x80 ? byte : cp866[byte - 0x80];
        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

[[nodiscard]] bool is_ascii(const std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

[[nodiscard]] std::optional<std::string> read_at(std::ifstream& file, const std::uint64_t offset, const std::size_t size) {
    std::string bytes(size, '\0');
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file.read(bytes.data(), static_cast<std::streamsize>(size))) return std::nullopt;
    return bytes;
}

[[nodiscard]] std::uint16_t version_needed(const std::uint16_t method, const bool zip64) {
    if (method == static_cast<std::uint16_t>(ZipMethod::zstd)) return 63;
    if (zip64) return 45;
    return 20;
}

} // namespace

const char* to_string(const ZipMethod method) noexcept {
    switch (method) {
    case ZipMethod::stored: return "stored";
    case ZipMethod::deflate: return "deflate";
    case ZipMethod::zstd: return "zstd";
    }
    return "unknown";
}

std::optional<ZipMethod> parse_zip_method(const std::string_view name) noexcept {
    if (name == "stored" || name == "store") return ZipMethod::stored;
    if (name == "deflate") return ZipMethod::deflate;
    if (name == "zstd") return ZipMethod::zstd;
    return std::nullopt;
}

std::uint32_t crc32_of(const std::string_view data) noexcept {
    uLong crc = crc32(0L, Z_NULL, 0);
    std::size_t at = 0;
    while (at < data.size()) {
        const auto chunk = static_cast<uInt>((std::min<std::size_t>)(data.size() - at, 1u << 30));
        crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data() + at), chunk);
        at += chunk;
    }
    return static_cast<std::uint32_t>(crc);
}

std::optional<std::string> compress_bytes(const std::string_view data, const ZipMethod method, const int level) {
    switch (method) {
    case ZipMethod::stored:
        return std::string(data);
    case ZipMethod::deflate: {
        z_stream stream{};
        if (deflateInit2(&stream, level > 0 ? level : Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                         Z_DEFAULT_STRATEGY)
            != Z_OK) {
            return std::nullopt;
        }
        std::string out(deflateBound(&stream, static_cast<uLong>(data.size())), '\0');
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_out = reinterpret_cast<Bytef*>(out.data());
        stream.avail_out = static_cast<uInt>(out.size());
        const auto status = deflate(&stream, Z_FINISH);
        const auto written = stream.total_out;
        deflateEnd(&stream);
        if (status != Z_STREAM_END) return std::nullopt;
        out.resize(written);
        return out;
    }
    case ZipMethod::zstd: {
        std::string out(ZSTD_compressBound(data.size()), '\0');
        const auto written = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), level > 0 ? level : 9);
        if (ZSTD_isError(written)) return std::nullopt;
        out.resize(written);
        return out;
    }
    }
    return std::nullopt;
}

std::optional<std::string> decompress_bytes(const std::string_view data, const ZipMethod method, const std::uint64_t size,
                                            std::string& error) {
    if (size > (std::numeric_limits<std::size_t>::max)()) {
        error = "Entry is too large for this platform.";
        return std::nullopt;
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    switch (method) {
    case ZipMethod::stored:
        if (data.size() != size) {
            error = "Stored entry size mismatch.";
            return std::nullopt;
        }
        return std::string(data);
    case ZipMethod::deflate: {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            error = "inflateInit2 failed.";
            return std::nullopt;
        }
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_out = reinterpret_cast<Bytef*>(out.data());
        stream.avail_out = static_cast<uInt>(out.size());
        const auto status = inflate(&stream, Z_FINISH);
        const auto produced = stream.total_out;
        inflateEnd(&stream);
        if ((status != Z_STREAM_END && !(status == Z_BUF_ERROR && produced == size)) || produced != size) {
            error = "Deflate data is corrupt (zlib status " + std::to_string(status) + ").";
            return std::nullopt;
        }
        return out;
    }
    case ZipMethod::zstd: {
        const auto produced = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
        if (ZSTD_isError(produced) || produced != size) {
            error = std::string("Zstandard data is corrupt: ")
                + (ZSTD_isError(produced) ? ZSTD_getErrorName(produced) : "size mismatch");
            return std::nullopt;
        }
        return out;
    }
    }
    error = "Unsupported compression method.";
    return std::nullopt;
}

std::uint32_t dos_time_of(const std::filesystem::file_time_type time) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
    const auto seconds = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const auto year = (std::max)(local.tm_year + 1900, 1980) - 1980;
    const auto date = static_cast<std::uint32_t>((year << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
    const auto clock = static_cast<std::uint32_t>((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    return (date << 16) | clock;
}

std::optional<ZipReader> ZipReader::open(const std::filesystem::path& archive, std::string& error) {
    std::ifstream file(archive, std::ios::binary);
    if (!file) {
        error = "Cannot open archive.";
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::uint64_t>(file.tellg());
    if (file_size < 22) {
        error = "Not a ZIP archive (too small).";
        return std::nullopt;
    }

    // The end record sits in the last 64 KiB + 22 bytes (comment length).
    const auto tail_size = static_cast<std::size_t>((std::min<std::uint64_t>)(file_size, 65535 + 22));
    const auto tail = read_at(file, file_size - tail_size, tail_size);
    if (!tail) {
        error = "Cannot read archive end.";
        return std::nullopt;
    }
    std::optional<std::size_t> end_at;
    for (std::size_t i = tail_size - 22 + 1; i-- > 0;) {
        if (u32(*tail, i) == end_signature) {
            end_at = i;
            break;
        }
    }
    if (!end_at) {
        error = "Not a ZIP archive (no end of central directory).";
        return std::nullopt;
    }

    std::uint64_t entry_count = u16(*tail, *end_at + 10);
    std::uint64_t directory_size = u32(*tail, *end_at + 12);
    std::uint64_t directory_offset = u32(*tail, *end_at + 16);

    const auto end_offset = file_size - tail_size + *end_at;
    if (end_offset >= 20) {
        if (const auto locator = read_at(file, end_offset - 20, 20); locator && u32(*locator, 0) == zip64_locator_signature) {
            const auto zip64_end_offset = u64(*locator, 8);
            const auto record = read_at(file, zip64_end_offset, 56);
            if (!record || u32(*record, 0) != zip64_end_signature) {
                error = "Corrupt ZIP64 end of central directory.";
                return std::nullopt;
            }
            entry_count = u64(*record, 32);
            directory_size = u64(*record, 40);
            directory_offset = u64(*record, 48);
        }
    }
    if (directory_offset + directory_size > file_size) {
        error = "Central directory lies outside the archive.";
        return std::nullopt;
    }

    const auto directory = read_at(file, directory_offset, static_cast<std::size_t>(directory_size));
    if (!directory) {
        error = "Cannot read central directory.";
        return std::nullopt;
    }

    ZipReader reader;
    reader.path_ = archive;
    reader.entries_.reserve(static_cast<std::size_t>(entry_count));
    std::size_t at = 0;
    for (std::uint64_t i = 0; i < entry_count; ++i) {
        if (at + 46 > directory->size() || u32(*directory, at) != central_header_signature) {
            error = "Corrupt central directory entry.";
            return std::nullopt;
        }
        ZipEntry entry;
        entry.flags = u16(*directory, at + 8);
        entry.method = u16(*directory, at + 10);
        entry.dos_time = (static_cast<std::uint32_t>(u16(*directory, at + 14)) << 16) | u16(*directory, at + 12);
        entry.crc32 = u32(*directory, at + 16);
        entry.compressed_size = u32(*directory, at + 20);
        entry.size = u32(*directory, at + 24);
        const auto name_length = u16(*directory, at + 28);
        const auto extra_length = u16(*directory, at + 30);
        const auto comment_length = u16(*directory, at + 32);
        entry.local_header_offset = u32(*directory, at + 42);
        if (at + 46 + name_length + extra_length + comment_length > directory->size()) {
            error = "Corrupt central directory entry.";
            return std::nullopt;
        }
        const auto raw_name = std::string_view(*directory).substr(at + 46, name_length);
        const auto extra = std::string_view(*directory).substr(at + 46 + name_length, extra_length);

        std::optional<std::string> unicode_name;
        for (std::size_t e = 0; e + 4 <= extra.size();) {
            const auto id = u16(extra, e);
            const auto length = u16(extra, e + 2);
            if (e + 4 + length > extra.size()) break;
            const auto field = extra.substr(e + 4, length);
            if (id == zip64_extra_id) {
                std::size_t f = 0;
                if (entry.size == max32 && f + 8 <= field.size()) entry.size = u64(field, f), f += 8;
                if (entry.compressed_size == max32 && f + 8 <= field.size()) entry.compressed_size = u64(field, f), f += 8;
                if (entry.local_header_offset == max32 && f + 8 <= field.size()) entry.local_header_offset = u64(field, f);
            } else if (id == unicode_path_extra_id && field.size() > 5) {
                unicode_name = std::string(field.substr(5));
            }
            e += 4 + length;
        }

        if ((entry.flags & utf8_flag) != 0 || is_ascii(raw_name)) {
            entry.name = std::string(raw_name);
        } else if (unicode_name) {
            entry.name = *unicode_name;
        } else {
            entry.name = cp866_to_utf8(raw_name);
        }
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
        reader.entries_.push_back(std::move(entry));
        at += 46 + name_length + extra_length + comment_length;
    }
    return reader;
}

std::optional<std::uint64_t> ZipReader::data_offset(const ZipEntry& entry, std::string& error) const {
    std::ifstream file(path_, std::ios::binary);
    if (!file) {
        error = "Cannot open archive.";
        return std::nullopt;
    }
    const auto header = read_at(file, entry.local_header_offset, 30);
    if (!header || u32(*header, 0) != local_header_signature) {
        error = "Corrupt local header.";
        return std::nullopt;
    }
    return entry.local_header_offset + 30 + u16(*header, 26) + u16(*header, 28);
}

std::optional<std::string> ZipReader::read_raw(const ZipEntry& entry, std::string& error) const {
    const auto offset = data_offset(entry, error);
    if (!offset) return std::nullopt;
    if (entry.compressed_size > (std::numeric_limits<std::size_t>::max)()) {
        error = "Entry is too large for this platform.";
        return std::nullopt;
    }
    std::ifstream file(path_, std::ios::binary);
    auto bytes = read_at(file, *offset, static_cast<std::size_t>(entry.compressed_size));
    if (!bytes) error = "Archive is truncated.";
    return bytes;
}

std::optional<std::string> ZipReader::read(const ZipEntry& entry, std::string& error) const {
    if ((entry.flags & 0x0001) != 0) {
        error = "Encrypted entries are not supported.";
        return std::nullopt;
    }
    const auto method = static_cast<ZipMethod>(entry.method);
    if (method != ZipMethod::stored && method != ZipMethod::deflate && method != ZipMethod::zstd) {
        error = "Unsupported compression method " + std::to_string(entry.method) + ".";
        return std::nullopt;
    }
    const auto raw = read_raw(entry, error);
    if (!raw) return std::nullopt;
    auto data = decompress_bytes(*raw, method, entry.size, error);
    if (!data) return std::nullopt;
    if (crc32_of(*data) != entry.crc32) {
        error = "CRC mismatch (archive changed or corrupt).";
        return std::nullopt;
    }
    return data;
}

ZipWriter::ZipWriter(std::filesystem::path path) : path_(std::move(path)) {
    temporary_ = path_;
    temporary_ += ".tmp";
    out_.open(temporary_, std::ios::binary | std::ios::trunc);
    if (!out_) error_ = "Cannot create " + temporary_.string();
}

ZipWriter::~ZipWriter() {
    if (!finished_) {
        out_.close();
        std::error_code ignored;
        std::filesystem::remove(temporary_, ignored);
    }
}

bool ZipWriter::add(const std::string_view name, const std::string_view data, const ZipWriteOptions options,
                    const std::uint32_t dos_time) {
    if (!ok()) return false;
    const auto crc = crc32_of(data);
    if (options.method != ZipMethod::stored) {
        if (auto compressed = compress_bytes(data, options.method, options.level);
            compressed && compressed->size() < data.size()) {
            return write_entry(name, *compressed, static_cast<std::uint16_t>(options.method), crc, data.size(), dos_time);
        }
    }
    return write_entry(name, data, static_cast<std::uint16_t>(ZipMethod::stored), crc, data.size(), dos_time);
}

bool ZipWriter::add_precompressed(const std::string_view name, const std::string_view compressed, const ZipMethod method,
                                  const std::uint32_t crc32, const std::uint64_t size, const std::uint32_t dos_time) {
    if (!ok()) return false;
    return write_entry(name, compressed, static_cast<std::uint16_t>(method), crc32, size, dos_time);
}

bool ZipWriter::write_entry(const std::string_view name, const std::string_view payload, const std::uint16_t method,
                            const std::uint32_t crc32, const std::uint64_t size, const std::uint32_t dos_time) {
    const bool zip64 = force_zip64_ || size >= max32 || payload.size() >= max32;
    std::string header;
    put32(header, local_header_signature);
    put16(header, version_needed(method, zip64));
    put16(header, utf8_flag);
    put16(header, method);
    put16(header, static_cast<std::uint16_t>(dos_time & 0xFFFF));
    put16(header, static_cast<std::uint16_t>(dos_time >> 16));
    put32(header, crc32);
    put32(header, zip64 ? max32 : static_cast<std::uint32_t>(payload.size()));
    put32(header, zip64 ? max32 : static_cast<std::uint32_t>(size));
    put16(header, static_cast<std::uint16_t>(name.size()));
    put16(header, zip64 ? 20 : 0);
    header += name;
    if (zip64) {
        put16(header, zip64_extra_id);
        put16(header, 16);
        put64(header, size);
        put64(header, payload.size());
    }
    out_.write(header.data(), static_cast<std::streamsize>(header.size()));
    out_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!out_) {
        error_ = "Write failed: " + temporary_.string();
        return false;
    }

    written_.push_back({
        .name = std::string(name),
        .method = method,
        .flags = utf8_flag,
        .crc32 = crc32,
        .dos_time = dos_time,
        .compressed_size = payload.size(),
        .size = size,
        .local_header_offset = offset_,
    });
    offset_ += header.size() + payload.size();
    return true;
}

bool ZipWriter::finish() {
    if (!ok() || finished_) return ok();

    std::string directory;
    for (const auto& entry : written_) {
        const bool zip64 = force_zip64_ || entry.size >= max32 || entry.compressed_size >= max32
            || entry.local_header_offset >= max32;
        put32(directory, central_header_signature);
        put16(directory, static_cast<std::uint16_t>((3 << 8) | 63)); // made by: Unix, 6.3
        put16(directory, version_needed(entry.method, zip64));
        put16(directory, entry.flags);
        put16(directory, entry.method);
        put16(directory, static_cast<std::uint16_t>(entry.dos_time & 0xFFFF));
        put16(directory, static_cast<std::uint16_t>(entry.dos_time >> 16));
        put32(directory, entry.crc32);
        put32(directory, zip64 ? max32 : static_cast<std::uint32_t>(entry.compressed_size));
        put32(directory, zip64 ? max32 : static_cast<std::uint32_t>(entry.size));
        put16(directory, static_cast<std::uint16_t>(entry.name.size()));
        put16(directory, zip64 ? 28 : 0);
        put16(directory, 0); // comment
        put16(directory, 0); // disk
        put16(directory, 0); // internal attributes
        put32(directory, entry.directory() ? 0x10 : 0);
        put32(directory, zip64 ? max32 : static_cast<std::uint32_t>(entry.local_header_offset));
        directory += entry.name;
        if (zip64) {
            put16(directory, zip64_extra_id);
            put16(directory, 24);
            put64(directory, entry.size);
            put64(directory, entry.compressed_size);
            put64(directory, entry.local_header_offset);
        }
    }

    const auto directory_offset = offset_;
    const bool zip64_end = force_zip64_ || written_.size() >= 0xFFFF || directory_offset >= max32
        || directory.size() >= max32;
    std::string end;
    if (zip64_end) {
        const auto zip64_end_offset = directory_offset + directory.size();
        put32(end, zip64_end_signature);
        put64(end, 44);
        put16(end, static_cast<std::uint16_t>((3 << 8) | 63));
        put16(end, 45);
        put32(end, 0);
        put32(end, 0);
        put64(end, written_.size());
        put64(end, written_.size());
        put64(end, directory.size());
        put64(end, directory_offset);
        put32(end, zip64_locator_signature);
        put32(end, 0);
        put64(end, zip64_end_offset);
        put32(end, 1);
    }
    put32(end, end_signature);
    put16(end, 0);
    put16(end, 0);
    put16(end, zip64_end ? 0xFFFF : static_cast<std::uint16_t>(written_.size()));
    put16(end, zip64_end ? 0xFFFF : static_cast<std::uint16_t>(written_.size()));
    put32(end, zip64_end ? max32 : static_cast<std::uint32_t>(directory.size()));
    put32(end, zip64_end ? max32 : static_cast<std::uint32_t>(directory_offset));
    put16(end, 0);

    out_.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    out_.write(end.data(), static_cast<std::streamsize>(end.size()));
    out_.close();
    if (!out_) {
        error_ = "Write failed: " + temporary_.string();
        return false;
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary_, path_, rename_error);
    if (rename_error) {
        error_ = "Cannot replace " + path_.string() + ": " + rename_error.message();
        return false;
    }
    finished_ = true;
    return true;
}

} // namespace openrts::core
