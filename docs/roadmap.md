# openRTS roadmap: train movement, signals, routes, multiplayer

Snapshot as of 2026-11-01. "Exists" means it is in the code and tested or seen
on screen. Reverse notes live in `docs/reverse/`.

## What exists

| Area | State |
|---|---|
| `formats` | ACE (structured + raw DXT1), binary `.s` shapes, `.sd`, world files (all object kinds: static, track, gantry, collide, signal, speed post), terrain tiles (`.t`, `_y.raw`), ENV files, `.trk` environment table |
| `assets` | content resolver (route / global lookup) |
| `renderer` | WebGPU shapes with RTS material/pass rules, sky + time of day, seasonal terrain, world streaming, frustum culling; RTS and experimental shading paths |
| `viewer` | free camera over a route, time/season/weather controls |
| `simulation` | track network and travellers with the RTS junction modes (`track.md`); trains with RTS physics, couplers, RTS brakes, the 395 valve and ЭПТ (`train.md`); signals: the RTS script VM, block_state/route_set, train chains (`signals.md`) |
| `formats` (trains) | `.con`, `.eng`/`.wag` with `.rtseng` merge, `.pat`, `.sms`/`.ws` |
| `audio` | `.sms` runner, miniaudio output, world and train sounds (`sound.md`) |
| `viewer` (trains) | drive a consist from a path start, follow camera, Train panel |
| `core` | content VFS (`vfs.md`) |
| `platform`, `compat`, `game` | empty stubs |
| reverse notes | rendering, sky, terrain, train physics (forces, coupler solver, traveller), cab/DLL ABI, signals and sigscr VM |

Steps 1 and 2 are done: a consist from the content can be driven over a
route with the keyboard. Not done from them: Dyntrack rendering, consist
changes are done since (see the leftovers below). Step 3 (signals) is done as `docs/signals.md` describes:
scripts compiled exactly as RTS does, evaluated on the train step, lit on
screen. Next: step 4, paths and interlocking, starting with the train's
path and junction reservation (how RTS reserves and sets junctions along a
`.pat`: the junction record keeps a list of reserving trains, released by
`0x48C85C`; who fills it is still to be traced).

Leftovers from earlier steps (2026-11-01):
- done: consist changes — uncoupling (menu: end cocks closed; coupler break:
  open, both parts brake), coupling when trains meet gently, buffer pushes
  and collisions as RTS decides (`train.md`); body sway on springs, rail
  joints; derailment as RTS starts it: overturning, sliding off, hoses torn,
  neighbours pulled, hits, folding, ground and obstacle contact
  (`reverse/runtime-overview.md`; RTS's solver for derailed trains is
  replaced by a simpler folding rule);
- `z_bias`: RTS parses it and never uses it in drawing
  (`reverse/shape-rendering.md`), so the RTS path is right as it is;
- Dyntrack: deferred, no installed route has any to check against;
- shunting signal modes: the signal system handles SHUNT/FAILED/EXCL; what
  sets a train's modes (activities' `SetSignalMode`, the dispatcher) belongs
  to step 4.

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
- Rendering: signal lights (`LightTex`, colours, flashing). Semaphore arms
  are dropped (the user's decision, 2026-11-01: not needed).
- The train reads the aspect and speed limit ahead. Passing a red signal is
  detected.

Result: signals react to the player's train and to switch positions.

### 4. Paths, routes, interlocking

Status (2026-11-01): items 1–3 below are done (`paths.md`): paths from .pat
and from Track control, junction reservations with RTS's queue and
takeover, signals clearing along the reserved way, speed limits from the
speed posts, the Track control window, test trains with a simple driver.
Not done from this step: signal modes and overrides (nothing sets them yet).

Order (the user's decision, 2026-11-01): the logic first, then the
**Track control** map in the viewer, then test AI trains. The dispatcher
console (Пульт ДСП) moves to step 6: in RTS it is a network client of the
server, and its stations' routes, players and messages only make sense with
multiplayer. Both share one map component (`reverse/dispatcher.md`: RTS draws
Track control, the route editor map and the console with the same code).

1. **Logic** — start from RTS's path module (`0x48B8C8`, `0x48BAF8`,
   `0x48BBF0`, `0x48D220`, `0x48DD94`, used by Track control):
   - `.pat` paths (player and AI) as node/direction chains over the TDB;
   - path reservation through junctions (the junction's list of reserving
     trains, released by `0x48C85C`), automatic switch setting ahead,
     manual switching refused on reserved junctions;
   - signals open along a set route (`route_set`, `block_state` see the
     reservation);
   - speed posts and the speed limit ahead, overspeed detection;
   - signal modes and overrides (`SetSignalMode`, `SignalOverride`) as the
     internal base, without UI yet.
2. **Track control in the viewer** (as RTS's in-game window, `0x549A60`):
   - map of the real track geometry from above: zoom, pan, trains,
     signals, junctions with their set branch, platform and station names,
     sidings and electrification layers;
   - train menu: jump to train, show / edit / save / load path, uncouple,
     cast PLAYER / STATIC / TRAFFIC, remove;
   - path editing: start, waiting, reverse and end points, start direction,
     waiting time; switch a junction on the path;
   - signal info, force update.
3. **Test AI trains** through Track control: spawn a consist on a `.pat`
   path or a chosen track point, give it a path, start it; a simple driver
   follows it, obeys aspects and speed limits and stops at red. They load
   signals and blocks and stand in for remote trains, not activities.

### 5. Cab and on-board systems

- 3-D cabs only: controls, instruments. No 2-D cabs and no `.cvf` (the
  user's decision, 2026-11-01).
- Legacy locomotive DLLs through a 32-bit host process, and the openRTS train
  plugin API (plan in `train-dll.md`), including `ALSN` (cab signalling) and
  `SpeedLimit` callbacks. This is where most RTS rolling-stock logic lives.
- Brakes in detail; cab and DLL-driven sound on top of the sound foundation
  (`sound.md`: `.sms` runner, miniaudio output, world sounds).

### 6. Multiplayer

See "Multiplayer: feasibility" below. The network carries train, switch and
signal state, so it builds on steps 1–4. The chunk dumper for `multdata`
and capture tooling do not depend on the simulation and can start earlier.

**Dispatcher console (Пульт ДСП)** — here, not in step 4 (see there):
- station routes and interlocking: route points (tracks, exits), routes
  between them (main track, one way, priority, composite), route
  generation, assembling a train's route;
- signal control: set aspect, automatic block, home signal, defaults;
- an HTML client over WebSocket on the same map component as Track
  control, in the console's look (grey background, black track, route
  colours, the console's icons);
- the console's forms by priority: trains (place, start, stop, reverse,
  speed, brakes, chocks), players (assign, ban, assistant, messages),
  events; later weather, electrification, cargo, situation save / load;
- RTS compatibility (Legacy on): the console and server protocol from
  `0x45D3EC`, `0x4C3DE0` and the server logs (`reverse/dispatcher.md`).

**Server "Legacy" switch** (the user's decision):

| | Legacy on | Legacy off |
|---|---|---|
| Clients | the original RTS client and openRTS | openRTS only |
| Protocol | the RTS-compatible one | the openRTS one |
| Simulation paths | RTS-compatible: 0.05 s step with one coupler sub-step, RTS physics and sound semantics, as the RTS clients have them | openRTS settings allowed (e.g. experimental coupler sub-steps) |

- **Agreed settings:** the server fixes the simulation step and the
  behaviour paths for everyone. Clients cannot pick their own.
- **Requirements this puts on the earlier steps:**
  - every behaviour openRTS changes for its own path keeps an RTS-compatible
    path (physics step, sound, signals);
  - the server can tell clients which path is in force.

### 7. Activities and full AI traffic (deferred)

Deferred until single-player driving and multiplayer work.

- `.act`/`.srv`/`.trf`: services, timetables, player train placement.
- Full AI: timetabled services, platform stops, activity events and outcomes
  (large RTS extension list in `RTSFileReference.html`).

### 8. PBR materials and models (last)

An add-on after everything else (the user's decision, 2026-11-01). Plan as
discussed: first PBR maps next to MSTS textures by naming convention
(normal, AO/roughness/metallic; tangents computed at load) with GGX shading
in the experimental path, sky reflections and sun shadows; then optional
glTF replacements of whole `.s` shapes with the same node names. The RTS
path always draws the MSTS version; the choice is visual only and never
affects simulation or multiplayer.

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
