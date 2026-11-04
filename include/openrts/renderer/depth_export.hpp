#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

// A single-sample copy of the multisampled scene depth, the same values
// (reversed Z, depth.hpp), the size of the window. It is for tools that pick
// a depth buffer by the window's size and cannot take a multisampled one:
// ReShade's generic depth add-on (INSTALL.md, "ReShade"). The copy is drawn
// as sixteen bands, a draw each: that add-on ignores a depth buffer drawn
// with three vertices or fewer, and a frame whose only depth buffer had
// eight draws or fewer (the RTS path, which has no depth pre-pass).
class DepthExport {
public:
    explicit DepthExport(WGPUDevice device);
    ~DepthExport();
    DepthExport(const DepthExport&) = delete;
    DepthExport& operator=(const DepthExport&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // Copies sample 0 of `source` (a multisampled depth texture with
    // texture binding usage) into the export texture, made or remade at
    // `width` x `height`.
    void record(WGPUCommandEncoder encoder, WGPUTextureView source, std::uint32_t width, std::uint32_t height);
    // The copy (Depth32Float, render attachment and copy source), or null
    // before the first record().
    [[nodiscard]] WGPUTexture texture() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
