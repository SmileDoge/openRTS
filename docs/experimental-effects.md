# Experimental path: shadows, every lamp, ambient occlusion, haze

RTS never had these. They belong to the experimental shading path
(`ShadingPath::experimental`); the RTS-compatible path does not use them and
draws as RTS does. The code is `renderer::SceneEffects`
(`include/openrts/renderer/scene_effects.hpp`, `renderer/src/scene_effects.cpp`),
read by the model and terrain shaders as bind group 2.

## The frame

1. The scene submits its models (`ModelRenderer::begin_frame`, `submit`),
   then `ModelRenderer::prepare` builds the draws and uploads the frame data
   once for every pass that follows.
2. `SceneEffects::begin_frame`: the cascades, the light lists and the
   effects uniform for this camera.
3. `SceneEffects::record`, before the main pass:
   - each shadow cascade: a depth pass of the terrain and every model that
     writes depth and is not blended, the cab included;
   - the camera's depth pre-pass (single-sampled), GTAO from it, a
     depth-aware blur;
   - the light clusters (a compute pass).
   Each depth pass culls what it draws by its own frustum: model instances
   by their bounding spheres (every slot has its own run of instance ids),
   terrain patches by theirs, neighbouring patches drawn in one call.
4. The main pass: the experimental shaders read the shadow map, the
   occlusion, the clusters and the haze parameters.

## Sun shadows

By default three cascades of a 2048 x 2048 depth array up to 250 m from the
camera (practical split, lambda 0.8: about 0-17 m, 17-45 m and 45-250 m; a
shadow map texel of about 1-2 cm, 3-4 cm and 15-20 cm). Each cascade is the bounding sphere of its
slice of the view seen orthographically from the sun, its origin snapped to
whole texels so the shadows do not swim as the camera moves, and 300 m more
towards the sun for casters out of view. Objects out of view within the
shadow distance are submitted as shadow casters only
(`ModelRenderer::submit_shadow_caster`). The depth passes are biased by the
slope; the lookup pushes the point off its surface by a texel (more on
slopes and with the softness) and filters 16 compares on a disc (a Vogel
disc turned per pixel), its radius the softness in texels. Shadows fade in with the sun's height
and strength and out over the last tenth of the distance; at night there
are none.

Settings (panel, under «Тени от солнца»; `--shadow-settings
distance,size,cascades,softness` for one run):
- «Дальность теней» 100-1000 m (`shadow_distance`);
- «Качество теней»: each cascade's map 1024, 2048 or 4096 texels a side
  (`shadow_quality`; 4096 is 64 MB a cascade);
- «Каскады теней» 2-4 (`shadow_cascades`);
- «Мягкость теней» 0.5-4 texels (`shadow_softness`).

Measured as below: off 2.57 ms, 250 m / 2048 / 3 / 1.0 3.83 ms, 100 m /
1024 / 2 2.99 ms, 1000 m / 4096 / 4 4.28 ms; the softness costs nothing.

## Every lamp (clustered lights)

RTS keeps nine further lights a ring (shape-rendering.md, Light rings). Here
every light the scene adds (`LightRings::all`: the trains' lights and the
burning scenery lamps the RTS selection keeps) is clustered: a 16 x 9 grid
of screen tiles by 24 slices of log depth (0.1 m to the far plane), each
cluster holding up to 64 lights whose sphere (the light's own reach and
spread) reaches it, nearest first; at most 512 lights. A fragment loops over
its cluster's lights with the same light shapes as the rings (lights.glsl,
or the physical falloff). The cab's own lamps stay as they are.

Two settings widen what lights the scene:
- «Дальность света» (`light_range`, `--light-range <m>`): scenery lamps
  light the scene while their object is this near instead of RTS's
  `MaxLightDistance` (400 m installed), each lamp's own `Distance` stretched
  in the same proportion; 0 keeps RTS's. Only lamps defined as light sources
  (`ESD_LightSources`) count: the installed routes have 194; most station
  floodlight masts are shapes with a lit night texture and light nothing,
  in RTS too.
- «Дальность прожекторов поезда» (`headlight_reach`, `--headlight-reach
  <factor>`, x1..x5): the trains' headlights and searchlights reach that
  many times their `.eng` `Radius` (the ED9M's 100 m), the beam's shape and
  fall-off stretched with it.

## Ambient occlusion (GTAO)

Ground-truth ambient occlusion (Jimenez et al. 2016, laid out as XeGTAO):
from the depth pre-pass, normals from the depth, two slices a pixel with six
steps each way within 1.2 m, interleaved-gradient noise, then a 5 x 5 blur
that keeps to surfaces of about the same depth. A slice of a tilted surface
can see more than 1; the value is clamped only after the blur, where the
slices average out (clamping before darkened flat ground by several
percent). It darkens the sky (ambient) light only, not the sun or the lamps.

## Aerial perspective

Instead of RTS's linear fog: extinction by distance through a haze thinning
with height (scale height 1.5 km), a third of the light left at RTS's fog
end (the sky dome's edge); the sky's light (the environment's ambient
colour) scattered in, a Henyey-Greenstein (g 0.76) glow towards the sun;
over the last quarter of that distance it turns into RTS's fog colour so the
end of the land stays hidden.

## Settings

The viewer panel (experimental renderer): «Тени от солнца», «Все источники
света, кластеры», «Затенение углов, GTAO», «Атмосферная перспектива»,
remembered in `viewer_state.ini` (`effect_shadows`, `effect_clusters`,
`effect_ao`, `effect_aerial`), the two ranges above (`light_range`,
`headlight_reach`) and the ground bounce (`ground_bounce`, see below).
`--effects shadows,clusters,ao,aerial | none` sets them for one run; `show-ao` shows the occlusion itself as the colour.

## Cost

ED9M on Kuban, the camera outside, 1280 x 720, 4x MSAA, RTX 4070 Ti SUPER,
`--no-vsync` (frame times from `--capture`, which now prints them):

| | ms a frame |
|---|---|
| RTS path | 2.17 |
| experimental, no effects | 2.48 |
| + shadows | 3.21 |
| + GTAO | 3.00 |
| all four | 3.74 |

The clusters and the haze cost next to nothing. GTAO scales with the
resolution (four times the pixels at 2560 x 1440).

## Threads

The frame is limited by the CPU, not the GPU (the main pass takes about
0.6 ms of GPU time at 1280 x 720). Most of the effects' CPU cost was wgpu's
work when the command buffer is finished (tracking and translating every
pass): about 0.3-0.4 ms a cascade, all on one thread.

With «Многопоточная запись» (`parallel_recording`, `--parallel-recording
on|off`, on by default) each shadow cascade, and the depth pre-pass with GTAO
and the clusters, is recorded and finished in a command buffer of its own on
a worker thread (`renderer::TaskPool`, up to five threads that live as long
as the window), while the main thread records the main pass; the buffers go
in one `QueueSubmit`, the workers' first. What the depth passes use is made
before they start (`ModelRenderer::prepare`, `TerrainRenderer::prepare_depth`);
during them each writes only its own slot's data.

| | one thread | parallel |
|---|---|---|
| 250 m, 2048, 3 cascades | 3.88 ms | 2.87 ms |
| 1000 m, 4096, 4 cascades | 4.93 ms | 3.09 ms |

The frames are the same. The GPU still runs the passes one after another
(WebGPU has one queue): only the CPU side is shortened.

Then (2026-11-02):
- **Far cascades kept** («Кэш дальних каскадов», `shadow_cache`,
  `--shadow-cache on|off`, on): the last two cascades of 3 or 4 (the last of
  2) are drawn with a tenth more room and kept; each is drawn again every
  other frame (the two alternating), and at once when its map no longer
  covers this frame's slice, the sun moved or the map was made again.
  250 m / 2048 / 3: 2.78 -> 2.69 ms; 1000 m / 4096 / 4: 3.30 -> 2.97 ms.
- **Scene culling on the workers**: which instances are drawn or only cast
  a shadow is decided in chunks of 512 on the pool, each instance's model
  found once a set of tiles (`RouteStreamer::visible_generation`); switches,
  signal parts and the submission stay in order on the render thread.
  0.44 -> 0.37 ms.
- **Batching (`prepare`)**: the depth passes' draws only when there are
  depth passes, grouped by a hash map on a worker while the main draws are
  batched (each draw's own binding: a texture override does not reach a
  cut-out's shadow); the per-instance data in chunks on the pool, the normal
  matrix from the 3 x 3 part. Without effects 0.85 -> 0.45 ms, with them
  0.86 -> about 0.6 ms.
- **All cascades in one pass: not done.** Measured, an empty pass costs wgpu
  about 5 microseconds (40 of them added 0.19 ms): one pass instead of
  three or four would save some 20 microseconds and lose the parallel
  recording.

- **Trains culled** (`TrainView::submit`, both paths): a car out of the
  frustum is not drawn, only given as a shadow caster within the shadow
  range; one beyond the draw distance is left out; the engineer's car is
  always drawn. 2TE10M with 300 gondolas, 133 cars in view: the simulation
  0.11 ms, posing and submitting 0.16 ms; the gondolas drawn as instances
  (120 cars, 56 draw calls). Frames unchanged.

- **Many kinds of cars** (2026-11-07, a player's station on a server: 8
  trains, about 500 cars, 1550 instances in view; `perf:` lines, the capture's
  draw report, the same view and server snapshot opened offline):
  - The frame was CPU-bound: 6.5 ms of work, the GPU pass 2.6 ms. Most of
    it: `finish` (wgpu closing the pass) 2.24 ms, `prepare` 1.58, `cull`
    1.0, `models` 0.52.
  - **Not a matter of instancing.** Cars of a kind were already one call per
    part. The 4400 calls came from 200 to 290 kinds of models of some 20
    parts each, nearly every part with a material of its own (3211 parts,
    3001 materials at the nearest level): no merging by material is left.
  - **The cost was a bind group per part.** Each part had its own group (its
    texture and a uniform slice of its constants), set before every call.
    Now a model's part constants are a storage buffer (`array<Draw>`, 144
    bytes a part) and a group is per model, texture, filter and unlit
    (`RenderModel::Impl::bindings`). Each instance id is a pair (instance,
    part slot): the vertex shader reads the part, the fragment shader gets
    its slot as a flat input.
  - **Multi-draw indirect** («Multi-draw indirect» in Performance,
    `--multi-draw on|off`, the remembered `multi_draw`; needs the device's
    `IndirectFirstInstance`, asked for at start): a model's consecutive
    parts with one group are one `MultiDrawIndexedIndirect`, in their order
    (an opaque decal after its body). The main pass's arguments are written
    in `prepare`, each depth slot's in its own region in `render_depth`.
    4419 calls become 1793 commands.
  - Measured: `finish` 2.24 -> 1.2 ms, `models` 0.52 -> 0.34, the node
    matrices of animated instances (8950 a frame) once per node from the
    parent's and the normal matrix from the 3 x 3 part: `cull` 1.0 -> 0.85.
    About 1.5 ms less a frame. Multi-draw on or off made no difference
    there: the calls are cheap, the groups were not.
  - Frames unchanged: pixel for pixel within the run-to-run noise (Vulkan,
    both paths, day and night; DirectX 12 identical byte for byte, OpenGL
    4 pixels).
- **What `finish` was** (2026-11-07): the stage is now four, `end` (the
  main pass ended), `post` (the depth copy for ReShade, the HDR tone pass
  and the panel), `encode` (`wgpuCommandEncoderFinish`) and `join` (waiting
  for the workers' passes); each worker's pass is timed too (`perf:`, the
  capture's effects line). On the station: `end` 0.001 ms, `post` 0.02,
  `encode` 1.2-1.5, `join` 0.001. The workers are done long before (the
  cascades 0.3-0.8 ms each, the camera's pre-pass 1.4-1.6): nothing waits
  for the shadows. wgpu takes the main pass's commands when the command
  buffer is finished, on the render thread: that is the cost, some 1800
  commands with their state changes.
- **Render bundles for the main pass: not done.** The opaque draws in four
  bundles (three on a pool of their own, one on the render thread): `encode`
  1.2 -> 1.0 ms but `models` 0.35 -> 0.65 (waiting for them), about 0.2 ms
  in all, and a run hung (wgpu: bundles finished beside the shadow passes
  and their buffer writes on other threads). Fewer state changes (models'
  geometry in shared buffers, textures in arrays) would be the way.

At 1280 x 720 the frame with four 4096 cascades is now near what the GPU
needs; the RTS path takes about 2.3 ms.

### The cab's own shadow map

The nearest cascade spans some 18 m of the view: 26 mm a texel at 2048,
37 mm with four 4096 cascades over 1000 m, a staircase on the desk. In the
cab (`SceneEffects::set_focus`, after the scene's submit) one more layer of
the shadow array is fitted to a 4.5 m sphere around the engineer's place,
seen from the sun with the cab's forward as its up and not snapped, so its
texels stay put on the cab while the train runs straight; the sphere sits
in cab coordinates and follows the eye only once he has moved a metre (the
body's sway does not shake it). It is drawn every frame in depth slot 5
with every caster in its box (300 m more towards the sun, as the
cascades). A point is looked up in it first while it lies inside with the
filter's reach to spare, otherwise in the cascades; the filter is at least
4 mm wide (the sun's penumbra about a metre behind a window frame).
Texels: 4.4 mm at 2048, 2.2 mm at 4096. Cost: within the noise at 2048,
+0.36 ms with four 4096 cascades. Setting «Чёткие тени в кабине»
(`shadow_cab`, `--cab-shadows on|off`, on).

The shadow passes already draw both faces of every triangle (no culling):
MSTS models are often one-sided, and a wall turned away from the sun must
still cast.

Checked on ED9M at 10:00 against the cascades alone: 1.25 % of the frame
darker in 147 coherent patches (99 % of those pixels in patches over
4 px), unchanged with a four times larger offset off the surface, so new
shadows of thin parts, not acne.

### Lamps by day

RTS lays a lamp over the vertex lighting with max() in display space
(shape.frag); by day the environments' ambient is full (SummerClear
ffffffff), so a cab lamp hardly shows. Here lamps add in linear light to
an ambient cut to 35 % in full sun, and the sun's shadows leave little
else: a cab lamp made for the night painted every shadow its colour. Now
the lamps (the clustered and ring lamps, the cab's, on models and terrain)
are scaled by `1 - Effects.lamps.x`: with the brighter of the sun's and
the sky's linear luma L, daylight = smoothstep over L 0.1..0.6, the
strength 1 - daylight * (1 - lamps_in_daylight). Setting «Лампы днём»
(`lamps_daylight`, `--lamps-daylight`, 0.25). ED9M at noon, cab lamps on:
mean change of the frame +14.4 -> +7.0; at night unchanged.

### Lamp shadows

Up to eight lamps throw shadows (`lamp_shadows`,
«Тени от фонарей», `--lamp-shadows 0..8`, 3; four until 2026-11-07), chosen
in `SceneEffects::set_focus` after the scene's submit. The place they are
for:
- at the desk (in the chair, or risen at it): the cab sphere (4.5 m round
  the eye, moved on when he moves a metre away);
- on foot anywhere in a cab or salon shape (since 2026-11-08; a salon of
  another car, the ED9M head car's own salon, the cab away from the desk):
  the shape's box, at most 16 m, fixed while he walks. With the desk's
  sphere a 25 m car was shadowed 4.5 m round the eye only, the outside
  lamps lit the rest through its walls, and their shadows came in steps as
  he walked;
- in flight (`ViewerScene::lamp_shadow_focus`): along the view to the
  ground (the terrain, up to 200 m; looking up or level, 40 m), the sphere
  over that stretch from the camera: its middle at 0.6 of the reach, its
  radius half the reach + 15 m, at least 25 m. Low over a station it takes
  the near lamps and the far ones; from above it lies on the ground. Until
  2026-11-07 it was 20 m around a point 12 m ahead, so in flight the
  shadows ended in a ring just ahead;
- otherwise 20 m around a point 12 m ahead of the camera. Each lamp is
scored by its RTS light shape (lights.glsl: cone or box, spot exponent,
attenuation, no surface) at the focus's middle and 0.4 of its radius each
way, the best of the seven times its brightness: a weak lamp right above
the cab and a strong floodlight far away pointed at it both count, one
pointed elsewhere does not. Where its beam falls (half its reach, at most
40 m, along its axis) counts too when inside the sphere, less towards its
edge: in a big sphere the seven points may all miss a lamp lighting the
ground between them. A lamp kept from the last frame counts half as much
again, so two alike do not take turns. Its map (a layer of a
separate depth array, half the cascades' size, at least 1024):
- outside the sphere: a perspective from the lamp just around the sphere
  (near plane up to 1 m, past the lamp's own housing);
- inside it: along its own cone, at most 120 degrees (a sphere lamp there
  is left out, it would need six maps);
- a parallel (box) lamp: orthographic along its axis over the sphere.
Drawn in depth slots 6..13 (both renderers now have 14) with the shadow
casters, out of view ones within 80 m too. In the lamp loops of the model
and terrain shaders a lamp in that list is multiplied by
`effects_lamp_shadow` (8 filtered compares; outside its map: lit). The
shadow fades out over the last 6 % towards a map's edge and, in flight,
from 0.75 of the sphere's radius to its edge: no hard line where the
shadows end.

At Krasnodar-1 at night from above the train: five lamps light the 33 m
sphere, all five throw shadows (four before).

ED9M at night in the cab: a lamp 8 m above the cab shining down and a
floodlight 99 m away high up pointed at it were chosen; the cab no longer
lit through the roof (70 % of the frame darker, one patch, the same with a
four times larger offset off the surface). Two maps cost about 0.1 ms.

**Without the clustered lights (2026-11-03).**
- The lamps are still chosen the same way, from the scene's lamps nearest first.
- The RTS light rings the shaders then use number their lamps differently. So each shadow map also carries its lamp's key, the translation of its `to_light` matrix (`lamp_shadow_keys`).
- The ring loops of the model and terrain shaders find a lamp's map by that key (`effects_lamp_shadow_of`).
- ED9M at night in the cab with the clusters off: 2 lamps chosen, 74 % of the frame darker. The result is the same with the clusters on.

**Sphere lamps (2026-11-03, `openrts_asset_dump --lights all`).**
- The installed routes place about 10 600 lamps (`ESD_LightSources` of 115 `.sd` files), all `LIGHT_CONE`, with no sphere or box lamp.
- Sphere lamps occur only in rolling stock (166 in 33 cab and salon `.sd` files). Those light the inside through the interior loop, which takes no lamp shadows.
- So the six-map cube a sphere lamp inside the cab sphere would need is left out:
  - about 0.3 ms;
  - 24 MB at 1024².

**A lamp's own model (2026-11-08).** «Фонарь не отбрасывает тень от своей
модели» (`lamp_own_model`, on): a scenery lamp's shadow map leaves out the
instance the lamp belongs to; the lamp still lights it. Some shapes put the
light's place inside the lamp's body, which then threw its own shadow
over everything round. The scene tags each world instance
(`ModelRenderer::set_owner`, its place in the visible list + 1) and its
lamps with the same (`ModelExtraLight::owner`); `SceneEffects` keeps each
shadow's lamp's owner (`depth_slot_owner`), and `render_depth` skips the
instances so tagged in that slot.

**Rows of lamps.** A `LightArray` row (`reverse/shape-rendering.md`) is a
light per lamp on this path: each is clustered and chosen for shadows on
its own; before, the one row-light's map was taken from its first lamp for
every lamp of the row. A row is seen or not by its whole length (the RTS
path: one sphere over the row; here each lamp by its own place, and its
`Distance` too), and a shape's distance on this path is from its near
side: from the far end of `platf_fonar_9.s` (200 m) the row was dropped
with its first lamp out of view.

### Ground bounce

The sunlit ground throws light back up; without it the underframe and
bogies get only the sky's ambient, cut to 35 % in full sun, and look
black beside the lit sides. The model shader adds
`reflectance * (sun * max(to_sun.y, 0) + sky * (1 - 0.65 * sun_strength))
* (0.5 - 0.5 * n.y) * occlusion` to the lighting: the ground taken as a flat
plane lit by the sun and the (already cut) sky, all of it on a surface
turned down, half on one turned sideways, none on one turned up, darkened
by the GTAO term so gaps under the body stay dark. Not on DarkShade
materials (dark already by design). Reflectance `ground_bounce`
(«Отражённый свет земли», `--ground-bounce 0..0.5`, 0.2, about ballast
and grass; 0: off), sent as `Effects.lamps.y`, 0 on the RTS path. No
extra pass: a few instructions a pixel.

ED9M side on at the bogies, 12:00, mean luma 0..255: the band of the car
side and the bogies 73.0 -> 79.5 (0.2) -> 83.1 (0.35), the darkest tenth of
it (deep under the body) 14.9 -> 16.1 -> 17.0, the ground unchanged.
