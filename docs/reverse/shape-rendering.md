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

## Textures and mipmaps — CONFIRMED (2026-11-02)

The ACE loader (`0x44F554`) returns a list of levels (`u32` texel count,
width, height, RGBA8 texels) and a mode; the upload (`0x44150C`) creates the
texture with `GL_LINEAR_MIPMAP_LINEAR` (set for MIN and MAG; MAG rejects it
and stays linear), `GL_REPEAT`, `GL_GENERATE_MIPMAP` 0, and then:

- options 17 or 21 (DXT1 with mipmaps): the file's levels, uploaded with
  `glCompressedTexImage2D`;
- options exactly 5 (raw channels with mipmaps): the file's levels, read in
  order after the whole scanline table (`8 * height - 4` bytes; the table's
  offsets are not used), uploaded with `glTexImage2D` level by level
  (progressively, smallest first, `GL_TEXTURE_BASE_LEVEL` following);
- anything else (options 0, 1, 13 …): level 0 only, and
  `gluBuild2DMipmaps` builds the rest (2x2 box filter).

So every texture is mipmapped. The loader allocates a full chain only for
square textures. In this installation (2685 ACE files of Kuban_RTS, the ED9M
and GLOBAL) 1737 have options 5, 645 options 17 / 21, the rest 0, 1 or 13;
every square options-5 file holds its full chain, rows in order. DXT1 files
hold DXT1 levels down to 4x4; their 2x2 and 1x1 entries are not DXT1 blocks
(`7carfloat.ace` ends in a single RGB texel).

openRTS (`formats::decode_ace_rgba`): the file's levels under the same rule
for square textures (DXT1 down to 4x4, the last two built from it), else
levels built with the same box filter; the textures of models, terrain and
sky are uploaded with their chain. Primitives with the filter `linear` or
`nearest` read only the base level, as GL does.

## Records

### CONFIRMED

`prim_state` (44 bytes): `+0` flags, `+4` named shader value, `+8` texture
indices (≤2), `+16` z bias (parsed, unused by drawing), `+20` vtx_state,
`+24` alpha_test_mode, `+28` light_cfg, `+32` z_buf_mode, `+40` texture count.

`vtx_state` (20 bytes): `+0` flags, `+4` matrix, `+8` light material.

**Matrix 0 is never applied** (CONFIRMED, 2026-11-11). `0x4AC5A4` builds the
nodes' world matrices (shape `+88`, 64 bytes each, only when there is more
than one matrix) with `0x4ACDF8`: for each node, its matrix times its
parents' going up the hierarchy while the index is above 0, so matrix 0 and
anything above it are left out of every chain. The draws (`0x4B1664`,
`0x4B042C`, `0x4ADA94`) multiply a primitive's matrix (or its animated one)
only when the vtx_state's matrix index is not 0. TSRE5 draws shapes so too.
In the installed content 765 shapes have a matrix 0 other than the identity
(484 in routes, 268 in GLOBAL, 13 in TRAINSET: interiors moved 0.5–0.8 m,
freight loads); Kievskij hod's `pineline6.s` has MAIN at (0, 13.5, 0) over
trees from y 0. openRTS: `parse_binary_shape_model` makes matrix 0 the
identity with no parent (its name kept), and an animation does not move it
(before, the tree line hung 13.5 m above the ground).

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
- Blend shaders (3 / 4): `alpha_test_mode != 0` → `GL_BLEND` off, alpha test
  on (`GL_GREATER 0.3`); otherwise blend on (`GL_SRC_ALPHA`,
  `GL_ONE_MINUS_SRC_ALPHA`), alpha test off. Every other shader draws with
  blend and alpha test off (`0x4B0EDC`); nothing carries over between
  primitives. The function's last argument, bit 0, keeps the alpha test as
  the caller set it; the object passes call it with 0 (`0x40BB90`). (Earlier
  notes here said other shaders inherit the previous primitive's state; the
  disassembly says otherwise.)
- `z_buf_mode == 3` → depth writes off for that primitive.
- `NamedShader` uniform is forced to 3 (unlit) when the light material is -11
  (HalfBright), the shader is 1, 3 or 6, prim_state flag `0x100` is set, an
  instance flag `0x40` is set, or it is night and the model has its night flag.
  Consequently the shader's own HalfBright branch is unreachable.
- `LightMatType` = vtx_state light material.
- No texture → texture 0 (black). Failed texture with `HideFailedTextures = 0`
  is drawn; a texture still loading skips the primitive.
- Blend shaders enable `GL_SAMPLE_ALPHA_TO_COVERAGE` when the config's
  `Antialiasing` has "+Alpha" (`byte_61D5D9`, `0x4B0E6A`, off again after the
  draw at `0x4B154D`; the same in `0x4BEDA0`), before the alpha-test check:
  blended primitives get it as well as alpha-tested ones. A texel's alpha is
  then its coverage too, so nearly transparent texels of a blended primitive
  write neither colour nor depth. The ED9M's cab counts on it: the needle
  shadow quads (`ST_KVT`, `ST_TMT` …, `s.ace`, BlendATexDiff, mostly alpha
  < 10) lie 0.4–0.8 mm in front of the panel backlight overlay `PRIBL`
  (`cabyessv9M.ace`, a LAMP display); without coverage their transparent
  part writes depth and `PRIBL`, drawn after `ST_KVT`, shows a hole with the
  unlit dial in it.

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

**CONFIRMED** (2026-11-02, `0x41B6D0`, `0x4E0BFC`, `0x408574`, `0x4B042C`):
- The instance flags are the world object's `StaticFlags` (hex, object
  `+56`). `0x4E0BFC` sets the pass byte: 2 when the model has a translucent
  primitive, then -1 for `0x40000` and 0 for `0x200000`, in that order (the
  last wins). After OpenRails' names these bits are Terrain and Global; the
  installed routes have Global on about 80 000 objects (`00200180`, the
  track shapes) and Terrain on about 9 000.
- Pass 1 visits the objects of pass bytes 0, 1 and 2; passes -2, -1, 0 and 3
  only their own; pass 2 only 2. So a Terrain object is drawn in pass -1
  only and its translucent primitives never; a Global one draws its
  depthless primitives in pass 0 and the plain ones in pass 1, and nothing
  in pass 2.
- The primitive filter (`0x4B042C`, the pass byte argument): -1 skips
  translucent primitives; 0 keeps only `z_buf_mode 3` ones; 1, for a model
  with any flagged primitive, keeps only plain ones; 2 keeps translucent
  ones but not those that also skip depth on a Global object; -2 and 3 keep
  all.
- Transfers (world `Transfer`: `FileName` a texture, `Width` / `Height`,
  default 100 m) are built by `0x4B4E60` as a shape of their own, pass byte
  -2: a grid on the terrain of 4 m cells (when wider than 100 m, cells of 4 %
  of the width, `(int)(4 * 0.01 * Width)`), the same cell across; each
  vertex at the terrain's height under it (`0x49CD24`) less the object's;
  normals up, colours white; u along x from the west edge, v from the north
  edge; BlendATexDiff, alpha test 0, `z_buf_mode` 1, light material -5; the
  texture from the route's `TEXTURES` through `0x4224F8` with the value 1
  (`SNOW` in winter), `LINEAR_MIPMAP_LINEAR` with a mip bias of -3; two
  distance levels, the grid to 300 m and one quad of the corners to 2000 m.
  RTS samples the terrain with one diagonal per cell (not the drawn
  checkerboard) and gives up (height 0) outside the object's tile.
- The light loop (train lights, signal lights; `0x449818`) runs inside the
  pass loop after pass 2 (after RTS's shadow maps) and before pass 3: lights
  are drawn after the glass and before the cab.

openRTS: `rts::object_pass` / `rts::visits` / `rts::draws_in_pass`, the
instance's `StaticFlags` given by `ModelRenderer::set_world_object`; the RTS
path draws passes -2..3 in that order. Transfers load as shapes named
`<world file>.<UiD>.transfer` (`renderer/src/transfer_shape.cpp`). RTS's
grid crosses the terrain's triangles, so between its vertices it sank under
the ground or floated over it, and the two flickered: the ground's triangles
showed through in patches (Novokuznetsk, 2026-11-09). openRTS instead cuts the
rectangle by the terrain's own triangles (8 m samples, the checkerboard
diagonals, across tile edges), so every piece lies in its triangle's plane.
It draws the pieces at every distance to 2000 m and a ten-thousandth of the
distance nearer (`GpuInstance::decal`, reversed Z), for what the two
transforms' rounding leaves. Train and signal glows are drawn after the
translucent pass and before the cab, laid over by their alpha
(`GlowBlend::over`).

**CONFIRMED** the cab (2026-11-02): the cab object (`dword_61D68C + 208`)
gets pass byte 3 in the cab views (`0x438D50`; 2 in view 4), so it is drawn
after every other object, all of its primitives in the shape's order in one
pass (pass 3 filters none), `z_buf_mode 3` ones included. Its draw is the
same `0x4B042C` (`0x40BB90`, flags 0). openRTS: `ObjectPass::last` for the
cab, in both shading paths (the experimental path does not sort the cab's
blended primitives either).

**CONFIRMED** node state flag 0x01 (not drawn) leaves out the node's own
primitives only, its children still show (the same lookup); RTS sets it for
an unlit lamp's node (`0x514630`) and a cab signal light that is out
(`0x525DB0`). openRTS: `NodeAdjustment::hidden` for those two; a cab's KEY
switch with its key out and a LAMP display without animation keys still
hide their nodes with their children (how RTS hides them is not traced).

**PROBABLE** the textures of light glows (`LightTex`, signal lights) and cab
screens are mipmapped like every RTS texture (the glow loop of `0x408574`
leaves the loader's `GL_LINEAR_MIPMAP_LINEAR`); openRTS mipmaps them.

**CONFIRMED** node state flag 0x40 (unlit) of a lit `SOURCE_VISIBLE` lamp's
node (`0x514630`) acts on that node's own primitives only (the state is
looked up by the primitive's matrix, `0x4B042C`). openRTS: `NodeAdjustment::unlit`,
drawn with the unlit light material like the night bit.

## Lighting

### CONFIRMED

`Light[0]` (std140, 192 bytes per light): `Diffuse`/`Ambient` from the
environment's colours, `Direction = (sin az·cos el, −sin el, −cos az·cos el)`,
`Type.x` = number of further lights. The ambient colour is raised component-wise
to the environment's `world_environment_ambient_light` (default: config
`DefaultAmbientLight`). Light[0] is driven by the time of day, see `sky.md`.
Further lights (`Light[1..]`) are vehicle/scene light sources, in ten rings
around the camera (below, "Light rings").

## Light rings — CONFIRMED (2026-11-02)

Every frame the further lights go into ten UBOs (`off_5E0494`,
`dword_5E03D0` = 10), each `Shader.NumLights` lights (10 installed: the sun
first, then nine), its `Light[0].Type.x` the number of further ones:

- Ring k is centred (k + 1) * L / 10 from the camera and reaches L / 10 to
  each side, L = `MaxLightDistance` (`ViewDistance` when it is 0, the
  default; `0x408574`). A light whose reach (a sphere) overlaps a ring goes
  into it: |centre - distance| <= reach + L / 10.
- The trains' lights first (`0x4AF7D4`, the reach their own), then the
  scenery's (`0x4B009C`). A full ring gives its farthest light's place to a
  nearer one; a farther one stays out.
- Scenery lamps (`ESD_LightSources`, 140-byte records at the shape's +108,
  `0x408574`): updated (`0x4B0284`) only while the object is within L and,
  if the lamp has a `Distance`, within it. A burning lamp is ringed when its
  sphere is in the view frustum; the sphere is its `Position` and
  (max(rX, rZ) + `LightFade`) * max(1.1, start + length + 0.1) (`LightFade`
  added to a radius, sic).
- An object takes ring floor((distance - radius) / (`ViewDistance` / 10)),
  ring 0 when that is not positive or when ring 0 holds no light, the last
  ring beyond (`0x4B042C`, by its bounding sphere; the terrain's patches the
  same, `0x4A2AD4`). The two steps differ (40 m and 130 m with the installed
  config), so an object only sees the lights of a band nearer the camera:
  lamps much farther than about 100 m light nothing.

openRTS: `renderer::LightRings`, the same rule for models and terrain
patches, in both shading paths (the experimental path may get its own
selection later).

## The flashlight — CONFIRMED (2026-11-08)

- Num2 (`WM_KEYDOWN` 0x62, `0x403068`) toggles `[0x61D55D]`; a new game
  clears it (`0x4044B9`).
- While it is on (and the GL lights work, `[0x5E04C6]`), every frame
  (`0x409A2C`) the record at `0x5E03D4` (a 192-byte light, the shaders'
  `GLLightObj`) is copied into the frame's lights first and into the rings
  its reach covers, placed at the eye and turned by the camera's yaw and
  pitch: a spotlight along the view.
- The record (defaults `0x405F15`): colour 0.643 / 0.4745 / 0.1725 / 1,
  size 0.436 / 0.233 / 50 (x, y, reach) and 0, type 1, spot exponent 90,
  attenuation 2 / 0.2 / 0.8. `flashlight.type`, `.colour` (hex ARGB),
  `.radx`, `.rady`, `.radz`, `.attenuation` (`0x4070FB`) override them;
  no installed file has them.
- openRTS: `RouteScene::collect_lights`, Num2 (RTS keys) or Shift+`
  (openRTS keys).

## Lamp fades — CONFIRMED (2026-11-02, `0x4B0284`, `0x51C364`, `0x4AEBEC`)

`LightFade ( a b )` is parsed into the lamp record (`+44` a, `+48` b); only
`a` is used, for fading in and out alike. Each update moves the level
(`+52`) by the time step over `a` towards 1 while the lamp burns and towards
0 when it does not (bits 1 fading in, 2 fading out, 4 fully on); `a` = 0
switches at once. Scenery lamps are stepped by the frame function
`0x408574` with the main loop's milliseconds (clamped to 8..1000 ms), so
their `LightFade` is in milliseconds; a cab's or salon's lamps (`0x514630`)
get seconds. The record belongs to the shape: every instance of it within
range steps it again. A lamp is ringed while it burns or fades; its colour
goes to the shader (`0x4AEBEC`) as red x level, green x level twice, blue
unscaled, alpha x level (sic). No installed scenery lamp has a `LightFade`;
cab lamps mostly `( 0.1 0.0 )`.

openRTS: scenery lamps as above (the RTS path packs the colour as RTS, the
experimental path scales it); cab lamps fade both ways with the first value
(they used to go out with the second, at once).

## Not yet ported

Dynamic track (pass 0): no installed route has any.

## Distance levels (openRTS, 2026-11-01)

Shapes carry several distance levels (`dlevel_selection` per level: the
TestRoute signal heads 100 / 250 / 500 / 2000 m, the ВЛ10к 300 … 2000 m),
each a simpler mesh. openRTS loads all levels of the first `lod_control`
and draws, per instance, the first level whose selection distance reaches
the camera's distance to the model centre, else the last one. RTS draws the
level an object holds at +104 (`0x4B1664`); where RTS chooses it (another
thread, distance, FOV or a LOD bias) is not traced, so the rule is the MSTS
one — PROBABLE. The hierarchy of the first level is used for all levels.

## Output colour format

RTS draws into a plain GL framebuffer: the shaders' outputs go to the screen
as they are. The viewer picks a non-sRGB surface format (BGRA8 / RGBA8
Unorm); an sRGB surface, which Vulkan on NVIDIA offers first, encodes the
values again and made the whole picture brighter than RTS.

## Antialiasing and anisotropy

The installed RTS config has `Antialiasing = 4x MSAA +Alpha` and
`Anisotropy = 16`. **CONFIRMED** `Antialiasing` (read at `0x405D2C`, parsed by
`0x4BD624` into `0x61D5D0`): the leading number is the sample count; mode
(`0x61D5D8`) 1 MSAA, 2 when the value has "csaa" (4 coverage samples, 8 with
"xq"), 3 with "fxaa"; "+alpha" sets `0x61D5D9`, alpha-to-coverage for the
blend shaders (above). `GL_MULTISAMPLE` is on for modes 1 and 2. openRTS: 4x
MSAA by default (`--msaa 1|4`), alpha-to-coverage on every blend-shader
pipeline (alpha-tested or blended) when multisampled, and 16x anisotropic
filtering on samplers whose min, mag and mip filters are all linear (models,
terrain).

## Face culling — CONFIRMED (2026-11-01)

- `create_game_window` (`0x40564D`) enables `GL_CULL_FACE` once; RTS never
  calls `glFrontFace` or `glCullFace`, so OpenGL's defaults hold:
  counter-clockwise triangles are the front, back faces are culled.
- It is turned off only around the shadow map pass (`0x47BEB0`, `0x47C5DC`),
  one textured overlay (`0x456E8C`) and the shader effects (`0x4BEDA0`,
  `0x4BF3F8`, `0x4BF878`) and turned back on after each; shapes
  (`draw_elements_1`) are always drawn with back faces culled.
- openRTS: model pipelines cull back faces with a **clockwise** front. With
  a counter-clockwise front (the reasoning "one X mirror in each, wgpu's ccw
  is OpenGL's") the visible faces were culled in the viewer, so the faces
  RTS shows come out clockwise on screen here; the cause (RTS reordering
  indices on load, or wgpu's convention) is not traced — UNKNOWN. Without
  culling the coplanar front and back faces of two-sided parts (a signal
  head's back plate) fought in depth. The viewer panel's "Cull back faces
  (RTS)" turns it off for comparison.

## Night and seasonal textures, scenery lamps (openRTS, 2026-11-02)

- `ESD_Alternative_Texture ( value )` of a shape's .sd takes an image from a
  subfolder of the folder it is found in (Kuban_RTS: `TEXTURES\NIGHT`,
  `SNOW`, `WINTER`, `WINTERSNOW`). **CONFIRMED** (`0x4224F8`, a switch on the
  whole value): 0 none; 252 the season's - `WINTERSNOW\` / `WINTER\`,
  `AUTUMNSNOW\` / `AUTUMN\`, `SPRINGSNOW\` / `SPRING\` (snowing / not), none
  in summer; 256 `NIGHT\` at night; 257 `NIGHT\` at night, else `SNOW\`;
  any other value `SNOW\`. `SNOW\`: in winter, or when it snows outside
  summer (season `byte_61D62C`, weather `byte_61D62D`). One folder, the
  image's file name after it. When night changes, `0x42288C` reloads the
  textures of every shape with its flag (+115 bit 0) from `NIGHT\` + name,
  or back. An image missing there: openRTS takes the plain one (RTS's
  loader `0x439DE8` only records the path; the miss is not traced). Night is the sky's (the first satellite
  outside its rise / set times, sky.md). RTS reloads route textures when it
  changes (`is_night`); openRTS reloads, on a worker thread, the models
  whose folders change (`ModelLibrary::set_texture_variant`), and models
  streamed under another variant on arrival.
- **CONFIRMED** (`0x4B042C`, the shape draw): a primitive is drawn unlit
  (`glDisable(GL_LIGHTING)`, and with shaders `NamedShader` set to 3, whose
  `shape.vert` branch takes Ambient 1) when it is night and its shape has
  the night bit (`ESD_Alternative_Texture` 0x100: 256, 257) - every
  primitive of the shape, whatever its texture -, or its light material is
  HalfBright (-11), or its shader is 1 / 3 / 6, or its `prim_state` flags
  have bit 0x100, or the instance's state of its node has flag 0x40. So
  Kuban's `Fon_Rostov.s` shows its night city at full brightness, and
  HalfBright is full brightness in RTS. The shadow pass (`0x4B1664`, flag
  0x10) leaves the same primitives out.
  - **CONFIRMED** `prim_state` flags: the first field of a `prim_state` in
    the .s, read as hex (`0x43A944`, `wcstoul(.., 16)`); nothing else sets
    it. Bit 0x100 draws the primitive unlit. No shape of this installation
    sets it (Kuban_RTS, GLOBAL\SHAPES: 9706 shapes, the ED9M and its cabs:
    every `prim_state` has flags 0; `train-check --prim-flags <folder>`).
  - **CONFIRMED** node state flags (per instance, 120 bytes a node, found by
    the node's matrix): 0x01 / 0x20 not drawn, 0x80 its own colour
    (`glColor4fv`), 0x40 drawn unlit. 0x40 and 0x01 are set by the lamp
    updates (`0x514630` for the 136-byte `InternalLight`s of cabs and
    salons, `0x526EC0` likewise) on the node named as the lamp: a lamp with
    `SOURCE_VISIBLE` keeps its node and draws it unlit while it burns (0x40);
    without it the node shows only while it burns (0x01 when off).
- **CONFIRMED** light sources (`0x4B0284`: the scenery's and the cabs'
  `InternalLight`s; `0x526EC0`: the lamps of the cab's own .sd): `RUNNING`
  burns (`0x4B0284` sets the lit state from it; a DLL's `SwitchLight`
  sets it); `NIGHT_AUTO` burns at night, or, with `AutoWhenWorldLit` (+88,
  > 0), while the world is at most that lit: `dword_61D60C`, the sun's
  ambient and direct colours, red, green and blue averaged over the six,
  before the ambient is raised (`0x408C19`); `NIGHT_ONLY` never by day; a
  lamp with none of them does not burn. `0x526EC0` has no
  `AutoWhenWorldLit`. openRTS: the scenery lamps by this rule
  (`SkyState::world_lit`). The older note follows:
- Light sources (`0x4B0284`, `0x526EC0`): `NIGHT_ONLY` off by day;
  `NIGHT_AUTO` on at night, or when a darkness value (`dword_61D60C`) is at
  most the light's threshold (+88) if it has one.
- `ESD_LightSources ( n LightSource ( Position PointsTo Radius ( rX rZ )
  Colour Flags Distance Attenuation ( start length kind ) SpotExponent ) )`:
  a scenery lamp (Kuban_RTS: 21 lamp and platform shapes, `NIGHT_AUTO
  LIGHT_CONE`). openRTS lights the scene with it through the further lights
  of the shaders (lights.glsl; models and terrain): light space z towards
  PointsTo, a cone spreading rX / rZ at the PointsTo plane (`Size` =
  rX / reach), a sphere or a box taking them as half sizes - **PROBABLE**.
  `LightArray ( n dx dy dz )`: a row of n lamps, each the one before moved
  by (dx, dy, dz) in the shape's space; RTS's lights.glsl lights each point
  by the row's nearest lamp (the light's array count and step). The
  Novokuznetsk platform `platf_fonar_9.s` has two rows, 4 and 5 lamps
  50.118 m apart: nine lamps. openRTS read no `LightArray` until 2026-11-08,
  so a row lit with its first lamp only (two of nine there); now the RTS
  path packs the row as one light as RTS does, the experimental path every
  lamp as a light of its own (its clusters and shadows each lamp's).
  `NIGHT_AUTO` / `NIGHT_ONLY` lamps burn at night only (the RTS file
  reference), within `Distance` and RTS's config `MaxLightDistance` (400) of
  the camera (superseded: see "Light rings"). Which of many lights RTS gives each object is **UNKNOWN**
  (Shader.NumLights 10, the sun first): openRTS takes the train's beams and
  the lamps nearest the camera, nine in all, for the whole frame.
