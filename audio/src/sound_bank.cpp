#include <openrts/audio/sound_bank.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>

#include <openrts/formats/sound.hpp>

#include "miniaudio_config.hpp"

namespace openrts::audio {
namespace {

[[nodiscard]] std::uint32_t u32(const std::string_view bytes, const std::size_t offset) {
    std::uint32_t value = 0;
    if (offset + 4 <= bytes.size()) std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}

// Loop region from the RIFF chunks: the first `smpl` loop (end inclusive),
// else the span between the first two `cue ` points.
void read_loop(const std::string_view bytes, AudioClip& clip) {
    if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE") return;
    std::vector<std::uint64_t> cues;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const auto id = bytes.substr(offset, 4);
        const std::size_t size = u32(bytes, offset + 4);
        const std::size_t body = offset + 8;
        if (id == "smpl" && size >= 36 && u32(bytes, body + 28) > 0 && body + 36 + 24 <= bytes.size()) {
            const auto start = u32(bytes, body + 36 + 8);
            const auto end = u32(bytes, body + 36 + 12);
            if (end > start) {
                clip.loop_start = start;
                clip.loop_end = static_cast<std::uint64_t>(end) + 1;
                break;
            }
        } else if (id == "cue " && size >= 4) {
            const auto count = u32(bytes, body);
            for (std::uint32_t i = 0; i < count && body + 4 + (i + 1) * 24 <= bytes.size(); ++i) {
                cues.push_back(u32(bytes, body + 4 + i * 24 + 20));
            }
        }
        offset = body + size + (size & 1);
    }
    if (!clip.has_loop() && cues.size() >= 2) {
        std::sort(cues.begin(), cues.end());
        clip.loop_start = cues[0];
        clip.loop_end = cues[1];
    }
    if (clip.loop_end > clip.frames) clip.loop_end = clip.frames;
    if (clip.loop_start >= clip.loop_end) clip.loop_start = clip.loop_end = 0;
}

} // namespace

std::shared_ptr<const AudioClip> decode_clip(const std::string_view bytes, std::string* error) {
    const auto fail = [&](const std::string& message) -> std::shared_ptr<const AudioClip> {
        if (error) *error = message;
        return nullptr;
    };
    auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) != MA_SUCCESS) {
        return fail("unsupported or damaged sound file");
    }
    auto clip = std::make_shared<AudioClip>();
    clip->channels = decoder.outputChannels;
    clip->sample_rate = decoder.outputSampleRate;
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length > 0) {
        clip->samples.resize(static_cast<std::size_t>(length) * clip->channels);
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(&decoder, clip->samples.data(), length, &read);
        clip->frames = read;
    } else {
        // Length unknown: read in blocks.
        std::vector<float> block(4096 * static_cast<std::size_t>(clip->channels));
        for (;;) {
            ma_uint64 read = 0;
            if (ma_decoder_read_pcm_frames(&decoder, block.data(), 4096, &read) != MA_SUCCESS || read == 0) break;
            clip->samples.insert(clip->samples.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(read * clip->channels));
            clip->frames += read;
        }
    }
    ma_decoder_uninit(&decoder);
    clip->samples.resize(static_cast<std::size_t>(clip->frames) * clip->channels);
    if (clip->frames == 0 || clip->channels == 0) return fail("sound file has no samples");
    read_loop(bytes, *clip);
    return clip;
}

std::shared_ptr<const AudioClip> load_clip(const core::Vfs& vfs, const core::VirtualPath& file, std::string* error) {
    std::string read_error;
    const auto bytes = vfs.read(file, &read_error);
    if (!bytes) {
        if (error) *error = read_error;
        return nullptr;
    }
    return decode_clip(*bytes, error);
}

std::optional<core::VirtualPath> find_sound_file(const core::Vfs& vfs, const core::VirtualPath& script,
                                                const std::string_view name) {
    for (const auto& candidate : formats::sound_file_candidates(script, name)) {
        const auto text = candidate.str();
        const auto dot = text.find_last_of('.');
        const auto slash = text.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            const auto stem = text.substr(0, dot);
            for (const char* extension : {".ogg", ".flac", ".mp3"}) {
                const auto variant = core::VirtualPath::parse(stem + extension);
                if (variant && !(*variant == candidate) && vfs.exists(*variant)) return variant;
            }
        }
        if (vfs.exists(candidate)) return candidate;
    }
    return std::nullopt;
}

std::shared_ptr<const AudioClip> SoundBank::get(const core::VirtualPath& file) {
    if (const auto it = clips_.find(file.key()); it != clips_.end()) return it->second;
    std::string error;
    auto clip = load_clip(*vfs_, file, &error);
    if (!clip) std::cout << "sound '" << file.str() << "': " << error << '\n';
    clips_.emplace(file.key(), clip);
    return clip;
}

} // namespace openrts::audio
