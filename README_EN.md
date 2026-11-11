# openRTS

[Русская версия](README.md) · **[Installing and running](INSTALL_EN.md)**

**openRTS** is an open, independent implementation of the RTrainSim (RTS)
train simulator. The goal is to run existing RTS and MSTS content (routes and
rolling stock of the former USSR railways: ED4M, ED9M, ChS4T, VL10, 2TE10
and others) on a modern engine, and to bring back RTS's behaviour wherever
the player sees it: physics, brakes, signalling, sound.

RTS's behaviour is recovered from its documentation, the content files and
analysis of `rts1.exe`. Every finding is marked with its confidence
(CONFIRMED / PROBABLE / UNKNOWN) in `docs/reverse/`.

> Work in progress. The latest version is [v0.4.3](../../releases/tag/v0.4.3):
> a nearly playable game. Driving from a 3-D cab with RTS's locomotive
> DLLs, signalling, a dispatcher window, a living route (forests, level
> crossings, road traffic, water, rain and snow, smoke), sound by the cab's
> rooms, coupling and uncoupling with the DLLs, and now network play on an
> RTS server together with `rts1.exe`; activities are not yet.

## What works

- **The start menu** `openrts_game.exe` in tabs: "Single player" (route,
  consist, path, time, season, weather, how the train starts), "Network
  game" (the server's address and the nick; the menu follows the login and
  the wait for a train), "Settings" (the RTS folder, language, keys,
  sound, graphics, the window mode) and "Controls" (the hotkeys, also a tab
  of the game's panel); the game runs as a process of its own.
- **Content:** a virtual file system (installation folders, zip packs,
  patches, mods, hot reload); ACE, `.s` / `.sd` shapes, `.w` world files,
  terrain, environment, `.trk`, the `.tdb` / `.tsection` track database,
  `.con`, `.eng` / `.wag` (with `.rtseng`), `.repaint`, `.pat` paths,
  `.sms` / `.ws` sound, `sigcfg.dat` / `sigscr.dat`, the `.rtsdb` route
  database, `.electrotype` / `.electrodb` electrification.
- **Graphics (WebGPU):**
  - RTS's material and pass rules;
  - sky and time of day, seasons and weather, night and seasonal textures;
  - route streaming, LOD, mipmaps, MSAA;
  - shape animation, signal and train lights, route lamps, cab and saloon
    lights, instancing of identical shapes;
  - forests, level crossings with gates, pickups, road traffic, water,
    rain and snow, diesel smoke, as RTS has them; trees can be kept off
    tracks and roads (a Graphics setting);
  - an HDR frame with tone mapping (`--hdr`);
  - Vulkan, DirectX 12 or OpenGL to choose from; depth for ReShade;
  - experimental renderer (R): sun shadows (cascades and a map of the cab's
    own), lamp shadows, ambient occlusion (GTAO), every lamp at once,
    light bounced off the ground, aerial haze; passes recorded on several
    threads.
- **Trains:**
  - RTS physics, couplers and automatic coupling, buffer hits and
    collisions;
  - RTS brakes: the 395 driver's valve, the electro-pneumatic brake (ЭПТ),
    the independent brake;
  - an optional "realistic air" brake model: flow along the brake pipe,
    483 / 292 / 305 distributors, a 280 m/s emergency wave (RTS's brakes by
    default);
  - the car menu (Num3 / the key left of `1`): at a coupling — end cocks,
    hoses, the EP cable, the SME cable and the high-voltage bus,
    uncoupling, fixing a broken coupler; at a bogie — brake shoe,
    handbrake, release valve, emergency valve, cargo information;
  - derailment as RTS does it (overturning in curves, running off the end
    of the track);
  - body sway on the springs, rail joints;
  - electrification as RTS has it: the kind of current and the voltage
    under every electric engine, neutral sections, changeover stations;
    the pantographs' current, burning the contact wire, the substations'
    load; animated pantographs;
  - trains without a DLL (AI, standing, simple stock): their lights by
    their conditions as RTS decides them; pantographs by RTS's rules
    (`AlwaysUsePanto`, `LowerPantoOnStop` and the rest); horn, bell, sand
    and doors from the keyboard;
  - passenger doors worked from the cab, with their sound;
  - standing consists go to sleep (STATIC) and cost no simulation time.
- **RTS locomotive DLLs** run unchanged in the 32-bit
  `openrts-dllhost32.exe`: RTS's call order, masters and slaves over the
  SME cable, auxiliary libraries (AuxLibraries such as `Brakes.dll`),
  adapters for older ts.h versions, ALSN with the full signal data, taking
  another cab (`ChangeLoco`). A DLL that crashes is switched off and the
  game goes on.
- **openRTS native plugins** (64-bit DLLs, `ORTSTrainPlugin` in the
  `.eng`) drive an engine, its cab, gauges and sound; the example is the
  VL10k's logic (`sdk/samples/vl10k`).
- **The 3-D cab:**
  - switches, keys, gauge needles and lamps move with the DLL's state;
  - the mouse as in RTS: click, wheel, dragging handles; the cab keys from
    the `.sd`;
  - the КЛУБ, САУТ, САВПЭ and ИТП screens; DISPLAY state nodes, blinking
    lamps; the cab signal (ALSN); the cab lamps;
  - the cab sound; the sound follows where the engineer is;
  - the "Cab (locomotive DLL)" window: switches, gauges, DLL messages.
- **On foot:** walking is the player's (the panel's "On foot", with a train
  or without): stand up at the desk, open the window and lean out, walk
  through the cab and into the next section, get down onto the ground by a
  door and climb back into any cab of his train (the far cab of a multiple
  unit too, its DLL going along) or of another train of his. In a cab only
  on its `.sd` planes, as in RTS (no climbing onto the seat or the desk).
  Outside, up the cars' steps and ladders and along their walkways (the
  ChME3: from the ground onto the walkway, through the door into the cab,
  up onto the roof) and on the route objects' boxes (`ESD_Complex`:
  footbridges, platforms, a lighting mast's ladder); off an edge he falls.
  Through an open passage door the next car's salon or cab shows (the
  ED9M, the VL80's second section), not its outside.
- **Network play on an RTS server** (`rts_server.exe`, together with
  `rts1.exe`, [more](docs/net.md)):
  - login by nick and the wait for a train from the dispatcher, as
    `rts1.exe`; the world from the server: time, junctions, signals, the
    line, the pause;
  - other players' trains move smoothly, with their lights, pantographs and
    sound;
  - own trains (several may be) with their DLLs and cab; `rts1.exe` sees
    and hears them;
  - couplings and cuts, trains taken off the track, deleted and reassigned
    by the dispatcher (a train taken away stands still with its DLLs);
  - speed posts by the train's category from the server, electric trains'
    load on the substations to the dispatchers.
- **Own server** `openrts-server` (RTS-compatible, Legacy mode,
  [more](docs/server.md)): `rts1.exe` and openRTS log in to it as to
  `rts_server.exe`. It decides junctions and runs the signals with every
  train, carries out couplings and cuts; the dispatcher works at its
  console: trains, assignments, routes, junctions, signals, time, pause,
  messages. Next: the server with its own console map, as `rts_server.exe`;
  the remote dispatcher's console after it.
- **Signalling:**
  - `sigscr.dat` scripts compiled exactly as RTS compiles them;
  - blocks, routes, occupancy by the whole train;
  - signal chains ahead of the player's train (at both ends, as RTS does);
  - shunting mode, failed and excluded signals, overridden aspects.
- **Paths and movement:**
  - `.pat` paths, junction reservation with RTS's queue;
  - speed limits from the speed posts and the `.trk`;
  - test trains with a simple driver that follow their path, keep the
    limits and stop at red.
- **Situations:** all the trains on the route are saved (Train window →
  Situations) and stand where they stood when loaded, or from the start
  menu. You can start:
  - in the cab: on foot by the chair of the leading cab (Enter sits down),
    as RTS does; time stands while the game loads, and the locomotive's
    DLLs start once the cab is shown;
  - beside the car, as RTS does;
  - above the train;
  - with no train (a flight over the route; trains through Track control).

  At the start a loading screen stays until the train (and its cab), the
  world around and the terrain are in.
- **The inspector (F12):** a tree of the map's objects (by tile) and the
  trains (by car), the chosen one's properties in a window of their own,
  choosing with a click in the world, "Go to"; the shape's materials, a
  FullBright one lit as ordinary for that shape only, saved into a mod
  (`docs/inspector.md`).
- **Track control (Ctrl+Alt+F7, not in a network game):**
  - the track map from above: trains, junctions, signals, platforms, station
    names, the player's figure with the way the camera looks; "Teleport
    here" from the right-click menu;
  - the Electrification layer with the power source switch;
  - building and editing paths, train roles, uncoupling;
  - placing consists with a preview of where the consist will stand and
    which way its front car faces; it can be slid along the track and
    turned before it is placed.
- **Sound:** `.sms` scripts (activation, triggers, curves, loops); route,
  train, cab and auxiliary library sounds (КЛУБ, САУТ); positional audio
  (miniaudio); level crossings and route sounds fade with distance as in RTS.
- **Camera:** the view from the first or the last car (F3, an orbit round
  it, the train still driven with the cab keys; the distance, zoom and
  angle kept for each car), the wheel nearer and farther, Ctrl+wheel zoom
  (signal lights do not swell with it), the mouse sensitivity (the start
  menu and the game's panel); the picture fades in after the loading
  screen.
- **Interface:** RTS's information panels (F5, Ctrl+F5, F6, F7, Ctrl+F7),
  pause, Russian and English.
- **For rolling stock authors:** an SDK for native 64-bit locomotive
  plugins (`train_plugin.h`, a C++ wrapper, a sample).

## Not yet

In the order of the plan (`docs/roadmap.md`):
1. The rest of the cab and consist: ALSN from the route's signals, native
   plugins from the `.eng`, the SME cable and high-voltage bus in the car
   menu. There will be no 2-D cabs.
2. The openRTS server's own protocol (Legacy off) and plugins, helpers and
   the dispatcher console (Пульт ДСП). The client for an RTS server and the
   server in Legacy mode are there.
3. Cars as entities with their own logic (the cab is the car's) and
   standard car classes: freight, passenger (compartments and so on),
   multiple-unit cars.
4. Activities and full AI traffic.
5. PBR materials.

## Prebuilt binaries

Every push to `master` is built on GitHub Actions. Ready archives (the start
menu, the game, the locomotive DLL host, the tools, `wgpu_native.dll`) are in
**Releases**; how to install and run them is in [INSTALL_EN.md](INSTALL_EN.md):
- **nightly** — a pre-release with the latest `master` build, replaced
  automatically; the window's title and the settings mark its version with
  the commit, e.g. "openRTS 0.6.2 (nightly f1a6b90)";
- **vX.Y.Z** — versions; the release is created automatically when a version
  tag is pushed.

A version's release text is its section of [`CHANGELOG.md`](CHANGELOG.md)
(in Russian); nightly's comes from the commits.

> **The Microsoft Visual C++ Redistributable (x64) is required.** Without it
> Windows reports that `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` or
> `MSVCP140.dll` was not found. Get it from Microsoft:
> [vc_redist.x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe)
> ([all versions](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)).
> It must be the x64 version; x86 will not do.

## Building

The main platform is Windows 10/11 x64. You need:
- Visual Studio 2022 or newer with C++ (C++20);
- CMake 3.24+ and Git;
- the [wgpu-native](https://github.com/gfx-rs/wgpu-native/releases) v27.0.4.0
  SDK (`wgpu-windows-x86_64-msvc-release.zip`), extracted to a folder with
  `include/` and `lib/`.

CMake fetches GLFW, Dear ImGui, glm, miniaudio, zlib and zstd by itself.

```powershell
cmake -S . -B build -DOPENRTS_ENABLE_WGPU=ON -DOPENRTS_WGPU_ROOT=C:\sdk\wgpu-native
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Without `OPENRTS_ENABLE_WGPU` only the libraries, tools and tests are built,
with no window. `-DOPENRTS_ENABLE_TRACY=ON` builds the Tracy profiler's
client in ([profiling](docs/profiling.md)).

## Running

The interface is in Russian by default; English is chosen in the start menu
(Settings → "Язык / Language") or in the game's openRTS panel, and
remembered.

You need an RTS installation with content: openRTS changes nothing in it and
ships no content of its own. A computer without Visual Studio needs the
Visual C++ Redistributable (x64), see "Prebuilt binaries".

The easiest way is the start menu. It has four tabs:
- **Single player:** route, consist, path, time, season, weather and how
  the train starts (running, switched on, cold), or an RTS scenario of the
  route (`RTS_ACT`: its trains and events — messages, F8 reports, signals,
  sounds; its traffic trains run as RTS's, [docs/activities.md](docs/activities.md),
  [docs/traffic.md](docs/traffic.md)); "Start".
- **Network game:** an RTS server's address and the nick, the servers
  joined before, where to start (in the cab or above the train);
  "Connect". The server gives the route, season, weather and
  time, the dispatcher the train. Until there is one the menu shows the
  wait, which can be cancelled. An own server is `openrts-server`, next to
  the game ([docs/server.md](docs/server.md)); the menu does not start it
  yet.
- **Settings:** the RTS folder (typed, or "Browse..." to pick it), language, key layout, the key hints at the
  bottom of the screen (they can be turned off; the game's panel too), the
  mouse sensitivity and the head motion in the cab (the game's panel too),
  sound and volume, the rail joints (as RTS, or openRTS: 25 m rails, welded
  strings with equalizing spans), the settings and log folder, updates.
- **Graphics:** every graphics setting of the game's panel in groups
  (screen, renderer, shadows, ambient occlusion, lamps, sky and water,
  world), presets from Low to Ultra, and each setting's load on the CPU
  and the GPU. The graphics API, the window mode (in a window, a borderless
  window, full screen; Alt+Enter in the game), VSync and the FPS limit (30
  to 500 or none) are there too. While the game's or the menu's window is
  not in focus or is minimised, it draws at most 30 frames a second; the
  streaming of models and textures is not slowed by it.
- **Controls:** the simulator's hotkeys (as in "Controls" below); the
  in-game openRTS panel has the same tab.

All of it is remembered in `%LOCALAPPDATA%\openRTS\viewer_state.ini`. The
game runs as a process of its own (its output goes to `openrts.log` there
too); the menu comes back when the game is closed.

```powershell
.\build\game\Release\openrts_game.exe
```

The game can also be started directly:

```powershell
.\build\viewer\Release\openrts_viewer.exe --window --route Kuban_RTS --root D:\Games\RTS --consist "ED4M-0050 12vag"
```

| Option | What it does |
|---|---|
| `--window --route <name or path>` | open a route |
| `--window --shape <name>` | open a single shape |
| `--window --view last` | reopen the last saved view (Ctrl+F6) |
| `--root <folder>` | the RTS installation (when the route is given by name) |
| `--consist <name>` | a consist from `TRAINS/CONSISTS_RTS` or `CONSISTS` |
| `--path <name>` | a `.pat` path to put the consist at the start of |
| `--start running\|on\|cold` | a train with DLLs starts running, switched on and standing, or cold |
| `--track-control` | open Track control at once |
| `--situation <name\|file>` | put the trains of a saved situation back |
| `--spawn cab\|car\|above` | start in the cab, beside the car or above the train |
| `--time 06:30:00`, `--speed <x>` | time of day and how fast it runs |
| `--season spring\|summer\|autumn\|winter` | season |
| `--weather clear\|snow\|rain` | weather |
| `--no-sound`, `--volume 0..1` | no sound, volume |
| `--msaa 1`, `--no-vsync`, `--no-batching` | graphics settings |
| `--fps-limit <30..500 \| 0>` | the FPS limit for this run (0: none), not remembered |
| `--multi-draw on\|off` | multi-draw indirect for models this run (on by default where the GPU has it) |
| `--window-mode windowed\|borderless\|fullscreen` | the window for this run: in a window, borderless, full screen (else as in the settings) |
| `--paused` | start paused (Pause resumes): nothing moves, not even a flashing signal; once the scene has loaded, captures repeat pixel for pixel |
| `--hdr` | an HDR frame with tone mapping |
| `--backend auto\|vulkan\|dx12\|opengl` | the graphics API (OpenGL: the RTS renderer only) |
| `--reshade-depth on\|off` | depth for ReShade ([INSTALL_EN.md](INSTALL_EN.md#reshade)) |
| `--experimental` | experimental renderer instead of RTS's |
| `--effects shadows,clusters,ao,aerial\|none`, `--cab-shadows on\|off`, `--lamp-shadows 0..4`, `--lamps-daylight 0..1`, `--ground-bounce 0..0.5` | experimental renderer settings for one run ([details](docs/experimental-effects.md)) |
| `--capture <file.ppm>` | take one frame and exit; `--capture-at <s>`, `--capture-car <n>`, `--capture-camera ex ey ez tx ty tz`, `--capture-cab` say when and from where |
| `--mp-connect <address>[:port]`, `--mp-nick <nick>` | join an RTS server (port 800 by default); the route, season and weather are the server's, no `--route` needed |
| `--mp-snapshot <file>`, `--mp-save-snapshot <file>` | open a saved server snapshot offline; save the one received |
| `--mount <folder or zip>` | mount a mod over the installation |
| `--no-hot-reload` | do not watch files for changes |

### Controls

Two key layouts (the start menu or the openRTS panel → Keys): RTS, with the
numeric keypad (Num0, Num1, Num2, Num3), and openRTS, without it, for
laptops (`-`, `=`, Shift and the key left of `1`, that key); openRTS by
default. The keys at hand: the
"Controls" tab of the start menu and of the game's panel.

**Camera and world**

| Keys | Action |
|---|---|
| RMB + mouse, WASD, Q/E, Shift | free camera |
| F | free / orbit camera |
| V | camera follows the train |
| F3 | view from the first car (an orbit round it), again from the last, again back; the train is still driven with the cab keys |
| Wheel / Ctrl+wheel | outside the cab: nearer and farther / zoom (the field of view; Ctrl+wheel in the cab too) |
| Num+ / Num− | field of view narrower / wider, as RTS (15–90°) |
| C | put the consist on the track near the camera |
| J | throw the junction ahead |
| T | the track diagram (yellow lines; off by default, remembered) |
| L | junction and signal labels |
| R | RTS / experimental renderer |
| Pause | pause |
| Alt+Enter | full screen / window |

**A train without a DLL**

| Keys | Action |
|---|---|
| Home / End | throttle |
| Page Up / Page Down | reverser |
| `]` / `[` | driver's brake valve (395) |
| `.` / `,` | independent brake |
| K | electro-pneumatic brake |
| Space (held) | horn |
| B | bell on / off |
| X (held) | sand |
| P | pantographs down / up |
| Y / U | left / right doors (standing) |

An engine with a DLL is driven from its cab with the mouse and the cab keys
from the `.sd`, as in RTS. On foot (the 3-D cab) the cab keys work at once;
the arrows walk. With the free camera the "Cab keys of the locomotive DLL
(on foot always on)" checkbox in the Train window gives them the keyboard
(the camera keys are off then; the F keys still work).

**The engineer and the cab** (the 3-D cab: the "3D cab view" checkbox in
the Train window)

| Keys | Action |
|---|---|
| Enter | in the chair, rise at the desk and sit back (as RTS) / sit down / open a door / climb in |
| Ctrl+Enter | get up from the chair, step out into the passage; outside at a door, climb in |
| Arrows, Shift, End | walk, run, slower |
| Num0 / `-` | open the window |
| Num1 / `=` | lean out |
| Num3 / the key left of `1` | the car menu at the coupling or bogie under the cursor (outside) |
| Num2 / Shift + the key left of `1` | the flashlight, as in RTS: a beam from the eyes along the view, 50 m |
| F12 | the inspector: a tree of the map's objects and trains, the chosen one's properties, the shape's materials (FullBright as ordinary, into a mod), `docs/inspector.md` |
| RMB + mouse | look round |
| Mouse | click works a control, the wheel moves it a step, dragging moves handles and blinds |
| Wheel at the chair | off the controls, the head nearer to what the cursor points at, or back |
| Alt | captions of the controls |

**Information**

| Keys | Action |
|---|---|
| F5, Shift+F5, Ctrl+F5 | cars, the whole train, limits and the kilometre |
| F6 | stations ahead |
| F7, Ctrl+F7 | platform names, signal names |
| Ctrl+Alt+F7 | Track control |
| Ctrl+F6 | save a debug view |

## Tools

| Program | Purpose |
|---|---|
| `openrts-ace` | ACE textures to PNG and back (DXT1, RGB, RGB + mask, RGBA), a file or a folder; `--register`: a double click on an `.ace` shows it as PNG, PNG files and folders get "to ACE (openRTS)" (also a button in the menu's Settings; the format: `docs/reverse/ace.md`) |
| `openrts-pack` | build and inspect content packs (`docs/vfs.md`) |
| `openrts-track-check` | check the track network, `.pat` paths and speed posts |
| `openrts-path-check` | drive test trains along paths: reservations, the planner |
| `openrts-signal-check` | compile every `sigscr.dat`, compare with RTS's assembly |
| `openrts-sound-check` | parse every `.sms` / `.ws` and the sound files |
| `openrts-train-check` | load every consist and drive it on a test track; `--dll` runs a locomotive DLL by a script (with `--screens` the cab screens, `--triggers` its sound triggers), `--cab` shows how a cab binds, `--brake ... --realistic` brakes |
| `openrts-dllhost32 --probe <folder>` | versions and layouts of every locomotive DLL |
| `openrts_asset_dump` | resolve assets the way RTS looks them up; `--lights <route\|all>` counts the placed shapes' lamps by kind |
| `openrts-net-dump` | an RTS server's snapshot package by package; `--root`/`--route` check it against the route |

## Layout

| Module | Responsibility |
|---|---|
| `core` | errors, paths, the virtual file system, encodings |
| `formats` | RTS/MSTS file formats |
| `assets` | content lookup and loading |
| `renderer` | drawing (WebGPU) |
| `simulation` | track network, trains, brakes, signals, electrification, dispatcher |
| `audio` | the `.sms` runner and audio output |
| `trainplugin` | locomotive logic: the bridge to RTS's DLLs and native plugins |
| `dllhost` | `openrts-dllhost32.exe`: the 32-bit process RTS's DLLs run in |
| `sdk` | a sample native plugin (the API is `include/openrts/plugin/train_plugin.h`) |
| `net` | the RTS protocol: packages, the world snapshot, the server connection |
| `server` | `openrts-server`: the openRTS server (Legacy), its copy of the route, the dispatcher's console |
| `viewer` | the game: window, input, camera, cab, panels |
| `game` | the start menu |
| `tools` | command-line checks |
| `platform`, `compat` | placeholders |

Lower layers do not depend on `viewer`, on a particular graphics API or on
RTS's DLLs.

## Documentation

- [INSTALL_EN.md](INSTALL_EN.md) — installing and the first start;
- `CHANGELOG.md` — changes by version (in Russian);
- `docs/roadmap.md` — the plan and the current state;
- `docs/car-architecture.md` — a design: cars as entities, components,
  modules, standard classes;
- `docs/train.md`, `docs/track.md`, `docs/signals.md`, `docs/paths.md`,
  `docs/activities.md`, `docs/traffic.md`, `docs/sound.md`,
  `docs/electrification.md` — how the subsystems work;
- `docs/train-dll.md` — the locomotive DLL bridge and the native plugin API;
- `docs/experimental-effects.md` — the experimental renderer: what it does
  and what it costs;
- `docs/profiling.md` — building with Tracy, what is marked, adding a zone;
- `docs/world-objects.md` — forests, level crossings, road traffic, water,
  rain and snow and smoke, as RTS builds and runs them;
- `docs/net.md` — playing on an RTS server: the protocol, own and other
  players' trains, couplings, sound, the line;
- `docs/server.md` — the openRTS server: running it, the dispatcher's
  commands, what it decides and relays;
- `docs/train-systems.md` — lights, pantographs, horn and doors without a
  DLL, the cab's displays and lamps, cables and coupler repair, taking
  another cab, native plugins, the line current and wire burning, ALSN;
- `docs/vfs.md` — content, packs and mods (a Russian tutorial:
  `docs/vfs-tutorial-ru.md`);
- `docs/reverse/` — RTS's behaviour with addresses and confidence levels.

`docs/ru/` holds Russian copies of these documents with the same layout.
The English text is authoritative; a change to an English document is made
to its Russian copy in the same change.

## Legal

openRTS is not affiliated with the authors of RTrainSim or MSTS. Routes,
rolling stock and other content are not part of this repository; you need
your own RTS installation. `docs/RTSFileFormat.dat`,
`docs/RTSFileReference.html`, `docs/export.pdf` and `docs/ext.pdf` are RTS's
documentation and belong to its authors; openRTS's licence does not cover
them.

openRTS is licensed under the **GNU General Public License v3.0**: you may
use, study, change and share it, and modified versions only under the same
licence and with their source. The full text is in [LICENSE](LICENSE).
