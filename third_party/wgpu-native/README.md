# openRTS's wgpu-native patches

openRTS's own build of wgpu-native (VR, docs/vr.md: the game's `--vr` and
`tools/xr-probe`): the official wgpu-native v27.0.4.0 with:

- `0001-d3d11-shared-texture-import.patch`: `wgpuDeviceImportD3D11SharedTexture`
  (a wgpu texture over a D3D11 texture shared by an NT handle, through
  wgpu-hal's Vulkan `texture_from_d3d11_shared_handle`; on D3D12 the handle
  opened by `ID3D12Device::OpenSharedHandle`, taken by `texture_from_raw`) and
  `WGPUNativeFeature_VulkanExternalMemoryWin32` to ask the device for it.

Published as github.com/SmileDoge/wgpu-native (branch `openrts`, the patch
as a commit); its GitHub Actions build the release openRTS's CI downloads
(tag `v27.0.4.0-openrts.4`, `wgpu-windows-x86_64-msvc-release.zip`).

Building it yourself (Windows, Rust; libclang from Visual Studio's LLVM component):

    git clone --depth 1 --branch v27.0.4.0 --recurse-submodules https://github.com/gfx-rs/wgpu-native.git
    cd wgpu-native
    git apply <openRTS>/third_party/wgpu-native/0001-d3d11-shared-texture-import.patch
    set LIBCLANG_PATH=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin
    cargo build --release

Then a package as the official one's: `include/webgpu/` (`ffi/wgpu.h`,
`ffi/webgpu-headers/webgpu.h`) and `lib/` (`target/release/wgpu_native.dll`,
`wgpu_native.dll.lib`), given to CMake as `-DOPENRTS_WGPU_ROOT=<package>` (the game
with VR; a separate build folder) or `-DOPENRTS_XR_WGPU_ROOT=<package>`
(the probe only). Without it the game builds with the official wgpu-native
and no VR.
