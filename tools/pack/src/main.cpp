// openrts-pack: builds, converts and inspects openRTS content packs
// (docs/vfs.md).
//
//   openrts-pack pack <folder|file>... -o <out.zip> [--as <virtual dir>]
//                [--method deflate|zstd] [--level N] [--threads N]
//                [--name N] [--kind route|trains|global|mixed] [--priority N]
//                [--version V] [--requires A,B]
//   openrts-pack unpack <pack.zip> <installation root>
//   openrts-pack convert <in.zip> -o <out.zip> --method deflate|zstd [--level N]
//   openrts-pack list <pack.zip>
//   openrts-pack verify <pack.zip>
//   openrts-pack conflicts <installation root>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/core/zip_archive.hpp>

namespace {

namespace fs = std::filesystem;
using openrts::core::VirtualPath;
using openrts::core::ZipMethod;

constexpr int usage_error = 64;

void print_usage() {
    std::cout << "usage:\n"
                 "  openrts-pack pack <folder|file>... -o <out.zip> [--as <virtual dir>] [--method deflate|zstd]\n"
                 "               [--level N] [--threads N] [--name N] [--kind K] [--priority N] [--version V]\n"
                 "               [--requires A,B]\n"
                 "  openrts-pack unpack <pack.zip> <installation root>\n"
                 "  openrts-pack convert <in.zip> -o <out.zip> --method deflate|zstd [--level N]\n"
                 "  openrts-pack list <pack.zip>\n"
                 "  openrts-pack verify <pack.zip>\n"
                 "  openrts-pack conflicts <installation root>\n";
}

[[nodiscard]] fs::path to_path(const std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

[[nodiscard]] std::optional<std::string> read_file(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// Files that are compressed already gain nothing from a second pass and are
// stored, so reading them from a pack costs the same as from a folder.
[[nodiscard]] bool already_compressed(const fs::path& file, const std::string_view data) {
    if (data.starts_with("SIMISA@F")) return true; // zlib-compressed shapes, textures, ...
    const auto ext = openrts::core::fold_case(utf8(file.extension()));
    for (const auto* known : {".wav", ".ogg", ".mp3", ".dds", ".png", ".jpg", ".jpeg", ".zip", ".7z", ".rar", ".pdf"}) {
        if (ext == known) return true;
    }
    return false;
}

[[nodiscard]] std::string human(const std::uint64_t bytes) {
    char buffer[32];
    if (bytes >= (1ull << 30)) {
        std::snprintf(buffer, sizeof(buffer), "%.2f GiB", static_cast<double>(bytes) / (1ull << 30));
    } else if (bytes >= (1ull << 20)) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MiB", static_cast<double>(bytes) / (1ull << 20));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    }
    return buffer;
}

struct Options {
    std::vector<std::string> positional;
    std::map<std::string, std::string> values;

    [[nodiscard]] std::optional<std::string> get(const std::string& key) const {
        const auto it = values.find(key);
        return it == values.end() ? std::nullopt : std::optional(it->second);
    }
};

[[nodiscard]] std::optional<Options> parse_options(const std::vector<std::string_view>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].starts_with("-")) {
            if (i + 1 >= args.size()) return std::nullopt;
            options.values[std::string(args[i])] = std::string(args[i + 1]);
            ++i;
        } else {
            options.positional.emplace_back(args[i]);
        }
    }
    return options;
}

[[nodiscard]] std::optional<openrts::core::ZipReader> open_zip(const fs::path& path) {
    std::string error;
    auto zip = openrts::core::ZipReader::open(path, error);
    if (!zip) std::cerr << utf8(path) << ": " << error << '\n';
    return zip;
}

// ---------------------------------------------------------------- pack

struct PackItem {
    fs::path file;
    std::string name; // member name (virtual path)
};

int pack(const Options& options) {
    const auto output = options.get("-o");
    if (options.positional.empty() || !output) {
        print_usage();
        return usage_error;
    }
    const auto method = openrts::core::parse_zip_method(options.get("--method").value_or("deflate"));
    if (!method || *method == ZipMethod::stored) {
        std::cerr << "--method must be deflate or zstd\n";
        return usage_error;
    }
    const int level = std::stoi(options.get("--level").value_or("0"));
    const auto threads = static_cast<std::size_t>(
        std::stoi(options.get("--threads").value_or(std::to_string((std::max)(1u, std::thread::hardware_concurrency())))));

    std::vector<PackItem> items;
    for (const auto& argument : options.positional) {
        const auto source = fs::weakly_canonical(fs::absolute(to_path(argument)));
        if (!fs::exists(source)) {
            std::cerr << "not found: " << argument << '\n';
            return 2;
        }
        // Member names: --as, else the path inside the installation, else the
        // folder name.
        fs::path base;
        std::optional<VirtualPath> prefix;
        if (const auto as = options.get("--as")) {
            prefix = VirtualPath::parse(*as);
            base = fs::is_directory(source) ? source : source.parent_path();
        } else if (const auto root = openrts::core::find_installation_root(source);
                   root && !source.lexically_relative(*root).empty()) {
            base = *root;
        } else {
            base = fs::is_directory(source) ? source.parent_path() : source.parent_path();
        }
        const auto add = [&](const fs::path& file) {
            auto name = (prefix ? *prefix : VirtualPath{}).resolve_filesystem(file.lexically_relative(base));
            if (name && !name->empty()) items.push_back({file, name->str()});
        };
        if (fs::is_directory(source)) {
            for (const auto& entry : fs::recursive_directory_iterator(source)) {
                if (entry.is_regular_file()) add(entry.path());
            }
        } else {
            add(source);
        }
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.name < b.name; });

    openrts::core::ZipWriter writer(to_path(*output));
    if (!writer.ok()) {
        std::cerr << writer.error() << '\n';
        return 2;
    }

    std::string manifest = "[Pack]\nName = " + options.get("--name").value_or(utf8(to_path(*output).stem())) + "\n";
    if (const auto kind = options.get("--kind")) manifest += "Kind = " + *kind + "\n";
    if (const auto version = options.get("--version")) manifest += "Version = " + *version + "\n";
    if (const auto priority = options.get("--priority")) manifest += "Priority = " + *priority + "\n";
    if (const auto requires_list = options.get("--requires")) manifest += "Requires = " + *requires_list + "\n";
    writer.add("openrts-pack.ini", manifest, {.method = ZipMethod::stored});

    // Compression runs on worker threads in windows of files; writing stays in
    // name order.
    struct Prepared {
        std::string name;
        std::string payload;
        ZipMethod method{};
        std::uint32_t crc{};
        std::uint64_t size{};
        std::uint32_t time{};
        std::string error;
    };
    const auto prepare = [&](const PackItem& item) {
        Prepared result{.name = item.name};
        const auto data = read_file(item.file);
        if (!data) {
            result.error = "cannot read " + utf8(item.file);
            return result;
        }
        result.size = data->size();
        result.crc = openrts::core::crc32_of(*data);
        std::error_code error;
        result.time = openrts::core::dos_time_of(fs::last_write_time(item.file, error));
        result.method = ZipMethod::stored;
        result.payload = *data;
        if (!already_compressed(item.file, *data)) {
            if (auto compressed = openrts::core::compress_bytes(*data, *method, level);
                compressed && compressed->size() < data->size()) {
                result.payload = std::move(*compressed);
                result.method = *method;
            }
        }
        return result;
    };

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t original = 0;
    std::uint64_t packed = 0;
    std::size_t stored = 0;
    const std::size_t window = threads * 8;
    for (std::size_t begin = 0; begin < items.size(); begin += window) {
        const auto end = (std::min)(items.size(), begin + window);
        std::vector<Prepared> results(end - begin);
        std::atomic<std::size_t> next{begin};
        std::vector<std::thread> workers;
        for (std::size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                for (auto i = next++; i < end; i = next++) results[i - begin] = prepare(items[i]);
            });
        }
        for (auto& worker : workers) worker.join();
        for (const auto& result : results) {
            if (!result.error.empty()) {
                std::cerr << result.error << '\n';
                return 2;
            }
            if (!writer.add_precompressed(result.name, result.payload, result.method, result.crc, result.size, result.time)) {
                std::cerr << writer.error() << '\n';
                return 2;
            }
            original += result.size;
            packed += result.payload.size();
            if (result.method == ZipMethod::stored) ++stored;
        }
        std::cout << "\r" << end << "/" << items.size() << " files" << std::flush;
    }
    if (!writer.finish()) {
        std::cerr << '\n' << writer.error() << '\n';
        return 2;
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "\rpacked " << items.size() << " files (" << stored << " stored): " << human(original) << " -> "
              << human(packed) << " (" << to_string(*method) << ") in " << seconds << " s -> " << *output << '\n';
    return 0;
}

// ---------------------------------------------------------------- unpack

int unpack(const Options& options) {
    if (options.positional.size() != 2) {
        print_usage();
        return usage_error;
    }
    const auto zip = open_zip(to_path(options.positional[0]));
    if (!zip) return 2;
    const auto root = to_path(options.positional[1]);
    std::size_t count = 0;
    for (const auto& entry : zip->entries()) {
        if (entry.directory() || openrts::core::fold_case(entry.name) == "openrts-pack.ini") continue;
        // Member names are virtual paths: `..` cannot leave the target root.
        const auto path = VirtualPath::parse(entry.name);
        if (!path || path->empty()) continue;
        std::string error;
        const auto data = zip->read(entry, error);
        if (!data) {
            std::cerr << entry.name << ": " << error << '\n';
            return 2;
        }
        const auto target = root / path->to_filesystem();
        fs::create_directories(target.parent_path());
        std::ofstream(target, std::ios::binary).write(data->data(), static_cast<std::streamsize>(data->size()));
        ++count;
    }
    std::cout << "unpacked " << count << " files to " << utf8(root) << '\n';
    return 0;
}

// ---------------------------------------------------------------- convert

[[nodiscard]] bool verify_archive(const fs::path& path, std::uint64_t& bytes) {
    const auto zip = open_zip(path);
    if (!zip) return false;
    bool ok = true;
    for (const auto& entry : zip->entries()) {
        if (entry.directory()) continue;
        std::string error;
        if (const auto data = zip->read(entry, error)) {
            bytes += data->size();
        } else {
            std::cerr << entry.name << ": " << error << '\n';
            ok = false;
        }
    }
    return ok;
}

int convert(const Options& options) {
    const auto output = options.get("-o");
    const auto method = openrts::core::parse_zip_method(options.get("--method").value_or(""));
    if (options.positional.size() != 1 || !output || !method || *method == ZipMethod::stored) {
        print_usage();
        return usage_error;
    }
    const int level = std::stoi(options.get("--level").value_or("0"));
    const auto zip = open_zip(to_path(options.positional[0]));
    if (!zip) return 2;

    openrts::core::ZipWriter writer(to_path(*output));
    std::size_t recompressed = 0;
    for (const auto& entry : zip->entries()) {
        if (entry.directory()) continue;
        std::string error;
        const auto stored = entry.method == static_cast<std::uint16_t>(ZipMethod::stored);
        const auto same = entry.method == static_cast<std::uint16_t>(*method);
        if (stored || same) {
            // Kept byte for byte (stored entries are already-compressed data).
            const auto raw = zip->read_raw(entry, error);
            if (!raw) {
                std::cerr << entry.name << ": " << error << '\n';
                return 2;
            }
            writer.add_precompressed(entry.name, *raw, static_cast<ZipMethod>(entry.method), entry.crc32, entry.size,
                                     entry.dos_time);
            continue;
        }
        const auto data = zip->read(entry, error); // CRC-checked
        if (!data) {
            std::cerr << entry.name << ": " << error << '\n';
            return 2;
        }
        writer.add(entry.name, *data, {.method = *method, .level = level}, entry.dos_time);
        ++recompressed;
    }
    if (!writer.finish()) {
        std::cerr << writer.error() << '\n';
        return 2;
    }
    std::uint64_t bytes = 0;
    if (!verify_archive(to_path(*output), bytes)) {
        std::cerr << "verification of the converted archive failed\n";
        return 3;
    }
    std::cout << "converted " << recompressed << " entries to " << to_string(*method) << ", verified " << human(bytes)
              << ": " << fs::file_size(to_path(options.positional[0])) << " -> " << fs::file_size(to_path(*output))
              << " bytes\n";
    return 0;
}

// ---------------------------------------------------------------- list / verify / conflicts

int list(const Options& options) {
    if (options.positional.size() != 1) {
        print_usage();
        return usage_error;
    }
    const auto zip = open_zip(to_path(options.positional[0]));
    if (!zip) return 2;
    std::map<std::uint16_t, std::size_t> methods;
    std::uint64_t size = 0;
    std::uint64_t compressed = 0;
    for (const auto& entry : zip->entries()) {
        std::cout << to_string(static_cast<ZipMethod>(entry.method)) << '\t' << entry.size << '\t' << entry.compressed_size
                  << '\t' << entry.name << '\n';
        ++methods[entry.method];
        size += entry.size;
        compressed += entry.compressed_size;
    }
    std::cout << zip->entries().size() << " entries, " << human(size) << " -> " << human(compressed);
    for (const auto& [method, count] : methods) std::cout << ", " << count << ' ' << to_string(static_cast<ZipMethod>(method));
    std::cout << '\n';
    return 0;
}

int verify(const Options& options) {
    if (options.positional.size() != 1) {
        print_usage();
        return usage_error;
    }
    std::uint64_t bytes = 0;
    const bool ok = verify_archive(to_path(options.positional[0]), bytes);
    std::cout << (ok ? "ok: " : "FAILED: ") << human(bytes) << " checked\n";
    return ok ? 0 : 3;
}

int conflicts(const Options& options) {
    if (options.positional.size() != 1) {
        print_usage();
        return usage_error;
    }
    const auto root = to_path(options.positional[0]);
    openrts::core::Vfs vfs;
    openrts::core::mount_installation(vfs, root, openrts::core::load_installation_config(root));
    for (const auto& source : vfs.sources()) {
        std::cout << to_string(source.layer) << '\t' << source.name << '\t' << source.files << " files"
                  << (source.error.empty() ? "" : "\terror: " + source.error) << '\n';
    }
    const auto found = vfs.conflicts();
    for (const auto& conflict : found) {
        std::cout << conflict.path.str() << ':';
        for (const auto& source : conflict.sources) std::cout << ' ' << source;
        std::cout << "  -> " << conflict.sources.back() << '\n';
    }
    std::cout << found.size() << " overridden paths\n";
    return 0;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return usage_error;
    }
    const std::string_view command = argv[1];
    const auto options = parse_options({argv + 2, argv + argc});
    if (!options) {
        print_usage();
        return usage_error;
    }
    try {
        if (command == "pack") return pack(*options);
        if (command == "unpack") return unpack(*options);
        if (command == "convert") return convert(*options);
        if (command == "list") return list(*options);
        if (command == "verify") return verify(*options);
        if (command == "conflicts") return conflicts(*options);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
    print_usage();
    return usage_error;
}
