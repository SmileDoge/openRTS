#include "thunder_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <openrts/core/virtual_path.hpp>

namespace openrts::viewer {
namespace {

constexpr double speed_of_sound = 343.0; // m/s
constexpr double day = 86400.0;

// Seconds from `start` to `time` (s of the day), over midnight too.
[[nodiscard]] double since(const double time, const double start) {
    return std::fmod(time - start + 2.0 * day, day);
}

// OPENRTS_LIGHTNING_TRACE set: each clap logged as it starts.
[[nodiscard]] bool trace() {
    static const bool on = [] {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t size = 0;
        const bool set = _dupenv_s(&value, &size, "OPENRTS_LIGHTNING_TRACE") == 0 && value != nullptr;
        std::free(value);
        return set;
#else
        return std::getenv("OPENRTS_LIGHTNING_TRACE") != nullptr;
#endif
    }();
    return on;
}

constexpr const char* kind_names[] = {"thunder_near_", "thunder_mid_", "thunder_far_"};

} // namespace

int ThunderView::kind_for(const float distance, const bool to_ground) noexcept {
    // A flash within the cloud rumbles more: heard as a strike 2 km further.
    const float heard = distance + (to_ground ? 0.0f : 2000.0f);
    return heard < 1500.0f ? 0 : heard < 5000.0f ? 1 : 2;
}

float ThunderView::volume_for(const float distance, const float strength, const bool to_ground) noexcept {
    // Full within 600 m, then falling as (600 / d)^0.8 (1 km 0.66, 3 km 0.28,
    // 12 km 0.09); a stronger strike louder; one within the cloud softer.
    const float fall = distance <= 600.0f ? 1.0f : std::pow(600.0f / distance, 0.8f);
    return fall * (0.7f + 0.3f * std::clamp(strength, 0.0f, 1.5f)) * (to_ground ? 1.0f : 0.7f);
}

void ThunderView::strike(const renderer::LightningStrike& strike, const float distance, const glm::vec3& at) {
    if (heard_.contains(strike.id)) return;
    heard_.emplace(strike.id, strike.start);
    waiting_.push_back({.id = strike.id,
                        .start = strike.start,
                        .delay = static_cast<double>(distance) / speed_of_sound,
                        .distance = distance,
                        .at = at,
                        .seed = strike.seed,
                        .strength = strike.strength,
                        .to_ground = strike.to_ground});
}

void ThunderView::load() {
    loaded_ = true;
    if (!vfs_) return;
    const auto folder = core::VirtualPath::parse("SOUND/OPENRTS/THUNDER");
    if (!folder) return;
    std::array<std::vector<core::VirtualPath>, 3> files;
    for (const auto& entry : vfs_->list(*folder)) {
        if (entry.directory) continue;
        const auto name = core::fold_case(entry.path.filename());
        const auto extension = core::fold_case(entry.path.extension());
        if (extension != ".wav" && extension != ".ogg" && extension != ".flac" && extension != ".mp3") continue;
        for (int kind = 0; kind < 3; ++kind) {
            if (name.starts_with(kind_names[kind])) files[kind].push_back(entry.path);
        }
    }
    for (int kind = 0; kind < 3; ++kind) {
        // In name order: the same clap for the same strike on every client.
        std::sort(files[kind].begin(), files[kind].end(), [](const auto& a, const auto& b) { return a.key() < b.key(); });
        for (const auto& file : files[kind]) {
            std::string error;
            const auto bytes = vfs_->read(file, &error);
            auto clip = bytes ? audio::decode_clip(*bytes, &error) : nullptr;
            if (!clip) {
                std::cout << "thunder: " << file.str() << ": " << error << std::endl;
                continue;
            }
            clips_[kind].push_back(std::move(clip));
            names_[kind].emplace_back(file.filename());
        }
    }
    std::cout << "thunder: " << clips_[0].size() << " near, " << clips_[1].size() << " middle, " << clips_[2].size() << " far claps"
              << (clips_[0].empty() && clips_[1].empty() && clips_[2].empty() ? " (no SOUND/OPENRTS/THUNDER: the openrts-thunder pack is missing)" : "")
              << std::endl;
}

void ThunderView::update(const double time, const float gain, audio::AudioEngine* audio) {
    // Strikes long past forgotten (or ahead: the clock was set back).
    std::erase_if(heard_, [time](const auto& entry) { return since(time, entry.second) > 150.0; });
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        const double passed = since(time, it->start);
        // The clock jumped past it (or back before it): dropped.
        if (passed > it->delay + 20.0 || passed > 200.0) {
            it = waiting_.erase(it);
            continue;
        }
        if (passed < it->delay) {
            ++it;
            continue;
        }
        if (!loaded_) load();
        // Its kind, else the nearest kind there are claps of.
        int kind = kind_for(it->distance, it->to_ground);
        for (const int other : {kind, kind == 0 ? 1 : kind - 1, kind == 2 ? 1 : kind + 1, 0, 1, 2}) {
            if (!clips_[other].empty()) {
                kind = other;
                break;
            }
        }
        const auto& set = clips_[kind];
        const float volume = volume_for(it->distance, it->strength, it->to_ground);
        // A little higher or lower, by the strike.
        const float pitch = 0.94f + 0.12f * static_cast<float>(it->seed % 1000u) / 1000.0f;
        if (!set.empty() && audio) {
            const auto pick = (it->seed / 1000u) % set.size();
            const auto voice = audio->play(set[pick], audio::VoicePlayback::once, audio::VoiceSpace::panned);
            if (voice != 0) {
                audio->set(voice, volume * gain * std::clamp(thunder_volume(), 0.0f, 2.0f), pitch, it->at);
                playing_.push_back({voice, volume, it->at, pitch});
            }
            if (trace()) {
                std::cout << "thunder: strike " << (it->id % 100000) << " heard " << passed << " s after, " << static_cast<int>(it->distance)
                          << " m: " << names_[kind][pick] << ", volume " << volume << " x " << gain << (voice != 0 ? "" : " (no voice)") << std::endl;
            }
        } else if (trace()) {
            std::cout << "thunder: strike " << (it->id % 100000) << " due, " << static_cast<int>(it->distance) << " m: "
                      << (audio ? "no claps" : "no sound") << std::endl;
        }
        it = waiting_.erase(it);
    }
    if (!audio) {
        playing_.clear();
        return;
    }
    std::erase_if(playing_, [audio](const Playing& p) { return !audio->playing(p.voice); });
    for (const auto& p : playing_) audio->set(p.voice, p.volume * gain * std::clamp(thunder_volume(), 0.0f, 2.0f), p.pitch, p.at);
}

void ThunderView::stop(audio::AudioEngine& audio) {
    for (const auto& p : playing_) audio.stop(p.voice);
    playing_.clear();
    waiting_.clear();
}

} // namespace openrts::viewer
