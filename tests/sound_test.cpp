// Sound scripts: parsing (.sms, .ws), the script runner against a recording
// voice sink, and WAV decoding with loop regions.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <openrts/audio/audio_engine.hpp>
#include <openrts/audio/sound_bank.hpp>
#include <openrts/audio/sound_instance.hpp>
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
    std::map<audio::VoiceId, std::uint64_t> seeks;

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

void test_ambient_runner() {
    auto script = std::make_shared<formats::SoundScript>(formats::parse_sound_script(ambient_script));
    std::map<std::string, std::shared_ptr<const audio::AudioClip>> clips{
        {"a.wav", test_clip()}, {"b.wav", test_clip()}, {"on.wav", test_clip()}};
    audio::SoundInstance sound(script, script->groups.at(0), formats::SoundOwner::environment,
                               [&](const std::string& name) { return clips[name]; }, 3);
    RecordingSink sink;
    audio::SoundInputs inputs;
    for (int i = 0; i < 30; ++i) sound.update(0.1f, inputs, sink);
    check(sound.active() && sink.voices.empty(), "random trigger disabled by the initial trigger");

    inputs[SoundVariable::variable1] = 12;
    for (int i = 0; i < 25; ++i) sound.update(0.1f, inputs, sink);
    check(sink.voices.size() == 2, "enabled random trigger fires every second");
    check(sink.voices[1].clip == clips["a.wav"] && sink.voices[2].clip == clips["b.wav"], "sequential selection");
    check(sink.voices[1].space == audio::VoiceSpace::flat && sink.voices[1].playback == audio::VoicePlayback::once,
          "environment one-shot is not positioned");
    check(near(sink.voices[2].volume, 0.5), "Volume_Min_Max");

    inputs[SoundVariable::variable1] = 4;
    for (int i = 0; i < 30; ++i) sound.update(0.1f, inputs, sink);
    check(sink.voices.size() == 2, "TriggerActions disabled the random trigger");

    const std::uint32_t on[] = {45, 45};
    inputs.discrete = on;
    sound.update(0.1f, inputs, sink);
    check(sink.voices.size() == 3, "trigger pair passes only the first 'on'");
    sound.update(0.1f, inputs, sink);
    check(sink.voices.size() == 3, "repeated 'on' without 'off' is dropped");
    const std::uint32_t off_on[] = {46, 45};
    inputs.discrete = off_on;
    sound.update(0.1f, inputs, sink);
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
}

} // namespace

int main() {
    test_parsing();
    test_station_runner();
    test_ambient_runner();
    test_default_distances();
    test_crossed_distances();
    test_loop_phase();
    test_decoding();
    test_engine();
    if (failures == 0) std::cout << "sound: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
