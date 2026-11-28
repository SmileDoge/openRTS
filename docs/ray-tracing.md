# Ray tracing: the sun's shadows by rays

Status: in the game from v0.50.0 (2026-11-27), the experimental (PBR) path,
Direct3D 12 and Vulkan.

## What it takes

- openRTS's wgpu-native fork from `v27.0.4.0-openrts.6` (SmileDoge/wgpu-native,
  branch `openrts`): ray queries over wgpu 27's `EXPERIMENTAL_RAY_QUERY`.
  The C API (`ffi/wgpu.h`): `wgpuDeviceCreateBlas` / `wgpuDeviceCreateTlas`,
  `wgpuBlasRelease` / `wgpuTlasRelease`,
  `wgpuCommandEncoderBuildAccelerationStructures`, a TLAS bound through
  `WGPUBindGroupLayoutEntryAccelerationStructure` /
  `WGPUBindGroupEntryAccelerationStructure` chained on the entries, the
  buffer usage `WGPUBufferUsage_BlasInput`. The fork allows the device's
  experimental features, takes the ray queries' limits from the adapter
  and carries wgpu-hal 27.0.4 with D3D12's ray tracing tier taken as at
  least 1.1 (NVIDIA's drivers report 1.2; upstream's equality refused them).
- CMake finds the API in the fork's `wgpu.h` and defines
  `OPENRTS_HAS_RAY_QUERY`; the device asks for `WGPUNativeFeature_RayQuery`
  where the adapter has it (`renderer::ray_queries()`).
- Direct3D 12: Shader Model 6.5, so DXC (`dxcompiler.dll` beside the
  program, as the game ships it).
- A GPU with ray tracing: GeForce RTX, Radeon RX 6000 and later, Intel Arc.

`tools/rt-probe` (`openrts-rt-probe.exe [vulkan]`): one triangle in a BLAS,
a TLAS of it, a compute shader shooting a ray at it and one past it;
"ray queries work" when the first hits and the second misses.

## The scene

`renderer::detail::RayScene` (`include/openrts/renderer/ray_scene.hpp`): one
TLAS (65536 instances at most), built every frame on its own command buffer
at the end of `ModelRenderer::prepare` (`build_ray_scene`), before the frame's
passes.

- Models: a still instance (rest pose, nothing hidden) one TLAS instance
  of its distance level's BLAS: the level's opaque primitives as its
  geometries, their rest-pose matrices in a transform buffer. An animated
  or partly hidden instance: each primitive its own BLAS (vertices in the shared pages or the
  model's own buffers, `first_vertex` / `first_index` into them), made the
  first frame it is queued, 384 new ones a frame at most. Out: blended
  primitives, alpha-tested ones and blend shaders' (trees, fences, glass):
  the rays would need their textures; the shadow maps keep them. An
  instance's transform: the model's times the primitive's local matrix,
  the animated one when it has one (as `placement()` in `model/interface`),
  so trains, pantographs and doors move their shadows. Every queued
  instance is in it, the shadow casters out of view too.
- The ground: each terrain tile one BLAS of its whole mesh, built as the
  tile is uploaded; the tiles added at each build as they are then
  (`RayScene::set_adder`), placed by the route's origin.
- The TLAS is bound in the scene effects' group (binding 14) and built
  every frame, even empty (a bound TLAS must be built).

## The shadow

`effects/ray_shadow`, a compute pass after the camera's depth pre-pass (the
pre-pass runs whenever shadows by rays are on): from each pixel's depth the
world point and the surface's normal (the nearer neighbours on each axis),
one ray towards a point of the sun's disc (0.27 degrees, widened by the
shadow softness), another point each pixel and frame (the temporal AA
averages them); the first hit ends it. The point is pushed off its surface
by 2 cm plus 1 mm a metre from the eye. It writes the mask (R32F, the
frame's size); the lit shaders read it (`common/ray_shadow`, binding 14 of
the effects' group; `common/ray_shadow_off`'s 1 without ray queries). A
point before the pre-pass's surface (glass, a blended body) takes what is
behind it. The sun's shadow is the least of the cascades' and the mask's,
times the clouds'.

Until the mask (the first version) each lit fragment traced four rays
itself: every overdrawn fragment paid, and the heavier shader ran slower;
on Kuban's yard with 600 cars (the testers' 80 fps against 165 with the
maps) the main pass grew by 4.9 ms at 1280 x 720. The mask: 0.58 ms.

With shadows by rays the sun's cascades (`DepthPassKind::sun_shadow`) leave
out the primitives in the ray scene: the maps hold the cut-outs and the
ground. The lamps' maps and the cab's are as they were.

## The lamps' shadows by rays (2026-11-27)

The same pass traces three more rays: to the three lamps lighting the
pixel most of its cluster's lamps (`physical_light` times their colour's
luminance) that have no shadow map of their own
(SceneEffects::choose_lamp_shadows), picked in order, not at random (no
noise, no denoiser needed). The mask is RGBA32Uint, packed: r the light
(0..255) of the sun (bits 0-7) and the three lamps (8-15, 16-23, 24-31), g
the first two lamps' indices + 1 (16 bits each), b the third's; the blur
averages each lamp's light only among the pixels that traced the same lamp.
In the lit shaders `effects_lamp_shadow` takes a lamp's map when it has
one, else the mask's light when the pixel traced that lamp, else 1. So the
mapped lamps (near the camera or the cab) keep their maps, and up to three
others of each pixel, however far, throw their shadows; a lamp marked
ORTSNoShadow takes no map but still a ray. Checked on the yard with 600
cars at 01:10: the rays' pass (the sun's and the lamps') 0.74 ms.

## Cut-outs: the texture's alpha in the rays (2026-11-27)

A cut-out primitive (alpha test or a blend shader, writing depth) whose
texture is not solid on every triangle goes into the ray scene as a
non-opaque geometry: its texture's alpha a layer of one R8 array
(RayScene::mask_layer, 512 x 512, 256 layers at most; a layer a texture and
cut-off), its corners' UVs in
one buffer, and a table of (UVs' first, layer) by geometry, an instance's
first entry its TLAS custom data (entry 0: opaque, the terrain's). In
`effects/ray_shadow` the candidate loop interpolates the UV by the hit's
barycentrics and confirms the hit where the mask's alpha is over the
drawing's own cut-off (its table entry's bits 16-23: RTS's 0.3 for an
alpha-tested surface, 0.9 for a blended one, a material's `alpha_cutoff`;
`docs/pbr-materials.md`, "Cut-outs and the shadows by rays"), so a car's
ribs and gaps, ladders, fences and leaves cast what they are drawn. Glass, a
lamp's shade or a shadow drawn under a car (a texture never 0.9 opaque) is
not in the ray scene at all (`shadow_mode` auto), nor a surface a material
gives `shadow_mode: "none"`. The nearest distance level is traced always.
With the array or buffers full a cut-out falls back to: solid when nine
tenths of its triangles are over its cut-off.

Past 60 m from the eye the rays read no texture: a mask solid, a
transmission all of its colour (a shadow's holes there a pixel or so).

Transmission (`shadow_mode: "transmission"`, glass): its candidates are not
confirmed; the ray takes `mix(1, colour, alpha)` of each (the table entry's
z its colour, kind 1, its layer the texture's alpha as it is, alpha_layer),
an instance's geometry once, 4 at most (a fifth, or under 2 % left: blocked).
The mask's a holds the sun's colour through glass (each channel over the
strongest, 8 bits; white 0xFFFFFF), r the strongest; common/ray_shadow's
effects_ray_sun_tint gives it to the experimental renderer's, the ground's
and the water's sun light; a lamp takes the brightness. Tried and dropped
(2026-11-27, the share of the ray pass against the main pass): the opaque
first in a ray of their own and the cut-outs in another (1.70 against 1.30:
most rays meet nothing and went through the scene twice); far cut-outs
letting light by (1.82: a ray let by went on through every tree). Every
cut-out solid: 1.08.

A layer is solid or open (255 or 0) against its cut-off (coverage_mask):
each mask texel's share of its source texels over the cut-off, then as many
texels solid as the source has, the highest shares first. A larger texture
keeps its share of solid, a bar half a mask texel wide stays a line (a
little wider), a hole a mask texel wide stays open; finer than that (a
2048 texture's one-texel bar or hole at 512) cannot be told apart, only its
share kept (the crossings of a fine grating). The nearest texel before
dropped or filled such detail by where it fell. A texture of 512 or less is
as it was (tests/ray_mask_test.cpp).

Measured 2026-11-27 (RTX 4070 Ti SUPER, D3D12, 1280 x 720, no DLSS/TAA,
no frame limit, 70 s a run, two runs; the ray pass, ms): the station view
`Kuban_RTS_20261127_144827` 3.07 with the textures read, 2.60 with every
cut-out solid; the forest view `Kuban_RTS_20261127_forest` (rows of poplars,
firs) 1.05 against 0.98. Reading the alpha costs 0.5 ms at most, 0.07 in
the forest: opacity micromaps would win no more than that.

`OPENRTS_RAY_CUTOUT_LOG=1` prints each cut-out given a mask: its texture,
shader, alpha test, blend, cut-off (0..255), the share of its triangles over
it and its texture's alpha (over 0.3, at least 0.9, any, partial).

## Opacity micromaps (2026-11-27)

The fork from `v27.0.4.0-openrts.6` makes opacity micromaps on Vulkan
(`VK_EXT_opacity_micromap`) and D3D12 (DXR 1.2's opacity micromap arrays,
ray tracing tier 1.2; inline ray queries on shader model 6.9 with
`RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`, naga patched): `ffi/wgpu.h`'s
`wgpuDeviceSupportsOpacityMicromaps`, `wgpuDeviceCreateOpacityMicromap`,
`wgpuOpacityMicromapRelease` and `wgpuDeviceSetNextBlasOpacityMicromaps`
(the next BLAS created on the thread takes them, one a geometry).

A cut-out with a mask (renderer/opacity_micromap.hpp): each triangle cut
into 256 micro-triangles (level 4), each opaque or transparent where every
mask texel under it is, else unknown (the texture's edge). The hardware
takes the opaque and skips the transparent itself; only the unknown reach
the shader's candidate loop and its mask. Equal micromaps shared, a
triangle all one state a special index. Baked on a worker thread of its
own: the model's BLAS is made the frame its micromaps are done. Not for a
transmission (glass) or a solid geometry. `OPENRTS_OMM=0`: none, for a
comparison. Near the eye the shadows are as without; past 60 m the
transparent micro-triangles still let light by where the rays took every
cut-out as solid (tests/opacity_micromap_test.cpp: the micro-triangles of
the bird curve tile the triangle once each; an opaque or transparent one is
so all over).

Measured 2026-11-27 (no frame limit, `--fps-background`, 70 s a run; the
ray pass with / without micromaps, ms): station D3D12 3.05 / 3.16, Vulkan
2.95 / 3.10; forest D3D12 1.04 / 0.93, Vulkan 0.97 / 1.04. Within a run's
spread: on these views the alpha read is a small share of the pass (the
measurements before them were partly at 30 fps, an unfocused window's).

## Settings

"Shadows by rays" (Graphics, the launcher's graphics settings;
`shadow_rays`, off by default), greyed where the GPU has none.
`OPENRTS_RAY_SHADOWS=0`: off for one run (a check beside the maps).

Checked on 2026-11-27 (RTX 4070 Ti SUPER, Direct3D 12, 1280 x 720): on
Kuban beside a ВЛ80с at 15:00 and 19:00 the catenary's wires, the posts and
the markers cast their shadows on the sleepers and the grass, which the
maps lost or blurred; on the yard with 600 cars (a debug view and the
`600wags` situation) the ray shadows' pass 0.58 ms and the whole frame
10.2 ms against 10.9 with the maps alone (the cascades lighter); no WebGPU
error.

## Next

- The cut-outs in the rays (an any-hit test of the texture's alpha in the
  ray query's candidate loop).
- Ambient occlusion and the sky's light by rays; reflections; a denoiser
  (NVIDIA's DLSS Ray Reconstruction where there is one).
