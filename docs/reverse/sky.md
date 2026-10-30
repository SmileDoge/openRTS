# RTS sky and time of day

Status: static analysis of `rts1.exe` (2021-08) plus `GLOBAL/SHADERS/sky.*` and
`satellite.*`. Implemented in `formats/src/environment.cpp`,
`renderer/src/rts_sky.cpp` (CPU) and `renderer/src/sky_renderer.cpp` (GPU).

## Environment loading (`0x55859C`)

### CONFIRMED

- Layer fade times: fade window kept only when `world_sky_layer_fadeout`
  exists. Normalised: fade-in start/end `-= 86400` when after fade-out start;
  fade-out end `+= 86400` when before fade-out start.
- Layer geometry: cap centre `(0, H, 0)`, ring of `nfaces` vertices at angle
  `-k·2π/n`: `(-R sin a, H, -R cos a)`, uv `tiles/2 · (sin a, cos a) + tiles/2`.
  Edge step `m` scales the previous ring by `r_m / r_{m-1}` at height `h_m`;
  its uv grows from the centre by `|h_{m-1} − h_m| / H + r_m / r_{m-1}`.
  Packed (`0x565CCC`) as a triangle fan (`n + 2`) plus one triangle strip per
  step (`2n + 2`, first pair repeated).
- `terrain_texslot ( name a b )`: `a == 2` clamps, else repeats.
- Satellites: rise time `-= 86400` when after set time. If satellite 0 has
  `world_sky_satellite_light ( 0 )`, every later satellite with light 1 is
  swapped into slot 0 (the last one wins). Slot 0 is the sun everywhere else.
- `world_environment_ambient_light` (hex), default `DefaultAmbientLight` from
  config, is the lower bound of `Light[0].Ambient`.

## Per frame (`0x417D74`)

### CONFIRMED

For each satellite, with `t` the time of day:

- twilight margin `m = |set − rise| / 10`, scaled by `(180 − rise_position)/90`
  when the camera tile Z ≥ 12384, else by `(rise_position − 180)/90`;
- visible when `rise − m ≤ t ≤ set + m`; the first satellite decides night
  (`t < rise || t > set`);
- `w = 1 − |culmination − t| / half_day`; azimuth from `rise_position` and `w`
  (two branches by tile Z); elevation `sin(w·π/2) · E`, where
  `E = (16384 − tile_z)·0.0225°` folded at 90°, `+22.5°` in summer,
  `−22.5°` in winter;
- scale `(low·(1−w) + high·w)/100`;
- ambient and direct colours mix the rise/high/set colours by weights from the
  twilight margin; channels ≥ 256 saturate, smaller ones are truncated to a byte.

Satellite 0's azimuth, elevation, ambient and direct colours become `Light[0]`
and its direction `(sin az·cos el, −sin el, −cos az·cos el)`.

Clear colour = sun direct colour. Sky fog: colour = sun direct colour (dimmed
when the sun is below −0.04°), start 300 m, end = max(500, last edge radius
of visible layers − 100).

Layers draw in order without depth test; satellites are drawn when the layer
index reaches `nlayers_behind_satellites + 1`. Layer alpha: fade-in
`1 − (end − t)/(end − start)`, fade-out `1 − (t − start)/(end − start)`, 0
when outside. Opaque shaders (`tex`, `texdiff`) blend only while fading;
`blendatex*`/`addatex*` always blend (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`). UV
scroll accumulates `scroll · dt` and wraps with `fmod 2`.

Satellites are quads at 2000 m, half size `100 · scale` (`· 1.5` for the
sun), coloured with their direct colour; the sun gets `DrawFlags 1` (edge
growth and fog in `satellite.vert`).

## Not ported

Night texture switching (`is_night` reloads route textures), the cab sky
background (`0x5656B0`), weather blending of a second environment
(`dword_61D644`), precipitation, water layers.
