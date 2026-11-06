# Car and train architecture (design)

Status: a design under discussion (2026-11-06). Nothing here is built.
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

## Open questions (the user's)

1. **Without Legacy:** how far may openRTS depart from RTS (own physics,
   own brake modules)?
2. **Saved situations:** placement only, or each car's full state
   (switches, doors)? A DLL's own state is opaque: only what the host shows
   could be kept.
3. **Compartments:** a class of car, or rooms inside a car (a coach with
   compartments and a conductor's room)?
4. **Passengers:** in this step or later?
5. **Lua modules:** the sandbox and a step budget (from the roadmap's Lua
   plan).
