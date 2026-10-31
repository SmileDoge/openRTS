#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::audio {

// A decoded sound file: interleaved 32-bit float frames at the file's own
// rate and channel count.
struct AudioClip {
    std::vector<float> samples;
    std::uint32_t channels{};
    std::uint32_t sample_rate{};
    std::uint64_t frames{};
    // Loop region in frames, [loop_start, loop_end); empty when the file has
    // none. Taken from the first `smpl` loop, else the first two cue points.
    std::uint64_t loop_start{};
    std::uint64_t loop_end{};

    [[nodiscard]] bool has_loop() const noexcept { return loop_end > loop_start; }
};

// Decodes a WAV (or another format miniaudio reads) from memory.
[[nodiscard]] std::shared_ptr<const AudioClip> decode_clip(std::string_view bytes, std::string* error = nullptr);
[[nodiscard]] std::shared_ptr<const AudioClip> load_clip(const core::Vfs& vfs, const core::VirtualPath& file,
                                                         std::string* error = nullptr);

// The sound file a script's `File ( "name" )` plays: the first existing
// candidate (formats::sound_file_candidates). A compressed file with the same
// name next to it (.ogg, .flac, .mp3) is preferred, so content can ship WAV
// for MSTS/RTS and a smaller file for openRTS without changing the script.
[[nodiscard]] std::optional<core::VirtualPath> find_sound_file(const core::Vfs& vfs, const core::VirtualPath& script,
                                                               std::string_view name);

// Clips by virtual path, decoded on first use and kept while referenced or
// until clear(). Failed loads are remembered, so a missing file is reported once.
class SoundBank {
public:
    explicit SoundBank(std::shared_ptr<const core::Vfs> vfs) : vfs_(std::move(vfs)) {}

    [[nodiscard]] std::shared_ptr<const AudioClip> get(const core::VirtualPath& file);
    void clear() { clips_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return clips_.size(); }

private:
    std::shared_ptr<const core::Vfs> vfs_;
    std::unordered_map<std::string, std::shared_ptr<const AudioClip>> clips_;
};

} // namespace openrts::audio
