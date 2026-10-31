// openrts-sound-check <installation root>
//
// Parses every .sms and .ws file of an installation, resolves the sound files
// the scripts use and (with the audio module) decodes them. Prints a summary,
// unknown keywords and the first failures. Exit code 1 on parse failures.
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <openrts/audio/sound_bank.hpp>
#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/sound.hpp>

namespace {

using namespace openrts;

void walk(const core::Vfs& vfs, const core::VirtualPath& directory,
          const std::function<void(const core::VirtualPath&)>& visit) {
    for (const auto& entry : vfs.list(directory)) {
        if (entry.directory) {
            walk(vfs, entry.path, visit);
        } else {
            visit(entry.path);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: openrts-sound-check <installation root>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));

    std::vector<core::VirtualPath> scripts;
    std::vector<core::VirtualPath> world_files;
    for (const char* top : {"SOUND", "TRAINS", "ROUTES"}) {
        const auto directory = core::VirtualPath::parse(top);
        if (!directory) continue;
        walk(vfs, *directory, [&](const core::VirtualPath& path) {
            const auto extension = core::fold_case(path.extension());
            if (extension == ".sms") scripts.push_back(path);
            if (extension == ".ws") world_files.push_back(path);
        });
    }

    std::size_t failed = 0;
    std::size_t streams = 0;
    std::size_t triggers = 0;
    std::size_t no_camera = 0;      // groups whose Activation lists no camera
    std::size_t no_activation = 0;  // ... of them without an Activation block
    std::vector<std::string> no_camera_examples;
    std::size_t inert_triggers = 0; // trigger blocks RTS does not recognise
    std::size_t inert_actions = 0;  // actions without a keyword RTS recognises
    std::map<std::string, std::size_t> unknown;
    std::map<std::string, std::vector<std::string>> missing; // sound file -> scripts
    std::set<std::string> resolved;
    std::vector<std::string> failures;
    for (const auto& path : scripts) {
        const auto script = formats::load_sound_script(vfs, path);
        if (!script.ok()) {
            ++failed;
            if (failures.size() < 15) failures.push_back(path.str() + ": " + script.error);
            continue;
        }
        for (const auto& name : script.unknown) ++unknown[name];
        for (const auto& group : script.groups) {
            const auto& activation = group.activation;
            if (!activation.cab_camera && !activation.external_camera && !activation.passenger_camera) {
                ++no_camera;
                if (!activation.present) ++no_activation;
                if (no_camera_examples.size() < 5) no_camera_examples.push_back(path.str());
            }
            streams += group.streams.size();
            for (const auto& stream : group.streams) {
                triggers += stream.triggers.size();
                for (const auto& trigger : stream.triggers) {
                    if (trigger.kind == formats::SoundTriggerKind::unknown) ++inert_triggers;
                    for (const auto& action : trigger.actions) inert_actions += action.kind == formats::SoundActionKind::none;
                    for (const auto& action : trigger.actions) {
                        for (const auto& file : action.files) {
                            if (const auto found = audio::find_sound_file(vfs, path, file.name)) {
                                resolved.insert(found->str());
                            } else {
                                missing[file.name].push_back(path.str());
                            }
                        }
                    }
                }
            }
        }
    }

    std::size_t sources = 0;
    std::size_t world_failed = 0;
    for (const auto& path : world_files) {
        const auto tile = formats::load_world_sounds(vfs, path);
        if (!tile.ok()) {
            ++world_failed;
            if (failures.size() < 15) failures.push_back(path.str() + ": " + tile.error);
        }
        sources += tile.sources.size();
    }

    // Decode every sound file the scripts reach.
    std::size_t decoded = 0;
    std::size_t looped = 0;
    std::vector<std::string> decode_failures;
    for (const auto& key : resolved) {
        const auto path = core::VirtualPath::parse(key); // key holds the path text
        std::string error;
        const auto clip = path ? audio::load_clip(vfs, *path, &error) : nullptr;
        if (!clip) {
            if (decode_failures.size() < 15) decode_failures.push_back(key + ": " + error);
            continue;
        }
        ++decoded;
        looped += clip->loop_end > clip->loop_start ? 1 : 0;
    }

    std::printf("scripts: %zu parsed, %zu failed; %zu streams, %zu triggers\n", scripts.size() - failed, failed, streams,
                triggers);
    std::printf("as RTS reads them: %zu triggers and %zu actions do nothing; %zu groups list no activation camera\n",
                inert_triggers, inert_actions, no_camera);
    std::printf("  of those, %zu have no Activation block\n", no_activation);
    for (const auto& example : no_camera_examples) std::printf("  e.g. %s\n", example.c_str());
    std::printf("world sound files: %zu, %zu failed, %zu sources\n", world_files.size(), world_failed, sources);
    std::printf("sound files: %zu used, %zu decoded (%zu with a loop region), %zu names missing\n", resolved.size(),
                decoded, looped, missing.size());
    if (!unknown.empty()) {
        std::printf("unknown keywords:\n");
        for (const auto& [name, count] : unknown) std::printf("  %-32s %zu scripts\n", name.c_str(), count);
    }
    std::size_t shown = 0;
    for (const auto& [name, users] : missing) {
        if (shown++ >= 10) break;
        std::printf("  missing %s (%zu scripts, e.g. %s)\n", name.c_str(), users.size(), users.front().c_str());
    }
    for (const auto& line : failures) std::printf("  FAIL %s\n", line.c_str());
    for (const auto& line : decode_failures) std::printf("  DECODE %s\n", line.c_str());
    return failed > 0 || world_failed > 0 || decoded != resolved.size() ? 1 : 0;
}
