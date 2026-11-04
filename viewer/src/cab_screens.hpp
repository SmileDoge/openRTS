#pragma once

// SCREEN displays of a 3-D cab (КЛУБ, САВПЭ, ИТП ...): RTS draws their
// sub-objects as textured quads on the screen's plane, from the .sd and the
// states, values and texts a DLL writes (RTS 0x46B668, frames 0x52AAD4;
// docs/reverse/cab-screens.md). This lays them out; the renderer draws them.

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/dllhost/protocol.hpp>
#include <openrts/formats/cab_description.hpp>

namespace openrts::viewer {

struct ScreenVertex {
    glm::vec3 position{};              // cab shape coordinates (or scene, once placed)
    glm::vec2 uv{};
    std::uint32_t colour = 0xFFFFFFFFu; // RGBA8, 0xAABBGGRR
};

// Triangles with one texture.
struct ScreenBatch {
    core::VirtualPath texture;
    bool blend = true; // BlendATex / AddATex (RTS enables blending for shaders 3, 4, 6, 7)
    std::vector<ScreenVertex> vertices;
};

class CabScreens {
public:
    // `cab_file`: the cab shape; texture names are relative to its folder.
    CabScreens(const formats::CabDescription& cab, const core::VirtualPath& cab_file);

    [[nodiscard]] bool empty() const noexcept { return screens_.empty(); }
    // `state`: the DLL's output for the cab's car; null: the .sd's initial
    // states. `elapsed` drives blinking and the smoothed arrows and bars.
    // `blinked_off`: by display index, the display is in the hidden half of
    // its DisplayFade blink (RTS 0x525DB0); null: none.
    void build(const dllhost::CarOutput* state, float elapsed, std::vector<ScreenBatch>& out,
               const std::vector<bool>* blinked_off = nullptr);

private:
    struct Object {
        std::vector<core::VirtualPath> textures;
        std::array<float, 4> area{}, uv{}; // as the frames last left them (RTS keeps them in the object)
        std::array<float, 4> dll_area{};   // the DLL's Area last seen
        std::uint32_t texture{};           // CurrentTex
        float frame{};                     // ARROW / BAR: the value shown, moving to Value
        float blink_clock{};
    };
    struct Screen {
        std::size_t display{};             // in the description's displays
        glm::vec3 top_left{}, top_right{}, bottom_right{};
        const formats::CabScreen* source{}; // its own objects, or the ReferenceScreen's
        std::vector<Object> objects;
    };
    const formats::CabDescription* cab_;
    std::vector<Screen> screens_;
};

} // namespace openrts::viewer
