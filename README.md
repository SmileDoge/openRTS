# openRTS

Independent, modular RTS/MSTS-compatible implementation.  The project is
written in C++20 and uses CMake so the common layers can build on Windows and
Linux.

## Build

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The default renderer is platform-neutral and null. The optional WebGPU backend
uses an externally supplied `wgpu-native` SDK, so ordinary builds remain free
from graphics-SDK downloads. To enable it, configure with
`-DOPENRTS_ENABLE_WGPU=ON -DOPENRTS_WGPU_ROOT=<extracted-sdk-root>`.

For a Windows MSVC SDK, a headless upload check can be run without a window:

```powershell
cmake -S . -B build `
  -DOPENRTS_ENABLE_WGPU=ON `
  -DOPENRTS_WGPU_ROOT=C:\path\to\wgpu-native-sdk
cmake --build build --config Debug
.\build\viewer\Debug\openrts_viewer.exe --ace D:\Games\MDDRTS\GLOBAL\TEXTURES\ACleanTrack2.ace
```

`viewer --ace` decodes the supported structured ACE variant into CPU RGBA8,
creates a `RGBA8Unorm` WebGPU texture and uploads it. It has no window or
presentation surface yet.

The next viewer pass is a fullscreen textured triangle: it will sample the
uploaded ACE texture through a WebGPU sampler rather than retaining the clear
pass as final output.

When GLFW is enabled (the default), `openrts_viewer --window` opens a
cross-platform native window without creating an OpenGL context. The WebGPU
surface/presentation pass is the next layer on top of that window.

## Module boundaries

| Target | Responsibility | May depend on |
|---|---|---|
| `openrts_core` | errors, logging, paths, VFS, encodings, math | standard library |
| `openrts_formats` | RTS/MSTS parsers and schema readers | core |
| `openrts_assets` | content resolution and CPU assets | core, formats |
| `openrts_renderer` | renderer API and backends | core |
| `openrts_platform` | window, input, audio, timing abstraction | core |
| `openrts_simulation` | train and world simulation | core, formats |
| `openrts_compat` | future legacy Win32 DLL compatibility boundary | core |
| `openrts_viewer` | asset/shape viewer executable | assets, renderer, platform |
| `openrts_game` | future game host executable | assets, renderer, platform, simulation |

`viewer` and `game` are composition roots.  Lower layers must not depend on
them, on a concrete graphics API, or on a legacy DLL implementation.

## First content milestone

`openrts_asset_dump` resolves a relative asset name without writing to the
installation.  The current supported contexts are route-local shapes/textures,
global shape/texture fallbacks, and trainset-local assets.  Main locomotive DLL
lookup follows the statically observed RTS order: trainset-local first, then
`GLOBAL/LIB`.

```powershell
.\build\tools\asset-dump\Debug\openrts_asset_dump.exe `
  --root D:\Games\MDDRTS `
  --kind texture `
  --route SomeRoute `
  --asset SomeTexture.ace
```

It can also resolve a Shape Description and its primary model relative to the
located `.sd` file:

```powershell
.\build\tools\asset-dump\Debug\openrts_asset_dump.exe `
  --root D:\Games\MDDRTS `
  --sd 050.sd
```

The command probes the resolved `.s` envelope and reports whether it is text,
binary, or compressed binary.  Texture references are intentionally not guessed
for malformed or unsupported shapes. For recognised compressed binary shapes,
the bounded image table is parsed and each texture name is resolved through the
same content context.

Compressed narrow binary `.s` files are inflated with zlib after validating the
SIMIS declared output size. CMake first uses a system `ZLIB`; otherwise it fetches
the pinned upstream zlib `v1.3.1` during configuration.
