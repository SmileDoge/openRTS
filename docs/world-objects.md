# World objects and effects RTS builds itself

Forests, level crossings, pickups, road traffic, water, rain and snow, and
diesel smoke: the parts of a route that are not plain shapes. Everything below
was read from `rts1.exe` (an IDA copy, never the installed file) and is
CONFIRMED unless it says PROBABLE.

## World objects

The world loader `0x41B6D0` gives each object a kind (object `+89`): `Static`
0, `Gantry` 1, `Speedpost` 2, `LevelCr` 3, `Signal` 4, `Pickup` 5, `Telepole`
6, `TrackObj` 8, `Platform` 9, `Siding` 10, `Hazard` 11, `CarSpawner` 12,
`Forest` 13, `Dyntrack` 14, `CollideObject`, `Transfer`. openRTS reads
`Forest`, `LevelCr`, `Pickup` and `CarSpawner` besides the earlier ones
(`formats::WorldObjectKind`); `Hazard` (deer) and `Dyntrack` are not done.

Across the installed routes:

| Route | Forests | Level crossings | Car spawners | Pickups | Water patches |
|---|---|---|---|---|---|
| Kuban | 184 | 25 | 187 | 0 | 1306 |
| Novokuznetsk | 6271 | 149 | 218 | 0 | 3079 |
| Kievskij hod | 1482 | 80 | 148 | 0 | 430 |
| Chernyshevsk | 0 | 160 | 88 | 0 | 7547 |
| Pavlovsk | 0 | 66 | 141 | 0 | 3546 |
| Uspensk | 8 | 30 | 121 | 39 | 2790 |

## Forests

`Forest ( TreeTexture Area ScaleRange Population TreeSize )` (`0x41B6D0`):
`Area` defaults to 100 x 100 m, `Population` is capped at 10000, and the forest
is built only when `TreeTexture`, `ScaleRange`, `Population` and `TreeSize` are
all present. `0x4B3C40` builds it as a shape of its own:

- **Placement:** no random numbers. With W x D the area and t the tree width,
  x = z = W / 4 to start, a step of ((W / t + 1)(D / t + 1)) / N x t, and for
  each tree: wrap x past W - t / 4 (moving z on by x / W x t), keep x and z at
  least t / 4 (z gets t added), wrap z past D - t / 4; then x += i x step ±
  t / 2 and z += step ± t / 2, the sign alternating. All variables are floats
  (openRTS stores them as floats too, so the trees stand where RTS's do).
- **`ScaleRange` is not used:** every tree is `TreeSize` wide and tall.
- **A tree:** two crossed quads, both sides drawn (8 triangles), standing on
  the terrain at the tree's place (the height relative to the object).
- **Material:** `BlendATexDiff` alpha-tested, light material -9
  (Circumference: lit as facing up), UVs 0..1 across and 0.99..0 up, mip bias
  -3, the texture from the season's folder (alternative value 252).
- **Reach:** one distance level, 1500 m.

openRTS: `renderer/src/forest_shape.cpp`, loaded as `<world>.<UiD>.forest`
like transfers, so seasons and hot reload work.

## Level crossings

`LevelCr` (`0x41B6D0`): `LevelCrParameters ( time distance )` (25 s, 50 m),
`CrashProbability`, `LevelCrData ( flags tracks )`, `LevelCrTiming ( initial
serious anim )` (60, 60, 5 s), and `TrItemId ( 0 n )` for its track items,
`( 1 n )` for its road items (only items of kind 5, level crossing, are kept).

- **The trains (`0x508520`, each train's step):** from the front, the level
  crossing items up to 2000 m ahead get the distance and the time to arrive
  (distance / speed); the walk stops at the first item ahead when the train is
  not moving towards it. The same from the rear for items behind while it
  backs. Items under the train are always marked (negative distance).
- **The crossing (`0x422C18`, each frame while drawn):** closed when an item's
  time is below `LevelCrParameters`' time or its distance below its distance;
  warned when the time is below `LevelCrTiming`'s first value. The road items
  get warned + closed (0, 1, 2) for the cars.
- **The shape (`0x420E7C`):** nodes whose names start with `gate` swing to
  the closed frame (the last key) and back; nodes named `signal*` swing to and
  fro while warned. PROBABLE: openRTS takes `LevelCrTiming`'s third value as
  the gates' swing time and 2 s for a light's swing (RTS scales the node's
  frame count by them, `0x448578`).
- **The bell:** `crossing.sms` (the route's `SOUND`, else the global one) at
  the crossing, trigger 3 when it closes or warns, 4 when it opens.

openRTS: `viewer/src/crossing_view.cpp`.

## Pickups

`Pickup` objects are drawn as their shape at rest. Their animation
(`PickupAnimData`, filling a tender) is not done.

## Road traffic

`CarSpawner ( CarFrequency CarAvSpeed TrItemId TrItemId )` (30 s, 20 m/s, the
two road database items), the route's `carspawn.dat` (`0x492BE4`:
`CarSpawnerItem ( "shape.s" distance [CarSpread CarAcceleration ( a d )
CarMaxVelocity CarSoundFile] )`, defaults 5 m, 1, 0.5 / 1 m/s², 33 m/s) and
the road database `<route>.rdb` (the track database format).

- **Sending (`0x494B28`):** every CarFrequency x (0.6 .. 0.96) s a car leaves
  the first road item towards the second, unless the last one sent is within
  2 m of the start or closer than the two cars' distances. Its speed is
  CarAvSpeed, at most its type's CarMaxVelocity. PROBABLE: openRTS picks the
  type by CarSpread weighted against how many were sent (RTS mixes in the
  tick count).
- **Driving (`0x49396C`, `0x493FB8`):** within 100 m ahead a car follows the
  car in front, stops 10 m before a road level-crossing item that is warned or
  closed, and slows for curves: 7, 10, 12 or 20 m/s for radii up to 50, 150,
  250 and 500 m. It speeds up by its acceleration and brakes by its
  deceleration when its stopping distance reaches the obstacle. The wheels
  (every animated node) turn 10 frames a metre over a 20-frame cycle. It is
  gone past the second item.
- Not done: the cars' own sounds (`CarSoundFile`, none on the installed routes).

- **Facing:** a road vehicle's shape faces its −z, the reverse of rolling
  stock (PROBABLE: Open Rails builds the car's matrix from the point behind
  towards the one ahead, a wagon's the other way; openRTS placed them like
  wagons and they drove backwards until 2026-11-06).

openRTS: `viewer/src/road_traffic.cpp`.

## Water

- **Where:** terrain patches with flag bits `0xC0` (`0x49B088`). Each gets a
  quad at the tile's water height, `terrain_water_height_offset` (token 251,
  four values) spread over the 17 x 17 patch corners as `0x4A2A5C` does (bug
  for bug: away from the first values rather than towards the others; equal
  values everywhere on the installed routes but one tile).
- **The layers (`world_water` in the environment, `0x55859C`):** each at its
  `world_water_layer_height` above the water, drawn in order right after the
  terrain (`0x408574`), depth test LEQUAL with writes; `BlendATex*` and
  `AddATex*` layers laid over by alpha, `Tex` / `TexDiff` opaque. RTS reads as
  many layers as the count says.
- **UVs (`0x4A2144`):** per patch, `uvtiles` repeats moved by `uvscroll` x time
  (wrapping past 1), or the frame's `uvstamp` with frames changing every
  `framelen`; a `world_water_layer_sky_reflection` layer's UVs follow the
  camera ((corner - eye) / 128 m).
- **Light:** as the terrain with the normal up (RTS `water.vert`).

openRTS: in `renderer/src/terrain_renderer.cpp` (`vs_water`, the terrain's
shading and effects).

## Rain and snow

`world_precipitation ( world_precipitation_type world_precipitation_density
world_precipitation_speed world_anim_shader )` (`0x55859C`; 1024 drops, 5 m/s
by default).

- **The drops (`0x41A318`):** in a box 50 m to each side and 30 m above and
  below the eye; each a vertical quad 4 m on a side facing the camera, falling
  at the speed, carried by the wind layers below their heights; out of the box
  sideways it comes back on the other side, below or above it it starts again
  near the top.
- **The frame:** the stamped frames by distance: 0 beyond 5 m, 2 within 5 m,
  3 within 2.5 m.
- **How many:** density x the precipitation amount (RTS: the start settings'
  `PrecipitationDensity`, 0..1) x the config's reduction. openRTS uses the
  full density whenever the environment (picked by the weather) has
  precipitation.
- **Sound:** the environment's, else `rain_ex.sms` / `snow_ex.sms`.

openRTS: `viewer/src/particle_effects.cpp` (`PrecipitationView`).

## Diesel smoke

`DieselSpecialEffects ( ExhaustN ( x y z  dx dy dz  width ) )` with RTS's
`Colour` (0xCF656577), `Density`, `Mass` (0.5..5), `Magnitude ( idle full )`
(default `DieselSmokeEffectInitialMagnitude` x 0.65 and
`DieselSmokeEffectMaxMagnitude`), `Rate ( idle full )` (default
`DieselSmokeEffect*SmokeRate`, the max x 0.01), `Dispersion` (at least 1),
`Texture` (default `GLOBAL\TEXTURES\t_diesel_smoke.ace`) (`0x44247C`).

- **What drives it:** a DLL's `ExhaustContr` (`RTSGetLocoObject` ot_exhaust:
  rate, magnitude, dispersion, mass, colour) when the engine has a DLL; else
  the `.eng` values at the engine's load (0.1 idling; up to 0.11 the idle
  values scaled down, then between idle and full). PROBABLE: openRTS takes the
  throttle for the player's train without a DLL and idle for AI trains (RTS:
  their pull against their maximum).
- **The puffs (`0x50D3B4`):** at the rate (more as the car runs and in the
  wind, so they keep together), pushed out along the nozzle, carried by the
  car's own speed that fades 8 % a second, rising as they thin, blown by the
  wind layers; each grows by its magnitude a second; its alpha goes from 0.7
  down as it outgrows its magnitude, and it is gone below 0.01. PROBABLE: the
  rise's third input taken as 0.1.
- **Drawn (`0x50E4F8`):** a camera-facing quad, half its size the puff's size x
  the nozzle width, coloured by the exhaust's colour and lit as facing up.

openRTS: `viewer/src/particle_effects.cpp` (`SmokeView`).

## Viewer

The panel's scene section turns road traffic, rain and snow, and diesel smoke
on and off (remembered). The capture report prints a `world:` line with the
water quads, forests, crossings (closed), pickups, car spawners and cars,
drops and smoke puffs around.
