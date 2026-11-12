# openRTS roadmap: train movement, signals, routes, multiplayer

Snapshot as of 2026-11-01, with later notes dated. "Exists" means it is in the code and tested or seen
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
| `script` | the Luau VM: sandbox, time limit; the brake distributor in Luau (`scripting.md`) |
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
- done (2026-11-04, `v0.2.0`): the world objects RTS builds or runs itself
  (`world-objects.md`): forests, level crossings with their gates, bells and
  the trains marking them, pickups (drawn, not animated), road traffic,
  water, rain and snow, diesel smoke. Not done: deer (`Hazard`), the pickup
  animation, car sounds;
- shunting signal modes: the signal system handles SHUNT/FAILED/EXCL; what
  sets a train's modes (activities' `SetSignalMode`, the dispatcher) belongs
  to step 4.

## What is needed

Current focus: single-player driving and multiplayer. Simple AI trains are
in scope as a testing tool; full AI traffic is deferred to the end (RTS's
scenarios and their traffic came early, step 7). Order follows
dependencies; items in the
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
The leftovers are done too (2026-11-01): signal modes and overrides
(`SignalSystem::set_override`, train flags for the mode its chain gives and
the heads it obeys, modes along the chain as `0x499948` / `0x50B6F4`), set
from Track control until activities and the dispatcher console use them; F7
car numbers.

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

The end of this step is release `v0.1.0`: no longer a viewer but a (nearly)
playable game. Order (the user's decisions, 2026-11-01):

0. **First understand the RTS ↔ DLL layer** (`docs/reverse/train-dll.md`,
   CONFIRMED / PROBABLE / UNKNOWN as usual), before any host code:
   - when RTS loads a DLL, and what happens on `InitRTSLibrary`,
     `DLLVersion`, `Init`;
   - each frame: what RTS fills in the shared objects before calling `Run`,
     the call order (main DLL, auxiliary libraries by `AuxCall` order,
     slave locomotives, wagons with DLLs), what `time` is, and what RTS reads
     back afterwards and when it uses it (force, brake rates, reservoirs,
     panto and main switch, lights, displays, sound triggers, messages);
   - the event calls: `ChangeLoco`, `LostMaster`, `CanWorkWith`,
     `CanSwitch` / `Switched`, `ALSN`, `SpeedLimit` — who triggers them and
     with what;
   - why it is built so (masters and slaves, multiple units over
     `SMECable` / `HVBus`, changing cabs, wagons with their own DLLs such as
     `Brakes.dll`), threading, the FPU state;
   - which objects a DLL can reach and write: the pointers between cars and
     the resolvers (`_RTSGetLocoObject`…), whether masters write into
     slaves, which fields RTS fills before a call (inputs) and which it
     reads after (outputs) — the host keeps the whole consist in one address
     space for this (`train-dll.md`, "One address space for the whole
     consist");
   - the existing notes (`reverse/legacy-dll-abi.md`, `train-dll.md`) are the
     start; the VL10k source (`vl10k_main.cpp`) shows the DLL side.
1. **DLL host:** the 32-bit `openrts-dllhost32`, the probe of all installed
   DLLs, the mirror objects and shared memory, the harness (`train-dll.md`).
2. **The `.eng` / `.wag` in full**, for both kinds of rolling stock:
   - **without a DLL** (AI trains, simple stock): traction and dynamic brake
     from the `.eng`, pantographs (raised and lowered with their animation,
     power only with a raised pantograph under a matching line voltage:
     `WorksOnLineVoltages`, `AlwaysUsePanto`, `_LowerPantoOnStop`),
     lights (MSTS `Lights` with their conditions, RTS light extensions),
     horn / bell / sanders / doors, multiple units (`SMECable`, `HVBus`,
     `NumEngines` / `EngineAxles`), all driven by default logic so an AI
     train looks and behaves right;
   - **with a DLL:** the same features driven by the DLL through the shared
     objects (`Engine` fields, `LightObj`, cab switches), locomotives and
     wagons alike (`AuxLibraries`);
   - every RTS / MSTS keyword the installed stock uses is read; the rest is
     listed by `openrts-train-check`.
3. **CabModel and the 3-D cab** from the cab `.sd` (switches, displays,
   lights, animations, click areas), one model for both DLL kinds
   (`train-dll.md`, "One cab for both kinds of DLL").
4. **Cab sound:** `RTSCabinSound` with `SoundSources`, DLL triggers and
   `VariableN`.
5. **Brakes in detail:** the "realistic air" openRTS option (off in Legacy):
   real main reservoir volume and compressor, the pipe flowing from car to
   car, release by level as a 483, a 250–300 m/s emergency wave, the
   stabiliser at its real rate (`train.md`, the brake experiment).
6. **SDK and parity:** the native plugin API (`train_plugin.h`), a C++
   wrapper, the VL10k logic ported as a native sample, the same scripted
   run through both must agree.
7. **Release `v0.1.0`:** CHANGELOG / README as a game, commit, push, tag.
   Done; `v0.2.0` (2026-11-04) followed with the world objects, train
   situations, start modes and the graphics API choice.

- 3-D cabs only: controls, instruments. No 2-D cabs and no `.cvf` (the
  user's decision, 2026-11-01).

**State (2026-11-01, done in one run):**
- 0 done: `docs/reverse/train-dll.md` (load / bind / Init, the frame,
  masters and slaves, signals, switches, host functions, adapters, cab
  input, content details).
- 1 done: `openrts-dllhost32` with probe, mirrors, adapters, shared memory,
  the harness (`train-check --dll`), tests (`openrts.dllhost`,
  `openrts.content.*`); 29 consists with DLLs run without a crash.
- 2 partly: `EngExtended` in full (`LibraryModule` + parameter,
  `EngineParameters`, `AuxLibraries`, `LocomotiveSeries`, `NumEngines`,
  `SMECable`, `HVBus`, `MaxNumSlaveLoco`), `.repaint`, the exterior
  animations and doors for DLLs, pantographs animated (from the DLL, else
  the rear one by the direction of travel). Done since (2026-11-04,
  `docs/train-systems.md`): lights without a DLL by their Conditions as RTS
  decides them, horn / bell / sand / doors keys, `AlwaysUsePanto`,
  `LowerPantoOnStop`, `RaiseSecondPantoWhenSpeedBelow`,
  `ExcludePantograph`, `PantographRaiseTime`, the sand supply.
  Done since: route electrification (`docs/electrification.md`): the line
  under each engine from the `.electrotype` / `.electrodb` as RTS finds
  it, given to the DLLs; `WorksOnLineVoltages` for engines without a DLL
  (power only on a kind of current they list).
- 3 done for legacy DLLs: the cab window (switches with hints, displays,
  DLL messages), RTS's cab keys (`AssignKey`, click / KEY / NONFIXED
  rules), the 3-D cab (the cab shape, the seat, switches and gauges on
  their nodes), the mouse on the 3-D cab (hover hint, click, wheel), Alt
  labels; the engineer on foot as RTS has him (chair keys, the window and
  head out, walking through the car and into the next section, out onto
  the ground, `docs/reverse/train-dll.md` §13). Done since (2026-11-04): DISPLAY
  state nodes and the displays' blink, cab lamps without a DLL by their
  Flags, `ALSN` with RTS's three arrays, taking another cab by walking
  into it (`ChangeLoco`), the car menu's cables, coupling repair and
  cargo, the line current, wire burning and substation load. Walking is
  the player's since 2026-11-06, not the train's: the main panel's "On
  foot" puts him on the ground under the camera with a train or without
  one; Enter at a cab door of his train gets in (in a network game, of
  any of his trains).
- 4 done: `RTSCabinSound` of the player's engine with the DLL's cab
  triggers and Var1..8; the camera kind for SMS activation.
- 5 done: `BrakeModel::realistic` (Train window, off by default; trains
  with a legacy DLL keep RTS's air).
- 6 done: the API, the C++ layer, the loader, a sample and the parity
  test; the `ORTSTrainPlugin` key in the game (cab, keys, mouse, gauges);
  the VL10k logic as a native example (`sdk/samples/vl10k`, one section,
  no parity run against the DLL: the user's call, 2026-11-04).
- 7 done: `v0.1.0`, then `v0.2.0` and `v0.3.0` (2026-11-04).

- This is where most RTS rolling-stock logic lives: in the DLLs, with
  `ALSN` (cab signalling) and `SpeedLimit` callbacks.

### 6. Multiplayer

See "Multiplayer: feasibility" below. The network carries train, switch and
signal state, so it builds on steps 1–4. The chunk dumper for `multdata`
and capture tooling do not depend on the simulation and can start earlier.

**Order (2026-11-04).** Steps 1–5 are done, and there is no network code
yet. Each stage ends with something that runs and is checked:

1. **The RTS protocol without a network** — done (2026-11-05, `net.md`):
   `openrts_net`, `openrts-net-dump`, the viewer's `--mp-snapshot`.
   - What: a library `openrts_net`: the framing (`u16` id, `u32` length,
     nested packages), a reader and writer, and the packages of the startup
     snapshot. Then the snapshot itself: time, junctions, signals,
     electrification, pickups, trains with their cars (`0x4607`,
     `0x4603` / `0x4604`, cargo), players.
   - Reverse first: the payloads the client reads (`0x4C3DE0`,
     `0x4CAEAC`).
   - Check: a tool prints `RTS/multdata` package by package; the viewer
     loads a world from it (time, junctions, signals, trains placed),
     still offline.
2. **openRTS as a client of the original `rts_server.exe` (Legacy).**
   - What: TCP, the login `0x4600` (version 101) and its replies, the
     network thread with a queue into the main loop.
   - In play:
     - the player's train goes out as `0x4603` / `0x4604` every 1.5 s;
     - other trains come in as remote trains: no physics of their own,
       positions from `0x4603`, extrapolated, stopped after 5 s without an
       update;
     - the server decides junctions, signals and time;
     - car menu actions, couplings (as RTS does them,
       `reverse/multiplayer.md`), messages, pause, "Server down".
   - Check: on one PC with `rts_server.exe`, an `rts1.exe` and openRTS see
     each other's trains, as the test of 2026-11-03 did with two `rts1.exe`.
   - Done (2026-11-06, `docs/net.md`): the observer, own trains with their
     DLLs, look and sound both ways, couplings and cuts, taken-away trains
     frozen, the pause, speed post categories and the line from the server.
     Checked live with an `rts1.exe`, the couplings offline.
3. **The openRTS server.** Legacy on is done (2026-11-09, `server.md`):
   `openrts-server` takes `rts1.exe` and openRTS clients as
   `rts_server.exe` does, its login counts the live server's; it decides
   junctions, runs the signals' scripts with every train on its copy of the
   network, carries out couplings and cuts, and its console is the
   dispatcher's (trains, assignments, junctions, signals, time, pause,
   messages). Checked with two openRTS clients; `rts1.exe` on it not yet.
   **Next (the user's call, 2026-11-09): the server as `rts_server.exe`,
   Legacy first, no extras.** `rts_server.exe` is itself the dispatcher's:
   its start form (route, season, weather, time, port), then its map
   console (`TfPult`, `reverse/dispatcher.md`). The remote console
   (`rts_server_DSP.exe`, `0x4500`, and openRTS's in the game, hidden until
   then) comes after. In order:
   1. What `rts_server.exe` does that openRTS's does not, read from it (on
      a copy): the signals ahead of a train with no route (in RTS the route
      the dispatcher gives, `0x4619`, looks like the train's authority:
      openRTS opens them, a placed train's exit signal cleared by itself on
      2026-11-09), who gets `0x4619`, what each console action sends, the
      station routes (`0x450B`).
   2. Track control as a module of its own (the map: track, junctions,
      signals and their names, places, trains, routes; actions as
      callbacks), for the viewer, the server's console and later the
      remote one and the scenario editor.
   3. The server's window: the start form and the map console on that
      module, with `rts_server.exe`'s actions (trains: place, assign,
      route, start / stop, remove; signals; junctions; players: message,
      kick, admit; time, pause; the log). The text commands stay for runs
      without a window (`--run`, `--command`).
   4. `rts1.exe` on it, live.
   Left after that:
   - One program for players and dispatchers, the Legacy switch and its
     rules, the agreed settings for everyone.
   - Coupling of two players' trains decided by the server in one step
     (Legacy off).
   - **Server plugins in LuaJIT** (planned, 2026-11-06): the server's admin
     installs them by hand, a folder each (`plugins/<name>/`), as Minecraft
     servers take theirs; full access, no sandbox, as a Java plugin has
     there. Events (a player joins or leaves, a train assigned, coupled or
     split, a message, a timer) and actions (messages, assigning trains,
     junctions and signals, pause, kick). What they do goes out as the
     packages already reversed, so `rts1.exe` clients see it too.
4. **Helpers** through the relay (the scheme tested on 2026-11-03), and
   for openRTS clients over the normal connection.
5. **The dispatcher console** — in openRTS itself, on Track control (the
   user's choice, 2026-11-09, in place of an HTML client); its first
   version is there, hidden until the server (stage 3) is done
   (`server.md`, "The dispatcher's console"). Left: the
   station routes of the route database, signal modes (automatic block),
   brakes and the other forms, RTS compatibility (`rts_server_DSP.exe`).
6. **Panels and forms** (planned, 2026-11-06): RmlUi (HTML/CSS-like
   markup, MIT) drawn by the renderer, with the logic in Lua; Luau is the
   candidate for client scripts and UI (built for a sandbox). Players and
   servers make their own panels with simple logic, as Garry's Mod's VGUI:
   a server plugin can send one to a player and get its buttons back as
   events, e.g. role play forms. Client scripts only ever run in a sandbox:
   no files, no network, never driving the train.

**Still to reverse for it.** Every package is catalogued with its payload
(2026-11-05, `reverse/multiplayer-protocol.md`). Left UNKNOWN:
- the first optional block of `0x4638` (a derailed car's placement) and
  three values of a `0x463B` stream;
- two flags of a station route segment (`0x450B`) and whether its priority
  does more than order the queue;
- why relayed coupler actions can miss on another client;
- a capture with the server's full data trace would check the counts and
  periods. The first live capture (2026-11-05, through the proxy) confirmed
  most of the catalogue and corrected a few fields
  (`reverse/multiplayer-protocol.md`, "Live capture"); what openRTS should
  do better is in `reverse/multiplayer.md`, "For openRTS".

**What the client lacks** (by stage 2):
- remote trains (above) with their lights (`0x4617`), pantographs
  (`0x463C`) and sounds (`0x4639` / `0x463D`, the whole state `0x463A`
  when the train loads), `reverse/multiplayer.md`, "Other players'
  trains";
- the world from the server, not its own: signal aspects (there is an
  aspect override to build on), locked junctions, time and pause, brake
  pressures of other trains (`0x4613`), electrification, weather areas
  (`0x4634`; openRTS has one weather for the whole route);
- the cab snapshot for a helper (`0x4624`).

**Dispatcher console (Пульт ДСП)** — here, not in step 4 (see there):
- station routes and interlocking: route points (tracks, exits), routes
  between them (main track, one way, priority, composite), route
  generation, assembling a train's route;
- a train's direction for its route: a button "reverse the direction" (a
  button, not a checkbox; the user's choice, 2026-11-05). RTS builds the
  route the way the train last moved, from `0x4603`: the reverser is not
  sent, so after a stop the dispatcher had to have the train nudged back;
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

**Helpers without open ports** (tested 2026-11-03, `reverse/multiplayer.md`):
- **RTS clients (Legacy on).** The openRTS server relays the helper link. Both sides are told to connect out to it, the engineer only once per helper. Proven with two original `rts1.exe` and a proxy in front of `rts_server.exe`.
- **openRTS clients.** There is no peer link at all: the helper's packages go over the normal server connection.

**A fun idea: the openRTS protocol tunnelled through an RTS server.**
- **The channel.**
  - An original `rts_server.exe` relays `0x461D` / `0x463F` to everyone unread.
  - Original clients drop them when the leading 20 bytes name no world object (`reverse/multiplayer.md`, "A data channel between openRTS clients").
  - So openRTS clients on an RTS server could talk to each other in their own protocol, unseen by RTS players.
- **One interface, two transports.** A `PeerChannel` interface would have two implementations:
  - `NativeChannel` on an openRTS server;
  - `RtsTunnelChannel` (`new RTSLegacyKostylChannel()`) on an RTS server: a marker and version, the receiver inside, splitting and zstd, a small byte budget (the server copies every package to every player).
- **What it could carry.** Only things that add to RTS, never what the RTS server decides:
  - smooth positions of each other's trains (5–10 Hz against RTS's 1.5 s);
  - realistic air;
  - our car menu cables;
  - chat.
- **Fallback.** If the tunnel stops working, the client stays a plain RTS client.

Not planned yet; tested by the code reading only.

### 7. Activities and full AI traffic (started 2026-11-10)

Deferred until single-player driving and multiplayer work, except RTS's
scenarios and their traffic trains, taken up early (2026-11-10) so that the
community's scenarios can be played.

- `.act`/`.srv`/`.trf`: services, timetables, player train placement.
  Started 2026-11-10 with RTS's scenarios (`RTS_ACT/*.act`,
  `docs/activities.md`): read whole; chosen in the start menu; the
  player's and the standing trains put on the track, the player's start;
  the events' engine after RTS's activity.pdf (messages, F8 reports,
  signal overrides, service changes, wagon states, the scenario's sounds,
  end valves).
- Traffic trains after RTS's model (`docs/traffic.md`,
  `docs/reverse/traffic.md`), done 2026-11-10: one rigid body on its path,
  no per-car physics, RTS's speed controller (the least of the limits
  times `TrainEfficiency`, `TrafficLimitsTable` curves with RTS's default
  table, the braking curve to the strictest target); started at
  `TrainStartTime`, removed at the path's end or `TrainRemoveTime`; block
  points in the dispatcher (service and global, `ReleaseBlockPoint`,
  `EnableBlockPoint`, `ReleaseOnTime` / `OnReach` / `OnApproach`); the
  player's path reserved; Track control's AI trains run the same way.
  Next: `SERIAL` starts, `SpawnTrain`, timetables and platform stops.
- **Traffic on the server too** (the user, 2026-11-10): the traffic logic
  lives in `simulation/` with no viewer types, stepped by one call the
  server can make as well, so a multiplayer server runs the scenario's
  traffic and the clients only show it.
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

### 9. openRTS's own tier (the final step, after RTS; plan, 2026-11-08)

The user's direction once RTS compatibility is as complete as it gets (it
is close: going further means rules `rts1.exe` never had). One engine, two
tiers of content:

- **The MSTS/RTS tier.** RTS routes and stock run by MSTS/RTS rules,
  faithful to `rts1.exe` (reverse engineering, as everything above). It
  stays as it is; nothing of the tier below changes it.
- **The openRTS tier.** Closer to Train Sim World or Trainz 2022 (not a
  copy): better models and graphics, track development, infrastructure. An
  RTS route is described almost entirely by MSTS/RTS rules; an openRTS
  route has the improved models, graphics, track and infrastructure.

Both run on the same core: the track network, signals, the dispatcher, the
physics, the network game. The core already works on an abstract track
network, not on the TDB, so an openRTS route only builds the same network
from its own files.

What the openRTS tier means:

| Area | MSTS/RTS | openRTS |
|---|---|---|
| Models | `.s`, textures with light painted in | glTF from Blender, PBR materials, proper levels of detail |
| Track | `tsection.dat` sections, the track written twice (`.w` + TDB) | splines with transition curves and cant; the track graph the one source, rails, sleepers and ballast built from it |
| Infrastructure | mast and wire shapes placed by hand | data tied to the track: catenary generated along electrified sections, signals on the graph with their logic |
| Terrain | a coarse grid, a texture per patch | finer heights, blended materials, procedural grass and trees (GPU grass instead of 2-4 BlendATexDiff quads), roads as splines |
| Rolling stock | `.eng` / `.wag` + DLL | the car as an entity with modules (`## Cars as entities`), PBR models |

- **The track is the big gain.** The hardest part of an MSTS-style route
  editor, keeping `.w` and the TDB in step, goes away: an openRTS route
  editor (on the inspector, `docs/inspector.md`) is far simpler than a
  clone of the MSTS one. Train and cab editors build on the same base.
- **Hybrid on the way:** an RTS route plus an openRTS layer in a mod
  (grass masks, PBR maps beside its textures, replaced models, the
  inspector's `.openrts` overrides): existing routes improve step by step
  without being rebuilt. Step 8's PBR is this layer for models.
- **Multiplayer:** openRTS-tier content is for openRTS clients only, the
  server's Legacy-off mode (`net.md`).
- **Order, when it starts:** the glTF + PBR model pipeline in the
  experimental renderer first (stock, scenery and infrastructure all need
  it); GPU grass at any time (it needs no new content); then the openRTS
  track and infrastructure on splines with the route editor.

## Left to do outside multiplayer (2026-11-04)

None of it blocks multiplayer.

**Cab and DLLs:**
- screens: close-up scenes, LINK scenes, `CopySubObjects`, the lighting of
  the `…Diff` shaders;
- `DisplayFade`: lamps and displays switch, RTS fades them;
- doors by name for the DLLs;
- native plugins only on the driven engine, not on the other engines of a
  multiple unit; the VL10k example has one section, no regenerative
  braking, no КЛУБ / САУТ;
- track items: a level crossing's `Time` / `Distance` are 0 (RTS's
  runtime fill not traced); the walk's mode and the train's length
  reference are PROBABLE (`reverse/train-dll.md` §19).

**Sound:**
- done (2026-11-12): RTS's volume law (30·log10, `sound.md`); the panning
  no longer takes 3 to 6 dB off every sound outside;
- a salon's own sound (`SoundInt`, one installed wagon);
- where RTS puts the listener while the head is out of the window;
- route ambient sounds (`ingame.sms`), the default signal and tower sounds
  of the `.trk`.

**Rolling stock and the world:**
- a report of the `.eng` keywords openRTS does not read
  (`openrts-train-check`), promised in step 5;
- how RTS connects the hoses when trains couple (not traced);
- RTS's solver for derailed trains (a simpler folding rule stands in);
- Dyntrack, deer (`Hazard`), the pickup animation, car sounds, drawing the
  wires from the catenary types.

**Later, planned:** Lua plugins and replaceable train systems, cars as
entities (both below), full AI traffic (step 7), PBR (step 8).

## Train plugins in Lua and replaceable train systems (plan, 2026-11-04)

Not started: the user's plans and what was worked out with them. Nothing
here is decided beyond that.

**Why.**
- Authors without a C++ compiler or a 32-bit DLL.
- People who know the real hardware: a Metrostroi developer wants to put the
  real ЭР9 circuit in.

### Lua plugins (Luau, 2026-11-12)

**The language (the user's call, 2026-11-12): Luau, for everything** —
train plugins, server plugins and client panels alike, one VM and one set
of bindings (`script/`, `docs/scripting.md`). Its sandbox and its interrupt
(a script that hangs is stopped) are built in; speed is no issue at these
loads. LuaJIT, planned before, is dropped. Done so far: the VM with its
sandbox and time limit, and the first module, the brake distributor
(`ORTSBrakeDistributor`) with its stand (`openrts-brake-stand`). The text
below still says LuaJIT where it was written for it; the plan carries over.

**How it fits:**
- one more implementation of the plugin interface (`train_plugin.h`), as
  the legacy bridge is. The simulation, `CabModel`, sound and network do not
  change;
- in the `.eng` the same key: `ORTSTrainPlugin ( "logic.lua" )`, chosen by
  the extension and read through the VFS (packs and mods work);
- one `lua_State` per car. Cars talk only through the host's train wires,
  never through shared globals.

**The API as authors see it** (a Lua layer over the C ABI):
- PascalCase names as in `ts.h` (`SetSwitch`, `TrainPipeRate`,
  `PostTrigger`, `ShowMessage`), so DLL logic such as `vl10k_main.cpp`
  ports almost line by line. What `ts.h` lacks gets names in the same
  style: `Train:Wire(n)`, `Track:Items(5000)`, `Param("MaxForce", 0)`;
- events are plain functions, only the used ones need to exist: `OnInit`,
  `OnStep`, `OnSwitchRequest`, `OnSwitched`, `OnSave`, `OnLoad`;
- properties instead of calls: `Engine.Force = …`; `Loco.Speed` is
  read-only. Writing a read-only or unknown field is an error with the file
  and line;
- cab controls by name or by the `.sd` ID. An unknown name fails at load and
  lists the cab's names;
- RTS units (m/s, N, atm), UTF-8 strings (Russian messages just work);
- a definitions file for the Lua Language Server, for completion and hints
  in the editor.

```lua
function OnStep(dt)
    local notch = Cab:Switch("C_MAIN")
    Engine.Force = notch * 30000 * Engine.Reverse
    Cab:Display("AMMETER").Value = Loco.LineCurrent
    if Loco.Speed > 25 and not overspeed then
        overspeed = true
        Sound:Post(120)
        ShowMessage("Превышение скорости!")
    end
end

function OnSwitchRequest(name, from, to)
    return not (name == "K_STR" and Loco.Speed > 0.5)
end
```

**For a real circuit (ЭР9 class)** — part of the first version, not added
later:
1. plugins on every car, `.wag` too: the motor, trailer and head cars each
   run their part;
2. train wires by number over the connections between cars (the СМЕ cable
   of the car menu, which already connects and parts). A car writes a wire
   and the others read it. Reads see the previous sub-step, so the order of
   the cars does not matter;
3. sub-steps: a script can ask for `StepRate = 200`, and the host calls it
   several times per simulation step (a 0.2 s relay is too coarse at 50 ms);
4. `require` from the train's folder through the VFS, for code in many
   files and modules shared by the cars;
5. a library of parts: a relay with on and off delays, a contactor, a DC
   traction motor, a compressor, a battery, a fuse. Authors can write their
   own;
6. debugging:
   - a viewer window with the live relays and wires the script publishes
     through `Debug`;
   - a log;
   - reloading the script on the fly, keeping its state through `OnSave` /
     `OnLoad`;
7. the link to the physics exists already: the line voltage, the current
   back into the contact network (substation load, wire burning), the car's
   force, the brakes through `TrainPipeRate` and ЭПТ, the cab, a sound for
   every relay click.

**Safety:**
- a sandbox by default: no `io`, `os` or `ffi`, `require` only from the VFS;
- the JIT and full access only for packs the player trusts (a switch per
  pack). LuaJIT's instruction-count hook does not fire inside compiled
  code, so a hang there cannot be stopped cleanly. Untrusted scripts run
  interpreted with a limit, which is fast enough at 20 steps per second;
- an alternative if the sandbox matters more than the speed: Luau, which has
  the sandbox and the interrupt built in. Not chosen.

**Multiplayer:**
- the step runs on the owner's machine and its outputs are synchronised, as
  for native plugins;
- `OnSave` / `OnLoad` serve joining mid-session;
- a helper gets the cab's state, not the whole circuit.

**The C ABI needs, for native plugins too:**
- `save` / `load` (in the principles of `train-dll.md`, not in the ABI yet);
- plugins on wagons;
- train wires and sub-steps;
- track items;
- debug values.

**Licences.**
- LuaJIT is MIT, compatible with GPLv3.
- Code brought from Metrostroi: check its licence first. The approach
  carries over freely; the code not necessarily.

**Work:**
- LuaJIT in the CMake build (`FetchContent` and its MSVC build);
- `trainplugin::LuaLocomotive`, the bindings, the sandbox;
- a sample (the VL10k or the fake engine in Lua);
- a parity test against the native version, as for the DLLs;
- docs.

### Replaceable train systems

The user's plan ("пока что просто в планах"):

| Car | Logic | Air brakes | Physics |
|---|---|---|---|
| ordinary wagon | standard MSTS | RTS | RTS |
| engine driven from the keyboard | MSTS + keyboard | RTS | RTS |
| engine with an RTS DLL | the DLL | RTS: the DLLs' valves are tuned to RTS's distributor (`reverse/air-brakes-real.md`) | RTS |
| engine with our plugin (native or Lua) | the plugin | per module: RTS or the author's own | RTS, with forces from the modules |

What follows from it:
- **RTS is a module too.** RTS's distributor, its traction and the keyboard
  driver's valve implement the same interfaces as an author's module. They
  are not hard-wired.
  - Choosing is choosing a module.
  - An author's module is checked by the same parity tests.
  - Legacy means RTS modules everywhere.
- **What belongs to the whole train stays there.**
  - The coupler solver is one per train. Cars give it their forces:
    traction, braking, extra resistance, later their own adhesion. A plugin
    does not replace the solver.
  - The brake pipe flow between cars is the train's model (RTS or the
    realistic one; both exist).
  - The devices in a car are its modules: the driver's valve, the
    distributor, the reservoirs, the cylinder. They take air from the pipe
    and give it back at their point. This is what lets a mixed train work:
    an author's valve with ordinary wagons on RTS distributors, or an own 483
    on some cars only.
- **Where modules are named:**
  - a plugin declares which modules it takes ("my air, my traction"); the
    rest keeps the default;
  - perhaps also in an ordinary `.wag`, so an own 483 works on a freight car
    behind any engine. This extends the first row; the user's decision.
- **Multiplayer:** the owner computes, the others see the outputs. Whether
  authors' modules are allowed under Legacy is a server setting.

## Cars as entities (a step of its own; plan, 2026-11-06)

Not started: the user's plan for a proper simulation, to be done as a step
of its own. The design under discussion: `car-architecture.md`. It comes before activities. The Lua plugins and the replaceable
train systems (above) build on it: their modules are a car's.

**Why.** A car's state lives in many places today, and every fix has to tie
them together by hand:
- the physics: `simulation::Train`, by index;
- lights, pantographs, outer doors, sound: `TrainView::Car`;
- the cab: `CabView`, which until 2026-11-06 lived only while the engineer
  was in it, so its doors shut behind him. Now cabs are kept by car key in
  `board_cabs_`, a patch.
- switches and the DLL's state: the DLL host's car mirrors;
- the car key and the network's look: `NetSession`'s records, the key
  carried through couplings by hand;
- getting on: decided in two places (the engineer's own cab, other cabs).

**What:**
- **A car is an entity.** It has a stable id (today's car key) and is made
  once with the car. Couplings, cuts, giving a train away and taking it
  back keep it. It owns its state and its logic:
  - body and running gear (the link to the physics);
  - doors (one state, seen from inside and outside), windows;
  - lights, pantographs, couplers and hoses, sounds;
  - the interior.
- **The cab is the car's.** Its switches, doors, windows, chairs and lamps
  are the entity's state. `CabView` only draws them and picks in them. The
  engineer is in a car, in one of its spaces.
- **Spaces and doors as RTS has them.** The `.sd` `Connects` areas (CAB,
  TAMBUR, SALON, EXTERIOR, WAGON_FRONT / WAGON_BACK) are a car's spaces. A
  door joins two of them. Passing it moves the engineer between spaces and
  cars, as RTS's passages to a cab, the train or outside
  (`reverse/train-dll.md` §13).
- **Logic as the entity's modules:**
  - the RTS DLL (the host keeps its state, the entity plugs it in);
  - a native plugin, later Lua;
  - the default logic.

  The replaceable train systems (brakes, traction) are such modules.
- **Standard classes** with default logic. The car's kind picks one
  (`.wag` / `.eng` type, RTS type); a plugin may replace or extend it:
  - freight car: brakes, handbrake, load and cargo, doors and hatches;
  - passenger coach: compartments (купе, плацкарт, seated), interior light,
    heating, doors; later passengers;
  - multiple-unit cars: head, motor, trailer (in the ED9M the whole car's
    interior is one cab shape);
  - locomotive: cab(s), traction, auxiliaries;
  - tender and the rest.
- **The train** is an ordered list of entities plus what belongs to the
  whole train: the coupler solver, the brake pipe, the train wires (СМЕ).
  Couplings and cuts move entities from one train to another.
- **Network.** An entity's state is what goes per car (look, sound,
  doors). Its owner computes it.
- **Content stays as it is.** The entities are openRTS's inside, not a new
  content format:
  - `.eng` / `.wag` / `.sd` / `.con` are read as now; a car's class comes
    from what they already say (its kind, RTS type, a cab or none);
  - RTS DLLs run through the same host and ABI (`ts.h`);
  - an author does something only to use what goes beyond RTS: an own
    module, a compartment class with its own logic.

  Under Legacy everything behaves as RTS's, which `rts1.exe` on the same
  server needs anyway.

**Work, roughly:**
1. The entity type and where the entities live.
2. `TrainView::Car`'s state and the cabs move into it.
3. The train as a list of entities.
4. The standard classes.

At each step, parity with today's behaviour: DLLs, sound, the network.

## Multiplayer: feasibility

How RTS does it is in `reverse/multiplayer.md` (2026-11-03):
- the client;
- the player mini-server inside `rts1.exe`;
- the helper peer link;
- the dedicated server with the dispatcher console;
- why the dispatcher cannot join `rts1.exe`.

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

## Viewer interface language (2026-11-01)

The viewer's interface is in Russian by default, English selectable in the
main window and remembered in `viewer_state.ini` (`language`). Strings stay
English in the code and go through `tr()` (`viewer/src/ui_text`): the key is
the English text, a `label##id` keeps its id, unknown text passes through.
New UI text needs a line in the Russian table there. MSVC compiles with
`/utf-8` so the Russian literals stay UTF-8.
