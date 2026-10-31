#pragma once

// MSTS/RTS sound definitions: .sms sound scripts and .ws world sound sources.
// See docs/sound.md.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>

namespace openrts::formats {

// Values a sound script reacts to. Variable1..3 come from MSTS (engine
// throttle, brake, ...), Variable4..8, Acceleration and Radius are RTS
// additions; RTS locomotive DLLs set the VariableN.
enum class SoundVariable : std::uint8_t {
    speed,
    distance, // from the listener to the sound source, m
    variable1,
    variable2,
    variable3,
    variable4,
    variable5,
    variable6,
    variable7,
    variable8,
    acceleration,
    radius,             // radius of the track section under the wagon
    distance_travelled, // m
    unknown,
};
inline constexpr std::size_t sound_variable_count = static_cast<std::size_t>(SoundVariable::unknown);

struct SoundCurve {
    SoundVariable control = SoundVariable::unknown;
    std::vector<std::pair<float, float>> points; // (variable, value), as written
    float granularity = 0.0f;
};

enum class SoundSelection : std::uint8_t { sequential, random };

enum class SoundActionKind : std::uint8_t {
    none, // no action keyword RTS recognises
    play_one_shot,
    start_loop,                     // loop the whole file
    start_loop_release,             // play to the loop region and loop it
    release_loop_release,           // leave the loop and play to the end
    release_loop_release_with_jump, // the same, jumping to the release part
    set_stream_volume,
    enable_trigger,
    disable_trigger,
    enable_stream,
    disable_stream,
    suspend_stream,
    halt_playback,
    post_trigger,      // RTS: send a trigger code
    set_variable,      // RTS: SetVariableValue ( name value )
    increase_variable, // RTS: IncreaseVariableValue ( name value )
};

struct SoundFile {
    std::string name;
    int parameter = -1; // second value of File ( "a.wav" -1 )
};

struct SoundAction {
    SoundActionKind kind = SoundActionKind::none;
    std::vector<SoundFile> files;
    SoundSelection selection = SoundSelection::sequential; // RTS: random only for RandomSelection
    float value = 1.0f;       // stream volume, variable value
    float fade = 0.0f;        // Fade ( s ) anywhere in the action
    std::uint32_t index{};    // trigger or stream (1-based), or trigger code
    SoundVariable variable = SoundVariable::unknown;
};

enum class SoundTriggerKind : std::uint8_t {
    unknown,  // a block RTS does not recognise; still counts for trigger indices
    initial,  // when the sound activates
    discrete, // trigger code from the simulation (or a DLL)
    variable, // a variable crosses a threshold
    random,   // after a random delay, repeatedly
    event,    // RTS: wagon on a joint or junction
};

enum class SoundEvent : std::uint8_t { wagon_on_joint, wagon_on_junction, wagon_on_any_joint, unknown };

struct SoundTrigger {
    SoundTriggerKind kind = SoundTriggerKind::unknown;
    std::string name; // block name as written (lower case)
    std::uint32_t code{}; // discrete
    SoundVariable variable = SoundVariable::unknown;
    bool increase = true; // _Inc_Past or _Dec_Past
    float threshold{};
    float minimum{}, maximum{}; // Delay_Min_Max, s
    float volume_minimum = 1.0f, volume_maximum = 1.0f;
    SoundEvent event = SoundEvent::unknown;
    std::int32_t event_parameter{}; // 0 both bogies, 1 front, 2 rear
    std::vector<SoundAction> actions;
};

struct SoundAttenuation {
    float scale = 1.0f;
    float distance = 0.0f;
    int type = 1; // 1 logarithmic, 2 linear
};

struct SoundStream {
    int priority{};
    float volume = 1.0f;
    std::vector<SoundTrigger> triggers;
    std::optional<SoundCurve> volume_curve;
    std::optional<SoundCurve> frequency_curve;
    std::vector<std::uint32_t> sources; // RTS SoundSources indices
    std::optional<SoundAttenuation> attenuation;
    bool allow_on_window_camera = false;
};

// Cameras an activation or deactivation block lists, and its distance.
struct SoundActivation {
    bool present = false; // the block exists
    bool cab_camera = false;
    bool external_camera = false;
    bool passenger_camera = false;
    std::optional<float> distance;
};

struct SoundSourcePoint {
    float x{}, y{}, z{}, volume = 1.0f;
};

struct SoundTriggerPair {
    std::vector<std::uint32_t> on, off, halt;
};

struct SoundGroup {
    int level{}; // ScalabiltyGroup number
    SoundActivation activation;
    SoundActivation deactivation;
    float volume = 1.0f;
    bool stereo = false;
    std::vector<SoundSourcePoint> sources;
    std::vector<SoundTriggerPair> trigger_pairs;
    std::vector<std::uint32_t> non_distributable; // RTS NonDistributableTriggers
    std::vector<SoundStream> streams;
};

// What a sound object belongs to; RTS picks the ScalabiltyGroup by it
// (rts1.exe 0x4DD0EC, kind byte of the sound object).
enum class SoundOwner : std::uint8_t {
    world,        // .ws sources, crossings, pickups (kind 6): the first group
    environment,  // weather and environment sounds (kind 8): the first group, no distances
    player_train, // the player's train (kind 5): group 7, else the first group
    train,        // other trains (kind 7): the first group numbered 5 or 0
};

struct SoundScript {
    std::vector<SoundGroup> groups; // in file order
    std::vector<std::pair<std::string, std::uint32_t>> named_triggers;
    // Keywords RTS does not recognise where they are written, for diagnostics.
    std::vector<std::string> unknown;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    // The group RTS uses for an owner, or nullptr (the object is silent).
    [[nodiscard]] const SoundGroup* select_group(SoundOwner owner) const;
};

// Where a File of a script is looked up, in order: next to the script, then
// the global SOUND folder (MSTS). Names may contain ../ and subfolders.
[[nodiscard]] std::vector<core::VirtualPath> sound_file_candidates(const core::VirtualPath& script,
                                                                   std::string_view file);

[[nodiscard]] SoundScript parse_sound_script(std::string_view bytes);
[[nodiscard]] SoundScript load_sound_script(const core::Vfs& vfs, const core::VirtualPath& file);

// One Soundsource of a .ws world sound file.
struct WorldSoundSource {
    std::uint32_t uid{};
    std::string file; // .sms, looked up in the route's SOUND folder
    RoutePosition position;
};

struct WorldSoundTile {
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    std::vector<WorldSoundSource> sources;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// `filename` gives the tile (w-004733+015131.ws).
[[nodiscard]] WorldSoundTile parse_world_sounds(std::string_view bytes, std::string_view filename);
[[nodiscard]] WorldSoundTile load_world_sounds(const core::Vfs& vfs, const core::VirtualPath& file);

} // namespace openrts::formats
