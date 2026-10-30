# RTS shape rendering

Status: static analysis of `rts1.exe` (2021-08 build) in IDA plus the GLSL sources
RTS loads from `GLOBAL/SHADERS`. No runtime GL trace has been made. Implemented in
`renderer/src/rts_material.cpp` and the RTS shading path of
`renderer/src/model_renderer.cpp`.

Note: `GLOBAL/SHADERS/shape.*.shader` in this installation are dated 2025-03-10
and add normal mapping (tangent attributes 5/6). The exe never enables those
attributes, so that part is a local modification and is not ported.

## Shader names

### CONFIRMED

`NamedShader` uniform values (`0x45B1E0`, shape loader `0x43A944`):

| Name | Value |
|---|---:|
| `tex` | 1 |
| `texdiff` | 2 |
| `blendatex` | 3 |
| `blendatexdiff` | 4 |
| `gloss` | 5 |
| `addatex` | 6 |
| `addatexdiff` | 7 |
| anything else | 0 |

The shape loader only recognises 1..5; `addatex*` is used by cab close-ups and
environment layers.

Texture filter names map to GL filters: `linearmiplinear`/`miplinear` →
`LINEAR_MIPMAP_LINEAR`, `linear`, `nearest`, `linearmipnearest`/`mipnearest` →
`LINEAR_MIPMAP_NEAREST`, `nearestmipnearest`, `nearestmiplinear`, else none.
The value is applied to both MIN and MAG filter (MAG rejects mipmapped values).
The texture's mipmap LOD bias goes to `GL_TEXTURE_LOD_BIAS`.

## Records

### CONFIRMED

`prim_state` (44 bytes): `+0` flags, `+4` named shader value, `+8` texture
indices (≤2), `+16` z bias (parsed, unused by drawing), `+20` vtx_state,
`+24` alpha_test_mode, `+28` light_cfg, `+32` z_buf_mode, `+40` texture count.

`vtx_state` (20 bytes): `+0` flags, `+4` matrix, `+8` light material.

Vertex colours are the raw `u32` 0xAARRGGBB of the shape vertex record.

### PROBABLE

The GPU vertex (48 bytes, attributes 0..4: position, normal, uv, colour1 @40,
colour2 @44, `GL_UNSIGNED_BYTE` not normalised) carries those `u32` values as
stored, so the shader sees bytes in B, G, R, A order. The packing code itself
was not located.

## Per-primitive state (`0x4B042C`)

### CONFIRMED

- Primitive flags (`0x4ABB28`): bit 0 = blend shader (3/4) with
  `alpha_test_mode == 0`; bit 1 = `z_buf_mode == 3`. Model flags = OR over all
  prim_states.
- Blend shaders only: `alpha_test_mode != 0` → `GL_BLEND` off, alpha test on
  (`GL_GREATER 0.3`); otherwise blend on, alpha test off. All other shaders do
  not touch blend or alpha test: they inherit the previous primitive's state.
- `z_buf_mode == 3` → depth writes off for that primitive.
- `NamedShader` uniform is forced to 3 (unlit) when the light material is -11
  (HalfBright), the shader is 1, 3 or 6, prim_state flag `0x100` is set, an
  instance flag `0x40` is set, or it is night and the model has its night flag.
  Consequently the shader's own HalfBright branch is unreachable.
- `LightMatType` = vtx_state light material.
- No texture → texture 0 (black). Failed texture with `HideFailedTextures = 0`
  is drawn; a texture still loading skips the primitive.
- Blend shaders enable `GL_SAMPLE_ALPHA_TO_COVERAGE` when MSAA "+Alpha" is on.

State around object drawing: terrain disables blend and alpha test and leaves
`glDepthFunc(GL_LEQUAL)`; `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`.

## Passes (`0x408574`)

### CONFIRMED

Objects are visited for `pass = -2..3` in object-list order; nothing is sorted.
Object pass byte: statics 1, transfers -2, dyntrack 0; `0x4E0BFC` raises it to 2
when the model has translucent primitives (wagons are always 2), `-1` / `0` for
instance flags `0x40000` / `0x200000`.

Primitive filter by pass: 1 → models with any flags draw only unflagged
primitives; 2 → only translucent primitives; -1 → skip translucent; 0 → only
no-depth-write primitives; -2, 3 → all.

So a non-blended `z_buf_mode 3` primitive of an ordinary static is never drawn.

## Lighting

### CONFIRMED

`Light[0]` (std140, 192 bytes per light): `Diffuse`/`Ambient` from the
environment's colours, `Direction = (sin az·cos el, −sin el, −cos az·cos el)`,
`Type.x` = number of further lights. The ambient colour is raised component-wise
to the environment's `world_environment_ambient_light` (default: config
`DefaultAmbientLight`). Light[0] is driven by the time of day, see `sky.md`.
Further lights (`Light[1..]`) are vehicle/scene light sources and are binned
by distance into 10 UBOs.

## Not yet ported

Interior lights (`LightsInt`), the night/instance unlit flags, alpha to
coverage, mipmap generation (so LOD bias has no visible effect yet), passes
other than 1 and 2, light source binning.
