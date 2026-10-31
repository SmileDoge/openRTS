# openRTS roadmap: train movement, signals, routes, multiplayer

Snapshot as of 2026-10-30. "Exists" means it is in the code and tested or seen
on screen. Reverse notes live in `docs/reverse/`.

## What exists

| Area | State |
|---|---|
| `formats` | ACE (structured + raw DXT1), binary `.s` shapes, `.sd`, world files (all object kinds: static, track, gantry, collide, signal, speed post), terrain tiles (`.t`, `_y.raw`), ENV files, `.trk` environment table |
| `assets` | content resolver (route / global lookup) |
| `renderer` | WebGPU shapes with RTS material/pass rules, sky + time of day, seasonal terrain, world streaming, frustum culling; RTS and experimental shading paths |
| `viewer` | free camera over a route, time/season/weather controls |
| `core`, `simulation`, `platform`, `compat`, `game` | empty stubs |
| reverse notes | rendering, sky, terrain, train physics (forces, coupler solver, traveller), cab/DLL ABI, signals and sigscr VM |

Nothing moves yet: there is no track database, no train and no game loop.

## What is needed

Current focus: single-player driving and multiplayer. Simple AI trains are
in scope as a testing tool; activities and full AI traffic are deferred to
the end. Order follows dependencies; items in the
same step can be done in parallel.

### 1. Track network (foundation for everything else)

- `GLOBAL/tsection.dat` + route `tsection.dat`: section sizes/curves, track
  shapes and their paths.
- Route `.tdb`: track nodes (end / vector / junction), vector sections,
  `TrItemTable` (signals, speed posts, level crossings, platforms, sidings,
  pickups), `TrSignalDirs`, `.rdb` for roads.
- **Track traveller**: position on a vector node, advance by distance across
  nodes and junctions, world position and heading. RTS `0x481440`,
  `0x4F5B34`.
- Viewer: draw the TDB as lines, pick a node, run a traveller along the track.
- Dynamic track (`Dyntrack`) rendering, which also needs the traveller's
  geometry.

### 2. Rolling stock and a moving train

- `.con`, `.eng`, `.wag` (MSTS + RTS extensions from `ext.pdf`/`export.pdf`):
  mass, size, couplers, friction, brakes, lights, cab references.
- Train model: cars with 1-D offsets, travellers per car and bogie, a
  projection to 3-D (pitch from bogie heights).
- Physics as reversed: resistance, grade, curves, overspeed, coupler solver
  with 0.05 s sub-steps, static-friction brakes, derailment.
- Simple controls without a cab: throttle / brake / reverser.
- Render wagons and engines at their traveller positions, wheel/bogie
  animation, follow camera.
- Junctions: switch state, the train takes the set branch, switch animation
  on track shapes.

Result: drive a train with the keyboard over a route.

### 3. Signals

- `sigcfg.dat` parser: light textures, light tables, signal types, draw
  states, aspects, signal shapes and sub-objects.
- sigscr: preprocessor (comments, `/*RTS#`, constant substitution), compiler
  to the same instruction set, VM with RTS semantics (see
  `docs/reverse/signals.md`). Test: compile all installed `sigscr.dat` files
  and compare with RTS's own `SignalScriptsGenerateAsm` output.
- Built-ins on top of the TDB: `block_state` (occupancy + junctions),
  `route_set`, `next_sig_*`, `this_sig_*`, `sig_feature`.
- Block occupancy from train travellers.
- Per-train evaluation pass (far to near, `NumClearAhead + 2` NORMAL signals).
- Rendering: signal lights (`LightTex`, colours, flashing), semaphore arms.
- The train reads the aspect and speed limit ahead. Passing a red signal is
  detected.

Result: signals react to the player's train and to switch positions.

### 4. Paths, routes, interlocking

- `.pat` paths (player and AI), path reservation through junctions.
- Automatic switch setting along a reserved path; manual switch control.
- Speed posts and the speed limit ahead.
- Signal modes and overrides (RTS: `SetSignalMode`, `SignalOverride`).
- Dispatcher view (map of track, signals, trains). RTS has a dispatcher
  console ("Пульт ДСП"), which also matters for multiplayer.
- **Test AI trains**: spawn a consist on a `.pat` path or a chosen track
  point and let a simple driver follow it, obey signal aspects and speed
  limits and stop at red. They exist to load signals and blocks and to
  stand in for remote trains, not to replay activities.

### 5. Cab and on-board systems

- Cab view (`.cvf`, 3-D cabs), controls, instruments.
- Legacy locomotive DLLs through a 32-bit host process, and the openRTS train
  plugin API (plan in `train-dll.md`), including `ALSN` (cab signalling) and
  `SpeedLimit` callbacks. This is where most RTS rolling-stock logic lives.
- Brakes in detail; cab and DLL-driven sound on top of the sound foundation
  (`sound.md`: `.sms` runner, miniaudio output, world sounds).

### 6. Multiplayer

See "Multiplayer: feasibility" below. The network carries train, switch and
signal state, so it builds on steps 1–4. The chunk dumper for `multdata`
and capture tooling do not depend on the simulation and can start earlier.

### 7. Activities and full AI traffic (deferred)

Deferred until single-player driving and multiplayer work.

- `.act`/`.srv`/`.trf`: services, timetables, player train placement.
- Full AI: timetabled services, platform stops, activity events and outcomes
  (large RTS extension list in `RTSFileReference.html`).

## Multiplayer: feasibility

Recreating the protocol is feasible:

- It is plain WinSock 2 TCP inside `rts1.exe`. The same binary has both the
  client (`0x4C1818` connect, `0x4C1B64` login replies) and a listening node
  (`0x52E4F0` bind/listen, "RTS Network Node"). The game logs network events
  (`log_multiplayer_events` `0x4D2FA8`, `logmult.txt`).
- The data is chunked. The captured startup data `RTS/multdata` (71 KB) walks
  cleanly as `u16 chunk id, u32 total length` records: 12 chunks, ids
  `0x4601..0x4648`. `multlogin` holds the server address
  (`127.0.0.1:800`) and login fields.
- Login replies cover protocol version, route version and nickname checks.
  Events cover new/remove/assign train, couple/uncouple, train id change,
  wagons and pickups.
- Both ends can run locally, so traffic can be captured and replayed against
  an openRTS implementation.

Options:

1. Own protocol between openRTS instances: simplest, no compatibility.
2. RTS-compatible client: openRTS joins an RTS server. This needs the chunk
   formats for trains, wagons, switches and signals, and the same simulation
   state semantics (IDs, consists, paths).
3. RTS-compatible server/dispatcher: the most work.

Recommended order: steps 1–4 first. Before that, a small reverse pass
and a chunk dumper for `multdata` can be done at any time, since it does not
depend on the simulation.
