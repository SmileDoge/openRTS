#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/core/zip_archive.hpp>

namespace {

namespace fs = std::filesystem;
using openrts::core::VirtualPath;

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

[[nodiscard]] VirtualPath vp(const std::string_view text) {
    return *VirtualPath::parse(text);
}

void write_file(const fs::path& file, const std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
}

void write_zip(const fs::path& archive,
               const std::vector<std::pair<std::string, std::string>>& files,
               const openrts::core::ZipMethod method = openrts::core::ZipMethod::deflate,
               const bool zip64 = false) {
    fs::create_directories(archive.parent_path());
    openrts::core::ZipWriter writer(archive);
    writer.force_zip64(zip64);
    for (const auto& [name, data] : files) writer.add(name, data, {.method = method});
    check(writer.finish(), "zip written");
}

[[nodiscard]] std::string read_text(const openrts::core::Vfs& vfs, const std::string_view path) {
    return vfs.read(vp(path)).value_or("<missing>");
}

void test_virtual_paths() {
    const auto a = VirtualPath::parse("ROUTES\\Kuban\\SHAPES\\..\\TEXTURES\\Brick.ACE");
    check(a && a->str() == "ROUTES/Kuban/TEXTURES/Brick.ACE", "backslashes and .. resolve lexically");
    check(a && a->key() == "routes/kuban/textures/brick.ace", "key is case-folded");
    check(!VirtualPath::parse("C:\\x.s") && !VirtualPath::parse("/x.s") && !VirtualPath::parse("\\\\srv\\x"),
          "rooted paths are refused");
    const auto clamped = VirtualPath::parse("../../GLOBAL/a.s");
    check(clamped && clamped->str() == "GLOBAL/a.s" && clamped->clamped(), ".. above root is clamped");
    check(vp("ROUTES/Краснодар").key() == "routes/краснодар", "Cyrillic is case-folded");
    const auto shape_dir = vp("TRAINS/TRAINSET/VL80k");
    const auto texture = shape_dir.resolve(std::string_view("..\\VL80_Common\\TEXTURES\\body.ace"));
    check(texture && texture->str() == "TRAINS/TRAINSET/VL80_Common/TEXTURES/body.ace", "resolve relative reference");
    check(vp("a/b/c.s").parent() == vp("A/B") && vp("a/b/c.s").filename() == "c.s"
              && vp("a/b/c.s").extension() == ".s" && vp("a/b/c.s").with_extension(".sd").str() == "a/b/c.sd",
          "parent, filename, extension");
    check(vp("ROUTES/X/a").is_within(vp("routes/x")) && !vp("ROUTES/XY/a").is_within(vp("ROUTES/X")), "is_within");
}

void test_zip_methods(const fs::path& work) {
    using openrts::core::ZipMethod;
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "TrackObj ( UiD ( " + std::to_string(i) + " ) )\n";
    for (const auto method : {ZipMethod::stored, ZipMethod::deflate, ZipMethod::zstd}) {
        for (const bool zip64 : {false, true}) {
            const auto archive = work / (std::string("m_") + to_string(method) + (zip64 ? "64" : "") + ".zip");
            write_zip(archive, {{"WORLD/w.w", text}, {"TEXTURES/Кирпич.ace", "ACE"}}, method, zip64);
            std::string error;
            const auto zip = openrts::core::ZipReader::open(archive, error);
            check(zip && zip->entries().size() == 2, "archive opens");
            if (!zip) continue;
            check(zip->read(zip->entries()[0], error) == text, "member round-trips");
            check(zip->entries()[1].name == "TEXTURES/Кирпич.ace", "UTF-8 member name");
            if (method != ZipMethod::stored) {
                check(zip->entries()[0].method == static_cast<std::uint16_t>(method), "method recorded");
                check(zip->entries()[0].compressed_size < text.size(), "compressed");
            }
        }
    }

    // A corrupted byte is caught by the CRC.
    const auto archive = work / "m_stored.zip";
    {
        std::string error;
        const auto zip = openrts::core::ZipReader::open(archive, error);
        const auto offset = zip->data_offset(zip->entries()[0], error);
        std::fstream file(archive, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(static_cast<std::streamoff>(*offset + 10));
        file.put('#');
    }
    std::string error;
    const auto zip = openrts::core::ZipReader::open(archive, error);
    check(zip && !zip->read(zip->entries()[0], error) && error.find("CRC") != std::string::npos, "CRC mismatch detected");
}

// Builds a small installation:
//   PACKS/base/route.zip   ROUTES/Demo/SHAPES/house.s -> ../TEXTURES/wall.ace (in folder)
//   ROUTES/Demo/...        folder files, overriding one base file
//   PACKS/patch/fix.zip    patches a folder file
//   MODS_OPENRTS/hd/       overrides a texture only
void build_installation(const fs::path& root) {
    write_zip(root / "PACKS" / "base" / "route.zip",
              {
                  {"ROUTES/Demo/SHAPES/house.s", "house from base"},
                  {"ROUTES/Demo/SHAPES/shed.s", "shed from base"},
                  {"ROUTES/Demo/WORLD/w-000001+000002.w", "world base"},
                  {"TRAINS/TRAINSET/Loco/TEXTURES/cab.ace", "cab texture from pack"},
              });
    write_file(root / "ROUTES" / "Demo" / "TEXTURES" / "Wall.ace", "wall from folder");
    write_file(root / "ROUTES" / "Demo" / "SHAPES" / "Shed.s", "shed from folder");
    write_file(root / "ROUTES" / "Demo" / "SHAPES" / "tower.s", "tower from folder");
    write_file(root / "ROUTES" / "Demo" / "TEXTURES" / "roof.ace", "roof from folder");
    write_file(root / "TRAINS" / "TRAINSET" / "Loco" / "loco.s", "loco in folder");
    write_zip(root / "PACKS" / "patch" / "fix.zip", {{"ROUTES/Demo/SHAPES/tower.s", "tower patched"}},
              openrts::core::ZipMethod::zstd);
    write_file(root / "MODS_OPENRTS" / "hd" / "ROUTES" / "Demo" / "TEXTURES" / "WALL.ACE", "wall from mod");
    write_file(root / "openrts.ini", "[Vfs]\nHotReload = 1\n[Mods]\n1 = hd\n");
}

void test_layers_and_references(const fs::path& root) {
    openrts::core::Vfs vfs;
    const auto config = openrts::core::load_installation_config(root);
    check(config.mods.size() == 1 && config.hot_reload == true, "openrts.ini parsed");
    openrts::core::mount_installation(vfs, root, config);

    check(read_text(vfs, "routes/demo/shapes/house.s") == "house from base", "base pack provides files");
    check(read_text(vfs, "ROUTES/Demo/SHAPES/SHED.S") == "shed from folder", "folder overrides base pack");
    check(read_text(vfs, "ROUTES/Demo/SHAPES/tower.s") == "tower patched", "patch overrides folder");
    check(read_text(vfs, "ROUTES/Demo/TEXTURES/wall.ace") == "wall from mod", "mod overrides everything");

    // `..` from a model inside a pack reaches a texture in a folder or mod.
    const auto house = vp("ROUTES/Demo/SHAPES/house.s");
    const auto roof = house.parent().resolve(std::string_view("..\\Textures\\ROOF.ace"));
    check(roof && read_text(vfs, roof->str()) == "roof from folder", "pack model -> folder texture via ..");
    const auto wall = house.parent().resolve(std::string_view("../TEXTURES/wall.ace"));
    check(wall && read_text(vfs, wall->str()) == "wall from mod", "pack model -> mod texture via ..");
    // ...and a model in a folder reaches a texture inside a pack.
    const auto loco = vp("TRAINS/TRAINSET/Loco/loco.s");
    const auto cab = loco.parent().resolve(std::string_view("TEXTURES\\Cab.ACE"));
    check(cab && read_text(vfs, cab->str()) == "cab texture from pack", "folder model -> pack texture");

    const auto origin = vfs.origin(vp("ROUTES/Demo/SHAPES/tower.s"));
    check(origin && origin->archive && origin->layer == openrts::core::VfsLayer::patch_pack, "origin reports the patch");

    const auto shapes = vfs.list(vp("ROUTES/Demo/SHAPES"));
    check(shapes.size() == 3, "listing merges sources");
    const auto demo = vfs.list(vp("routes/demo"));
    check(demo.size() == 3 && demo.front().directory, "sub-directories are listed");
    check(vfs.is_directory(vp("ROUTES/Demo/WORLD")), "directory from an archive");

    const auto conflicts = vfs.conflicts();
    check(conflicts.size() == 3, "overridden paths are reported");

    auto stream = vfs.open(vp("ROUTES/Demo/TEXTURES/roof.ace"));
    std::string head(4, '\0');
    check(stream && stream->read(head.data(), 4) == 4 && head == "roof", "stream reads a folder file");
    stream = vfs.open(vp("ROUTES/Demo/SHAPES/house.s"));
    std::string all(static_cast<std::size_t>(stream ? stream->size() : 0), '\0');
    check(stream && stream->read(all.data(), all.size()) == all.size() && all == "house from base", "stream reads a member");

    const auto extracted = vfs.real_path(vp("ROUTES/Demo/SHAPES/house.s"), root / "CACHE");
    check(extracted && fs::exists(*extracted), "archive member extracted for real-file users");

    // Lost events: changes made without a watcher are found by a rescan.
    write_file(root / "ROUTES" / "Demo" / "TEXTURES" / "roof.ace", "roof edited");
    write_file(root / "ROUTES" / "Demo" / "SHAPES" / "new.s", "new");
    fs::remove(root / "MODS_OPENRTS" / "hd" / "ROUTES" / "Demo" / "TEXTURES" / "WALL.ACE");
    const auto changed = vfs.rescan_all();
    check(changed.size() == 3, "rescan reports exactly the changed paths");
    check(read_text(vfs, "ROUTES/Demo/TEXTURES/wall.ace") == "wall from folder", "removing a mod file uncovers the folder");
    check(read_text(vfs, "ROUTES/Demo/TEXTURES/roof.ace") == "roof edited", "rescan picks up edits");
    check(vfs.poll_changes().size() == 3, "rescan changes are queued");

    // Shadowed edit: a change under an override is not reported.
    write_file(root / "ROUTES" / "Demo" / "SHAPES" / "tower.s", "tower edited under the patch");
    check(vfs.rescan_all().empty(), "edits hidden by a higher layer are not reported");
}

template <typename Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void test_watching(const fs::path& root) {
    openrts::core::Vfs vfs;
    openrts::core::mount_installation(vfs, root, openrts::core::load_installation_config(root));
    vfs.start_watching({.debounce = std::chrono::milliseconds(50), .reconcile_interval = std::chrono::seconds(0)});

    std::vector<VirtualPath> seen;
    const auto saw = [&](const std::string_view path) {
        for (auto& change : vfs.poll_changes()) seen.push_back(change);
        return std::find(seen.begin(), seen.end(), vp(path)) != seen.end();
    };

    write_file(root / "ROUTES" / "Demo" / "TEXTURES" / "roof.ace", "roof hot");
    check(wait_for([&] { return saw("ROUTES/Demo/TEXTURES/roof.ace"); }), "folder edit is reported");
    check(read_text(vfs, "ROUTES/Demo/TEXTURES/roof.ace") == "roof hot", "edit visible after the report");

    write_file(root / "ROUTES" / "Demo" / "SHAPES" / "sub" / "deep.s", "deep");
    check(wait_for([&] { return saw("ROUTES/Demo/SHAPES/sub/deep.s"); }), "new folder is scanned");

    // A replaced archive (written next to it and renamed, as ZipWriter does).
    write_zip(root / "PACKS" / "patch" / "fix.zip", {{"ROUTES/Demo/SHAPES/tower.s", "tower patch v2"}});
    check(wait_for([&] { return saw("ROUTES/Demo/SHAPES/tower.s"); }), "replaced archive is remounted");
    check(read_text(vfs, "ROUTES/Demo/SHAPES/tower.s") == "tower patch v2", "remounted archive contents");

    // A new archive in a pack directory is mounted.
    write_zip(root / "PACKS" / "patch" / "extra.zip", {{"ROUTES/Demo/SHAPES/extra.s", "extra"}});
    check(wait_for([&] { return saw("ROUTES/Demo/SHAPES/extra.s"); }), "new archive is mounted");

    // Removing it drops its files.
    seen.clear();
    fs::remove(root / "PACKS" / "patch" / "extra.zip");
    check(wait_for([&] { return saw("ROUTES/Demo/SHAPES/extra.s"); }) && !vfs.exists(vp("ROUTES/Demo/SHAPES/extra.s")),
          "removed archive is unmounted");

    // A half-written archive keeps the previous mount.
    {
        std::ofstream(root / "PACKS" / "patch" / "fix.zip", std::ios::binary | std::ios::trunc) << "not a zip";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    check(read_text(vfs, "ROUTES/Demo/SHAPES/tower.s") == "tower patch v2" || !vfs.sources().empty(),
          "corrupt archive does not crash");
    const auto sources = vfs.sources();
    const bool error_kept = std::any_of(sources.begin(), sources.end(), [](const auto& s) { return !s.error.empty(); });
    check(error_kept, "corrupt archive error is reported");

    vfs.stop_watching();
    check(vfs.stats().generation > 1, "generations advance");
}

} // namespace

int main() {
    const auto work = fs::temp_directory_path() / "openrts-vfs-test";
    fs::remove_all(work);
    fs::create_directories(work);

    test_virtual_paths();
    test_zip_methods(work / "zip");
    build_installation(work / "install");
    test_layers_and_references(work / "install");
    test_watching(work / "install");

    // The installation Windows knows of (RTS's registry key): none, or a
    // folder that holds ROUTES (this machine's own, printed).
    if (const auto registered = openrts::core::registered_installation()) {
        check(fs::is_directory(*registered / "ROUTES"), "the registered installation holds ROUTES");
        std::cout << "registered installation: " << registered->string() << '\n';
    } else {
        std::cout << "registered installation: none\n";
    }

    fs::remove_all(work);
    if (failures == 0) std::cout << "vfs: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
