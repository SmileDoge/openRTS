#pragma once

// MSTS / RTS transfers: a texture laid on the ground (world `Transfer`
// objects). RTS builds each as a shape of its own (rts1.exe 0x4B4E60) and
// draws it in object pass -2. Internal to openrts_renderer.

#include <cstdint>
#include <optional>
#include <string>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/shape_binary.hpp>

namespace openrts::renderer {

// The path a transfer is loaded by, as if it were a shape:
// `<world file stem>.<UiD>.transfer` next to the world file.
[[nodiscard]] std::optional<core::VirtualPath> transfer_shape_path(const core::VirtualPath& world_file, std::uint32_t uid);

struct TransferShape {
    formats::ShapeModel model;          // one image: `texture`
    std::string texture;                // FileName, in the route's TEXTURES
    std::optional<core::VirtualPath> texture_folder;
    std::string error;
};

// Reads the world file named by `path` and builds that transfer: a grid on
// the terrain (4 m cells; 4 % of the width above 100 m wide), BlendATexDiff,
// lit as an ordinary surface (-5), normals up, the texture's mip bias -3;
// a single quad from 300 m, nothing past 2000 m.
[[nodiscard]] TransferShape build_transfer_shape(const core::Vfs& vfs, const core::VirtualPath& path);

} // namespace openrts::renderer
