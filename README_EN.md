# openRTS

[Русская версия](README.md)

**openRTS** is an open, independent implementation of the RTrainSim (RTS)
train simulator. The goal is to run existing RTS and MSTS content (routes and
rolling stock of the former USSR railways: ED4M, ChS4T, VL10, 2TE10 and
others) on a modern engine, and to bring back RTS's behaviour wherever the
player sees it: physics, brakes, signalling, sound.

RTS's behaviour is recovered from its documentation, the content files and
analysis of `rts1.exe`. Every finding is marked with its confidence
(CONFIRMED / PROBABLE / UNKNOWN) in `docs/reverse/`.

> Work in progress. Today it is a route viewer with train driving, signalling,
> a dispatcher window and sound, not a finished game.

## What works

- **Content:** a virtual file system (installation folders, zip packs,
  patches, mods, hot reload); ACE, `.s` / `.sd` shapes, `.w` world files,
  terrain, environment, `.trk`, the `.tdb` / `.tsection` track database,
  `.con`, `.eng` / `.wag` (with `.rtseng`), `.pat` paths, `.sms` / `.ws`
  sound, `sigcfg.dat` / `sigscr.dat`, the `.rtsdb` route database.
- **Graphics (WebGPU):**
  - RTS's material and pass rules;
  - sky and time of day, seasons and weather;
  - route streaming, LOD, MSAA;
  - shape animation, signal lights, instancing of identical shapes.
- **Trains:**
  - RTS physics, couplers and automatic coupling, buffer hits and
    collisions;
  - RTS brakes: the 395 driver's valve, the electro-pneumatic brake (ЭПТ),
    the independent brake;
  - derailment as RTS does it (overturning in curves, running off the end
    of the track);
  - body sway on the springs;
  - standing consists go to sleep (STATIC) and cost no simulation time.
- **Signalling:**
  - `sigscr.dat` scripts compiled exactly as RTS compiles them;
  - blocks, routes, occupancy by the whole train;
  - signal chains ahead of the player's train (at both ends, as RTS does).
- **Paths and movement:**
  - `.pat` paths, junction reservation with RTS's queue;
  - speed limits from the speed posts;
  - test trains with a simple driver that follow their path, keep the
    limits and stop at red.
- **Track control (Ctrl+Alt+F7):**
  - the track map from above: trains, junctions, signals, platforms;
  - building and editing paths, train roles, uncoupling;
  - placing consists with a preview of where the consist will stand and
    which way its front car faces; it can be slid along the track and
    turned before it is placed.
- **Sound:** `.sms` scripts (activation, triggers, curves, loops), route and
  train sounds, positional audio (miniaudio).
- **RTS information panels:** F5, Ctrl+F5, F6, F7, Ctrl+F7.

## Not yet

In the order of the plan (`docs/roadmap.md`):
1. 3-D cabs and locomotive DLLs through a 32-bit host process, ALSN cab
   signalling, cab sound. There will be no 2-D cabs.
2. Multiplayer (an RTS-compatible Legacy mode and an own protocol) and the
   dispatcher console (Пульт ДСП).
3. Activities and full AI traffic.
4. PBR materials.

## Prebuilt binaries

Every push to `master` is built on GitHub Actions. Ready archives (the viewer,
the tools, `wgpu_native.dll`) are in **Releases**:
- **nightly** — a pre-release with the latest `master` build, replaced
  automatically;
- **vX.Y.Z** — versions; the release is created automatically when a version
  tag is pushed.

A version's release text is its section of [`CHANGELOG.md`](CHANGELOG.md)
(in Russian); nightly's comes from the commits. While the repository is
private, only its members can download the builds.

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
with no window.

## Running

You need an RTS installation with content: openRTS changes nothing in it and
ships no content of its own. A computer without Visual Studio needs the
Visual C++ Redistributable (x64), see "Prebuilt binaries".

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
| `--track-control` | open Track control at once |
| `--time 06:30:00`, `--speed <x>` | time of day and how fast it runs |
| `--season spring\|summer\|autumn\|winter` | season |
| `--weather clear\|snow\|rain` | weather |
| `--no-sound`, `--volume 0..1` | no sound, volume |
| `--msaa 1`, `--no-vsync`, `--no-batching` | graphics settings |
| `--experimental` | experimental shading instead of RTS's |
| `--mount <folder or zip>` | mount a mod over the installation |
| `--no-hot-reload` | do not watch files for changes |

### Controls

| Keys | Action |
|---|---|
| RMB + mouse, WASD, Q/E, Shift | free camera |
| F | free / orbit camera |
| C | put the consist on the track near the camera |
| Home / End | throttle |
| Page Up / Page Down | reverser |
| `]` / `[` | driver's brake valve (395) |
| `.` / `,` | independent brake |
| K | electro-pneumatic brake |
| X | change to the other cab |
| V | camera follows the train |
| J | throw the junction ahead |
| T, L | track overlay, junction and signal labels |
| R | RTS / experimental shading |
| F5, Shift+F5, Ctrl+F5 | cars, the whole train, limits and the kilometre |
| F6 | stations ahead |
| F7, Ctrl+F7 | platform names, signal names |
| Ctrl+Alt+F7 | Track control |
| Ctrl+F6 | save a debug view |

## Tools

| Program | Purpose |
|---|---|
| `openrts-pack` | build and inspect content packs (`docs/vfs.md`) |
| `openrts-track-check` | check the track network, `.pat` paths and speed posts |
| `openrts-path-check` | drive test trains along paths: reservations, the planner |
| `openrts-signal-check` | compile every `sigscr.dat`, compare with RTS's assembly |
| `openrts-sound-check` | parse every `.sms` / `.ws` and the sound files |
| `openrts-train-check` | load every consist and drive it on a test track |
| `openrts_asset_dump` | resolve assets the way RTS looks them up |

## Layout

| Module | Responsibility |
|---|---|
| `core` | errors, paths, the virtual file system, encodings |
| `formats` | RTS/MSTS file formats |
| `assets` | content lookup and loading |
| `renderer` | drawing (WebGPU) |
| `simulation` | track network, trains, brakes, signals, dispatcher |
| `audio` | the `.sms` runner and audio output |
| `compat` | the bridge to locomotive DLLs (in progress) |
| `viewer` | the viewer: window, input, camera, panels |
| `tools` | command-line checks |

Lower layers do not depend on `viewer`, on a particular graphics API or on
RTS's DLLs.

## Documentation

- `CHANGELOG.md` — changes by version (in Russian);
- `docs/roadmap.md` — the plan and the current state;
- `docs/train.md`, `docs/track.md`, `docs/signals.md`, `docs/paths.md`,
  `docs/sound.md` — how the subsystems work;
- `docs/vfs.md` — content, packs and mods (a Russian tutorial:
  `docs/vfs-tutorial-ru.md`);
- `docs/train-dll.md` — the plan for the locomotive DLL bridge;
- `docs/reverse/` — RTS's behaviour with addresses and confidence levels.

## Legal

openRTS is not affiliated with the authors of RTrainSim or MSTS. Routes,
rolling stock and other content are not part of this repository; you need
your own RTS installation. `docs/RTSFileFormat.dat`,
`docs/RTSFileReference.html`, `docs/export.pdf` and `docs/ext.pdf` are RTS's
documentation and belong to its authors. The project's licence is not chosen
yet.
