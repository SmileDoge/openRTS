// Sound scripts: parsing (.sms, .ws), the script runner against a recording
// voice sink, and WAV decoding with loop regions.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <openrts/audio/audio_engine.hpp>
#include <filesystem>
#include <span>
#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/audio/interior_acoustics.hpp>
#include <openrts/audio/sound_bank.hpp>
#include <openrts/audio/sound_instance.hpp>
#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/sound.hpp>

namespace {

using namespace openrts;
using formats::SoundVariable;

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

bool near(const double a, const double b, const double tolerance = 1e-4) { return std::abs(a - b) < tolerance; }

// Records what the runner asks for.
class RecordingSink final : public audio::VoiceSink {
public:
    struct Voice {
        std::shared_ptr<const audio::AudioClip> clip;
        audio::VoicePlayback playback{};
        audio::VoiceSpace space = audio::VoiceSpace::flat;
        bool released = false;
        bool stopped = false;
        float volume{};
        float pitch{};
    };

    audio::VoiceId play(std::shared_ptr<const audio::AudioClip> clip, audio::VoicePlayback playback,
                        audio::VoiceSpace space) override {
        voices[next] = {std::move(clip), playback, space};
        return next++;
    }
    void release(audio::VoiceId voice) override { voices[voice].released = true; }
    void stop(audio::VoiceId voice) override { voices[voice].stopped = true; }
    void set(audio::VoiceId voice, float volume, float pitch, const glm::vec3&) override {
        voices[voice].volume = volume;
        voices[voice].pitch = pitch;
    }
    bool playing(audio::VoiceId voice) const override {
        const auto it = voices.find(voice);
        return it != voices.end() && !it->second.stopped && !finished.contains(voice);
    }
    void seek(audio::VoiceId voice, std::uint64_t frame) override { seeks[voice] = frame; }
    std::optional<std::uint64_t> cursor(audio::VoiceId voice) const override {
        const auto it = cursors.find(voice);
        return it != cursors.end() ? std::optional(it->second) : std::nullopt;
    }
    std::map<audio::VoiceId, std::uint64_t> seeks;
    std::map<audio::VoiceId, std::uint64_t> cursors; // set by a test: where a voice is

    // Time passes: the one-shots and released loops finish (the test clips
    // are a few milliseconds long); loops go on.
    void advance() {
        for (const auto& [id, voice] : voices)
            if (voice.playback == audio::VoicePlayback::once || voice.released) finished[id] = true;
    }

    [[nodiscard]] std::size_t live() const {
        std::size_t count = 0;
        for (const auto& [id, voice] : voices) count += playing(id) ? 1 : 0;
        return count;
    }

    std::map<audio::VoiceId, Voice> voices;
    std::map<audio::VoiceId, bool> finished;
    audio::VoiceId next = 1;
};

std::shared_ptr<const audio::AudioClip> test_clip(const std::uint32_t rate = 22050) {
    auto clip = std::make_shared<audio::AudioClip>();
    clip->channels = 1;
    clip->sample_rate = rate;
    clip->frames = 100;
    clip->samples.assign(100, 0.0f);
    return clip;
}

const char* const station_script = R"(SIMISA@@@@@@@@@@JINX0x1t______
Tr_SMS (
    Skip ( a station: loop while near )
    ScalabiltyGroup ( 3
        Activation ( ExternalCam ( ) Distance ( 550 ) )
        Deactivation ( CabCam ( ) Distance ( 550 ) )
        Volume ( 0.5 )
        Streams ( 1
            Stream (
                Priority ( 2 )
                Triggers ( 2
                    Variable_Trigger ( Distance_Dec_Past 500
                        StartLoop ( 1 File ( "loop.wav" -1 ) SelectionMethod ( RandomSelection ) Fade ( 1.0 ) ) )
                    Variable_Trigger ( Distance_Inc_Past 510 ReleaseLoopRelease ( Fade ( 1.0 ) ) )
                )
                VolumeCurve ( DistanceControlled CurvePoints ( 3 0 1.0 300 1.0 500 0.0 ) Granularity ( 0.01 ) )
                FrequencyCurve ( DistanceControlled CurvePoints ( 2 0 11025 1000 22050 ) Granularity ( 100 ) )
            )
        )
    )
    ScalabiltyGroup ( 7 Streams ( 0 ) )
)
)";

const char* const ambient_script = R"(SIMISA@@@@@@@@@@JINX0x1t______
Tr_SMS (
    ScalabiltyGroup( 3
        Activation ( ExternalCam() CabCam() PassengerCam() )
        Deactivation ( )
        DiscreteTriggerPairs ( Pair ( On ( 1 45 ) Off ( 1 46 ) ) )
        Streams ( 2
            Stream ( Triggers ( 4
                Skip( 01 ) Initial_Trigger ( DisableTrigger ( 2 ) )
                Skip( 02 ) Random_Trigger ( Delay_Min_Max ( 1 1 ) Volume_Min_Max ( 0.5 0.5 )
                    PlayOneShot ( 2 File ( "a.wav" -1 ) File ( "b.wav" -1 ) SelectionMethod ( SequentialSelection ) ) )
                Variable_Trigger ( Variable1_Inc_Past 10 EnableTrigger ( 2 ) )
                VARIABLE_TRIGGER ( Variable1_Dec_Past 5 TriggerActions ( 2 DisableTrigger ( 2 ) SetStreamVolume ( 0.25 ) ) )
            ) )
            Stream ( Triggers ( 2
                Discrete_Trigger ( 45 PlayOneShot ( 1 File ( "on.wav" -1 ) SelectionMethod ( RandomSelection ) ) )
                Discrete_Trigger ( 46 ReleaseLoopReleasep ( ) )
            ) )
        )
    )
)
)";

void test_parsing() {
    const auto script = formats::parse_sound_script(station_script);
    check(script.ok(), "station script parses");
    check(script.groups.size() == 2, "two groups");
    const auto* group = script.select_group(formats::SoundOwner::world);
    check(group && group->level == 3, "world sounds use the first group");
    const auto* player = script.select_group(formats::SoundOwner::player_train);
    check(player && player->level == 7, "the player's train prefers group 7");
    check(!script.select_group(formats::SoundOwner::train), "other trains need group 5 or 0");
    check(!script.select_group(formats::SoundOwner::car), "a car's own script needs group 0 (RTS kind 0)");
    const auto two = formats::parse_sound_script("Tr_SMS ( ScalabiltyGroup ( 5 ) ScalabiltyGroup ( 0 ) )");
    const auto* own = two.select_group(formats::SoundOwner::car);
    check(own && own->level == 0, "kind 0 skips group 5 for group 0");
    if (!group) return;
    check(group->activation.external_camera && !group->activation.cab_camera, "activation cameras");
    check(group->activation.distance && near(*group->activation.distance, 550), "activation distance");
    check(near(group->volume, 0.5), "group volume");
    const auto& stream = group->streams.at(0);
    check(stream.triggers.size() == 2, "Skip blocks are not triggers");
    const auto& start = stream.triggers[0];
    check(start.kind == formats::SoundTriggerKind::variable && start.variable == SoundVariable::distance
              && !start.increase && near(start.threshold, 500),
          "Distance_Dec_Past 500");
    check(start.actions.size() == 1 && start.actions[0].kind == formats::SoundActionKind::start_loop
              && start.actions[0].files.at(0).name == "loop.wav" && near(start.actions[0].fade, 1.0),
          "StartLoop with file and fade");
    check(stream.volume_curve && stream.volume_curve->control == SoundVariable::distance
              && stream.volume_curve->points.size() == 3,
          "volume curve");

    const auto ambient = formats::parse_sound_script(ambient_script);
    const auto& streams = ambient.groups.at(0).streams;
    check(ambient.groups.at(0).trigger_pairs.size() == 1 && ambient.groups.at(0).trigger_pairs[0].on.at(0) == 45,
          "trigger pairs");
    check(streams.at(0).triggers.at(3).actions.size() == 2, "TriggerActions holds two actions");
    check(streams.at(0).triggers.at(1).actions.at(0).selection == formats::SoundSelection::sequential,
          "sequential selection");
    check(streams.at(1).triggers.at(1).actions.size() == 1
              && streams.at(1).triggers.at(1).actions[0].kind == formats::SoundActionKind::none,
          "misspelt ReleaseLoopReleasep is no action (rts1.exe needs '(' right after the keyword)");
    check(std::find(ambient.unknown.begin(), ambient.unknown.end(), "action:releaseloopreleasep") != ambient.unknown.end(),
          "misspelt keyword is reported");

    // Counts limit the lists; an unknown trigger keeps its slot; fields are
    // found anywhere in the group's text (the stream's Volume here).
    const auto quirks = formats::parse_sound_script(R"(SIMISA@@@@@@@@@@JINX0x1t______
Tr_SMS ( ScalabiltyGroup ( 0
    Streams ( 1
        Stream ( Volume ( 0.3 ) Triggers ( 3
            Dist_Travelled_Trigger ( Dist_Min_Max ( 1 2 ) PlayOneShot ( 1 File ( "a.wav" -1 ) ) )
            Initial_Trigger ( EnableTrigger ( 3 ) )
            Discrete_Trigger ( 5 PlayOneShot ( 2 File ( "a.wav" -1 ) File ( "b.wav" -1 ) File ( "c.wav" -1 ) ) )
            Discrete_Trigger ( 6 PlayOneShot ( 1 File ( "d.wav" -1 ) ) )
        ) )
        Stream ( Triggers ( 0 ) )
    ) ) )
)");
    const auto& quirk_group = quirks.groups.at(0);
    check(quirk_group.streams.size() == 1, "Streams ( 1 ) reads one stream");
    const auto& quirk_triggers = quirk_group.streams.at(0).triggers;
    check(quirk_triggers.size() == 3, "Triggers ( 3 ) reads three of four");
    check(quirk_triggers.at(0).kind == formats::SoundTriggerKind::unknown && quirk_triggers.at(2).code == 5,
          "Dist_Travelled_Trigger (unknown to RTS) keeps index 1");
    check(quirk_triggers.at(2).actions.at(0).files.size() == 2, "PlayOneShot ( 2 ...) reads two files");
    check(quirk_triggers.at(2).actions.at(0).selection == formats::SoundSelection::sequential,
          "no SelectionMethod: sequential");
    check(near(quirk_group.volume, 0.3), "group Volume is the first Volume in the group's text");
    check(formats::parse_sound_script("SIMISA@@@@@@@@@@JINX0x1t______\nTr_SMS ( )").ok(), "empty script is silent");

    const auto world = formats::parse_world_sounds(
        "SIMISA@@@@@@@@@@JINX0W0t______\nTr_Worldsoundfile ( Soundsource ( Position ( 1199.5 1 485.5 ) "
        "FileName ( sta.sms ) UiD ( 100000 ) ) )",
        "w-004733+015131.ws");
    check(world.ok() && world.tile_x == -4733 && world.tile_z == 15131, "world sound tile");
    check(world.sources.size() == 1 && world.sources[0].file == "sta.sms" && near(world.sources[0].position.x, 1199.5)
              && world.sources[0].uid == 100000,
          "world sound source");
}

void test_station_runner() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(station_script));
    const auto clip = test_clip();
    audio::SoundInstance sound(script, *script->select_group(formats::SoundOwner::world), formats::SoundOwner::world,
                               [&](const std::string&) { return clip; }, 1);
    RecordingSink sink;
    audio::SoundInputs inputs;
    const auto at = [&](const float distance) {
        inputs[SoundVariable::distance] = distance;
        sound.update(0.1f, inputs, sink);
    };

    at(800);
    check(!sound.active() && sink.voices.empty(), "inactive beyond the activation distance");
    at(540);
    check(sound.active() && sink.voices.empty(), "active inside 550, loop waits for 500");
    at(400);
    check(sink.voices.size() == 1 && sink.voices[1].playback == audio::VoicePlayback::loop, "loop starts below 500");
    // Volume: 0.5 group x curve(400) = 0.5 x 0.5, faded in for 0.1 of 1 s.
    check(near(sink.voices[1].volume, 0.5 * 0.5 * 0.1, 1e-3), "group volume, distance curve and fade-in");
    check(near(sink.voices[1].pitch, (11025.0 + 11025.0 * 0.4) / 22050.0, 1e-3), "frequency curve gives the pitch");
    for (int i = 0; i < 20; ++i) at(200);
    check(near(sink.voices[1].volume, 0.5, 1e-3), "full volume after the fade-in, curve 1 at 200 m");
    at(520);
    check(!sink.voices[1].released && !sink.voices[1].stopped, "release with fade keeps the voice meanwhile");
    for (int i = 0; i < 11; ++i) at(520);
    check(sink.voices[1].stopped, "faded-out voice is stopped");
    at(600);
    check(!sound.active(), "deactivates beyond 550");

    // Spawning inside the loop range starts the loop at once.
    audio::SoundInstance near_sound(script, *script->select_group(formats::SoundOwner::world),
                                    formats::SoundOwner::world, [&](const std::string&) { return clip; }, 2);
    RecordingSink near_sink;
    inputs[SoundVariable::distance] = 100;
    near_sound.update(0.1f, inputs, near_sink);
    check(near_sink.voices.size() == 1, "first update evaluates thresholds as crossed");
    inputs.camera = audio::SoundCamera::cab;
    near_sound.update(0.1f, inputs, near_sink);
    check(!near_sound.active() && near_sink.voices[1].stopped, "cab camera deactivates and stops");
}

// rts1.exe defaults: activation within 500 m, deactivation beyond 550 m when
// the script gives no distances.
void test_default_distances() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( ScalabiltyGroup ( 5 Activation ( ExternalCam ( ) ) Streams ( 1 Stream ( Triggers ( 1
    Initial_Trigger ( StartLoop ( 1 File ( "a.wav" -1 ) ) ) ) ) ) ) ))"));
    const auto clip = test_clip();
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::world,
                               [&](const std::string&) { return clip; }, 4);
    RecordingSink sink;
    audio::SoundInputs inputs;
    inputs[SoundVariable::distance] = 520;
    sound.update(0.1f, inputs, sink);
    check(!sound.active(), "not active beyond the default 500 m");
    inputs[SoundVariable::distance] = 490;
    sound.update(0.1f, inputs, sink);
    check(sound.active() && sink.voices.size() == 1, "active within 500 m");
    inputs[SoundVariable::distance] = 540;
    sound.update(0.1f, inputs, sink);
    check(sound.active(), "still active up to 550 m");
    inputs[SoundVariable::distance] = 560;
    sound.update(0.1f, inputs, sink);
    check(!sound.active(), "deactivates beyond 550 m");
}

// A deactivation distance below the activation distance (mdd_ed4m-engMotor:
// Activation 1000 m, Deactivation 300 m): between the two the group must
// not start and stop on alternate updates; deactivation wins.
void test_crossed_distances() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( ScalabiltyGroup ( 5 Activation ( ExternalCam ( ) Distance ( 1000 ) ) Deactivation ( Distance ( 300 ) )
    Streams ( 1 Stream ( Triggers ( 1 Initial_Trigger ( StartLoop ( 1 File ( "a.wav" -1 ) ) ) ) ) ) ) ))"));
    const auto clip = test_clip();
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::train, [&](const std::string&) { return clip; }, 4);
    RecordingSink sink;
    audio::SoundInputs inputs;
    inputs.camera = audio::SoundCamera::external;
    inputs[SoundVariable::distance] = 500;
    for (int i = 0; i < 10; ++i) sound.update(0.1f, inputs, sink);
    check(!sound.active() && sink.voices.empty(), "between 300 and 1000 m: silent, no restarts");
    inputs[SoundVariable::distance] = 250;
    for (int i = 0; i < 10; ++i) sound.update(0.1f, inputs, sink);
    check(sound.active() && sink.voices.size() == 1, "within 300 m: one loop, started once");
    inputs[SoundVariable::distance] = 350;
    sound.update(0.1f, inputs, sink);
    check(!sound.active(), "beyond 300 m it stops");
}

// Random loop phase: whole-clip loops start at a random frame, other voices
// at 0; preload fetches every clip before anything plays.
void test_loop_phase() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( ScalabiltyGroup ( 7 Activation ( ExternalCam ( ) Distance ( 500 ) ) Streams ( 3
    Stream ( Triggers ( 1 Initial_Trigger ( StartLoop ( 1 File ( "a.wav" -1 ) ) ) ) )
    Stream ( Triggers ( 1 Initial_Trigger ( StartLoopRelease ( 1 File ( "b.wav" -1 ) ) ) ) )
    Stream ( Triggers ( 1 Discrete_Trigger ( 5 PlayOneShot ( 1 File ( "c.wav" -1 ) ) ) ) ) ) ) ))"));
    const auto clip = test_clip();
    std::vector<std::string> looked_up;
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::player_train,
                               [&](const std::string& name) {
                                   looked_up.push_back(name);
                                   return clip;
                               }, 9);
    sound.preload();
    check(looked_up.size() == 3, "preload fetches all three clips");
    sound.set_random_loop_phase(true);
    RecordingSink sink;
    audio::SoundInputs inputs;
    inputs.camera = audio::SoundCamera::external;
    inputs[SoundVariable::distance] = 10;
    sound.update(0.1f, inputs, sink);
    check(sink.voices.size() == 2, "two loops started");
    check(sink.seeks.size() == 1 && sink.seeks.begin()->second < clip->frames, "only the StartLoop voice is moved, inside the clip");
    const std::uint32_t five = 5;
    inputs.discrete = std::span(&five, 1);
    sound.update(0.1f, inputs, sink);
    check(sink.voices.size() == 3 && sink.seeks.size() == 1, "one-shots start at 0");
}

// Going into the cab and out again (RTS 0x4A9CF4 / 0x4A9BA8): the loop a
// discrete trigger started outside comes back where it was, the initial
// trigger does not fire again, and a code that came while in the cab runs
// when the group is back.
void test_cab_round_trip() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( ScalabiltyGroup ( 7 Activation ( ExternalCam ( ) ) Deactivation ( CabCam ( ) ) Streams ( 2
    Stream ( Triggers ( 3
        Initial_Trigger ( StartLoop ( 1 File ( "idle.wav" -1 ) ) )
        Discrete_Trigger ( 4 ReleaseLoopRelease ( ) )
        Discrete_Trigger ( 5 StartLoop ( 1 File ( "hum.wav" -1 ) ) ) ) )
    Stream ( Triggers ( 1 Discrete_Trigger ( 7 StartLoop ( 1 File ( "fan.wav" -1 ) ) ) ) ) ) ) ))"));
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips;
    for (const char* name : {"idle.wav", "hum.wav", "fan.wav"}) clips[name] = test_clip(1000);
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::player_train,
                               [&](const std::string& name) { return clips.at(name); }, 3);
    RecordingSink sink;
    audio::SoundInputs inputs;
    inputs.camera = audio::SoundCamera::external;
    inputs[SoundVariable::distance] = 0;
    sound.update(0.01f, inputs, sink);
    check(sink.voices.size() == 1 && sink.voices.begin()->second.clip == clips["idle.wav"], "outside: the initial loop");
    const std::uint32_t four = 4, five = 5, seven = 7;
    // RTS 0x4A8C00: a StartLoop is ignored while the stream's loop loops.
    inputs.discrete = std::span(&five, 1);
    sound.update(0.01f, inputs, sink);
    check(sink.voices.size() == 1, "the hum waits: the idle loop still loops");
    inputs.discrete = std::span(&four, 1);
    sound.update(0.01f, inputs, sink);
    sink.advance(); // the released idle pass ends
    inputs.discrete = std::span(&five, 1);
    sound.update(0.01f, inputs, sink);
    inputs.discrete = {};
    sound.update(0.03f, inputs, sink); // the hum has played 0.04 s = 40 frames
    inputs.camera = audio::SoundCamera::cab;
    sound.update(0.01f, inputs, sink);
    check(!sound.active() && sink.live() == 0, "in the cab: the exterior group is off");
    inputs.discrete = std::span(&seven, 1);
    sound.update(0.01f, inputs, sink);
    inputs.discrete = {};
    const auto before = sink.voices.size();
    inputs.camera = audio::SoundCamera::external;
    sound.update(0.01f, inputs, sink);
    std::vector<std::shared_ptr<const audio::AudioClip>> started;
    for (auto it = sink.voices.begin(); it != sink.voices.end(); ++it) {
        if (it->first > before) started.push_back(it->second.clip);
    }
    check(started.size() == 2 && started[0] == clips["hum.wav"] && started[1] == clips["fan.wav"],
          "outside again: the hum goes on, the fan the code in the cab asked for starts, no initial loop");
    const auto hum = static_cast<audio::VoiceId>(before + 1);
    check(sink.seeks.contains(hum) && sink.seeks[hum] >= 30 && sink.seeks[hum] <= 50, "the hum resumes where it was");
}

// Codes that came while the group was off are taken as states only: a horn
// blown in the cab does not sound outside when the engineer steps out; a
// loop started there is on, one started and released is not.
// RTS object flag 0x20 (crossings, .ws sources): streams fade with distance
// by their Attenuation, the default rule without one (0x4A7764), unless the
// volume curve is distance controlled.
void test_world_fade() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( ScalabiltyGroup ( 3 Activation ( ExternalCam ( ) Distance ( 350 ) ) Deactivation ( Distance ( 350 ) ) Streams ( 3
    Stream ( Triggers ( 1 Discrete_Trigger ( 3 StartLoop ( 1 File ( "bell.wav" -1 ) ) ) ) )
    Stream ( Triggers ( 1 Discrete_Trigger ( 3 StartLoop ( 1 File ( "gate.wav" -1 ) ) ) )
             VolumeCurve ( DistanceControlled CurvePoints ( 2 0 1 1000 1 ) Granularity ( 0.1 ) ) )
    Stream ( Triggers ( 1 Discrete_Trigger ( 3 StartLoop ( 1 File ( "horn.wav" -1 ) ) ) )
             Attenuation ( 1 200 2 ) ) ) ) ))"));
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips;
    for (const char* name : {"bell.wav", "gate.wav", "horn.wav"}) clips[name] = test_clip(1000);
    const auto volumes = [&](const float distance, const bool fades) {
        audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::world,
                                   [&](const std::string& name) { return clips.at(name); }, 5);
        RecordingSink sink;
        audio::SoundInputs inputs;
        inputs[SoundVariable::distance] = distance;
        inputs.fades_with_distance = fades;
        const std::uint32_t close[] = {3};
        inputs.discrete = std::span(close);
        sound.update(0.05f, inputs, sink);
        inputs.discrete = {};
        sound.update(0.05f, inputs, sink);
        std::map<std::shared_ptr<const audio::AudioClip>, float> by_clip;
        for (const auto& [id, voice] : sink.voices) by_clip[voice.clip] = voice.volume;
        return std::array<float, 3>{by_clip[clips["bell.wav"]], by_clip[clips["gate.wav"]], by_clip[clips["horn.wav"]]};
    };
    const auto near = volumes(0.0f, true), far = volumes(100.0f, true), flat = volumes(100.0f, false);
    check(std::fabs(near[0] - 1.0f) < 1e-4f && std::fabs(far[0] - 0.25f) < 1e-4f, "world fade: the default rule halves at 50 m, 0.25 at 100 m");
    check(std::fabs(far[1] - 1.0f) < 1e-4f, "world fade: a distance-controlled volume curve is not faded again");
    check(std::fabs(far[2] - 0.5f) < 1e-4f, "world fade: the stream's own Attenuation (type 2: 1 - 100 / 200)");
    check(std::fabs(flat[0] - 1.0f) < 1e-4f, "world fade: off for objects without flag 0x20 (rolling stock)");
}

void test_past_codes() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( ScalabiltyGroup ( 7 Activation ( ExternalCam ( ) ) Deactivation ( CabCam ( ) ) Streams ( 3
    Stream ( Triggers ( 1 Discrete_Trigger ( 10 PlayOneShot ( 1 File ( "horn.wav" -1 ) ) ) ) )
    Stream ( Triggers ( 2
        Discrete_Trigger ( 20 StartLoop ( 1 File ( "comp.wav" -1 ) ) )
        Discrete_Trigger ( 21 ReleaseLoopRelease ( ) ) ) )
    Stream ( Triggers ( 2
        Discrete_Trigger ( 30 StartLoopRelease ( 1 File ( "whistle.wav" -1 ) ) )
        Discrete_Trigger ( 31 ReleaseLoopRelease ( ) ) ) ) ) ) ))"));
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips;
    for (const char* name : {"horn.wav", "comp.wav", "whistle.wav"}) clips[name] = test_clip(1000);
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::player_train,
                               [&](const std::string& name) { return clips.at(name); }, 5);
    RecordingSink sink;
    audio::SoundInputs inputs;
    inputs[SoundVariable::distance] = 0;
    const std::uint32_t in_cab[] = {10, 20, 30, 31};
    // Started in the cab: the group has never been on.
    inputs.camera = audio::SoundCamera::cab;
    inputs.discrete = std::span(in_cab);
    sound.update(0.01f, inputs, sink);
    inputs.discrete = {};
    sound.update(0.01f, inputs, sink);
    inputs.camera = audio::SoundCamera::external;
    sound.update(0.01f, inputs, sink);
    const auto playing = [&] {
        std::vector<std::shared_ptr<const audio::AudioClip>> clips_on;
        for (const auto& [id, voice] : sink.voices)
            if (sink.playing(id)) clips_on.push_back(voice.clip);
        return clips_on;
    };
    auto on = playing();
    check(on.size() == 1 && on[0] == clips["comp.wav"], "first time outside: the compressor runs, no horn, no whistle");
    // Into the cab again: the horn, then the compressor off.
    inputs.camera = audio::SoundCamera::cab;
    sound.update(0.01f, inputs, sink);
    const std::uint32_t later[] = {10, 21};
    inputs.discrete = std::span(later);
    sound.update(0.01f, inputs, sink);
    inputs.discrete = {};
    const auto before = sink.voices.size();
    inputs.camera = audio::SoundCamera::external;
    sound.update(0.01f, inputs, sink);
    check(sink.voices.size() == before && playing().empty(), "outside again: the compressor stays off, the horn is not blown");
    // A horn blown outside still sounds.
    const std::uint32_t horn = 10;
    inputs.discrete = std::span(&horn, 1);
    sound.update(0.01f, inputs, sink);
    on = playing();
    check(on.size() == 1 && on[0] == clips["horn.wav"], "outside: the horn sounds at once");
}

// Trigger pairs as RTS keeps them (0x4A87C8) and the named triggers table
// (0x4DD0EC), which a DLL reads through RTSIsSMSTriggerOn / RTSGetNamedInteger.
void test_trigger_pairs() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( NamedTriggersTable ( "Aux On" 1 "aux off" 2 "100" 7 )
ScalabiltyGroup ( 7 DiscreteTriggerPairs ( 2 Pair ( On ( 1 45 ) Off ( 1 46 ) Halt ( 1 47 ) ) Pair ( On ( 1 50 ) Off ( 1 51 ) ) )
Streams ( 1 Stream ( Triggers ( 2
    Discrete_Trigger ( 45 PlayOneShot ( 1 File ( "on.wav" -1 ) ) )
    Discrete_Trigger ( 46 PlayOneShot ( 1 File ( "off.wav" -1 ) ) ) ) ) ) ) ))"));
    check(script->named_triggers.size() == 3 && script->named_triggers[0].first == "aux on"
              && script->named_triggers[0].second == 1 && script->named_triggers[2].first == "100"
              && script->named_triggers[2].second == 7,
          "NamedTriggersTable: name and code in pairs, names lower-cased");
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips{{"on.wav", test_clip()}, {"off.wav", test_clip()}};
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::player_train,
                               [&](const std::string& name) { return clips.at(name); }, 9);
    RecordingSink sink;
    audio::SoundInputs inputs;
    const auto send = [&](const std::uint32_t code) {
        sink.advance();
        inputs.discrete = std::span(&code, 1);
        const auto before = sink.voices.size();
        sound.update(0.01f, inputs, sink);
        inputs.discrete = {};
        return sink.voices.size() > before;
    };
    sound.update(0.01f, inputs, sink);
    check(!sound.trigger_pairs(), "no pair state before a pair code");
    check(send(46), "an Off code passes before its pair is known");
    check(!send(46), "a second Off is dropped");
    check(send(45), "On passes");
    check(!send(45), "a second On is dropped");
    const auto state = sound.trigger_pairs();
    check(state && state->known[0] == 1u && state->on[0] == 1u, "pair 0 known and on, pair 1 not known");
    send(47);
    const auto halted = sound.trigger_pairs();
    check(halted && halted->known[0] == 1u && halted->on[0] == 0u, "Halt sets the pair off");
    check(send(45), "On passes again after Halt");
}

// A stream plays one sound at a time (RTS 0x4A8C00): one-shots wait for a
// free stream, a StartLoop leaves a looping loop alone and restarts anything
// else, ReleaseLoopReleaseWithJump jumps.
void test_stream_rules() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( ScalabiltyGroup ( 7 Streams ( 3
    Stream ( Triggers ( 1 Discrete_Trigger ( 1 PlayOneShot ( 1 File ( "click.wav" -1 ) ) ) ) )
    Stream ( Triggers ( 3
        Discrete_Trigger ( 2 StartLoopRelease ( 1 File ( "comp.wav" -1 ) ) )
        Discrete_Trigger ( 3 ReleaseLoopReleaseWithJump ( ) )
        Discrete_Trigger ( 4 ReleaseLoopRelease ( ) ) ) )
    Stream ( Triggers ( 2
        Discrete_Trigger ( 5 StartLoopRelease ( 1 File ( "region.wav" -1 ) ) )
        Discrete_Trigger ( 6 ReleaseLoopReleaseWithJump ( ) ) ) ) ) ) ))"));
    auto region = std::make_shared<audio::AudioClip>(*test_clip());
    region->loop_start = 20;
    region->loop_end = 60;
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips{
        {"click.wav", test_clip()}, {"comp.wav", test_clip()}, {"region.wav", region}};
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::player_train,
                               [&](const std::string& name) { return clips.at(name); }, 5);
    RecordingSink sink;
    audio::SoundInputs inputs;
    inputs.camera = audio::SoundCamera::cab;
    inputs.at_listener = true; // the cab's own script: not placed
    const auto send = [&](std::initializer_list<std::uint32_t> codes) {
        const std::vector<std::uint32_t> list(codes);
        inputs.discrete = std::span(list.data(), list.size());
        sound.update(0.05f, inputs, sink);
        inputs.discrete = {};
    };
    const auto count = [&](const char* name) {
        std::size_t n = 0;
        for (const auto& [id, voice] : sink.voices) n += voice.clip == clips.at(name) ? 1 : 0;
        return n;
    };
    const auto live = [&](const char* name) {
        std::size_t n = 0;
        for (const auto& [id, voice] : sink.voices) n += voice.clip == clips.at(name) && sink.playing(id) ? 1 : 0;
        return n;
    };
    sound.update(0.05f, inputs, sink);

    send({1, 1});
    check(count("click.wav") == 1, "two clicks in one update: the second waits for nothing, it is dropped");
    check(sink.voices.rbegin()->second.space == audio::VoiceSpace::flat, "a sound heard at the listener is not placed");
    send({1});
    check(count("click.wav") == 1, "a click while the stream plays the last one is dropped");
    sink.advance();
    send({1});
    check(count("click.wav") == 2, "a click on a free stream plays");

    send({2});
    send({2});
    check(count("comp.wav") == 1 && live("comp.wav") == 1, "StartLoopRelease while the loop loops: nothing new");
    send({4});
    send({2});
    check(count("comp.wav") == 2 && live("comp.wav") == 1, "StartLoopRelease while a released pass plays out: that stops, the loop starts anew");
    // The ChS4t's auxiliary compressor above 7 atm: start and release with a jump every step.
    send({3});
    sink.advance();
    const auto before = count("comp.wav");
    for (int i = 0; i < 20; ++i) {
        send({2, 3});
        check(live("comp.wav") == 0, "start and jump in one update: nothing left playing");
    }
    check(count("comp.wav") == before + 20, "each step's start is gone at once (no voices pile up)");
    // A jump after the loop played 30 frames of 100: from frame 70, once.
    send({2});
    const auto loop = sink.next - 1;
    sink.cursors[loop] = 30;
    send({3});
    check(sink.seeks.contains(loop) && sink.seeks[loop] == 70 && sink.voices[loop].released, "WithJump: from (length - played)");
    // With a loop region: on from where it is to the end, no jump.
    send({5});
    const auto in_region = sink.next - 1;
    sink.cursors[in_region] = 40;
    send({6});
    check(!sink.seeks.contains(in_region) && sink.voices[in_region].released, "WithJump in a loop region: plays on to the end");
}

// Rooms and sound proofing (RTS 0x51434C, 0x5143DC): a ChS4t-like cab, two
// cabins and the machine room between them.
void test_rooms() {
    const auto cab = formats::parse_cab_description(R"(SIMISA@@@@@@@@@@JINX0t1t______
Shape ( cab.s ESD_Interior ( InteriorGeometryInfo (
    InteriorAreas ( 3
        InteriorArea ( "CABIN1" -1.5 9.5 1.5 7.66 ChildNodes ( 0 ) )
        InteriorArea ( "MACHINE" -1.5 7.66 1.5 -7.40 ChildNodes ( 0 ) )
        InteriorArea ( "CABIN2" -1.5 -7.40 1.5 -9.5 ChildNodes ( 0 ) ) )
    InteriorDoors ( 3
        InteriorDoor ( Name ( DOOR_A ) Connects ( MACHINE EXTERIOR ) SoundProof ( 0.5 0.9 ) )
        InteriorDoor ( Name ( DOOR_C2 ) Connects ( MACHINE CABIN1 ) SoundProof ( 0.6 0.9 ) Opened ( ) )
        InteriorDoor ( Name ( DOOR_C4 ) Connects ( Machine Cabin2 ) SoundProof ( 0.6 0.9 ) ) )
    InteriorWindows ( 2
        InteriorWindow ( Name ( okno_1 ) Position ( 0.8 2.5 8.5 ) SoundProof ( 0.5 0.8 ) )
        InteriorWindow ( Name ( okno_3 ) Position ( 0.8 2.5 -8.5 ) ) ) ) ) ))");
    check(cab.ok() && cab.doors.size() == 3 && cab.windows.size() == 2, "rooms: the cab parses");
    if (cab.doors.size() != 3 || cab.windows.size() != 2) return;
    check(near(cab.doors[0].sound_closed, 0.5) && near(cab.doors[0].sound_open, 0.9) && !cab.doors[0].opened,
          "door SoundProof");
    check(cab.doors[1].opened, "Opened ( ) opens a door from the start");
    check(cab.windows[0].has_position && near(cab.windows[0].position[2], 8.5), "window Position");
    check(near(cab.windows[1].sound_closed, 0.0) && near(cab.windows[1].sound_open, 0.0),
          "a window without SoundProof passes nothing");

    const audio::InteriorAcoustics rooms(cab);
    constexpr int out = audio::InteriorAcoustics::outside;
    check(rooms.room_at(0, 8.5f) == 0 && rooms.room_at(0, 0) == 1 && rooms.room_at(0, -8.5f) == 2,
          "the room of a point");
    check(rooms.room_at(0, 12) == out && rooms.room_at(2, 8) == out && rooms.room_at(0, 9.5f) == out,
          "outside every rectangle (edges excluded)");
    const std::vector<float> shut(3, 0.0f), windows_shut(2, 0.0f);
    // CABIN1 to the open air: its window 0.5 beats the way through the
    // machine room (0.6 x 0.5).
    check(near(rooms.transmission(0, out, shut, windows_shut), 0.5), "cabin to the outside, all shut");
    check(near(rooms.transmission(0, out, shut, std::vector<float>{1.0f, 0.0f}), 0.8), "the cabin's window open");
    check(near(rooms.transmission(0, out, std::vector<float>{1.0f, 1.0f, 0.0f}, windows_shut), 0.81),
          "both doors open: 0.9 x 0.9");
    check(near(rooms.transmission(0, out, std::vector<float>{0.5f, 0.0f, 0.0f}, windows_shut), 0.5),
          "a half-open door passes between its two values");
    check(near(rooms.transmission(1, 0, shut, windows_shut), 0.6), "machine room to the cabin: the cabin door");
    check(near(rooms.transmission(out, 0, shut, windows_shut), 0.3),
          "the outside into the cabin: doors only (windows count towards the outside)");
    check(near(rooms.transmission(2, 2, shut, windows_shut), 1.0) && near(rooms.transmission(out, out, shut, windows_shut), 1.0),
          "the same room passes everything");
    check(near(rooms.transmission(2, out, shut, windows_shut), 0.3), "the other cabin: its window passes nothing");
}

// SoundSources, Attenuation, the head out of a window and the room's share
// (RTS 0x44B6B4, 0x4A7764, 0x4A78A0).
void test_sources_and_window() {
    using formats::SoundAttenuation;
    check(near(audio::attenuation_gain(SoundAttenuation{.scale = 1, .distance = 10, .type = 1}, 5), 0.75)
              && near(audio::attenuation_gain(SoundAttenuation{.scale = 1, .distance = 10, .type = 1}, 20), 0.25),
          "Attenuation type 1");
    check(near(audio::attenuation_gain(SoundAttenuation{.scale = 1, .distance = 10, .type = 2}, 5), 0.5)
              && near(audio::attenuation_gain(SoundAttenuation{.scale = 1, .distance = 10, .type = 2}, 20), 0.0),
          "Attenuation type 2");
    check(near(audio::attenuation_gain(SoundAttenuation{.scale = 1, .distance = 10, .type = 3}, 10, 100), 0.75)
              && near(audio::attenuation_gain(SoundAttenuation{.scale = 1, .distance = 10, .type = 3}, 40, 100), 0.25),
          "Attenuation of another type: halves at a fifth of the deactivation distance");

    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(R"(
Tr_SMS ( ScalabiltyGroup ( 7 Activation ( CabCam ( ) ) SoundSources ( 2 0 2 8 0.5  0 2 0 1.0 ) Streams ( 3
    Stream ( Sources ( 1 1 ) Triggers ( 1 Initial_Trigger ( StartLoop ( 1 File ( "a.wav" -1 ) ) ) ) )
    Stream ( AllowStreamOnWindowCamera ( ) Triggers ( 1 Initial_Trigger ( StartLoop ( 1 File ( "b.wav" -1 ) ) ) ) )
    Stream ( Sources ( 2 0 1 ) Attenuation ( 1 10 1 ) Triggers ( 1 Initial_Trigger ( StartLoop ( 1 File ( "c.wav" -1 ) ) ) ) ) ) ) ))"));
    check(script->ok() && script->groups.at(0).sources.size() == 2, "SoundSources parse");
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips;
    for (const char* name : {"a.wav", "b.wav", "c.wav"}) clips[name] = test_clip(1000);
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::player_train,
                               [&](const std::string& name) { return clips.at(name); }, 5);
    RecordingSink sink;
    const std::vector<audio::SoundSourceLevel> levels{{0.5f, 0.0f}, {0.6f, 5.0f}};
    audio::SoundInputs inputs;
    inputs.camera = audio::SoundCamera::cab;
    inputs.sources = levels;
    inputs.gain = 0.7f;
    sound.update(0.01f, inputs, sink);
    const auto volume = [&](const char* name) {
        for (const auto& [_, voice] : sink.voices)
            if (voice.clip == clips[name] && !voice.stopped) return voice.volume;
        return -1.0f;
    };
    check(near(volume("a.wav"), 0.6 * 0.7, 1e-4), "a stream with Sources plays at its point's level");
    check(near(volume("b.wav"), 0.7, 1e-4), "a stream without Sources plays at its own volume, times the room's share");
    check(near(volume("c.wav"), 0.5 * 0.7, 1e-4), "the loudest point through the stream's Attenuation");
    inputs.window = true;
    sound.update(0.01f, inputs, sink);
    check(near(volume("a.wav"), 0.0) && near(volume("c.wav"), 0.0) && near(volume("b.wav"), 0.7, 1e-4),
          "the head out of a window: only AllowStreamOnWindowCamera streams");
}

void test_ambient_runner() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(ambient_script));
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips{
        {"a.wav", test_clip()}, {"b.wav", test_clip()}, {"on.wav", test_clip()}};
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::environment,
                               [&](const std::string& name) { return clips[name]; }, 3);
    RecordingSink sink;
    audio::SoundInputs inputs;
    const auto tick = [&] {
        sound.update(0.1f, inputs, sink);
        sink.advance();
    };
    for (int i = 0; i < 30; ++i) tick();
    check(sound.active() && sink.voices.empty(), "random trigger disabled by the initial trigger");

    inputs[SoundVariable::variable1] = 12;
    for (int i = 0; i < 25; ++i) tick();
    check(sink.voices.size() == 2, "enabled random trigger fires every second");
    check(sink.voices[1].clip == clips["a.wav"] && sink.voices[2].clip == clips["b.wav"], "sequential selection");
    check(sink.voices[1].space == audio::VoiceSpace::flat && sink.voices[1].playback == audio::VoicePlayback::once,
          "environment one-shot is not positioned");
    check(near(sink.voices[2].volume, 0.5), "Volume_Min_Max");

    inputs[SoundVariable::variable1] = 4;
    for (int i = 0; i < 30; ++i) tick();
    check(sink.voices.size() == 2, "TriggerActions disabled the random trigger");

    const std::uint32_t on[] = {45, 45};
    inputs.discrete = on;
    tick();
    check(sink.voices.size() == 3, "trigger pair passes only the first 'on'");
    tick();
    check(sink.voices.size() == 3, "repeated 'on' without 'off' is dropped");
    const std::uint32_t off_on[] = {46, 45};
    inputs.discrete = off_on;
    tick();
    check(sink.voices.size() == 4, "'off' re-arms the pair");
}

// Minimal PCM16 WAV with an optional smpl loop or cue points.
std::string wav(const std::uint32_t frames, const int loop_start, const int loop_end, const bool use_cue) {
    std::string data(frames * 2, '\0');
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto sample = static_cast<std::int16_t>(i * 100);
        std::memcpy(data.data() + i * 2, &sample, 2);
    }
    const auto u32 = [](std::uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [](std::uint16_t v) { return std::string(reinterpret_cast<const char*>(&v), 2); };
    std::string chunks = "fmt " + u32(16) + u16(1) + u16(1) + u32(22050) + u32(44100) + u16(2) + u16(16);
    chunks += "data" + u32(static_cast<std::uint32_t>(data.size())) + data;
    if (loop_end > loop_start) {
        if (use_cue) {
            std::string cue = u32(2);
            for (const int position : {loop_end, loop_start}) {
                cue += u32(1) + u32(static_cast<std::uint32_t>(position)) + "data" + u32(0) + u32(0)
                    + u32(static_cast<std::uint32_t>(position));
            }
            chunks += "cue " + u32(static_cast<std::uint32_t>(cue.size())) + cue;
        } else {
            std::string smpl(28, '\0');
            smpl += u32(1) + u32(0);
            smpl += u32(0) + u32(0) + u32(static_cast<std::uint32_t>(loop_start))
                + u32(static_cast<std::uint32_t>(loop_end)) + u32(0) + u32(0);
            chunks += "smpl" + u32(static_cast<std::uint32_t>(smpl.size())) + smpl;
        }
    }
    return "RIFF" + u32(static_cast<std::uint32_t>(chunks.size() + 4)) + "WAVE" + chunks;
}

void test_decoding() {
    std::string error;
    const auto plain = audio::decode_clip(wav(1000, 0, 0, false), &error);
    check(plain && plain->frames == 1000 && plain->channels == 1 && plain->sample_rate == 22050, "PCM16 WAV decodes");
    check(plain && !plain->has_loop(), "no loop region without smpl or cue");
    check(plain && near(plain->samples[10], 1000.0 / 32768.0, 1e-4), "samples are converted to float");
    const auto smpl = audio::decode_clip(wav(1000, 200, 799, false));
    check(smpl && smpl->loop_start == 200 && smpl->loop_end == 800, "smpl loop (end inclusive)");
    const auto cue = audio::decode_clip(wav(1000, 300, 700, true));
    check(cue && cue->loop_start == 300 && cue->loop_end == 700, "cue points, sorted");
    check(!audio::decode_clip("not a sound file", &error) && !error.empty(), "garbage is rejected");
}

// The real mixer, offline: a looping clip is audible, and a source to the
// camera's right (MSTS +X when looking along +Z) is louder on the right.
void test_engine() {
    audio::AudioEngine engine({.device = false, .channels = 2, .sample_rate = 22050});
    check(engine.ok(), "offline engine starts");
    auto clip = std::make_shared<audio::AudioClip>();
    clip->channels = 1;
    clip->sample_rate = 22050;
    clip->frames = 2205;
    for (std::uint64_t i = 0; i < clip->frames; ++i) {
        clip->samples.push_back(0.5f * static_cast<float>(std::sin(2.0 * 3.14159265 * 440.0 * static_cast<double>(i) / 22050.0)));
    }
    engine.set_listener({0, 0, 0}, {0, 0, 1});
    const auto voice = engine.play(clip, audio::VoicePlayback::loop, audio::VoiceSpace::panned);
    check(voice != 0, "voice starts");
    engine.set(voice, 1.0f, 1.0f, {10, 0, 0});
    std::vector<float> mix(2 * 22050);
    engine.render(mix.data(), 22050);
    double left = 0;
    double right = 0;
    for (std::size_t i = 0; i < mix.size(); i += 2) {
        left += mix[i] * mix[i];
        right += mix[i + 1] * mix[i + 1];
    }
    check(left + right > 100.0, "looping voice is audible for a whole second");
    check(right > left * 4.0, "a source on the right (+X) is louder in the right channel");
    check(engine.playing(voice), "loop keeps playing");
    engine.release(voice);
    engine.render(mix.data(), 22050);
    engine.update();
    check(!engine.playing(voice) && engine.active_voices() == 0, "released loop ends and is freed");

    // A silent loop is held where it is (not mixed) and goes on from there.
    const auto quiet = engine.play(clip, audio::VoicePlayback::loop, audio::VoiceSpace::panned);
    engine.set(quiet, 1.0f, 1.0f, {0, 0, 10});
    engine.render(mix.data(), 1000);
    engine.set(quiet, 0.0f, 1.0f, {0, 0, 10});
    const auto held_at = engine.cursor(quiet);
    engine.render(mix.data(), 5000);
    check(engine.audible_voices() == 0 && engine.active_voices() == 1 && engine.playing(quiet), "silent loop held, its voice kept");
    check(held_at && engine.cursor(quiet) == held_at, "held loop does not advance");
    engine.set(quiet, 1.0f, 1.0f, {0, 0, 10});
    engine.render(mix.data(), 1000);
    check(engine.audible_voices() == 1 && engine.cursor(quiet) != held_at, "heard again, the loop goes on");
    engine.set(quiet, 0.0f, 1.0f, {0, 0, 10});
    engine.release(quiet);
    check(engine.active_voices() == 0, "a held loop released ends at once");

    // The places are the heard voices': held ones take none; one heard
    // again waits for a free place.
    audio::AudioEngine two({.max_voices = 2, .device = false, .channels = 2, .sample_rate = 22050});
    const auto a = two.play(clip, audio::VoicePlayback::loop, audio::VoiceSpace::panned);
    const auto b = two.play(clip, audio::VoicePlayback::loop, audio::VoiceSpace::panned);
    check(two.play(clip, audio::VoicePlayback::loop, audio::VoiceSpace::panned) == 0, "no place past max_voices heard");
    two.set(a, 0.0f, 1.0f, {0, 0, 10});
    two.set(b, 0.0f, 1.0f, {0, 0, 10});
    const auto c = two.play(clip, audio::VoicePlayback::loop, audio::VoiceSpace::panned);
    const auto d = two.play(clip, audio::VoicePlayback::loop, audio::VoiceSpace::panned);
    check(c != 0 && d != 0 && two.active_voices() == 4 && two.audible_voices() == 2, "held voices take no place");
    two.set(a, 1.0f, 1.0f, {0, 0, 10});
    check(two.audible_voices() == 2 && two.playing(a), "heard again with no place free: it waits");
    two.stop(c);
    two.set(a, 1.0f, 1.0f, {0, 0, 10});
    check(two.audible_voices() == 2 && two.active_voices() == 3, "a place freed: it goes on");
}

// The ED9M's cab script (MaDaDev): the DLL posts 16 for a controller step,
// 15 for the reverser; in the cab they must start one-shots.
void test_content_ed9m_cab(const std::filesystem::path& root) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    const auto path = core::VirtualPath::parse("TRAINS/TRAINSET/mdd_ED4m-Alias/Sound/mdd_ed4m-cabHead.sms");
    if (!path || !vfs.exists(*path)) {
        std::cout << "ed9m cab: not installed, skipped\n";
        return;
    }
    // The USAVP library's script: its NamedTriggersTable, the stops of the
    // routes' trains the DLL reads through RTSGetNamedInteger (0x4DD0EC).
    if (const auto usavp = core::VirtualPath::parse("TRAINS/TRAINSET/mdd_Libraries/Usavp/Usavp.sms"); usavp && vfs.exists(*usavp)) {
        const auto script = formats::load_sound_script(vfs, *usavp);
        check(script.ok() && script.named_triggers.size() == 7955, "Usavp.sms: 7955 named triggers");
        check(!script.named_triggers.empty() && script.named_triggers.front().first == "mdd_info_r20_7161_number"
                  && script.named_triggers.front().second == 7161,
              "Usavp.sms: the first entry, lower-cased, with its code");
    }
    // RTS 0x44247C: no RTSCabinSound, so the Engine block's Sound is the cab script.
    const auto g = formats::load_wagon(vfs, "mdd_ED9M-0062", "mdd_ED9M-0062_G", true);
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    check(g.ok() && lower(g.cab_sound).ends_with("mdd_ed4m-cabhead.sms"), "ed9m: the cab script from Engine ( Sound )");
    check(g.ok() && lower(g.sound).ends_with("mdd_ed9m-enghead.sms"), "ed9m: the exterior script from Wagon ( Sound )");
    if (g.ok() && g.engine && g.engine->aux_libraries.size() >= 2) {
        const auto& brakes = g.engine->aux_libraries[0];
        const auto& usavp = g.engine->aux_libraries[1];
        check(lower(brakes.sound).ends_with("brakes.sms") && brakes.sound_single_wagon && !brakes.sound_external &&
                  std::abs(brakes.sound_position[2] - 9.254f) < 1e-3f && brakes.sound_type == formats::AuxSoundType::wagon,
              "ed9m: Brakes.sms, single wagon, at the cab");
        check(lower(usavp.sound).ends_with("usavp.sms") && usavp.sound_external && !usavp.sound_single_wagon,
              "ed9m: Usavp.sms, external");
    } else {
        check(false, "ed9m: two aux libraries");
    }
    auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(vfs, *path));
    check(script->ok(), "ed9m cab: the script parses");
    const auto* group = script->select_group(formats::SoundOwner::player_train);
    check(group != nullptr, "ed9m cab: group 7");
    if (!group) return;
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips;
    audio::SoundInstance sound(script, *group, formats::SoundOwner::player_train,
                               [&](const std::string& name) {
                                   auto& clip = clips[name];
                                   if (!clip) clip = test_clip(1000);
                                   return clip;
                               }, 3);
    RecordingSink sink;
    audio::SoundInputs inputs;
    inputs.camera = audio::SoundCamera::cab;
    inputs[SoundVariable::distance] = 0;
    sound.update(0.01f, inputs, sink);
    check(sound.active(), "ed9m cab: active in the cab");
    for (const std::uint32_t code : {16u, 15u}) {
        sink.advance();
        const auto before = sink.voices.size();
        inputs.discrete = std::span(&code, 1);
        sound.update(0.01f, inputs, sink);
        inputs.discrete = {};
        bool started = false;
        for (const auto& [id, voice] : sink.voices)
            if (id > before && voice.playback == audio::VoicePlayback::once) started = true;
        check(started, code == 16 ? "ed9m cab: code 16 plays the controller" : "ed9m cab: code 15 plays the reverser");
    }
}

// The ChS4t's cab script with chs4t.dll's codes (docs/sound.md, "How a stream
// runs its actions"): 67 starts kompr_pant2.wav, 68 releases it with a jump.
void test_content_chs4t_compressor(const std::filesystem::path& root) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    const auto path = core::VirtualPath::parse("TRAINS/TRAINSET/ev_CHS4t-Alias/Sound/ev_CHS4t-cab.sms");
    if (!path || !vfs.exists(*path)) {
        std::cout << "chs4t cab: not installed, skipped\n";
        return;
    }
    auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(vfs, *path));
    const auto* group = script->select_group(formats::SoundOwner::player_train);
    check(script->ok() && group != nullptr, "chs4t cab: the script parses, group 7");
    if (!group) return;
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips;
    audio::SoundInstance sound(script, *group, formats::SoundOwner::player_train,
                               [&](const std::string& name) {
                                   auto& clip = clips[name];
                                   if (!clip) clip = test_clip(1000);
                                   return clip;
                               }, 7);
    RecordingSink sink;
    audio::SoundInputs inputs;
    inputs.camera = audio::SoundCamera::cab;
    inputs[SoundVariable::distance] = 0;
    sound.update(0.05f, inputs, sink);
    const auto compressor = [&](const bool playing_only) {
        std::size_t n = 0;
        for (const auto& [id, voice] : sink.voices) {
            const auto it = std::find_if(clips.begin(), clips.end(), [&](const auto& c) { return c.second == voice.clip; });
            if (it == clips.end() || it->first.find("kompr_pant2") == std::string::npos) continue;
            if (!playing_only || sink.playing(id)) ++n;
        }
        return n;
    };
    const auto step = [&](std::initializer_list<std::uint32_t> codes) {
        const std::vector<std::uint32_t> list(codes);
        inputs.discrete = std::span(list.data(), list.size());
        sound.update(0.05f, inputs, sink);
        inputs.discrete = {};
    };
    // Below 7 atm: 67 every step, the compressor loops on.
    for (int i = 0; i < 20; ++i) step({67});
    check(compressor(false) == 1 && compressor(true) == 1, "chs4t: below 7 atm one compressor loop, not restarted");
    // Above 7 atm: 67 and 68 every step, silence as in RTS.
    for (int i = 0; i < 20; ++i) {
        step({67, 68});
        check(compressor(true) == 0, "chs4t: 67 + 68 in one step leave nothing playing");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        test_content_ed9m_cab(argv[1]);
        test_content_chs4t_compressor(argv[1]);
        if (failures == 0) std::cout << "sound content: all checks passed\n";
        return failures == 0 ? 0 : 1;
    }
    test_parsing();
    test_station_runner();
    test_ambient_runner();
    test_default_distances();
    test_crossed_distances();
    test_loop_phase();
    test_cab_round_trip();
    test_past_codes();
    test_world_fade();
    test_trigger_pairs();
    test_stream_rules();
    test_rooms();
    test_sources_and_window();
    test_decoding();
    test_engine();
    if (failures == 0) std::cout << "sound: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
