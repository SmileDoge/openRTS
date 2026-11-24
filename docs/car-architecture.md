# Car and train architecture (design)

Status: a design under discussion (2026-11-06). Step 1 (`CarId`) is built
(2026-11-23, "Step 1: CarId" below), step 2 too (the doors, the lights,
the pantographs: "Step 2: ..."), step 3 ("Step 3: the cab's state"), step 4 ("Step 4:
modules"); the rest is not.
- The plan it serves is the roadmap step "Cars as entities"
  (`roadmap.md`); the replaceable train systems there build on it.
- **Proposed** marks what this document suggests; the user's questions are
  at the end.
- Existing content and RTS DLLs work unchanged (see "Goals").

## Why: where a car's state lives today

| Where | What it holds about a car | Notes |
|---|---|---|
| `simulation::CarState` (`Train::cars()`) | physics, air (pipe, reservoirs, cylinder), handbrake, couplers, hoses, cables, valves, chocks, body sway | by index: the index changes with every coupling and cut |
| `TrainView::Car` (`viewer/src/train_view.cpp`, about 5500 lines with the rest) | its definition and key, the shape and its loading, pantographs (wanted and drawn), outer doors, lights, freight, the car, cab and aux sounds, the network's look and sound (`remote_*`, `net_*`) | the simulation's state mixed with drawing and audio objects |
| `TrainView::Unit` | the train, its role, category, AI driver, DLL host flags, "remote" | |
| `CabView` | the cab's switch poses, doors, windows, collision, screens | lived only while the engineer was in it (its doors shut behind him) until 2026-11-06; now kept by car key in `board_cabs_`, a patch |
| the DLL host (`dllhost`, `CarMirror`) | the DLL's state, the switches, `Init` once per car | by car key; parked when a car leaves the host |
| `NetSession` | the server's train records, car keys, own and remote trains | the key carried through couplings by hand |
| `Engineer` | where he is: mode, feet, car index | the car by index again |

What came of it, only on 2026-11-06:
- **Doors.** A cab change shut the doors of the cab left.
- **Boarding.** It was written twice: the engineer's own cab and other cabs.
- **Unread cabs.** A cab not read yet showed every door open.
- **Car keys.** They had to be carried through the network's couplings by
  hand, or a car's DLL was lost.

## Goals

- **One place per car.** Its state lives in one place under one stable id.
- **Logic apart from data.** The logic comes in replaceable modules: RTS's,
  a plugin's, Lua's (the roadmap's "Replaceable train systems").
- **No window needed.** The simulation runs without one: the openRTS server
  runs the same code.
- **Multiplayer.** A car's owner computes it, the others receive its state.
- **Legacy: as RTS.** The 0.05 s step, the RTS DLL ABI (`ts.h`), what
  `rts1.exe` sees on the same server.
- **Content as it is.** `.eng` / `.wag` / `.sd` / `.con` are read as now.
  An author does something only to use what goes beyond RTS.

Not goals:
- a general ECS engine (hundreds of cars, not millions);
- a new content format;
- a new DLL ABI.

## The proposed model

### The car (proposed)

- **`CarId`:**
  - stable and unique in the game: today's car key;
  - made once with the car;
  - kept through couplings, cuts, a train given away and taken back,
    situations;
  - RTS's car number (`train << 18 | record << 10 | UiD`) stays a field
    for the network.
- **Definition:** what the content says (`.eng` / `.wag`, its `.sd`, the
  consist entry). Read once, never written.
- **Components:** plain data, each present when the car has it.

  | Component | Holds |
  |---|---|
  | body | flipped, the link to the train's physics (placement, velocity) |
  | couplers | both ends: coupler state, hoses, end valves, cables (СМЕ, high-voltage bus), chocks |
  | brakes | pipe, reservoirs, cylinder, distributor state, handbrake |
  | pickups | pantographs wanted and raised |
  | doors | every door of the car, outer and inner (`.sd` `ExteriorDoors` and `InteriorDoors`), one state each: the car's shape and the cab's draw the same door |
  | lights | each light's state and fade |
  | interior | the spaces and the doors between them (below) |
  | cab | the desk's switches, displays, windows, chairs, lamps |
  | cargo | loads and their shapes |
  | sound | triggers and variables as data (the audio objects stay in the view) |

- **Modules:** the logic, attached to the car in an order.
  - A module reads inputs and the components and writes components:
    `step(car, train, dt)` plus events (a switch, a key, a coupling).
  - The RTS DLL module adapts the DLL host. The DLL's own state stays in
    the host process; what it shows (switches, outputs) is mirrored into
    the components.
  - Native plugins, later Lua (the roadmap's plan).
  - Default modules: MSTS logic, the keyboard driver, lights, doors,
    pantographs without a DLL.
  - System modules: brakes, traction, as the roadmap's "Replaceable train
    systems".
- **A class is a preset:** which components and default modules a car gets,
  chosen from its data. Not a C++ inheritance tree; a plugin swaps modules
  without subclassing.

### The train (proposed)

- An ordered list of `CarId`, each with its orientation.
- What belongs to the whole train:
  - the coupler solver and the physics arrays it runs on;
  - the brake pipe's flow between cars;
  - the train wires: СМЕ, the high-voltage bus, control circuits.
- **Couplings and cuts** merge and split the lists. A car's components are
  not touched, so nothing is carried by hand.
- **Roles** stay the train's: driven, AI, standing, remote.

### Layers (proposed)

| Layer | Holds |
|---|---|
| `simulation` | the car store (`CarId` → car), the trains (lists of ids), the step; no drawing, no audio objects |
| `trainplugin` | module adapters: the DLL host, native plugins, Lua |
| `viewer` | a car view per car: its model, the cab model, sounds, picking; built from the components, keeps nothing the simulation needs |
| `net` | the components' replication; under Legacy RTS's packages map onto them |

The openRTS server takes `simulation` and `trainplugin` without `viewer`.

### The step (proposed)

Under Legacy every 0.05 s, the same order each time (determinism for
multiplayer and for the parity tests):
1. Inputs: keys, the mouse on the cab, the network.
2. The cars' modules (a DLL's `Run` among them), in train order.
3. What is the train's: train wires, brake pipe, couplers.
4. Physics.
5. Outputs: what changed per car goes to the view, the sound and the
   network.

### Ownership and the network (proposed)

- Every car has an owner: the client driving its train, else the server
  (AI, standing trains).
- Only the owner runs the car's modules. Today's frozen DLLs of a train
  given away (kind 2) become the general rule.
- Components carry change marks; what changed is sent. Under Legacy RTS's
  packages map onto the components:
  - look and lights: `0x4617`;
  - pantographs: `0x463C`;
  - sound: `0x4639` / `0x463D`, the whole state `0x463A`;
  - doors and brakes: as `net.md` describes.

### Interior and the engineer (proposed)

- **Spaces** are the `.sd` `Connects` areas: CAB, TAMBUR_*, SALON, TUALET,
  EXTERIOR, WAGON_FRONT / WAGON_BACK. In the ED9M the whole car's interior
  is one cab shape with several spaces.
- **Doors** join two spaces. A coupling joins WAGON_BACK of one car with
  WAGON_FRONT of the next.
- **The engineer is an occupant** at (`CarId`, space, position); later
  passengers are occupants too.
- **One rule for every door**, as RTS (`reverse/train-dll.md` §13): Enter
  works the door, Ctrl+Enter goes through. Getting on, getting off and
  passing to the next car become one passage.
- **The cab driven from** is the cab space with a desk he entered. Entering
  it is the module change (`ChangeLoco`), as RTS's `0x436734`.

### Standard classes (proposed)

Chosen from the data; a plugin may replace a module or add one.

| Class | Chosen by | Components | Default modules |
|---|---|---|---|
| locomotive | `.eng` with an engine, not in a multiple unit | cab(s), pickups, brakes, couplers, lights, doors | its DLL, else the keyboard driver and MSTS logic |
| multiple-unit car: head, motor, trailer | the car's RTS type, `EngExtended`, the consist | as its kind; the head with a cab | the unit's DLL in each car that has one |
| passenger coach | `.wag` of the passenger kind | doors, interior (compartments), lights, heating, brakes | default |
| freight car | `.wag` of a freight kind | brakes, handbrake, cargo, doors and hatches | default |
| tender and the rest | their kind | as needed | default |

## Moving there

One step at a time; at each, today's behaviour stays, checked by:
- the DLL parity tests;
- the offline sound tests;
- the fake server's network scenarios;
- the viewer's `--capture-*` hooks (`--capture-board` and the rest).

Steps:
1. **`CarId` everywhere** (the car key exists). A car store in
   `simulation`; trains refer to ids. The viewer is unchanged.
2. **Doors, lights, pantographs** leave `TrainView::Car` for components;
   the viewer reads them.
3. **The cab's state** goes into the car; `CabView` keeps only drawing and
   picking; `board_cabs_` goes.
4. **Modules:** the DLL adapter, the keyboard driver and the defaults
   behind one module interface.
5. **The step** moves into `simulation`: `TrainView` stops driving it, and
   the server can run it without a window.
6. **Interior:** the graph and occupants.
7. **Standard classes.**

### Step 1: CarId (built 2026-11-23)

Architecture only: physics, doors, brakes, cabs, the DLLs and the network
protocol behave as before.
- **`simulation::CarId`** (`car_registry.hpp`) is the car key the viewer
  always kept (one u32, 0 none): no second id system.
- **`CarRegistry`**, one per world: the viewer's (`TrainView`) and the
  server's (`GameServer`), never shared.
  - `create()` gives an id never given in that world; ids are never used
    again, not after the car is gone either. Running out (2^32 - 1) throws
    `CarIdsExhausted`.
  - `release()` retires a car's id when the car is gone for good: a train
    deleted, every train replaced (a saved game, a situation, a consist
    loaded), a preview closed, a train that could not be made. A retired id
    is found no more.
- **Trains:** `CarSetup::id` and `CarState::id`. The id lives in the car's
  state, so a cut, a coupling (a train standing tail first included) and a
  turn move it with the car; `Train::car_ids()` is the ordered list,
  `index_of()` finds a car. The viewer checks at each coupling and cut that
  its cars' keys are the train's ids, in order (an error line once if not).
- **Saved games:** keys are saved as before (the car line's 7th field).
  Loaded into a world that never gave them they stay; else they map to new
  ids (`restore_saved_keys`), the DLLs' states and the trains' first keys
  following. A save from before the keys gets new ids.
- **Network:** identity is the server's uid. The client maps
  `server uid -> CarId` (`for_server_uid`): connected again, it finds the
  cars it has; ids need not match between computers. A uid repeats with a
  train number, so a uid another live record holds gets a new car. A train
  deleted on the server releases its cars; one only taken off keeps them.
- **The DLLs:** slots stay positional, in the train's order and direction;
  the host follows cars by key as before.
- **Pointers:** a car's cab freed (`drop_board_cab`) clears the listener's
  and the rain's pointers to it.
- **Tests:** `openrts.car_registry` (create, release, no reuse, running
  out, saves with and without keys, the server's uids and reconnecting);
  `openrts.train` `test_car_ids` (a cut in the middle and back, a flipped
  car, a train tail first, a turn); `openrts.server` `car_identity` (add,
  couple, cut, couple back, delete: ids kept, then retired, never reused).

### Step 2: doors (built 2026-11-23)

Architecture only: the doors open, shut, sound and go over the network as
before.
- **Components** (`car_components.hpp`): `CarComponents`, kept by
  `CarRegistry` by the car's id (`components(id)`, `find_components(id)`).
  Made empty when first asked, they stay where they are until the car is
  released, and go with it.
- **`CarDoors`**: the car's outer doors (the `.sd`'s `ExteriorDoors` with a
  node of their own): name, `.sd` index, GlobalID, AnimTime, how far open,
  the engineer's wish from outside; every door's GlobalID in `.sd` order
  (the DLL's door order); the groups another computer sends open (0x45FC).
  `step()` moves a door, `open_groups()` is what goes out.
- **The viewer** keeps only what draws a door: its shape node and last
  frame (`Car::door_nodes`, door k is node k). `doors_of(car)` reads the
  component; a train made anew (a train put back) starts its doors shut, as
  before.
- **Still the viewer's:** what decides a door's target (the DLL's door
  states, the cab's doors, the Y / U keys, a helper's cabs) waits for the
  modules (step 4); the cab's InteriorDoors stay the cab's (step 3).
- **Tests:** `openrts.car_registry` `components` (lifetime, doors' pace,
  groups); `openrts.train` `test_car_ids` (a car's doors with it after a
  cut); the viewer's door pick, boarding and passage runs printed the same
  before and after.

### Step 2: lights (built 2026-11-23)

As the doors: the lights shine, fade and go over the network as before.
- **`CarLights`** (`CarComponents::lights`): each light's runner (its
  level, the States' clock and the State shown), each light's wanted state
  at the last update (what goes out), the lights another computer sends.
- **The viewer** keeps nothing of them: `lights_of(car)` reads the
  component; a train made anew starts its lights out, as before.
- **Still the viewer's:** what decides a light (the DLL's light flags, the
  MSTS conditions, the owner's) waits for the modules (step 4).
- **Tests:** `openrts.car_registry` `components` (lights sent, shining);
  the viewer's runs of the ChS7 and the ЭД9М gave the same lights, glows
  and beams before and after.

### Step 2: pantographs (built 2026-11-23)

As the doors and the lights: the pantographs rise, sound, give the line
and go over the network as before.
- **`CarPantographs`** (`CarComponents::pantographs`): the ones wanted up,
  the ones the sounds last knew (45 / 46, 66 / 67 without a DLL), the ones
  another computer sends, how far each has risen (0 .. 1, `step()` at
  PantographRaiseTime's pace).
- **The viewer** keeps the shape's side: the nodes, where each stands
  along the car (the rear one by the direction), the animation's frames.
  Drawn at `rise x` the last frame; up when risen (RTS 0x5031D8), so the
  line still comes when the animation has risen.
- **Still the viewer's:** what decides them (the DLL's, a native plugin's,
  the rules: RaiseSecondPantoWhenSpeedBelow, ExcludePantograph, the AI's)
  waits for the modules (step 4).
- **Tests:** `openrts.car_registry` `components` (the pace up and down);
  the viewer's ChS7 and ЭД9М runs: the same pantographs and line as before.

### Step 2 checked against v0.39.1 (2026-11-23)

Both builds traced the same way (each car's pantographs, lights and doors
a line when they change; the cab's DLL lamps), run on the same scenarios.
- **Pantographs, single player (ChS7):** lowered and raised by the cab's
  switch, twice each build: the same sequence of states; up 2.966 / 2.967
  s after the command; the rise sampled every 0.25 s the same to 0.01.
  The first second differs between runs of the same build (which car's
  shape loads first), not between builds.
- **Network (a local server, two clients, both builds):** the ChS7's
  pantographs and lights and the ED9M's lights and doors seen by the other
  client in the owner's order, ending the same; a client connecting again
  gets the trains' present state; a change of cab (the ED9M's rear cab)
  the same in both builds. The owner's first state reaches a remote train
  one frame earlier or later from run to run, in both builds. Not carried
  in either build: an outer door opened by a cab door (only the passenger
  door groups go over the network).
- **The ChS7's L_PSS lamp:** its DLL blinks it every second under the
  ALSN's aspect 1 (as Kuban's start gives), steady under 3 and 7; both
  builds blink it at the same moments; a capture shows it lit or not by
  its moment.
- No regression found.

### Step 3: the cab's state (built 2026-11-23)

The cab and the salon behave as before; their state is the car's.
- **`CarCab`** (`CarComponents::cab`): the cab's (or salon's) doors and
  windows (how far open, where going), the displays' blinking clocks, the
  FreeAnimations (the wipers: phase, way, moving). The switches stay the
  DLL's (its host keeps them by the car).
- **`CabView`** keeps the drawing and the picking: the .sd, the nodes and
  frames, the surfaces. `bind_state()` ties it to the car's state (a car
  seen first takes the .sd's starting states); unbound (a test) it keeps
  its own. TrainView makes every cab view through `make_cab()`, bound.
- **`board_cabs_`** is only a cache of views now: a view dropped and made
  anew finds the doors and windows as they were left. A car released: the
  views drawing it unbind first (they keep a copy), then its state goes.
- A train made anew (put back) keeps its cab's state, as the views kept by
  the car's key kept it before.
- **Tests:** `openrts.content.engineer_vl10k` (two views of a car share it,
  a view made anew finds it, unbound keeps a copy); the viewer's door pick
  (ChS7, ЭД9М), boarding cabs one after another and back, the passage
  printed the same as step 2's build.

### Step 4: modules (built 2026-11-24)

What drives a car is one interface, `CarModule` (`train.hpp`, the former
`LocomotiveController`); the train runs as before.
- **The modules:** `keyboard` (driving without a DLL), `dll` (a legacy
  DLL's adapter, openRTS's cab brake devices over it), `plugin` (an
  openRTS 64-bit plugin). RTS's physics and air stay the train's, the
  default under every module.
- **The car's signals:** a step a module reads `EngineInputs` (speed,
  pipe, reservoirs, cylinders, the line) and sets `EngineDemands` (force,
  pipe and reservoir rates, ЭПТ, the valve's УР, sanding, power).
- **`name()`, `version()`, `reads()`, `save()`, `restore()`:** a saved
  game has each module's own block, `k<car>.<name> <version>` with its keys
  under `k<car>.<name>.`, and `modules 1`. A block of another module (the
  car's system changed) or of a version the module does not read is left
  out: it keeps its starting state. A save from before blocks (no
  `modules`) is read as before. A DLL's own state stays the DLL host's
  (`LegacyConsist::save_state`).
- `Train::set_module()` / `module()`; modules move with their cars (a cut,
  a coupling, a train turned).
- One module a car for now; a car of several (its own brakes beside the
  DLL) comes with the first such system.
- **Tests:** `openrts.train` "module blocks" (the block written, restored;
  an old save; another version and another module left out), the rest of
  the suite unchanged.

## The user's answers (2026-11-24)

1. **Without Legacy:** as far as it can go, without overdoing it. RTS's
   behaviour stays the default module; openRTS's own (physics, brakes) are
   modules beside it, chosen. Content formats (.eng, .wag, .sd) stay as
   they are; openRTS departs only where RTS is wrong or at its limit.
2. **Saved situations:** each module has `save` / `restore`, in the module
   interface from step 4 on.
   - The DLL module saves only what its host sees (switches, pressures,
     doors); a DLL's own state is opaque, so a restored one may behave a
     little differently, and that is said.
   - Each module writes its own block with its name and version; an
     unknown module or an old version: the block is skipped, the module
     starts with its defaults, no error.
   - Order: the car's components (doors, lights, cab) first, then the
     modules.
   - The same saved state can bring a car to a computer that joins a
     server late. Text, in the placements' manner.
3. **What a car is:** four separate properties, then its rooms.
   - **Class:** passenger, freight, locomotive, service.
   - **Place in the train:** head or intermediate (a head car may be a
     motor car; an intermediate one may have a cab too). Only as the
     author marks it; unmarked: not set, and everything works as now. It
     is the car's design; where it stands in the coupling now is worked
     out apart (two ЭД9М coupled: the second's head car is in the middle,
     still a head car).
   - **Traction:** motor, trailer, none. Marked by the author; unmarked:
     .eng motor, .wag none (trailer is never guessed).
   - **Layout:** compartments, open sleeper, seats, locomotive. A
     template, one file for many cars.
   - **Rooms:** nodes of a graph joined by doors: compartment 1..N,
     corridor, vestibule, toilet; for a locomotive cab 1, cab 2, engine
     room, vestibule, service room. One mechanism for every class.
   - RTS content has no layouts: they are openRTS's own files. A car
     without one gets its class's default (a passenger car: one saloon and
     vestibules; a locomotive: the cabs of its .sd). No car changes.
     Whatever a property needs is turned on only for cars marked so.
4. **Passengers:** later. The room graph is made so that people can be in
   it (the engineer, a helper, passengers later).
5. **Lua modules:** yes, a sandbox and a step budget. A Lua module has the
   same interface as a C++ one, `save` / `restore` too; over its budget it
   skips the step and the train does not hang.
6. **The network (2026-11-24):** openRTS with openRTS only, no Legacy
   path: no RTS clients, no RTS protocol, no server Legacy switch. The
   server runs the trains: a client sends its input (controller, keys, cab
   buttons), the server steps the modules and the physics and sends the
   state to everyone; the client draws, plays the sounds, moves the cab
   (a lever moves at once, the gauges come from the server, the train's
   motion smoothed between the server's states). A DLL train runs on the
   server through the x86 host (it cannot be predicted on the client). The
   cab's own state goes only to that train's engineer. This follows step 5;
   the RTS network code goes when it is built. Content compatibility stays
   (DLLs, formats, RTS physics as the default modules).
