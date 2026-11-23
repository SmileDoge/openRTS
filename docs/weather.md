# Weather and thunderstorms

RTS knows three weathers, the activity's and the server's `weather`: clear
(0), snow (1) and rain (2). They choose the environment file
(`SummerRain` ...), the seasonal and snow textures, the rain or snow
falling round the eye (`world_precipitation`, `PrecipitationView`), its
sound (`rain_ex.sms`, `snow_ex.sms`) and the cover of openRTS's own clouds
(clear 42 %, snow 85 %, rain 92 %; experimental-effects.md, "openRTS sky").
openRTS's dynamic weather (below) changes it by itself.

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

**The server's weather while it runs** (2026-11-21, the organiser): the
server sets the game's weather at any time (its window's top bar, the
`weather` command) and sends it to its openRTS players and consoles with
`0x45FA` `openrts_weather` (`net::OpenrtsWeather`): one that stands
(clear, snow, rain, a thunderstorm), or the dynamic weather below, the same
for everyone by the server's seed, its settings and its clock (the game
seconds the cycle has run, kept by the server, stopped while paused, sent
again every 30 s; a player's cycle takes it and runs on). The start form's
"Динамическая (openRTS)" starts the game with it. RTS's players keep their
login's weather; the hello's thunderstorm counts only from a server that
sends no weather.

## Dynamic weather (openRTS's own, 2026-11-18)

Testers' proposal: a weather that changes by itself, by the season, and
can be changed in the game. RTS keeps one weather for the whole game.
`viewer/src/weather_cycle.hpp`; the panel's «Динамическая погода» under
«Погода» (off by default: as RTS), remembered (`weather_dynamic` and the
settings below). In a network game the server's: its own dynamic weather
or one that stands (Network, above).

**What a weather is** (`WeatherMix`): the clouds' cover (0..1), the
precipitation (0..1, rain, or snow in winter) and a thunderstorm's
strength (0..1). The scene makes of it:
- RTS's weather once 8 % falls (rain, or snow in winter), else clear: the
  environment file, the seasonal and snow textures, the falling drops and
  their sound, the rails by the weather. That switch is a step, as RTS's
  is when a scenario changes the weather; the rest comes gradually;
- the clouds' cover of openRTS's sky (the player's own cover, «Облачность
  по погоде» off, still wins);
- the drops: 0.3 + 1.4 x the precipitation times RTS's density (a light
  rain 0.4, a downpour 1.7, a thunderstorm's as before), its sound 0.3 ..
  1.1 as loud;
- a thunderstorm (lightning, thunder) once its strength is 0.3 with rain;
  the clouds darken as it gathers (`NativeSkyFrame::storm`), the lightning
  strikes «Молний в минуту» x its strength.

**How it goes** (`weather_at(time, seed, settings, season)`): the game's
time cut into spells of «Погода держится» (10..240 game minutes, 45),
each beginning up to half a spell late and turning into the next over
«Сменяется за» (1..60, 12; at most half a spell), smoothly. A spell's
weather is drawn from the seed and its number (splitmix64, the same on
every compiler) by the climate: with the share «Доля времени с осадками»
it rains or snows (0.25..1 of it, the clouds 85..97 %), and of those rains
«Доля гроз среди дождей» are thunderstorms (0.6..1 strong, heavy rain,
97 %); else the clouds are about «Обычная облачность» (±30 %, 5..85 %).
Snow in winter, rain otherwise; no thunderstorms in winter. «По сезону»
(on by default) takes the season's climate instead of the sliders:

| Season | Usual cover | With rain or snow | Thunderstorms among the rains |
|---|---|---|---|
| winter | 70 % | 35 % (snow) | none |
| spring | 55 % | 30 % | 10 % |
| summer | 40 % | 25 % | 35 % |
| autumn | 70 % | 40 % | 5 % |

The same seed and time give the same weather, as the lightning does: a
server can later give every player the same weather with its seed and
clock alone. A new seed each run; «другая погода» draws another.

**Its clock** runs with the game's: stopped while it stands, 60 times as
fast at x60. The weather shown eases towards the cycle's over some 20
game seconds (also while the clock stands), so a slider moved by hand
does not make it jump. It starts from the weather chosen (the start menu,
a scenario, a save) and holds it a spell, then goes its own way.

**By hand:** «Сменить сейчас на» ясно (15 % clouds) / облачно (80 %) /
дождь or снег (60 %) / гроза: from what is shown to it over a change, held
a spell, then back to the cycle over another change. The panel shows the
weather now and when the next one begins to come.

**Saved games** keep its clock and seed (`weather_cycle=` in the header,
docs/saves.md): it goes on as it was.

Checked: `tests/weather_cycle_test.cpp` (the same seed and time, the same
weather; a summer week a second at a time changes at most 0.2 % a second,
rains 30 % of the time, storms 10 %, never snows; winter snows, never
rains or storms; a dry climate never rains, an all-wet one always; a
change by hand comes, holds and goes back). In the game
(`OPENRTS_WEATHER_CHECK`: on for the run, 600 times as fast, the weather
logged every game hour; Kuban, 30 game hours): clouds 12..97 %, rains of
1700..3300 drops, a thunderstorm of 0.91, the environment switched between
clear and rain again and again.

## Raindrops on the cab's glass (openRTS, 2026-11-22)

While it rains (not the snow) and the engineer is in a cab or salon, its
glass gathers drops (the panel's World tab: "Raindrops on the cab's glass",
on by default).

- **Which glass.** The blended surfaces of the room he is in
  (`ModelLighting::room`, the cab shape's box) near its walls and facing
  out: the windscreen and the side windows; the gauges' glass inside, and
  roof lights, stay dry (`rain_on_glass` in the models' shaders).
- **Which glass, more.** Only large primitives (their box's middle extent
  at least 25 cm): a gauge's glass and its needle stay dry even close to
  the windscreen.
- **The drops.** In three layers of cells (1.7–3.5 cm, turned against each
  other: no grid), as many as the time since it was wiped and the rain's
  amount give, more on the windscreen at speed, fewer on the side windows;
  each a small lens (the sky caught on its upper edge), over a thin uneven
  film of water that hazes the view. On the move they run up the windscreen
  and back along the side windows, drawn out the faster; standing they
  slide down. How far they have run is integrated each frame, so a change
  of speed carries them on from where they are.
- **Snow.** While it snows the glass gathers flakes instead: they stick
  where they hit and stay, fine flakes and clumps with ragged edges; on the
  move the windscreen takes them faster, the side windows slower. The
  wipers clear them the same way.
- **The wipers.** The cab's running FreeAnimations (the wipers the DLL
  starts, `CabView::wiper_points`, the animated nodes and all below them):
  their triangles where they are each frame, and the strips their edges
  swept since the last, are marked wiped in a 64 x 32 grid over the room's
  wall they are on (`TrainView::rain_glass`); there the drops start again
  from none, gathering slower the more there are. A blade at rest wipes
  nothing, and what it wiped stays wiped while it rests (a DLL's wipers
  running with pauses).

## openRTS's rain and snow (2026-11-22)

RTS draws a few thousand flat sprites of its environment's texture. openRTS
(the World tab: "Rain and snow: openRTS's", on by default; off: RTS's
sprites) draws its own (`renderer::WeatherParticleRenderer`):

- **Made on the GPU.** 60 000 rain drops or 40 000 flakes at RTS's density
  (twice in a thunderstorm), each made in the vertex shader from its index
  and the clock in a box around the eye that wraps as it moves: the CPU
  keeps none, the field never ends.
- **Rain.** Thin streaks along each drop's speed against the camera's (a
  1/40 s exposure, at most 3 m): straight down standing, slanting into
  the train and longer as it runs. A streak far off thinner than a pixel
  stays one pixel, its light shrinking with it.
- **Snow.** Soft round flakes falling 0.7–1.4 m/s, each tumbling on its
  own, carried by the environment's lowest wind layer; drawn out a little
  at speed.
- Both lit by the scene's light, fading in near the eye and out at the
  box's edge, hidden by the cab's walls and a tunnel's roof (depth-tested);
  none fall inside the cab the eye is in. They stay in the world when the
  scene's origin moves.
- **Strength.** "Rain and snow strength": as the weather, drizzle, rain, a
  downpour or the player's own (0.1–2); it sets the particles, the drops on
  the glass and the fog.
- **Fog.** "Fog in rain and snow" (on by default): while it falls the fog
  ends nearer, 3500 m (snow 2200 m) over 1 + 1.5 × the strength.
