# openRTS materials (PBR)

openRTS's experimental renderer lights surfaces physically (GGX, the sky's
own light, docs/experimental-effects.md). What a surface is for that light
comes from RTS's data alone unless an openRTS material says more: how rough
or smooth it is, whether it is metal, its occlusion, its small relief, what
glows by itself. Materials are JSON5 files in the VFS, so a mod adds them
without touching the content (PBR stage 4, 2026-11-17).

- No material files: everything looks as before (RTS's light materials,
  HiShine / LoShine shiny, Normal matte).
- The RTS renderer ("as RTS draws") never reads them.
- They take effect with the experimental renderer, HDR and PBR on.

## Where they are

| File | For | Format |
|---|---|---|
| `body.material.json5` beside `body.ace` | every model drawing that texture | `openrts-material/1` |
| `MATERIALS/body.material.json5`, `MATERIALS/<train>/body.material.json5` | every `body.ace` wherever it lies; below a folder, only in the train folders whose name holds it (`#`: any characters) | `openrts-material/1` |
| `loco.s.material.json5` beside `loco.s` | some parts of that shape | `openrts-shape-material/1` |

A texture's file comes first (one texture is often drawn by dozens of
models through alias folders: written once). A shape's file is for the
exceptions: a texture that needs another look on one model (the glass of a
texture shared with the body). For a night or season texture
(`TEXTURES/NIGHT/body.ace`) the base texture's file is read, then the
variant's own (`NIGHT/body.material.json5`) over it.

A mod puts them at the same path: `MODS_OPENRTS/<mod>/TRAINS/TRAINSET/LOCO/body.material.json5`.

**The MATERIALS folder** (2026-11-23, testers: snow masks without
touching the trains). A folder `MATERIALS` in the installation's root (or
a mod's) holds materials found by the texture's name, wherever the
texture lies: one file dresses every train drawing it (there are 18 ChS7
folders, each with its own `zdsLoco_chs7-6A.ace`).

- A file right in `MATERIALS` applies to every texture of its name.
- A file in a folder below applies only to textures whose train folder
  (the one in `TRAINS/TRAINSET`, else the texture's own) holds that
  folder's name, ignoring case: `MATERIALS/chs7/` for `mdd_CHS7-016` ..
  `mdd_CHS7-318`, `MATERIALS/mdd_vl10-/` for the VL10s but not the VL10u's
  (`mdd_vl10u-…`). With `#` the name is a pattern of the whole folder name,
  `#` any run of characters (`MATERIALS/mdd_CHS7-2#/`; Windows keeps `*` and
  `?` out of a folder's name). A common name (`kuz1.ace`) belongs in such a
  folder, not right in `MATERIALS`.
- Deeper folders are only for order; the top one decides.
- Maps in the file are found beside it, as anywhere.
- Layered as a texture's file: the general ones, then the folders', then
  the one beside the texture, each over the one before.
- The folder is listed again when the VFS changes (hot reload).

## How a part's material is made

Field by field, each layer over the one before:

1. RTS: the part's light material (HiShine roughness 0.35, LoShine 0.5,
   Normal and others 1 with a tenth of the reflection, Circumference and
   DarkShade no reflection), its texture, its alpha test and blending;
2. the texture's file (then its night or season variant's);
3. the shape's entries for the part, in their order;
4. the inspector's changes (F12), until saved.

A field not set by a layer comes from the one below. A number never drops
a map from below. A new map without its partner resets the partner to the
default (`orm` without `layout`: "orm"; `normal` without `normal_y`: "up"),
so a partner of another map is never carried over. A new `orm` drops the
single maps below it.

As in glTF, with a map the number is its factor: where the roughness,
metallic or specular number is still RTS's, a layer bringing a roughness or
metallic map makes it 1 (the map says how the surface shines). An emissive
map with no factor anywhere takes 1; without a map nothing glows.

## The texture's file

```json5
// body.material.json5, beside body.ace
{
  format: "openrts-material/1",
  orm: "body_orm.png",        // R occlusion, G roughness, B metallic
  normal: "body_n.png",
  normal_y: "up",             // "down": a map from the DirectX / Unreal presets
  // without maps, numbers only:
  // roughness: 0.45, metallic: 0,
  emissive: "body_e.png", emissive_strength: 2,
}
```

`sdk/materials/template.material.json5` has every key with a comment.

| Key | Values | What |
|---|---|---|
| `format` | `"openrts-material/1"` | required; another major version is not read |
| `roughness` | 0..1 | perceptual roughness, 0 a mirror; with a map its factor |
| `metallic` | 0..1 | 0 a dielectric (paint, glass, wood), 1 bare metal; with a map its factor |
| `specular` | 0..1 | the share of a dielectric's reflection (KHR_materials_specular) |
| `reflectance` | 0..1 | a dielectric's F0 (0.04: IOR 1.5) |
| `base_color_factor` | [r, g, b] or [r, g, b, a], 0..1 | times the texture, linear |
| `base_color` | a file | instead of the shape's texture (a repainted colour) |
| `orm` | a file | packed occlusion, roughness, metallic |
| `layout` | `"orm"`, `"mrao"`, `"hdrp_mask"` | the packed map's channels (below) |
| `ao_map`, `roughness_map`, `metallic_map` | a file each | one map each (its red), over orm's channel |
| `ao_strength` | 0..1 | how much the occlusion map darkens |
| `normal` | a file | tangent-space normal map |
| `normal_y` | `"up"`, `"down"` | the map's green: OpenGL / glTF / Blender up, DirectX / Unreal down |
| `normal_strength` | 0..2 | |
| `emissive` | a file | what glows by itself, gamma 2.2 |
| `emissive_factor` | [r, g, b], 0..1 | |
| `emissive_strength` | 0..100 | |
| `height` | a file | relief: white its top, black `height_scale` under it (below) |
| `height_scale` | 0..0.1 | the relief's depth, metres (0.01 default) |
| `height_reference` | 0..1 | the map's level the model's surface lies at (1 default: white) |
| `snow` | a file | snow lying on the model: its colour (gamma 2.2), alpha where it lies (below) |
| `alpha_mode`, `alpha_cutoff`, `double_sided`, `wet` | | read, not applied yet (a warning says so) |

Files are relative to the material file's folder (no absolute paths, no
drive letters), PNG or ACE. An unknown key, a value out of range or of the
wrong type: a warning in the log, the field left out. A file that does not
parse: an error in the log; if it was read well before in this run (hot
reload), that reading stays.

### Packed maps

Inside openRTS a packed map is always ORM; others are repacked when loaded:

| `layout` | R | G | B | A |
|---|---|---|---|---|
| `orm` (glTF, Blender, Substance "glTF PBR Metal Roughness") | occlusion | roughness | metallic | - |
| `mrao` | metallic | roughness | occlusion | - |
| `hdrp_mask` (Unity HDRP mask) | metallic | occlusion | detail mask (unused) | smoothness (roughness = 1 - smoothness) |

Trainz's PBR materials (`m.pbrmetal`, `m.pbrmetalmasked`) keep a `_p` map of
R metallic, G roughness, B occlusion: `orm: "body_p.png", layout: "mrao"`. Named as
`roughness_map` and `metallic_map` instead, both take its red (metallic, mostly
0) and the surface becomes a mirror; a roughness map whose red is 0 all over
while its other channels are not gets a warning in the log (the console, F2).

Substance Painter's "Unreal Engine 4 (Packed)" preset writes the same
channels as ORM (R occlusion, G roughness, B metallic) but its normal map is
DirectX's: `normal_y: "down"`. Check the preset's output channels in its
export window before relying on it.

Colour maps (`base_color`, `emissive`) are sRGB (taken as gamma 2.2, as the
ACE textures); ORM and normal maps are linear data.

### Height maps (relief, parallax)

A grey image in the same texture coordinates as the colour: white the
relief's top, black its bottom, `height_scale` metres apart (rivets
2-5 mm, ribs and panel seams 5-15 mm). The model's own surface lies at
the map's level `height_reference`: 1 (white) when the relief is cut into
the surface (seams, grooves), the base sheet's level when things stand out
of it (rivets, ribs; a map whose sheet is a dark grey of 28 has 0.11).
What is brighter than it stands out, what is darker sinks.

- openRTS shifts every map of the part (colour, ORM, normal, emission)
  where the eye's ray meets the relief (parallax occlusion mapping: 8 steps
  seen square on, up to 32 seen edgewise, the last two steps interpolated).
  The outline stays flat, and the relief casts no shadow of its own.
- It needs no tangents: a step on the surface is mapped to one in the image
  by the screen's derivatives, so mirrored images and parts without a normal
  map take it too.
- A cut-out part (alpha test: a body's windows) keeps its alpha where it is
  not shifted (the cut-out is the model's edge); glass (blended) takes no
  relief.
- Out to «Рельеф до» metres from the eye (30 by default), faded out over
  its last quarter; the Graphics setting «Рельеф по картам высот материалов
  (parallax, не как в RTS)» turns it off (`parallax`, `parallax_distance`;
  `--parallax on|off`).
- Stored in BC4 (one channel, an eighth of RGBA8) when the textures are
  compressed (`.h4` in the cache).
- A height map is the heights themselves: flat areas one grey, a rib a
  bright band as wide as the rib. An image of the details' outlines (an
  edge or curvature bake) lifts only the outlines and shows little.

### Snow maps (2026-11-23)

A map of the snow on the model, painted over its own texture's unwrap:
the snow's colour, and in alpha where it lies (255 the thickest, 0 none).
It is all a material needs; nothing else (no ORM) has to come with it:

```json5
// kuz1a.ace -> kuz1a.material.json5: every model drawing kuz1a.ace
{ format: "openrts-material/1", snow: "kuz1a_snow.png" }
```

Since the unwrap is each model's own, a map goes with the texture (or a
shape's entries, below), not with a kind of train. How much of it shows
(`ModelLighting::snow_cover`, 0..1) is the scene's: none in summer, 0.8 in
winter from the start, all of it after some 4 minutes of a snowfall,
gone in some 10 minutes after one outside winter. It grows from the
thickest snow (the alpha's highest) out. Off with "Rain and snow".

## The shape's file

```json5
// loco.s.material.json5, beside loco.s
{
  format: "openrts-shape-material/1",
  primitives: [
    { match: { name: "Glass_BlendATexDiff" }, roughness: 0.05, metallic: 0 },
    { match: { image: "body.ace", matrix: "BODY" }, orm: "body_orm.png" },
    { match: { index: 12, image: "lamp.ace" }, emissive: "lamp_e.png", emissive_strength: 4 },
    { match: { image: "bogie.ace", all: true }, roughness: 0.8 },
  ],
}
```

`match` selects parts (primitive states) of the shape; every field given
has to match:

| Field | What |
|---|---|
| `name` | the part's label in the shape (its `prim_state` block's name) |
| `image` | its texture's file name (no folders; case does not count) |
| `matrix` | the node it hangs on |
| `index` | its index among the shape's primitive states |
| `all` | `true`: every part that matches |

Matching more than one part applies to none (a warning names them) unless
`all: true` says so; matching none is a warning too. The steadiest choice:
the name when no other part has it (74 % of the installation's shapes name
every part, but only 54 % uniquely), else the image and the node, else the
index with the image. `openrts-shape export` writes each part's steadiest
`match` into the glTF; the inspector picks it the same way.

## Making a material, step by step

1. **Export the model.** `openrts-shape export TRAINS/TRAINSET/LOCO/loco.s --out loco_gltf`
   writes the nearest distance level as glTF 2.0: `loco.gltf`, `loco.bin`
   and the textures as PNG. A node per matrix with its name, a material per
   part with the texture as its base colour; each material's
   `extras.openrts` has the shape, the part's index, name, image, node,
   shader, light material and its `match`. MSTS's left-handed coordinates
   become glTF's right-handed ones (z negated, the triangles' order
   reversed); texture coordinates stay as they are (both put (0, 0) at the
   image's top left).
2. **Paint.** Import it in Substance Painter or Blender, take the original
   texture as the base colour, bake the occlusion, paint roughness and
   metallic. Keep the model's texture coordinates as they are.
3. **Export the maps** with the "glTF PBR Metal Roughness" preset (ORM,
   normal map green up) and name them after the texture: `body_orm.png`,
   `body_n.png`. Normal maps are tangent-space, MikkTSpace (the default of
   Blender and Substance): openRTS makes the same tangents.
4. **Write the material** from `sdk/materials/template.material.json5`
   beside the texture (a shape's file for exceptions).
5. **Check it in openRTS.** The experimental renderer with HDR and PBR;
   the graphics panel's "Show the material (a check)" shows the base colour,
   roughness, metallic, the normals after the map, the occlusion (the map's
   and GTAO's) or the emission as the colour (`--material-view base|roughness|metallic|normal|ao|emissive`
   for one run). Files are watched: saving a material or a map redraws the
   model. The inspector (F12, "PBR material (openRTS)") shows a part's
   material, where each field came from (RTS, texture, texture variant,
   shape, inspector) and what was wrong with the files.
6. **Make it a mod**: the files under their content paths in a folder,
   then `openrts-pack pack <folder> -o MODS_OPENRTS/<mod>.zip` (or the folder
   itself in `MODS_OPENRTS`), and the mod listed in `openrts.ini`.

`sdk/materials/demo-vl80s-606` is such a mod with numbers only: the
VL80s-606's body paint (roughness 0.42), roof (0.75), glass (0.05) and
bogies (0.8).

Typical values: paint 0.35-0.5, rusty iron about 0.7, rubber about 0.9,
glass about 0.05, a rail's rolled head about 0.2 with metallic 1. Maps of
2048 x 2048 for a locomotive's body are plenty; a map is uploaded as RGBA8
with its mip levels (about 5.3 MB at 1024 x 1024, 21 MB at 2048 x 2048).

Content is its authors': improve and share stock models only with their
authors' leave (MaDaDev's stock has its own licence). A material file
holds no part of the model or its textures, only names and numbers; maps
painted over a texture are a derivative of it.

## The inspector

F12, a part chosen, "PBR material (openRTS)": a part of the shape (its
index, texture, name and node), its material as drawn with each field's
source, the messages about its files, and sliders for roughness, metallic
and the reflection's share. A change shows at once; "Save the material to
the mod" writes the shape's file (`<shape>.material.json5`) into the
inspector's mod (`openrts_edits` by default). It is machine-written: the
entries already in the shape's file are kept, comments are not; a file of
your own elsewhere is never changed (the mod's copy wins over it).

## How it is drawn

- The part's numbers and factors go to the GPU in its draw's uniform
  (224 bytes, was 160); its maps are bound beside its texture (bindings 3-6
  of the model's group), 1 x 1 stand-ins without (ORM white, the normal
  flat, the emission black, the height white), read only where the part has
  them.
- Occlusion from the map darkens indirect light only (the sky, the bounce,
  the light in the cab), together with GTAO by the darker of the two (not
  both: the same corners twice); the sky's reflection takes it as specular
  occlusion. The sun and the lamps are not darkened by it.
- Normal maps need tangents: openRTS makes them with MikkTSpace (the
  reference implementation, as Blender and Substance bake), only for the
  models with a normal map, in a vertex buffer of their own with a second
  pipeline (other models' vertices stay 40 bytes). The texture coordinates
  are taken with v up, so the bitangent points up the image as the map's
  green does; a vertex shared by triangles of both hands (a mirrored UV
  seam) is split. Triangles without texture area get no normal map (a
  message counts them).
- Hot reload: the material files and maps are the model's dependencies.

## The ground (TERRTEX, 2026-11-25)

A ground texture takes a material file as a model's texture does, by its
name: `ROUTES/<route>/TERRTEX/grass.material.json5` beside `grass.ace`,
over it the season's (`TERRTEX/SNOW/grass.material.json5` when the snow's
texture is drawn), under them `MATERIALS/grass.material.json5`.

- Read: `normal` (`normal_y`, `normal_strength`), the ORM maps (`orm` and
  its `layout`, or `ao_map`, `roughness_map`, `metallic_map`; occlusion by
  `ao_strength`), `roughness`, `metallic`, `reflectance`, `specular`.
- The maps lie over the ground texture as it does (the same coordinates;
  the microtexture is not under them).
- The ground's own is Lambert alone (no reflection). A file that does not
  give `specular` makes it 1: the surface reflects as its roughness says.
- Drawn by openRTS's lighting (the experimental path); RTS's lighting draws
  the ground as before. The normal map's frame comes from the screen (the
  terrain has no tangents).
- Not read for the ground: `base_color`, `emissive`, `height`, `snow`.
- Read once, when the texture is first drawn (no hot reload).

## Not yet

- `alpha_mode`, `alpha_cutoff`, `double_sided` (the pipelines are RTS's).
- Wetness (`wet`): the rain's darkening and gloss.
- DDS maps (an ACE of openRTS's block formats is read, `docs/reverse/ace.md`).
- Materials of transfers and forests; the ground's height and snow maps.
- glTF back into a shape (export only).
