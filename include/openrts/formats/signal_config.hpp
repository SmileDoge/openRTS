#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

// RTS numbering (docs/reverse/signals.md); a larger aspect is less restrictive.
enum class SignalAspect : std::uint8_t {
    stop = 0,
    stop_and_proceed = 1,
    restricting = 2,
    approach_1 = 3,
    approach_2 = 4,
    approach_3 = 5,
    clear_1 = 6,
    clear_2 = 7,
    block_obstructed = 8,
    failed = 9,
};

enum class SignalFunction : std::uint8_t { normal = 1, shunting = 2, distance = 3, repeater = 4, info = 5 };

[[nodiscard]] std::optional<SignalAspect> signal_aspect_from_name(std::string_view name);
// Names of signal types and shapes are matched case-insensitively, as RTS
// lower-cases them with CharLowerBuffW: ASCII and Cyrillic (UTF-8 in, UTF-8 out).
[[nodiscard]] std::string signal_name_key(std::string_view name);
[[nodiscard]] const char* to_string(SignalAspect aspect) noexcept;

struct SignalLightTexture {
    std::string name;
    std::string file;
    std::array<float, 4> uv{0.0f, 0.0f, 1.0f, 1.0f}; // u0 v0 u1 v1
};

struct SignalLightColour {
    std::string name;
    std::array<std::uint8_t, 4> argb{};
};

struct SignalLight {
    std::uint32_t index{};
    std::string colour;      // LightsTab entry name
    std::array<float, 3> position{};
    float radius{};
};

struct SignalDrawLight {
    std::uint32_t light{};
    bool flashing = false;
};

struct SignalDrawState {
    std::uint32_t index{};
    std::string name;
    std::vector<SignalDrawLight> lights;
    float semaphore_position = -1.0f; // SemaphorePos, -1 when absent
};

struct SignalAspectEntry {
    SignalAspect aspect = SignalAspect::stop;
    std::string draw_state;       // name
    int draw_state_index = -1;    // resolved against the type's draw states
    float speed = -1.0f;          // m/s, -1 = no limit (SpeedMPH / SpeedKPH)
};

struct SignalType {
    std::string name; // signal_name_key (scripts are matched case-insensitively)
    std::string display_name;
    SignalFunction function = SignalFunction::normal;
    std::string light_texture;
    bool semaphore = false; // SignalFlags ( SEMAPHORE )
    std::vector<SignalLight> lights;
    std::vector<SignalDrawState> draw_states;
    std::vector<SignalAspectEntry> aspects;
    std::uint32_t num_clear_ahead{};
    float flash_on{};  // SigFlashDuration, s
    float flash_off{};
    float semaphore_info = -1.0f;

    [[nodiscard]] int draw_state_index(std::string_view name) const;
};

enum class SignalSubObjectKind : std::uint8_t { signal_head, decor, number_plate, gradient_plate, user, other };

struct SignalSubObject {
    std::uint32_t index{};
    std::string name;
    std::string description;
    SignalSubObjectKind kind = SignalSubObjectKind::other;
    std::uint32_t user{};      // USERn
    std::string signal_type;   // SigSubSType, signal_name_key
    bool optional = false;
    bool default_on = false;
    bool back_facing = false;
    bool junction_link = false;
    std::vector<std::uint32_t> junction_links; // SigSubJnLinkIf
};

struct SignalShape {
    std::string file; // signal_name_key
    std::string description;
    std::vector<SignalSubObject> sub_objects;
};

struct SignalConfig {
    std::vector<SignalLightTexture> light_textures;
    std::vector<SignalLightColour> colours;
    std::vector<SignalType> types;
    std::vector<SignalShape> shapes;
    std::vector<std::string> script_files;
    bool shunting_auto_mode = false; // RTS_ShuntingAutoMode
    std::vector<std::string> warnings;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    [[nodiscard]] const SignalType* find_type(std::string_view name) const;
    [[nodiscard]] const SignalShape* find_shape(std::string_view file) const;
    [[nodiscard]] const SignalLightColour* find_colour(std::string_view name) const;
    [[nodiscard]] const SignalLightTexture* find_texture(std::string_view name) const;
};

[[nodiscard]] SignalConfig parse_signal_config(std::string_view bytes);
// ROUTES/<route>/sigcfg.dat.
[[nodiscard]] SignalConfig load_signal_config(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
