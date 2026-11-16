# Weather and thunderstorms

RTS knows three weathers, the activity's and the server's `weather`: clear
(0), snow (1) and rain (2). They choose the environment file
(`SummerRain` ...), the seasonal and snow textures, the rain or snow
falling round the eye (`world_precipitation`, `PrecipitationView`), its
sound (`rain_ex.sms`, `snow_ex.sms`) and the cover of openRTS's own clouds
(clear 42 %, snow 85 %, rain 92 %; experimental-effects.md, "openRTS sky").

## Thunderstorm (openRTS's own, 2026-11-16)

A fourth weather, «Гроза»: rain with lightning. RTS has none; to RTS
programs it is rain. Testers asked for one ("а есть какие нибудь планы на
погоду? как в том же тме гроза оч красиво смотрелась"); a storm in other
simulators is mostly a strong light at night, here the lightning lights the
clouds round it from inside, the land, the rain and the cab, and its
channel is drawn to the ground.

Where it is chosen: the start menu's weather («Гроза»), the viewer panel's
(«Погода»), `--weather storm` (the game and the viewer), and the server's
`--weather storm` (docs/server.md). A scenario's weather is RTS's (0..2).

What changes under it:
- **Clouds** (openRTS's sky): 97 % cover; the layer from 1.1 km above sea
  (or 500 m over the eye where the route lies high), 4.4 km thick instead
  of 2.2; its base darker, the light from below cut by 65 % (the land under
  a storm is in its shadow). The player's own cover («Облачность» with
  «Облачность по погоде» off) still wins.
- **Rain:** 1.7 times RTS's density of drops (`PrecipitationView`, up to
  twice).
- **Lightning**, below.

- **Thunder**, below ("Thunder").

### Where and when it strikes

`renderer::lightning_strikes(eye, time, rate)` (lightning.hpp) gives the
strikes alight at a moment. The land is cut into cells of 2 km and the time
of day into slots of 10 s; each cell strikes at most once a slot, its
chance, its place in the cell, its moment in the slot, its kind and
strength hashed from the cell and the slot (splitmix64, not the standard
library's distributions, which differ between compilers). So every client
of a network game sees the same strike at the same place and moment with
no package for it, as long as their clocks agree (the server's time).

The chance: `rate` strikes a minute within 12 km of the eye (6 by
default), spread over the cells there, times the storm's gathering at the
cell, `0.2 + 2.7 n^2` of a smooth value noise on a 16 km lattice drifting
6 m/s north-east (about 1 on average, 0.2..2.9): some minutes many strikes
on one side, then fewer. Over an hour at one place the count is within
0.35..2.2 times the rate (`openrts.lightning`).

A strike: 60 % to the ground, 40 % within the cloud; strength 0.5..1.5.

### A strike's light

`lightning_flash(strike, age)`, its life at most 1.2 s:
- **To the ground:** the stepped leader's faint glow (0.05) for 20..50 ms,
  then 1..4 return strokes (20 %, 30 %, 30 %, 20 %), each rising in 3 ms
  and dying in 18..40 ms, 35..125 ms apart, the first at the strike's
  strength, the others 0.4..0.85 of it; three in ten go on glowing at 0.22
  (a continuing current) for 80..200 ms. So most strikes flicker.
- **Within the cloud:** 2..6 softer pulses (0.3..0.75), 10 ms up, 30..80 ms
  down, 40..180 ms apart.

A frame shows a strike's brightest within its own span of the clock
(`lightning_flash_seen`, every 2 ms): taken at one instant, at 30 frames a
second most strokes fell between the frames (the first strike of a test
run showed 0.02 of its strength). None while the clock stands (a flash
would stay lit) nor in a time-lapse faster than 16 times.

### On the clouds

The strike most seen (its strength over `1 + (d / 4 km)^2`, kept between
its strokes) lights the clouds: a light 300 m inside the cloud's base over
the ground point (a strike to the ground) or at 40 % of the layer's height
(within the cloud). The clouds' march adds, at each step in a cloud,
`exp(-d / 1500 m) / (d^2 / 1e6 + 0.25) + 0.02 exp(-d / 8000 m)` of it, d
the step's distance to the light, through what lies in front, scattered out
like the sky's light: the cloud glows round the strike, brightest where it
is thick, a faint share far through the whole deck. That goes into a
texture of its own (R16F, half the view's size, `NativeSky`), for a light
of 1, and is built up over the frames like the clouds' (0.12 a frame) while
the strike is the same; a new strike starts it again. The sky adds it times
the flash's colour (0.80, 0.86, 1.0, linear) x its light x 0.6. Under RTS's
sky dome (openRTS's sky off) the clouds do not glow.

### On the scene

`ModelLighting::flash` (the encoding of `sun_ambient`) is added to the
ambient light of the shapes and the terrain, the haze's sky light (the
experimental path) and the rain's and smoke's sprites; not to what decides
the lamps by day, so the headlights do not dim in a flash. Its strength:
every strike's light times `1 / (1 + (d / 2.5 km)^2)` (one within the
cloud 0.6 of it), at most 1.5, times 0.45, times the flash's colour.

As much as the sky round the eye is open: none deep in a tunnel, less in a
cab. Each frame, under a storm, the occlusion probe tests 24 points on the
far plane over the whole view (6 across, 4 up) in the scene pass after the
opaque objects (before the glass): a point passes only where nothing is
drawn in front, the sky. A quarter of them open (a field, a cab's
windows a little less) takes the flash whole; the share is eased, up in
half a second, down in two.

### The channel

`lightning_channel(strike, height)`: from the cloud's base (250 m into it)
down to the ground point, the top leaning up to a quarter of the height
aside; midpoint displacement, 7 halvings (129 points), each middle pushed
aside by 13 % of its piece's length (less up and down); 3..7 branches from
the upper two thirds going down and out (6..28 % of the height, width 0.55,
brightness 0.2..0.5), half of them with a twig. The top's last eighth fades
into the cloud. The ground point's height is the land's there when it is
loaded, else the eye's less 2 m.

Drawn by `LightningRenderer` after the sky (before the openRTS water and the
glass): each piece a strip 1.5 px wide at its core (the main channel; at
least 1.2) with a halo six times as wide, added, depth-tested without
writing depth; a piece past the far plane is kept on it, in front of the
sky, behind whatever is drawn. Its light: 7 x the strike's light x
`exp(-d / 6 km)` (the rain's haze), in the frame's encoding (the HDR frame
keeps it over 1, and the bloom takes it on the experimental path).

### Thunder

`viewer::ThunderView`: each strike, once (by its id), is heard
distance / 343 m/s after it began (a strike within the cloud: from its
flash, the cloud's base above the eye), on the game's clock, so it waits
while the clock stands and comes sooner in a time-lapse. A clap for its
distance: near within 1.5 km, middle to 5 km, far beyond (a flash within
the cloud as if 2 km further: it rumbles); the clap of that kind picked by
the strike's seed among the kind's files in name order, so every player of
a game hears the same; pitch 0.94..1.06 by the seed. Volume: full within
600 m, then `(600 / d)^0.8` (1 km 0.66, 3 km 0.28, 12 km 0.09), times
`0.7 + 0.3 x strength`, a flash within the cloud 0.7 of it, times the share
of outside sound that reaches the listener (the cab's walls,
`TrainView::outside_gain`) and «Громкость грома». Panned from where it
struck (`VoiceSpace::panned`: direction only). A clap due over 20 s late (the
clock jumped) is dropped.

The claps are openRTS's own pack `openrts-thunder` (docs/vfs.md, "openRTS's
own packs"): `SOUND/OPENRTS/THUNDER/thunder_near_*.wav`, `thunder_mid_*`,
`thunder_far_*` (also `.ogg`, `.flac`, `.mp3`), loaded at the first clap. A
mod can replace them or add more of a kind. The pack's are cut from three
Freesound recordings by `tools/sound/make_thunder.py` (`packs/thunder`,
`CREDITS.txt`):

| Clap | From | Made |
|---|---|---|
| `thunder_near_1` | InspectorJ, "Thunder, Very Close, No Rain, C" (CC BY 4.0) | whole, 44.1 kHz |
| `thunder_near_2` | InspectorJ, "Thunder, Very Close, Rain, A" (CC BY 4.0) | 0.95..7.6 s, the rain faded out from 4.5 s, the hiss above 7 kHz off |
| `thunder_mid_1`, `_2` | the same two | below 1.2 kHz, the start softened (0.12 s), 22.05 kHz |
| `thunder_far_1`, `_2` | TSP-Talk, "Extreme Thunder Impact - Munich Trudering" (CC0) | 0.55..22 s and 5..30 s, below 450 Hz, rising over 0.5..0.6 s |
| `thunder_far_3` | InspectorJ's first | below 350 Hz, rising over 0.35 s |

Each mixed to one channel, its loudest second at -16 dBFS RMS (no peak above
-1 dBFS), 16-bit WAV. A test run (Kuban, night, four minutes): 30 strikes, 27 claps by its end, the claps
heard 4.2 s after one 1442 m away (near), 6.4 s after one at 2.2 km
(middle), 35 s after one at 12 km (far), at 0.5 in the cab.

### Settings

The viewer panel under «Погода» = «Гроза»: «Молний в минуту» (0..30, 6),
«Яркость молний» (0..3, 1) and «Громкость грома» (0..2, 1), remembered
(`lightning_rate`, `lightning_brightness`, `thunder_volume`); «Ударить
молнией впереди» strikes once to the ground straight ahead at «Расстояние
пробной молнии, м» (100..12000, 2000), this client only, to try the light
and the thunder at a distance (not while the clock stands or runs past
x16).

### Network

The login's weather byte stays RTS's: the server sends rain. An openRTS
server adds a byte to its answer to `openrts_hello` (docs/net.md): 1 when
the game is a thunderstorm. An openRTS player then plays it (with the rain
it was given). Older players read only the features.
