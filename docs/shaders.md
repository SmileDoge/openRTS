# The renderer's shaders

Since 2026-11-23 the renderer's WGSL lives in files of its own under
`renderer/shaders`, not in C++ strings.

## Where they are

| Folder | What |
|---|---|
| `common/` | pieces several shaders take: `light_shapes` (RTS's light shapes), `effects_struct` and `effects_main` (the new renderer's frame data and its functions: shadows, clusters, aerial perspective), `pbr`, `fog_ring` |
| `model/` | models: `interface` (bindings, the frame, the raindrops on the cab's glass), `rts` (the RTS path), `experimental` (the new renderer), `depth` |
| `terrain/` | the ground, the water, their depth |
| `sky/` | RTS's sky, openRTS's sky, the sky's light, lightning |
| `effects/` | GTAO, the light clusters, the sun shadows' cache, the sky's prefilter, light shafts |
| `post/` | tone map, bloom, auto exposure, depth export |
| `screens/` | the cab's screens |
| `world/`, `overlay/` | glow sprites, rain and snow, the occlusion probe; debug and wide lines |

A shader's name is its path there without `.wgsl`: `model/experimental`.
In the code: `detail::shader_code("model/experimental")`
(`renderer/src/shader_code.hpp`), the text with a zero at its end.

**Pieces, not whole modules.** A pipeline's module is still put together
in C++ from several pieces, as before: a model's is `common/light_shapes` +
`model/interface` + `common/fog_ring` (+ with the new renderer
`common/effects_struct` + `common/effects_main` + `common/pbr`) + `model/rts`
or `model/experimental`. So a piece alone is not a valid module, and an
editor's WGSL checker shows the names it takes from the others as unknown.

## How they get into the program

`cmake/embed_shaders.cmake` writes every `.wgsl` file into
`shader_sources.inc` (the renderer's build folder, `generated/`) as a byte
array, carriage returns left out; `renderer/src/shader_code.cpp` takes it
in. A changed, added or removed file remakes it at the next build (the
files are globbed with `CONFIGURE_DEPENDS`). The released program carries
them all: nothing to ship beside it.

`.wgsl` files are kept with LF endings (`.gitattributes`).

## Editing without a build

`--shader-dir <folder>` (the viewer), or the environment's
`OPENRTS_SHADER_DIR` (any program, the game's too): the renderer reads a
shader from that folder where it finds its file there, and takes the
built-in copy for the rest. Point it at the source tree's
`renderer/shaders` and an edited shader takes effect at the next start,
without building. The log says once that it reads from there.

Checked 2026-11-23: with the folder set to `renderer/shaders` the frame is
the built-in copy's; a folder holding only an edited `sky/rts_sky` changes
the sky and nothing else.

Not yet: picking an edit up while the program runs (the pipelines made
again).

## The move (2026-11-23)

The 39 strings were moved out word for word (a script), each with the
comment that stood above it in C++ now at the top of its file. Four
frames (the new renderer by day in rain from the cab, at night, in a
thunderstorm; the RTS path by day), `--paused`, Kuban, Krasnodar-1, are
the same before and after within a run's own noise (1-2 of 255; one pixel
of the thunderstorm's, its lightning).

## Direct3D 12: DXC, not FXC (2026-11-23)

wgpu turns the WGSL into HLSL on Direct3D 12 and compiles it with FXC unless
it is given Microsoft's DXC. FXC took 20 s for the models' pipelines on a
Ryzen 9800X3D and minutes on a Radeon RX 570's machine (the start menu
"not responding" at "starting the graphics"; it got there in the end).
Vulkan: 2.5 s, 0.1 s once its driver has them cached.

`dxcompiler.dll` and `dxil.dll` beside the program (`wgpu_context.cpp`,
`dxc_beside_program`) make Direct3D 12 use DXC: 1.0-2.2 s, the frames the
same within a few pixels' rounding. The release carries them (CI downloads
DirectXShaderCompiler v1.9.2609; CMake's `OPENRTS_DXC_ROOT` copies them
beside the programs), their terms in `licenses/dxc`, the GPL's additional
permission in `LICENSE-EXCEPTIONS.md`. The log says which:
`graphics: Direct3D 12's shaders by DXC` (or FXC), then
`graphics: the models' shaders ready in N s`.

wgpu keeps no pipeline cache on Direct3D 12; with DXC it is not needed.
