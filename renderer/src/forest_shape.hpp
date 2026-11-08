#pragma once

// MSTS / RTS forests: world `Forest` objects, trees RTS builds itself as a
// shape (rts1.exe 0x41B6D0 reads the object, 0x4B3C40 builds it). Internal
// to openrts_renderer.

#include <cstdint>
#include <optional>
#include <string>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/shape_binary.hpp>

namespace openrts::renderer {

// The path a forest is loaded by, as if it were a shape:
// `<world file stem>.<UiD>.forest` next to the world file.
[[nodiscard]] std::optional<core::VirtualPath> forest_shape_path(const core::VirtualPath& world_file, std::uint32_t uid);

struct ForestShape {
    formats::ShapeModel model; // one image: `texture`
    std::string texture;       // TreeTexture
    std::optional<core::VirtualPath> route_textures; // the route's TEXTURES
    std::string error;
};

// Reads the world file named by `path` and builds that forest as RTS does:
// Population trees, each two crossed quads of TreeSize standing on the
// terrain, placed by RTS's fixed pattern over Area (no random numbers;
// ScaleRange is not used), BlendATexDiff alpha-tested, lit as Circumference
// (-9: the normal up), the texture's mip bias -3, nothing past 1500 m. With
// openRTS's clearance on (forest_clearance.hpp), trees too near a track or
// road are left out.
[[nodiscard]] ForestShape build_forest_shape(const core::Vfs& vfs, const core::VirtualPath& path);

} // namespace openrts::renderer
