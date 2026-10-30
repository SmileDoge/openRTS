# RTS terrain and environment selection

Status: real-content checks on Kuban_RTS plus static analysis of `rts1.exe`
(`0x422648`, `0x4224F8`) and `GLOBAL/SHADERS/terrain.*`. Implemented in
`formats/src/terrain.cpp` and `renderer/src/terrain_renderer.cpp`.

## Tile files

### CONFIRMED (content)

- `TILES/<name>.t` (binary `JINX0t6b`) + `<name>_y.raw` (u16 heights).
  `<name>` is a 15-level quadtree path over [-16384, 16384) tiles, two levels
  per hex digit; verified for all 9 tiles around the Kuban start.
- Height = `floor + scale · y`; 256 samples per side, 8 m apart.
- Orientation from edge continuity over hilly tiles: columns grow east (+x),
  rows grow south (−z); row 0 matches the north neighbour's row 255, column 255
  the east neighbour's column 0.
- 16×16 patches of 16 samples. Patch texture coordinates use patch-local
  sample indices: `u = X + W·j + B·i`, `v = Y + C·j + H·i` (Kuban: `X = Y ≈
  0.002`, `W = H ≈ 0.0623`, i.e. one texture per patch with a half-texel inset).
- Terrain shaders: `DetailTerrain` (texture + microtexture), `AlphaTerrain`
  (texture only). Texture names are UTF-16 and may be Cyrillic.

### PROBABLE

- Normals are computed from heights; `_n.raw` (65536 bytes) and `_e.raw` are
  not interpreted.
- AlphaTerrain draws with a neutral microtexture (the RTS shader multiplies by
  a microtexture and 2; what RTS binds for it was not traced).
- Patch flags are ignored (no hidden patches seen).

## Triangulation (`0x49D228`)

### CONFIRMED

At full detail RTS fans each 2×2 sample block around its centre (odd row,
odd column) sample. Quad diagonals therefore alternate in a checkerboard and
always pass through the odd-odd sample: the quad at (row, column) is split
along (row, column)–(row+1, column+1) when row and column have the same
parity, otherwise along the other diagonal. Vertex normals come from a
per-sample table (not yet decoded; openRTS computes them from heights).

### Content check (Kuban_RTS)

- Track object origins lie on the terrain (median difference 1 mm over ~3000
  objects): the grid is neither shifted nor flipped.
- Along straight track centre lines the diagonal choice makes no difference
  (track follows sample lines). About 6 % of centre-line points are > 20 cm
  under the terrain, concentrated in a few tiles (up to 89 % in one tile) and
  not at tile edges: terrain there was not lowered to the track bed. Track
  shapes carry no `ESD_Cover_Terrain`; whether RTS hides terrain there is
  UNKNOWN.

## Shader (`terrain.vert/frag`)

`colour = max(diffuse · clamp(−2·N·L), ambient)`;
`texel = texture(uv) · microtexture(uv · 16) · 2`; fog by clip-space depth;
`texel.a == 0` writes black. Depth `LEQUAL` with writes, no blend.

## Seasonal textures (`0x422648`)

Season 0..3 = spring, summer, autumn, winter; weather 0 clear, 1 snow, 2 rain.

| Season | Snow | Search order under `TERRTEX\` |
|---|---|---|
| summer | any | (none) |
| spring | yes | `SPRINGSNOW\`, `SNOW\`, `SPRING\` |
| spring | no | `SPRING\` |
| autumn | yes | `AUTUMNSNOW\`, `SNOW\`, `AUTUMN\` |
| autumn | no | `AUTUMN\` |
| winter | yes | `WINTERSNOW\`, `SNOW\` |
| winter | no | `SNOW\` |

Fallback: `TERRTEX\<name>` (RTS also logs an error).

## Environment selection

The `.trk` `Environment` block maps `<Season><Weather>` (e.g. `WinterSnow`)
to an ENV file in `ENVFILES\`. The fog block computed by the sky (see
`sky.md`) is the same uniform block bound to the shape and terrain shaders,
so scene geometry fades to the sky colour (PROBABLE: buffer binding read from
decompiled code).

## Not ported

Terrain LOD / error-threshold selection (full detail within 2 tiles is drawn),
distant `LO_TILES`, additional light sources on terrain, dynamic terrain
(RTS-specific), terrain holes.
