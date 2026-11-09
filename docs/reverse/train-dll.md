# The RTS ↔ locomotive DLL layer

How `rts1.exe` and the locomotive DLLs work together: when each call
happens, what is passed and read back, and why. Roadmap step 5, item 0. The
host design that follows from it is in `docs/train-dll.md`; the ABI basics
(module lookup, `InitRTSLibrary`, `DLLVersion`, adapters) in
`legacy-dll-abi.md`.

Sources:
- `rts1.exe` (static analysis in IDA; addresses below);
- `ts.h` (TS_VERSION 104), the header RTS ships for DLL authors
  (`TRAINS/TRAINSET/tsrLoco_VL80k-125/CAB/ts.h`; not copied here);
- the VL10k DLL source (`TRAINS/TRAINSET/tsrLoco_VL10k-1461/cab/vl10k_main.cpp`),
  the DLL side of the same contract.

CONFIRMED means read in the code (and, where noted, agreeing with `ts.h` or
the VL10k source); nothing here was run against a live RTS.

## 1. The objects are RTS's own cars

**CONFIRMED.** The objects a DLL receives are not copies made for it: they
are RTS's internal car objects, laid out exactly as `ts.h` declares them with
4-byte packing. The offsets found earlier in the physics
(`runtime-overview.md`) match `ts.h` field by field:

| Offset | `ts.h` field | Physics use found before |
|---:|---|---|
| `+0x0C` | `Wagon::IsLocomotive` (WCL class: 1 electric, 2 diesel) | "engine kind" |
| `+0x0D` | `PantoRaised` (bit per pantograph) | written by the `Run` dispatch |
| `+0x1C` | `Flags` (bit 0 flipped, bit 2 player in, byte 2 pipes / cocks, byte 3 cables) | "flags; bit 0 flipped" |
| `+0x24` | `Mass` | mass |
| `+0x2C` | `BrakeForce` | dissipative force accumulator |
| `+0x34` / `+0x38` | `IndependentBrakePressure` / `AuxiliaryPressure` | — |
| `+0x44` / `+0x48` | `Velocity` / `Acceleration` | velocity / acceleration |
| `+0x4C` / `+0x50` / `+0x54` | `Resistance` / `Slope` / `Force` | resistance / grade / active force |
| `+0x58` | `double ZOrd` (4-aligned) | offset of the car in the train |
| `+0x60` / `+0x64` / `+0x68` | `BrakeCylinderPressure` / `TrainPipePressure` / `TrainPipeRate` | cylinder / pipe |
| `+0x98`, `+0xD8` | `TrackPDP TrPos[2]` (64 bytes each) | the two track travellers |
| `+0x138` | `void *wag` | the parsed ENG/WAG type |
| `+0x158` | `AnimParts` (120-byte nodes) | pantograph parts |
| `+0x184` | `Locomotive::LocoFlags` | — |
| `+0x194` | `MainResPressure` | — |
| `+0x1A4` | `cab` | "cabin object" |
| `+0x1B0` / `+0x1BC` | `Slaves` / `locoA` (`Engine*`) | — |
| `+0x1D8` / `+0x1DC` | `LibModule` (library descriptor) / `AuxLibs` | — |
| `+0x1EC` | `ElectricLocomotive::Run` | "primary DLL Run pointer" |
| `+0x1F8` | `LineVoltage` (electric) / `FuelAmount` (diesel) | — |

`Engine` (208 bytes) and `ElectricEngine` (216, allocated as `0xD8` in
`0x5026FC`) are separate objects reached through `locoA`.

So "inputs" and "outputs" are only a matter of who writes a field and when:
RTS's physics writes velocities and pressures into the same memory the DLL
reads, and the DLL writes `Engine` fields RTS reads after the call. A DLL can
reach every object of the train through pointers (`Slaves`,
`FirstInGroup` / `LastInGroup`, `cab`, `locoA`) and through the resolvers, and
can keep pointers between calls.

## 2. Loading and binding

### When a DLL is loaded

**CONFIRMED (`0x468C74`, entering a locomotive).** When the player takes a
locomotive (`a2`):
1. every locomotive of the train loses `LocoFlags` bit 2 ("in player's
   join");
2. the cab is loaded if it is not (`0x51CD88`; in multiplayer a network
   message is sent);
3. `Flags |= 4` (player in), `LocoFlags |= 0x10 | 0x40` (has cabin, cabin
   loaded for player);
4. nothing more unless the train is the player's (type 1) or type 4, and the
   locomotive is neither cold nor AI (`LocoFlags & 0x88`);
5. if the ENG names a DLL (`wag + 568`), the DLL is bound and initialised
   (`0x5026FC`, below); a failure shows message 0x94 with the ENG name;
6. `ChangeLoco` and the multiple-unit grouping follow (section 3).

Other trains (traffic, static) never load DLLs.

### Binding (`0x5026FC`)

**CONFIRMED.**
- Skipped if the DLL is already bound (`LocoFlags & 0x20`, "script loaded").
- `LibParam` = the `LibraryModule` parameter, `StackSize` = `VariableStack`,
  `NumEngines` / `NumAxles` from the ENG.
- The library descriptor is found or created by name and loaded once per
  module (`0x56D9C8`; descriptor reference count). `StackSize` is raised to
  `DLLVersion(DLLV_STACKSIZE)` when that is larger.
- The `Engine` object is allocated (`0xD8` bytes for an electric engine,
  `0x4FE6C8`; the diesel variant `0x4FE734`) with its variable stack.
- The callbacks are resolved by name: `_CanWorkWith`, `_Init`, `_ChangeLoco`,
  `_LostMaster` (stored as `LostSlave`), `_CanSwitch`, `_Switched`, `_ALSN`,
  `_SpeedLimit`, `_Run`. Auxiliary libraries are created and loaded
  (`0x56DC3C`, `0x56E970`).
- **`Init(eng, loco, State, time = 0.0, AirTemperature)`**, then the
  adapters' deferred updates (section 7).
  - `State`, low byte: bit 0 when the train has cars (the caller's flag:
    some car is a wagon or a cold engine, `0x468C74`'s `v29`; or train byte
    `+2` = 1), bit 1 when it starts running (the train's speed `+0x5C`
    non-zero, or byte `+2` = 2); replaced by the activity's start state
    when the train has one (train `+0x14` → `+0x48`). `ts.h`/`export.pdf`:
    0 cold without cars … 3 running with cars.
  - **CONFIRMED** the activity's start state: `Train ( TrainState ( n ) )`
    of the `.act` (`0x471B9C`, `"trainstate"` → activity train `+0x48`);
    RTSFileReference: 0 standing without cars, 1 standing with cars, 2
    running without cars, 3 running with cars; 0 is a cold train (lights
    off, diesels stopped, pantographs down, no sounds); the default is 0
    for static trains and 1 for the others. **UNKNOWN**: without
    `TrainState`, a train whose activity `Type` (`+16`, default 2) is 2 gets
    2 there; which type 2 is was not traced.
  - openRTS: `--start cold|on|running` (the viewer) and the Train window
    give `Init` an activity's TrainState (0, 1, or the train's own bits).
    The ED9M (MaDaDev) runs `Init` only for a non-zero state: cold, it
    leaves everything off.
  - **PROBABLE** train byte `+2` is the dispatcher's state: the console's
    "Редактирование поезда" has `cbTrainState` = Холодный / Включен /
    Движется (0 / 1 / 2), and `0x5026FC` reads byte `+2` = 1 as bit 0 and
    = 2 as bit 1.
  - **CONFIRMED** by the VL10k source (`vl10k_main.cpp`, `Init`): 0 (the
    default case) leaves it cold (handbrake on, main reservoir 1.2,
    pantographs down, breaker open); 1 raises the pantographs, closes the
    breaker, charges the air (main reservoir 7.8, pipe 5.5) and holds it on
    the independent brake (3.0); 2 and 3 the same with the main reservoir at
    9.2 and the independent brake released. With `0x100` it also sets the
    player's cab switches (132, 116, 117; 38 for 2 / 3).
  - Cold engines (`Cold ( )` in the `.con`, `Cold` of an activity
    `WagonGroup`; `LocoFlags` bit 3) get no DLL at all and count as cars.
  - `0x100` the player is in this locomotive, `0x200` train type 4,
    `0x400` train type 5.
- **After `Init`:** the pantograph animation parts jump to their end frame
  for every raised pantograph (`PantoRaised`), or to frame 0; auxiliary
  `Init` (`0x56EE38`); a DLL that exports `SpeedLimit` sets train flag
  `0x400000` (the train then computes speed limits for it); `PTValue` =
  pipe − 0.2.
- Failure releases the descriptor and the auxiliary libraries.

## 3. Masters and slaves

### Grouping (`0x468C74`, entering)

**CONFIRMED.**
- The group is the run of cars joined by cables: `Flags` bits `0x10000` /
  `0x20000` (byte 3, `WagonData::Cables`, front / rear) are followed both
  ways from the player's locomotive.
- For each locomotive in the group other than the player's, unless it is
  cold or AI: the **master's** `CanWorkWith(slave, series)` is asked
  (`series` = the slave's `LocomotiveSeries` string, ENG `+386`); a refusal
  leaves it out. VL10k answers yes only for `"vl10k"`.
- An accepted slave: its cab is loaded (`LocoFlags |= 0x10`), `|= 4` (in the
  player's join); when the player's locomotive is master (`LocoFlags & 1`),
  the slave's master bit is cleared and its own slave list freed; its DLL is
  bound and **initialised without the player** (`State` without `0x100`);
  it is stored in the master's `Slaves` array (`NumSlaves`).
- **Who is master is the DLL's decision:** `ChangeLoco` sets `LocoFlags`
  bit 1 (VL10k: always when there was no previous master; otherwise only if
  the previous one's reverser is neutral and a few switches allow it).
- **`ChangeLoco(loco, Prev, State)`**: `Prev` is the locomotive that had
  this one among its slaves (the previous master), `State` is the wagon
  `Flags` field. Auxiliary handlers run before and after (event 7).
- On the train's first entry the pipe, cylinder, auxiliary and `PTValue`
  pressures of every car are copied from the locomotive (train flag 4).

### Regrouping (`0x4F995C`)

**CONFIRMED.** Called after coupling (`0x469ED8`), uncoupling (`0x469858`),
menu actions (`0x463F84`) and network events (`0x57B8E4`): the groups are
rebuilt the same way. New slaves are bound and initialised; a locomotive
that was a slave and is no longer in its master's group gets
**`LostMaster(lost, master, State)`** with auxiliary handlers before and
after (event 8).

**CONFIRMED (2026-11-04) in detail:**
- Nothing happens for a one-car train or a train without flag 4 (the
  player has been in it).
- The groups are runs of cars joined by control cables (Wagon byte `+30`
  bits 1 / 2). A group counts its engines that are neither cold nor AI.
- Every master in a group (`LocoFlags & 0x21` = `0x21`: master, DLL
  bound) gets a new slave list:
  - its `CanWorkWith` (`+452`) is asked about each other engine of the
    group, already a slave or not;
  - an accepted one has its cab loaded if it has none (failing that, it is
    left out); its DLL is bound and initialised only if it is not bound yet
    (`0x5026FC`, skipped with `LocoFlags & 0x20`); its engine sound is made
    (`0x5030B4`);
  - a slave of the old list missing from the new one gets `LostMaster`.
- So a DLL is bound and initialised once per car. A car that is cut off and
  coupled again keeps its DLL's state; nothing calls `Init` or
  `ChangeLoco` for it again.
- **Coupling** (`0x469ED8`): the train taking the other in regroups if it
  has flag 4. Each car taken into the player's train from a train without
  flag 4 gets its sounds again (`0x500EDC`, group 7 for engines with
  `LocoFlags` bit 7, else 5).
- **Uncoupling** (`0x469858`): the cut part is a new train that copies the
  train's flags (`0x4EEC50`), flag 4 too, and both parts regroup.
  - In single player the cut part is LOOSE (kind 3). If the player's part
    has no master and the cut part has one, they swap places (`0x469744`).
  - The `Run` dispatch takes every train with flag 4 that is not of kind 2
    or 4 (below), so **a cut-off engine's DLL keeps running** with the
    player out of it.
- **VL10k (source):** `LostMaster` clears the master's commands in the
  slave's `var[4]` (breaker, fans, positions: `&= ~14016`, `~(2 << 16)`).
  A section cut off and coupled back is a slave again and follows the
  controller, but pulls only after the driver gives those commands again.

**openRTS** (`dllhost::Command::reconsist`, `LegacyConsist::reconsist`):
- after a coupling or a cut the host gets the new car list. The mirrors of
  the cars that stay move with them, new cars get new mirrors, and the
  groups are made again as above (new slaves bound and initialised with the
  train's state of the moment, lost ones `LostMaster`);
- the host runs several trains (2026-11-05): the player's first, then the
  parts cut off from it, each with its own train object, car list and
  groups. Their DLLs run every step as RTS runs them (State without
  `0x100`); the signal pass (`SpeedLimit`, `ALSN`, `Cabin::Signal`) and the
  track item list are the player's train's only (types 1, 2, 4 in RTS; a
  cut part is LOOSE). A part cut off moves by its DLLs' outputs while it
  stands; coupled back it is one train again. The player can take a part
  over (the Train window's "drive"): the host makes it the first train and
  enters its engine without a second `Init`. A car in no train (its unit
  removed) is kept aside with its mirror until it comes back;
- the sounds of a part cut off (its exterior `.sms` and its libraries')
  get its DLLs' triggers and variables; the library sounds follow RTS's
  camera rules for a train that is not the player's (`sound.md`, "Library
  sounds"). Taking a part over enters its first DLL engine with a driving
  cab (`CabView`; an ED9M motor car's `Cabin` is its salon);
- the game follows cars by a key of its own, since the `.con` UiD repeats
  between consists;
- the viewer keeps the cab, the engineer (seated stays seated) and a
  native plugin;
- tests: `openrts.dllhost` (fake DLL: the cut engine's `Run` sees its own
  train's inputs), `openrts.content.dll_vl10k_couple` (section b of the
  VL10k cut off, in a train of its own, and coupled back).

### Masters writing into slaves

**CONFIRMED (VL10k source).** The master changes its slaves directly: the
slave's variable stack (`SlaveLoco(i)->Eng()->var[4]` flag bits, `var[5]`,
`var[12]`) and `Engine::UR`. Nothing in RTS copies these: the slave's DLL
reads them on its next `Run`. This is why one address space per consist is
required (`train-dll.md`).

## 4. Each frame

### Timing

**CONFIRMED.** The simulation `0x424488` runs once per rendered frame with
`dt = timeGetTime()` delta clamped to 8–1000 ms (`runtime-overview.md`).
`Run` gets that `dt` in seconds (`*a2 × 0.001`). VL10k integrates with it
(`x += rate * time`). There is no fixed outer step: only the coupler solver
sub-steps at 0.05 s.

### Before the DLLs

**CONFIRMED (`0x424488`).** For every car of every train with flag 4 that
is not of kind 2 or 4 (the trains whose DLLs run):
- `Force` = 0;
- `TrainPipeRate` := `TrainPipePressure` (the field doubles as "pressure at
  the start of the frame" for the pipe's Δp);
- `EPTvalue` = −1 (no EP demand);
- electric locomotives: `LineVoltage`, `LineFreq`, `LineCurrent` = 0.

### The calls

**CONFIRMED.** Every car with `IsLocomotive` and an `Engine` gets the `Run`
dispatch (`0x5031D8`), **in the order of the cars in the train** — not
masters first. A slave ahead of its master therefore sees what the master
wrote in the previous frame.

`0x5031D8` for one locomotive:
1. `State` = `0x100` if the player is in it (`Flags & 4`), `| 0x200` / `|
   0x400` for train types 4 / 5. The low byte is 0.
2. Electric: `PantoRaised` from the pantograph animation parts when the
   shape has them, else from `ElectricEngine::Panto`. If `LineVoltage` is 0
   it is looked up (`0x503CE4`, the catenary).
3. A flipped car (`Flags & 1`) gets `Velocity` and `Acceleration` negated:
   the DLL sees them in the locomotive's own direction.
4. Auxiliary `Run` handlers ordered before the main call.
5. **`Run(eng, loco, State, dt, AirTemperature)`** — directly with `locoA`
   and the car, or through the adapters for old DLL versions.
6. The adapters' deferred updates (section 7).
7. Auxiliary handlers ordered after.
8. `Velocity` / `Acceleration` negated back.
9. Electric: the pantograph parts are set from `ElectricEngine::Panto`
   (the part's pantograph number in byte 1 of its `+4`). Diesel: `FuelAmount
   -= FuelConsuming · dt`.
10. Both: the independent brake cylinder moves towards
    `IndependentBrakeValue` at the wagon type's release / apply rates
    (`+324` / `+328`); `EngineFlags` bit 2 releases it; SMS triggers 14 / 54
    when it starts or stops with no train brake applied.
    `MainResPressure += MainResRate · dt`, `AuxiliaryPressure += AuxilaryRate
    · dt` (both kept ≥ 0).
11. A cab not on screen has its switches stepped here (`0x526CB0`), which can
    fire `Switched` (section 6).

### Reading the outputs

**CONFIRMED (`0x424488`, right after the dispatch):**
- **Sanding** (`Engine::Sanding`): `LocoFlags |= 0x100`, adhesion × the ENG's
  sanding factor; `SandLeft` counts down; after sanding stops the effect lasts
  over the locomotive's length of travel (`LastSanding`).
- **Wheel slip:** adhesion = ENG `+204` × sanding × slip factor (if it slipped
  last frame) × weather. With no per-engine data, `|Force| > mass · 10 ·
  adhesion` sets `Wheelslip` = 15 and **`Force` = 0**. With `EngineForce[]`
  per engine, each engine is checked against its axles; a slipping engine's
  force is replaced by `0.1 + (GetTickCount() % 10) / 30` of itself (10–40 %)
  and its wheel sets spin.
- **Car force:** `Force` = `Engine::Force` (sign flipped for a flipped car).
- **Brakes:** `BrakeSystemsEngaged` bit 1 → `TrainPipeRate` drives the pipe;
  bit 2 → `EPTvalue` is spread along the EP line in both directions until a
  car without EP or a disconnected EP cable; bit 3 → `ChargingRate` drives the
  second pipe. `HandbrakeValue` = `HandbrakePercent` / 100 (negative
  clamped to 0).
- **`GlobalMsg`:** up to 16 `{action, id}` pairs, applied across the train
  (lights, doors, texture changes, on-screen messages; `ts.h` `GM_*`).
  `ShowMessage` stores a raw `wchar_t*` into two of the shorts.
- `Engine::BrakeForce`, `Power`, `DynamicBrakePercent` and the rest of the
  physics use are read where the physics needs them (ported in
  `simulation/train.cpp`, `train.md`).

## 5. Signals: `SpeedLimit` and `ALSN`

**CONFIRMED (`0x508520`, the per-train signal pass, every frame for train
types 1, 2 and 4).** For each locomotive with a bound DLL (`LocoFlags &
0x20`):
- **`SpeedLimit(loco, Route, Signal, Event)`**: three `SpeedLimitDescr
  {Limit, NextLimit, Distance}` passed by value (m/s, m). A part is −1 when
  the train's flags say it is not known. Auxiliary `SpeedLimit` handlers
  follow (`0x56F068`).
- **`ALSN(loco, A, nA, B, nB, C, nC, limit, x, y, reversed)`**, only for a
  locomotive whose cab has scripted signal lights (`Cabin::SignalScripted`):
  three arrays of `SignalsInfo` (32 bytes: distance, four aspects, flags,
  speed limit, `RTSSignalData*`) with their counts, built from the signal
  objects the pass collected (adapted with code 23 for old DLLs); `limit` =
  the lowest of the train's limits; two integer fields of the train; and
  `reversed` = the locomotive faces against the train's direction. Auxiliary
  `ALSN` handlers follow (`0x56EF60`).
  - UNKNOWN: which signals go into each of the three arrays exactly (front
    pass, rear pass, both), and the meaning of the two integers.
- **CONFIRMED** `Cabin::Signal` (`SignalsInfo` at Cabin `+108`), filled by
  the same pass for the player's train: the leading car's cab (car 0, or the
  last car when the train runs the other way) gets the next signal —
  distance (`+108`), aspect (`+112`, the train's `+160`; 8 when it is
  BLOCK_OBSTRUCTED), speed limit (`+132`), `RTSSignalData*` (`+136`); the cab
  at the other end gets aspect 8 at 0 m. DLLs read it (the VL10k's ЭПК and
  КЛУБ: `cab->Signal.Aspect[0]`, `Distance`, `SpeedLimit`).
- **CONFIRMED** The locomotive signal lights (`LocomotiveSignalization`,
  the cab update `0x525DB0`): the aspect shown is `Cabin::Signal.Aspect[0]`
  (past the aspect count: 0); the engine's `ALSNOn` (`Engine +106`, int16)
  0 puts every light out, -1 lights them all, another negative value shows
  the aspect in its low 12 bits (the DLL's own choice), positive shows the
  signal's. A light is shown (node flag bit 0 clear) when the aspect's mask
  has its bit. The loader (`0x523F94`): `SignalAspects ( n Aspect ( k l1
  .. lk ) .. )`, the count first, a bitmask per aspect of
  ceil(lights / 32) words (Cabin `+64`); indices past the lights (the VL10k
  lists 10..15 for the other end) never show.

## 6. Switches: `CanSwitch` and `Switched`

**CONFIRMED.** Every `ActSwitch` has `State` and `SetState`; the cab steps
`State` towards `SetState` over `AnimTime`; player input and DLLs only write
`SetState`.
- **`CanSwitch(loco, eng, SwitchID, SetState)`** (`0x528540`) when the player
  operates a switch (input `0x437AA4`, `0x437E64`, `0x43823C`) or a network
  peer does (`0x4C9120`). Auxiliary handlers ordered before the main call
  can veto it, then the main DLL, then the handlers after. VL10k uses it to
  refuse unsafe operations and to post the click sound.
- **`Switched(loco, eng, SwitchID, PrevState)`** (`0x5288CC`) when a switch
  reaches its new position: the cab update (`0x526CB0`, `0x52586C`,
  `0x525D1C`, `0x525DB0`) copies `SetState` into `State` and calls it; also
  from input and the network. Auxiliary handlers before and after (event 4).
  So `Switched` is the end of the switch's animation, not the click.

## 7. Host functions and adapters

**CONFIRMED.**
- **`_RTSGetInteger(loco, code, index)`** (`0x43929C`): 1 world time; 2 the
  number of wagons (relative to `index`); 3 night; 4 the number of units; 6
  the train's direction relative to the locomotive (−1 / 1); 7 tape roll
  (1 recording, 3 recording this locomotive). Code 5 (`RTS_TRIGGERID`) goes
  through `_RTSGetNamedInteger`; others give 0.
- **`_RTSGetLocoObject(dll, loco, code, index, sub)`** (`0x536650`):
  3 / 21 the car itself; 4 door (`index`); 7 cabin; 12 `Engine`; 13 exhaust
  (diesel); 14 free animation; 16 light (`LightObj`, 164 bytes); 17 sound
  (0 exterior SMS, 1 cab SMS); 19 ENG parameter (12-byte `EngineParam`); 20
  slave `index`. Anything else, or an index out of range: null.
- **Adapters (DLL versions 101–103):** the resolvers hand out shadow objects
  in the old layouts (`0x537718`), and after every call into the DLL RTS
  runs the deferred updates (`0x53782C` on the car, `0x538548` on the cab and
  the engine) that copy the shadows back. Current DLLs (104 with adapter
  compliance 39) get the real objects. Details in section 10.
- **The auxiliary dispatcher** (`0x56EADC`): the stack keeps parallel arrays
  of library, event and order. Entries with a negative `AuxCall` order run
  before the main call, the others after; `LibFlags` bit 1 tells the library
  which. Events: 1 `Run(lib, loco, State, dt, AirTemperature)`, 2 `ALSN`,
  3 `SpeedLimit`, 4 `Switched(loco, lib, id, prev)`, 5
  `CanSwitch(loco, lib, id, set)`, 7 `ChangeLoco(lib, prev, State)`, 8
  `LostMaster(lib, master, State)`. The `.eng` keywords of `AuxCall` are, in
  RTS's table order, `run`, `alsn`, `speedlimit`, `switched`, `canswitch`
  (codes 1–5); there is no keyword for 7 / 8 (PROBABLE: added for libraries
  exporting the functions). `Init(lib, loco, State, 0, AirTemperature)` is
  called for every library that exports it, without an `AuxCall`
  (`0x56EE38`). Module paths are looked up as given, then under
  `GLOBAL\LIB\` (main DLLs: `TRAINS\TRAINSET\`, then `GLOBAL\LIB\`, then as
  given; `0x56D9C8`).
- **`_RTSGetCabObject(dll, cab, code, index, sub)`** (`0x536B14`): `index`
  is the position in the cab's array, which is the `.sd` `ID`: 1 switch
  (104 bytes), 2 display (108), 4 door (120), 5 area (68), 6 window (100), 7
  the cabin, 8 close-up scene (140), 9 its sub-object (`index` scene, `sub`
  object, or `sub` = −1 to search every scene by ID), 10 / 11 a display's
  screen and its sub-objects (by position; docs/reverse/cab-screens.md),
  14 free animation (48), 15 internal light
  (136), 18 texture change (24), 24 chair (108), 25 seat (100). `ByName`
  variants compare `lstrcmpiW` with the object's name (`+8` of activators).
- **`_RTSGetNamedInteger(sms, 5, name)`** looks the name up in the SMS's
  named triggers; **`_RTSIsSMSTriggerOn`** answers −3 without an SMS, −2
  without its stream table, −4 for an unknown trigger (`0x439548`).
- **`_RTSSetIntegerG`** (`0x43939C`): code 6 sets the train's direction, only
  for a train without cars (returns 2 when it changed, 1 otherwise); code 7
  starts / stops the tape recorder (RTS's replay) for this locomotive.
- **FPU:** RTS does not change the FPU control word around DLL calls: the
  only `fldcw` instructions in the executable are in runtime math helpers.
  DLLs run with the process's mode, PROBABLY the Borland RTL default
  (`0x1332`: 64-bit precision, invalid / zero-divide / overflow unmasked).
  Not checked at run time.

## 8. Why it is built this way

- **One object model, edited in place.** RTS 1.x grew from MSTS-style data:
  the DLL interface exposed the engine's own structures instead of copying
  values, which is cheap and lets a DLL drive anything (cab, lights, doors,
  sounds) without new API calls. The price is the fixed layout, hence
  `DLLVersion` size checks and the adapter layer for older layouts.
- **Masters and slaves** model multiple-unit control: the master's DLL
  decides (`ChangeLoco`, `CanWorkWith`) and drives the slaves through their
  stacks; RTS only builds the groups from the cables and keeps the lists.
- **Auxiliary libraries** let content add systems (brakes, safety devices,
  radio) to any locomotive or car without touching its main DLL, ordered
  around it.
- **`Switched` at the end of the animation** keeps the cab's visible state
  and the logic in step: a breaker closes when the handle gets there.
- **Velocity flipped around the call**: every DLL can assume its own
  forward direction, whichever way the car stands in the train.

## 9. What the openRTS host takes from this

- Mirror objects in the `ts.h` layout for every car of the consist, one
  address space, stable addresses, pointers between them as RTS sets them
  (`Slaves`, `cab`, `locoA`, `wag`-derived counts).
- The same call sequence: bind and `Init` on entering (with the `State` bits
  above), grouping by cables with `CanWorkWith`, `ChangeLoco` / `LostMaster`
  on regrouping, per step the pre-call resets, `Run` in car order with the
  flip, the auxiliary ordering, then the output reading above;
  `SpeedLimit` / `ALSN` from the signal pass; `CanSwitch` on input,
  `Switched` when a switch arrives.
- **Step (openRTS choice):** openRTS runs everything on a fixed 0.05 s step
  (`train.md`), so `Run` gets `dt` = 0.05 at 20 Hz instead of once per frame.
  DLLs integrate with `dt`, so their behaviour should match; displays update
  at 20 Hz.
- **FPU (openRTS choice):** the host sets 64-bit precision like Borland but
  keeps FP exceptions masked, so a DLL bug gives inf / NaN instead of
  killing the host.

## 10. Adapters for older DLLs (`0x5364D0`, `0x537DC8`, `0x537E10`)

**CONFIRMED.** On load RTS calls `InitRTSLibrary` first, then `DLLVersion`:
- TS_VERSION below 101: refused. `DLLVersion(20) != 44` / `(21) != 16`
  set two module flags (track item layouts). `DLLVersion(19)` is the stack.
- Adapter compliance below 39: the DLL must have today's sizes exactly
  (switch 104, display 108, screen 76, loco 480, engine 208, light 164,
  internal light 136, close-up sub-object 204), else it is refused.
- Compliance 39 and TS_VERSION 104: run directly. 101–103: through
  adapters chosen by a per-version table of variants per object code:

  | Version | Loco (3) / slave (20) | Cabin (7) | SignalsInfo (23) |
  |---|---|---|---|
  | 101 | 1 | 1 | 1 |
  | 102 | 2 | 1 | 1 |
  | 103 | — | 1 | 1 |

- **An adapter** copies the whole object to a shadow (`0x214` bytes for a
  loco, `0x160` for a cabin), then deletes the fields the old layout lacks
  by moving the rest down (`0x537CD0`): loco variant ≤ 1 deletes `+476`
  (`AuxLibs`), ≤ 2 deletes `+384` (`Wagon::WagonObjs`); cabin variant 1
  deletes `+136` (`Signal.SignalInfo`, the `RTSSignalData*` that SignalsInfo
  gained in 104). SignalsInfo arrays for `ALSN` become 28-byte elements.
  After the call (`0x53782C`) the gaps are reinserted, the deleted fields
  restored from the real object, and the shadow copied back.
- A shadow is refreshed when it is fetched while clean; fetched again before
  the copy-back it is handed out as it is (a master's write into a slave's
  shadow survives until the slave's own call copies it back).
- Not adapted: the engine (code 12), lights, wagons (21): their old and new
  layouts share every field DLLs use, or the DLL only gets element
  pointers.

What it means for content (`openrts-dllhost32 --probe` over TRAINSET): 41
of the 54 installed DLLs are TS_VERSION 104, 12 are 102 with compliance 39
(adapted), one (`VL80k000.dll`, a spare copy no `.eng` names) is 102
without compliance and RTS refuses it too.

## 11. Cab switch input (`0x437AA4`, `0x437E64`)

**CONFIRMED.**
- **A click or a key** steps `SetState` one position, ping-ponging at the
  ends (the "goes down" bit `0x10` of `Flags` remembers the direction);
  `LOOP` wraps instead; `DISABLED` ignores; `CHECKDISTANCEONPRESS` needs the
  driver within `Radius`. A step down / up (two keys, `AssignKey ( "up"
  "down" )`) goes one way only.
  - This is how one cab shape with two desks works (the VL60's
    `cabin.sd`: switches 0.. at one end, 100.. at the other, the same
    `AssignKey` at each, all `CHECKDISTANCEONPRESS` with `Radius` 2..4): a
    key works the desk the driver is at. openRTS measures from his eye in
    cab coordinates to the `.sd` `Position` as it is, as a click's pick
    does (`0x528EA0`); PROBABLE that RTS takes the same point for a key.
    Before 2026-11-08 a key worked every switch bound to it, both desks
    at once.
- **CONFIRMED `ParentObject ( id )`** of a cab object goes to
  `Activator +34` (the loaders at `0x516695`, `0x517291`, `0x517B9C`,
  `0x51894F`; -1 none). The cab update (`0x52586C`) gives a switch with
  one its parent's `SetState` each time, then it moves there with its own
  `AnimTime` and gets its `Switched` as any switch. The EP1M's blind is
  twelve pieces (`shtora8_1`, 601, the handle; `shtora8_2`.. 630.. with
  `ParentObject ( 601 )`, `Radius ( 0 )`). openRTS: the host does the same
  each step; the cab draws a piece where its root is every frame, and a
  click or a drag on a piece works its root (before 2026-11-08 the pieces
  were dragged one by one).
- **CONFIRMED `ActSwitch::Key` (`+72`, 4 bytes)** holds the `AssignKey`
  keys (`0x45AE90`, from the switch loader): per key, two at most, its
  character in upper case (`Space` 0x20) and a byte of modifiers (1
  `shift`, 2 `ctrl` or `control`, 4 `alt`), the parts joined by `+`; the
  second key at `+74` / `+75`. The MaDaDev `Brakes.dll` reads them in its
  `Run` (`0x416460`..`0x4168CF`): the auxiliary brake handle (`BR_254ID*`)
  in one of its unfixed positions springs back (the DLL sets it to 1, or
  from 5 to 4) unless `GetAsyncKeyState` of its `Key[2]` / `Key[0]` or of
  the left mouse button (1) is down. openRTS left the bytes 0 before
  2026-11-08, so the handle sprang back even while held. The DLL host is
  another process; `GetAsyncKeyState` is the session's, so a key held in
  the game counts.
- **CONFIRMED the 367 brake lock** (`BR_367ID*`) in `Brakes.dll`'s
  `CanSwitch` (`0x421A3C`): refused while another cab's lock has its key in
  (state not 0), or while the train pipe (`Wagon +0x64`, `TrainPipePressure`)
  is above 0.2: the pipe must be vented first (the 395 in VI), as on the
  real locomotive. The same for a click, a key or the cab window's +.
- **`CanSwitch` order:** RTS writes the new state into `SetState` (`+48`)
  first, then calls `CanSwitch`, and a refusal restores the old state.
  Anything the DLL writes into `SetState` during the call stays.
  - The ChS7 (`chs7.dll`, switch 0 `C_MAIN`) relies on this:
    - a step from 0 (state 12) to 11 sends the controller to +1 (1);
    - a step to 2 sends it back to 0 (12);
    - its field-weakening sector (5..10) is entered through switch 200,
      which moves `FixedState` and `SetState` to 10 and sets bit `0x1000`
      of the engine's flags.
  - openRTS used to set `SetState` after an allowed call, overwriting the
    DLL's choice. The controller then stayed at 11 and the next step went
    into the field-weakening sector.
- **`KEY` switches** have a "key out" state 0 below their positions: the
  first press inserts the key at `InitialState + 1`; Shift with the key
  there takes it out (CONFIRMED `0x437AA4`, `+56`; an earlier note here
  said `FixedState`); the positions are 1..`States`. A `CONTROLLERX` /
  `CONTROLLERY` key is taken hold of by a press and moved by dragging; with
  Shift held a drag takes it out (the ED9M's reverser `C_REV`, `States ( 3
  1 )`: in and out at neutral, state 2). So a `KEY`
  switch with `States ( 2 0 )` has the states 0, 1, 2 (the VL10k checks
  `Switch(38) >= 2` for "forward").
- **CONFIRMED** the cab loader (`0x51CD88`) makes as many switches as
  `Switches ( n` declares (an id at or past n is an error, 0x19), likewise
  displays: an id below n without a block is a switch at state 0. DLLs rely
  on it: the MaDaDev `Brakes.dll` reads a switch through
  `RTSGetCabObject(cab, 1, id)` and takes "none" as -1, i.e. set — the
  ED9M's head car asked for undeclared ids of its 790 and vented the pipe
  (`TrainPipeRate` -1.2) while openRTS made only max id + 1 switches.
- **CONFIRMED** the loader (`0x51B98C`): `States ( n s )` sets NumStates
  (`+52`) n and InitialState, State and SetState (`+56`, `+44`, `+48`) s; a
  `KEY` switch then gets State = SetState = 0 (its key out) and NumStates
  n + 1; FixedState (`+64`) comes only from `FixedState ( x )`. Every cab
  of a train starts so; a DLL's `Init` puts the player's key in (the ED9M:
  `SetSwitch(1, 3)` for a running start, `(1, 2, SetBoth)` for 1). The ED9M
  refuses its reverser while a slave head car (LibParam 0 or 3) has its key
  in, so starting the other cabs with the key in locked it.
- **CONFIRMED** exterior lights (`0x44247C`, one light `0x50036C`):
  `Lights ( n` makes n definitions (72 bytes), a `Light` with `LightID` goes
  to that index, `RTSLights ( m` adds m after them. A light: `Type` (0 a
  glow sprite with `LightTexture`, path from `TRAINS\TRAINSET`; 1..4 the
  light sources of RTS's shaders), `Conditions ( Headlight Control Service
  Unit TimeOfDay Weather )`, `FadeIn`, `FadeOut`, `Cycle`, `States ( n
  State ( LightColour Duration Position Azimuth Elevation Transition Radius
  SourceRadius Angle ) )`, `Attenuation ( start length kind )` (default
  2 / 0.6 / 0.4), `SpotExponent` (x 128, default 64), `LightArray`. A DLL
  switches them with `SwitchLight` (LightObj Flags 8, by LightID).
  **CONFIRMED** the car's light objects (`0x4F0BC8`, zeroed,
  then filled by the definition's `Type`): Type 0 makes a glow (LightObj
  Type 1), Type 1 a light source (LightObj Type 2), other Types nothing. A
  Type 1 light with `Unit` 3 or 5 (the train's last car) is turned round
  the car's middle (`0x4F11A7`: Position x and z negated, its Y angle +
  180): such lights are written as if at the front. Glows are not turned.
  The VL80T's rear section (`mdd_vl80t-1390b`) has its marker glows at
  z -7.6 (its cab) and their beams (Unit 5) at +7.6 with no Azimuth; not
  turned, openRTS lit the front section from the coupling until
  2026-11-08.
  **CONFIRMED** angles (loader `0x50036C`): the X angle is −`Elevation`
  (first value), the Y angle is `Azimuth` (second value) + π, and the lamp
  faces its −Z (OpenGL). With +Z as forward, that is a turn by +`Elevation`
  about X: a positive `Elevation` dips the light (the 2ES4k's headlights,
  +3), a negative one lifts it (the ChS7's roof lamp on its sloping front,
  −38). Across TRAINSET, type 0 glows have 3609 negative and 1355 positive
  elevations; type 1 beams have only positive ones. openRTS turned the
  other way until the ChS7's roof glow showed it.
  openRTS draws type 0 as a flat glow facing where the light points and
  feeds 1..4 to the shape and terrain shaders. **CONFIRMED** (the shader
  source, GLOBAL\SHADERS\lamp.vert.shader): a glow quad has UV (0,0) at its
  +x +y corner, (1,1) at -x -y, turned by the light's matrix. The ED9M's
  searchlight cone is built of glows: lights 14..19 (on with the searchlight
  at its third position, switch K_1), quads of 0.8 m at (0, 3.871, 11.6)
  turned by Azimuth +90 / -90 (Elevation 0 or 45) with the mirrored
  textures light1.ace / light.ace, bright at the lamp. Their size follows
  RTS's field of view (the quirk in signals.md, "Drawing a light"): at the
  installed FOV 65 the cone starts at the lamp (beam half angle `Angle`, reach `Radius`:
  PROBABLE readings).
  openRTS (2026-11-09, not RTS): a glow is the light's own size near, but
  never less than 1.5 pixels (half size, `GlowSprite::min_pixels`), its
  light spread over the bigger quad (`spread_light`: dimmed by the sizes'
  ratio squared), so on the whole as bright as it is. Far off a buffer
  light or a route board's glyph is smaller than a pixel: MSAA's four
  samples caught it now and then and averaged it, with MSAA off its one
  sample a pixel missed it and the light went out.
- **CONFIRMED** (decompiled copy of `mdd_Libraries\Usavp\Usavp.dll`, run in
  openrts-train-check and the viewer) the ED9M's route board (маршрутный
  указатель) is made of exterior lights, not a screen: `Lights ( 1100`,
  20 places of 45 glyph glows each (`MARSHUZ\*.ace`, Radius 0.035), LightID
  100 + 50 x place + glyph (digits 0..9, then А..Я, line, dash, dot; 49 is
  a space and has no light). Places 0..9 (IDs 100..599) are one field,
  10..19 (600..1099) the other. The USAVP aux library writes a text
  (`0x401E34`): it clears the field's lights and sets `LightObj` Flags 8 on
  one light per place, through `RTSGetLocoObject(code 16, LightID)`. Its
  `Init` (`0x406820`) decides who writes: with the registry value under
  `HKLM\SOFTWARE\WOW6432Node\Microsoft\Microsoft Games\Train Simulator\1.0`
  present and the engine's `Flags2` (`Engine+124`, which `ed9m.dll`'s Init
  sets to 1) non-zero, the ИТП does; otherwise both fields show
  "МАДАДЕВ" and the ИТП cannot change them. The ИТП (display 498, buttons
  150..158 by `ITP_*` parameters, all handled in the aux `CanSwitch` with
  the new state 1) starts in its menu at item 0 with no route chosen: Down
  to item 3 and Enter opens the route list (104 built-in names, "В ДЕПО"
  first); Enter takes the route and returns to the menu at the same item;
  item 1 + Enter writes the route to the board; item 0 + Enter forgets the
  route; F1 picks the other field; F3 / Esc open the menu. openRTS draws
  the lit glyphs as train glows (verified: "В ДЕПО" lit 112 214 265 325
  374 in the viewer and drawn on the head car's front).
- **CONFIRMED** doors: DLLs post `GM_DOOR_OPEN` / `GM_DOOR_CLOSE` (1 / 2)
  with a `GlobalID` in `Engine::GlobalMsg` (+144, 16 action / id pairs, ts.h
  `GM_*`); the ED9M sends them every step for both sides. RTS (`0x424488`,
  for every car of the train) sets at once the `State` of the car's
  exterior doors with that `GlobalID` and of the `InteriorDoor`s with it in
  both of the car's interiors (cab and salon), and sends their nodes to the
  open (or shut) frame; the animation follows on its own. The ED9M's DLL
  reads its cab's passenger doors (`DOOR_A`, `B`, `D`, `E` of `cab9.sd`,
  `GlobalID` 1 / 2, `CanBeOpened ( 0 )`): seeing them shut it lit its door
  lamp again after 2.5 s. `GM_SMS_POST` (5) id plays that trigger in the
  sound of every car of the train (`0x46AF1C`): the ED9M's door sounds 105 /
  106. openRTS: the host does all of it; the viewer animates the passenger
  doors of every car (by the node's own key frames), and the cab's or
  salon's InteriorDoors with a `GlobalID` follow the train's doors (the
  engineer cannot open those by hand, as `CanBeOpened 0` says); the cab
  doors (`DOOR_1`, `DOOR_2`) follow the cab's doors of the same name.
  openRTS (2026-11-09; RTS's handling of turned cars not traced): the sides
  are the sending car's, so a car turned the other way in the train (its
  Flags bit 0 not as the sender's) opens its other GlobalID, 1 for 2 and 2
  for 1 (an ED4M's turned third car had opened its doors on the far side).
  Without a DLL the Y / U keys are the driven cab's sides, swapped alike.
- **CONFIRMED** cab and salon lamps (`LightSources`, `0x51C364`, 136-byte
  `InternalLight`): `Name` (its node), `GlobalID` (+84), `Position` and
  `PointsTo` (x negated; without `PointsTo` straight down), `Radius ( x z )`
  half sizes at the `PointsTo` plane, `SourceRadius`, `SpotExponent` 0..1,
  `Colour`, `LightFade ( in out )`, `Flags` (`NIGHT_ONLY` 0x8000,
  `NIGHT_AUTO` 0x4000, `SOURCE_VISIBLE` 0x2000, `POSITIONAL` 0x1000,
  `RUNNING` 0x800, `PROJECTION` 0x400, `LIGHT_CONE` 0x200, `LIGHT_SPHERE`
  0x100), `Projection`, `AutoWhenWorldLit`, `Attenuation`, `LightArray`.
  RTS's `shape.frag.shader` (GLOBAL\SHADERS) lights the scene with them in
  its `LightsInt` block: kind 4 cone, 2 sphere, 3 box (a lamp with neither
  flag; PROBABLE). A DLL switches one by `SwitchLight` (State) or a group by
  `GM_LIGHT_ON` / `GM_LIGHT_OFF` id = `GlobalID` (the ED9M's salon light:
  group 2 in every car; it posts `GM_LIGHT_OFF 2` every step while off).
  **CONFIRMED** (`0x514630`): the lamp's node (named as the lamp: the
  ED9M's `LP_1` .. `LP_6`, lit lamp covers) shows only while the lamp
  burns, unless the lamp has `SOURCE_VISIBLE`: then it stays and is drawn
  unlit while lit (shape-rendering.md). openRTS does both (`cab_view.cpp`,
  `NodeAdjustment::unlit`).
- **CONFIRMED** texture changes (`0x51CD88`): `TextureChanges ( n Change (
  TexID "file" Change GlobalID ) )`, 24-byte `TexChange` (ts.h); a DLL sets
  `SetState` (`ChangeTexture`) or posts `GM_TEX_CHANGE` / `GM_TEX_REVERT`
  with the `GlobalID`. `TexID` is the cab shape's image index (the ED9M:
  image 42 `klub_u.ace` becomes `klub_y.ace` with the KLUB backlight, 28
  `RCN.ace` becomes `RCL.ace`); the file is looked up next to the cab shape.
  openRTS draws it with the renderer's texture overrides
  (`ModelRenderer::submit(..., TextureOverride)`, a binding per draw and
  texture made when first needed).
- **CONFIRMED** picking (`0x417114`): a control is a candidate only with
  the eye within its `Radius` of its `Position` (`0x528EA0`). One with an
  animation node is drawn into the stencil, depth-tested against the cab
  (`0x5290C8`): it wins when its node covers the pixel under the cursor (a
  later control over an earlier one). One without a node wins when its
  projected `Position` lies within 25 px of the cursor, the nearest first,
  and never over a node hit (`0x529124`). The node is drawn as it is posed,
  so a lowered curtain is grabbed low. openRTS: `CabView::pick` tests the
  nodes of switches drawn away from their rest pose where they are drawn.
- **CONFIRMED** hints (`0x4121BC`, drawn `0x40FC06`): only while Alt is held
  in the cab, for the control under the cursor: its `StateHints` entry for
  the state (a `KEY` switch's key-out state: the `Hint`), else its `Hint`
  (`0x528CF4`), one light grey line in the middle of the screen. RTS never
  draws text at a control's `Position`, so cabs with Positions well off the
  controls look right there. openRTS's captions sit at the middle of the
  control's own surfaces instead (the `Position` only for a control without
  a node).
- **CONFIRMED** the Pause key (`VK_PAUSE`, window procedure `0x402FC3`)
  toggles `game_paused` in a single-player game; on a network game it
  follows the server's suspension (and refuses with "Can't pause while
  driving train" in one case not traced). RTS also pauses while its window
  is inactive or minimised (not done in openRTS).
- **CONFIRMED** `NONFIXED` switches are buttons. The mouse (`0x4121BC`):
  a press steps one position up (direction +1, not the ping-pong) and holds
  the switch; the release steps back towards `FixedState` and then sets
  `SetState` = `FixedState` outright. A key (`0x414340`): held, a pressed
  button steps up (a released one takes the click step); let go, the switch
  steps towards `FixedState` one position per key delay. Without
  `FixedState ( x )` the rest position is 0.
- **CONFIRMED** dragged controls (`0x4121BC`; switch fields: +44 State, +48
  SetState, +68 Flags, +84 the frame in states, +88 / +92 `SwitchPrecision`
  PrecX / PrecY; the node state +16 its frame, +32 frames, +36 states):
  - A press on a `CONTROLLERX` / `CONTROLLERY` switch takes hold of it (a
    `PRECISE` one too: `PRECISE` alone is a plain click). Each frame while
    the button is down, with the cursor's move since the last frame:
  - **CONFIRMED** a switch's node has frames = the shape's animation frame
    rate x `AnimTime` x (positions - 1) (`0x51C1C8` -> `0x448578`; the
    positions if that is 0), and the node is drawn at key time frame /
    (frames - 1) x its keys' span + their first frame (`0x4ACEEC`): the
    keys spread over those frames whatever their own count. The ED9M's
    curtains: 30 frames a second x 1 s x 1 = 30 frames, keys 0..5: 0.1
    frame a pixel drags one 290 px from up to down. Those are pixels of RTS's
    window (the installed config: 1080 high, FOV 65); openRTS turns its own
    cursor move into them by the angle of view (`rts_pixels_per_pixel`), so
    a control keeps up with the cursor as in RTS whatever the resolution
    and field of view (the viewer's is about 49 degrees).
  - `PRECISE` (`0x437E64`): the node's frame moves by dx * PrecX + dy *
    PrecY (cursor pixels; y grows downwards), clamped to the animation
    (wrapped with `LOOP`); State-to-be = frame / ((frames − 1) / (states −
    1)), one further in the move's direction, asked of the DLL
    (`CanSwitch`, refused: nothing moves); then SetState = the frame's
    state, +84 = the frame in states (fractional), and crossing a whole
    state sets State and fires `Switched`. Released, SetState = State and
    the node stays where it was left (a curtain at any height). Without a
    `SwitchPrecision` a `PRECISE` drag is a click step.
  - A controller without `SwitchPrecision`: one step in the move's
    direction (right / up: +1, left / down: −1), and the hold ends: one
    position a press.
  - A controller with `SwitchPrecision` (`0x527140`): as `PRECISE`, the
    node driven by hand; released (`0x527190`), SetState = the nearest
    whole state and the node animates there.
  - `KEY` switches count from 1, Shift at the key's state takes the key out.
  - ED9M (`cab9.sd`): the curtains `STOR_1` / `STOR_2` are `PRECISE
    CONTROLLERY`, `SwitchPrecision ( 0.0 0.1 )`; the master controller
    `C_MAIN` and the reverser are `CONTROLLERY`, the 395 brake valve and
    the searchlight `K_1` `CONTROLLERX`. Installed trains: 1768 `PRECISE`,
    2345 `CONTROLLERX`, 1183 `CONTROLLERY`, 667 `SwitchPrecision`.
  - openRTS: `LegacyCab::grab` / `drag` / `drag_end` (the viewer turns the
    cursor's move into states with `SwitchPrecision` and the node's frames)
    and the host's `drag_switch` command (protocol 3), which keeps a dragged
    switch's frame until its SetState is changed otherwise; its reply
    carries the switch's new state, shown at once (RTS moves the node in the
    mouse handler; waiting for the next 50 ms step made a curtain lag the
    cursor). A press on a
    controller no longer clicks it (as RTS: only a move does). Deviation: a
    `PRECISE` switch without `SwitchPrecision` takes one step per press
    like a controller (RTS clicks it every frame while held). Upward
    crossings fire `Switched` at once (RTS's check in `0x437E64` only sees
    downward ones; its cab update catches up the others).
- **`Switched` fires** from the cab update (`0x52586C`) when `State` differs
  from `SetState` and the switch's animation node has got there (at once
  without a shape); `State` := `SetState`, then `Switched(prev)`. A DLL
  setting both (`SetSwitch(.., SetBoth)`) moves a switch without a
  `Switched`.
- `.sd` keywords and `Flags` bits, in RTS's table order: `controllerx` 1,
  `key` 2, `nonfixed` 4, `controllery` 0x20, `precise` 0x40, `overscale`
  0x80, `loop` 0x10000, `disabled` 0x20000, `checkdistanceonpress` /
  `displaysequence` 0x40000, `stretch_uv` 0x80000 (PROBABLE for the bits
  after `overscale`: matched with the `ts.h` comment order).

## 12. Content details DLLs rely on

- **`.repaint` files** (RTS): a `Cabin` or a wagon shape can name
  `x.repaint` = `Repaint ( Shape ( "a.s" ) ShapeDescription ( "a.sd" )
  TextureReplace ( "old.ace" "new.ace" ) ... )`. Without resolving it the
  cab and the car's exterior details are missing, and DLLs that reach for
  them crash (`mdd_EP1`, `mdd_2ES4k`, `mdd_EP2D` in the probe).
- **Free animations and doors** of the car shape's `.sd`
  (`ESD_Exterior_Details`): DLLs find them by name (`FindAnim("KONTR")` in
  the draisine) or index (`Door(4)` of every car in the EP2D) and use the
  result unchecked.
- **`WagonObjects`** of the car shape's `.sd` (`ESD_Exterior_Details`):
  `DragA` / `DragB` (the wheel chocks: `ChildNodes`, `FirstStateInvisible`,
  `NumStates`), `EndValveA` / `B`, `BrakePipeA` / `B`, `ChargingPipeA` / `B`
  (`StateKeyFrames`). openRTS draws a chock's nodes only while a chock lies
  at that end (by the node's place in the shape); the cocks and hoses are
  not posed yet.
- **CONFIRMED How RTS finds a cab's blocks**: the cab loader (`0x51CD88`)
  looks each keyword up in its parent block's text with `0x4578C0`, at any
  depth (a match at a word boundary, case aside: the keywords are lower
  case UTF-16), and counts only `(` and `)` (the pair at `0x5F9BA6`) to find
  where the block found ends; quoted text is skipped. A `}` is no bracket.
  So a block closed with `}` or left open only stretches its parent, and
  the blocks after it are found all the same:
  - 26 cab `.sd` files (MaDaDev's `mdd_VL80-Alias/CABT`, `EP1M`, `EP1`,
    `EP20`, `EP2K`, `2ES4k`, `2TE10`, `VL80cm`, rz's `TEP70`) close
    `Animations` with `}`;
  - the 2TE116's `cabEDTKLUB.sd` leaves `InteriorDoors` open over
    `InteriorWindows` and `InteriorChairs`; the draisine's
    `drezina_cab2.sd`, `LocomotiveSignalization` over
    `InteriorGeometryInfo`.

  openRTS reads the tree strictly and looks such a block up nested in the
  one left open (`child_or_nested`). Before 2026-11-08 these cabs had no
  chairs, doors or windows, and the engineer sat at the cab shape's origin,
  mid-section (VL80T, EP1M).
- **The VL60PK's brakes** (`Brakes.dll`, `mdd_vl60pk-*.eng`): one cab shape,
  two desks; the working one is where the brake valve's block key (`BR_367ID1`
  50 / `BR_367ID2` 150) is in. Started on or running, the EPK whistles at
  once and only the RBS (`PB_RBS*`, key M) stops it; the RB does not. The
  `.eng` gives `PB_RB1 ( 107 )` for the desk whose RB is switch 7 (a typo
  for 7): there the RB never counts.

## 13. The engineer on foot (`0x41B230`, `0x43778C`, `0x51CD88`)

- **CONFIRMED** `.sd` `InteriorChairs ( n Chair ( ... ) )` (the cab loader
  `0x51CD88`): a chair is 108 bytes; `Position` at +16, `Radius` +28,
  `StartDirection` +60, `Pos1` +72, `Pos3` +96 (X negated on reading, as
  every `.sd` position: it matches the cab shape's own X); **PROBABLE**
  `Pos2` +84 (the keyword there is an unnamed 4-character string).
- **CONFIRMED** config.ini `engineer.Height` (default 1.8, `0x61D400`) and
  `engineer.Height1` (0.8, `0x61D404`); the installation here has 1.81 / 0.81.
- **CONFIRMED** Seated (`byte_61D394 == 3`), `0x43778C` sets the engineer's
  y to `Pos1.y + Height1 - Height`; **PROBABLE** the eye is `Height` above
  that, i.e. `Height1` above `Pos1` (in the VL10k's cab: 3.31, 0.81 over
  the cushion, 1.27 over the floor).
- **CONFIRMED** `0x41B230` (sets the engineer's place, e.g. from a saved
  state): with a current chair, seated puts him at `Pos1`, not seated at
  `Pos3` (`+96`). RTSFileReference: `Pos2` where Enter stands him up,
  `Pos3` where Ctrl+Enter does.
- **PROBABLE** He walks on `InteriorPlanes` (rectangles with a height), which
  in the VL10k lie at 1.75, 0.29 under the visible floor (2.04): RTS's
  standing eye, `Height` above them, is 1.52 over the floor.
- **CONFIRMED** Sitting down and standing up move the camera along a short
  path over 2 s (`sub_4352B0` with 2.0).
- The current chair is `[0x61D6A8]` (set by `0x41B434`; a saved state's
  or the server's chair by its index, `0x47BC69`, `0x579EDA`). Which chair a
  new game starts by is not traced. openRTS starts the engineer on foot at
  `Pos3` of the first chair of the leading cab, by the train's way (players
  report that RTS starts him by a chair or by the train, not seated): the
  EP1M has chairs 0 and 1 in its rear cab and 2 and 3 in the front one,
  where its DLLs put the keys in (the 367 lock 206 and the control key 205
  at 2 for a start with power); the VL60PK's chair 0 is at the front.
  PROBABLE.
- **CONFIRMED** Keys in a chair (`0x4121BC`): Enter without Ctrl — chair
  action 3 (`0x436CA8`): up to `Pos2` and back down again (bit 0 of the
  chair's +44), still at the chair; Ctrl+Enter — action 2: to `Pos3`, on
  foot (`byte_61D394 = 2`); **Num0** opens / closes the chair's
  `HeadOutWindow` when it `CanBeOpened` and the head is in; **Num1** puts
  the head out (action 4, bit 1 of +44) when that window is open, and back.
  openRTS (2026-11-08, `Engineer::risen`): the same; before, Enter got him
  up on foot at `Pos2`. Risen he stands on the floor at `Pos2`, still in
  the chair's mode: the arrows do not walk him. openRTS's own addition, not
  RTS's: the wheel at the chair, off the controls, moves the head nearer to
  what the cursor points at (up to 0.6 m) or back (`Engineer::lean`); the
  `.sd` `CloseUps` scenes (`CameraPosition`, `CameraDirection`, a few cabs
  such as the ER2t's timetable) are another thing, not done.
- **CONFIRMED** Moving (`0x4121BC`, `GetAsyncKeyState`): the arrows walk
  (Up / Down along the view, Left / Right sideways) outside (`byte_61D394`
  1) and in the train (2, 6) at `player_speed`; Shift at
  `player_speed_with_shift`, Shift+End twice that, End alone a fifth. In a
  chair (3, 4) the arrows turn the head (Up / Down pitch, Left / Right yaw)
  and move the eye a tenth as far, kept near the chair (`0x4343E8`). Alt
  shows the caption of the control under the cursor (states 2..4). **Num3**
  works the object picked near the engineer (`0x415A34`: types 15..17,
  Ctrl for type 15) — not in openRTS.
- **CONFIRMED** Doors, the same keys at every door and from either side
  (`0x4121BC`, modes 1 outside, 2 in a cab, 6 in the train; the door is
  the object `0x414D24` picks near the engineer, type 1):
  - Enter: the door opens or shuts (`0x438708` with -1). A door the player
    may not open (flags `& 4` clear) shows a message (`off_5E04F0`)
    instead.
  - Ctrl+Enter: through it (`0x438708` with 1 opens it, then the passage
    `0x4310B0`; in a cab `0x431270` out, `0x4313FC` to the next car). A
    shut door he may not open stops him with the same message.
  - The passage ends by the door's other side (`0x435654`, the word at
    `+64`): -3 a cab (loaded, `0x436734` makes its engine the player's,
    mode 2; "Failed to load cabin" when it does not), -2 the train
    (mode 6), -1 outside (mode 1), -7 a second space of the car
    (`+372`; UNKNOWN which).
- **CONFIRMED** `InteriorWindows` (`0x51CD88`): 100 bytes, type 6 (chairs
  are type 5); `Position` +16, `Radius` +28, `CanBeOpened` +60,
  `HeadOutPosition` +64, `AnimTime` +76, `SMSTriggers` +96/+98.
- **PROBABLE** `Cabin ( "x.s" 1 )`: the cab shape turned round in its car
  (the VL10k's b sections, whose cab is at the rear of the body and whose
  `DOOR3` "WAGON_BACK" then faces section a).
- openRTS: walks on the cab shape's own surfaces (floor, walls, desks, doors
  by their animated nodes), the eye 0.85 `Height` over his feet (1.54);
  outside, on the ground and (optionally) the world's objects. Walking
  into the face of an open outer door (its plane, inside its width) gets
  him off; before 2026-11-06 only where the wall stopped him dead, so
  sliding along it he had to search the doorway. Getting on, RTS's keys
  at every outer door (since 2026-11-06; before, Enter climbed in at a
  door without a leaf or of a cab only stood at): Enter opens or shuts it,
  Ctrl+Enter climbs in, a passenger door only when the train has opened
  it. A way in is an outer door the player opens or one with its
  doorway's floor (`InteriorPlane`). The cab of any car of the driven
  train or of another own train takes him; his car and its DLL follow
  (`TrainView::walk_up_to_cab`, `board`). A cab only stood at reads its
  doors as the `.sd` leaves them until its shape is read. A cab he leaves
  stays with its doors, inner and outer, as RTS keeps each car's cabin
  (wagon `+556`, which `0x435654` takes back), and the car's outer doors
  follow it; before 2026-11-06 it was dropped and its doors shut. The
  switches and the DLL's state are the DLL host's, per car (`Init` once,
  `loco_flags & 0x20`): a cab left and entered again finds them as they
  were. Head out:
  the eye at `HeadOutPosition` (RTSFileReference: "the camera when the
  player looks out of the window"). The mouse picks a control by the cab
  shape's triangles (the node it meets, or the switch's `.sd` Position on
  the ray); a click is RTS's click step, the wheel one step up or down.

## 14. Line voltage (`0x503CE4`, `0x5B53A8`)

Called from the `Run` dispatch (`0x5031D8`) when `LineVoltage` is 0, once
per train (skipped when train byte `+5` bit 0 is set).

- **CONFIRMED** For every electric locomotive of the train (car `+12` = 1):
  - pantographs down (car byte `+13`, the raised-pantograph bitmask, is 0),
    or the locomotive's burnt flag (`LocoFlags` bit 9, `+389` & 2), or car
    flag `+32` & 0x20: no voltage from it; its current still counts; a burnt
    one cools for 60 s, then the flag clears;
  - otherwise the voltage and frequency at its pantographs:
    - with the route's `.electrodb` loaded (`dword_61D77C`): `0x5B53A8` reads
      the track section under the car (traveller `+176`, offset `+192`):
      the `SectionElectrification` pieces (`Electricity ( n  section start
      flags … )`, electrosection 0 = dead) and `Catenary` pieces, the
      electrosection's substation (`.electrotype`: `ElectricSupplyType`
      `Voltage`, `Frequency`, `VoltageDrop`; substation `Power`,
      `PowerDecline`);
    - else a global override (`dword_61D774`, voltage `+12`, frequency
      `+16`; PROBABLY the multiplayer server's);
    - else, when the route is electrified (**PROBABLE**: `.trk`
      `Electrified ( 1 )`): the middle of the locomotive's **first**
      `WorksOnLineVoltages` range, (Min + Max) / 2, and its frequency — any
      locomotive gets a voltage it works on;
    - else 0: no catenary.
  - the activity's `SetTrainElectricity` (activity train `+40` voltage,
    `+44` frequency; negative = keep) replaces it.
- **CONFIRMED** The train takes the highest voltage of its locomotives with
  its frequency; every electric locomotive gets that `LineVoltage` /
  `LineFreq` (`+504` / `+508`), and `LineCurrent` (`+512`) = the train's
  power (sum of the engines' `+212`) / voltage, split by the number of
  raised pantographs. The current loads the electrosections
  (`0x5B6030`, voltage drop).
- **CONFIRMED** Over the current limit (`MaxLineCurrent`, ENG `+596`) or the
  burn time (`MaxLineCurrentCatenaryBurnTime`, ENG `+608`) the pantograph
  burns (`LocoFlags |= 0x200`, a sound / message).
- **CONFIRMED** `0x5B53A8` in more detail: the wire first (the node's
  `Catenary` pieces: type, start, **length**, an unknown value, flags; a
  node's only piece with start <= 0 and length < 0 is the whole node, any
  other needs length > 0 — so "start 64.9, length -1", which three
  installed nodes have, is no wire); then the last `Electricity` piece
  starting at or before the car (electrosection id, start, flags;
  electrosection 0 = dead). The electrosection's source: the piece's cached
  one if any; none when the section's state (`+10`) is -2 / -3; the only
  source; the `CurrentSource`; else every source with `Active & 3` and its
  substation on (`+44` bit 0 clear), the highest voltage, and with two
  feeding the lower frequency. One source must have `Active & 2` and (`&
  1` or be the cached one). A source's voltage: the substation's `Voltage`
  × `FeederQ` / 100 − the supply type's `VoltageDrop` (V/km) × the distance
  in km to the substation (`UseSubstationPos`, flag `0x200`) or to
  `FeederPos` (flag `0x100`), at least 0.
- **CONFIRMED** `0x491DC8` (electrosection): `FeederQ` defaults to 100;
  `CurrentSource ( i )` (valid index) sets `Active` bit 0 on source i only
  and bit 1 on every source; without it the section's state is -1.
- **CONFIRMED** `0x490A14`: `Section ( n )` of `SectionElectrification` is
  TDB track node n (index n − 1, vector nodes only; the installed routes
  agree, `openrts.content.electrification`).
- **CONFIRMED** the globals of `0x503CE4`: `dword_61D77C` the number of
  electrosections, `dword_61D774` / `dword_61D714` the number and array of
  supply types (an `.electrotype` without electrosections: the first type's
  `Voltage` / `Frequency` everywhere). **PROBABLE** `dword_61D7CC` is the
  running activity: with one, a section with several sources and a
  `CurrentSource` feeds the first source whose substation voltage and
  frequency fall in the engine's `WorksOnLineVoltages` (in their order),
  cached on the piece.
- openRTS (`simulation::Electrification`): all of the above except the
  cache (the choice is made per engine each time); the load, the burning
  and the HV bus groups are in §17; the distance is horizontal (the
  feeders' heights are 0 in the files); without an `.electrotype` and
  `WorksOnLineVoltages` an engine gets 3 kV DC (RTS reads a value it never
  set there); the activity's automatic choice is a Track control option.
- RTS does not pick AC or DC for the locomotive: it gives the wire's
  voltage and frequency, and the DLL decides (the VL82m cab has two line
  voltmeters, "Сеть переменный ток" 0–30 kV and "Сеть постоянный ток"
  0–4 kV). What RTS does with `WorksOnLineVoltages` for an engine without a
  DLL was not traced.
- Content: Kuban 27.5 kV 50 Hz; Chernyshevsk 3 kV DC; Pavlovsk and
  Novokuznetsk both (27.5 kV AC, 3.3 / 3.5 kV DC); Kievskij hod and the test
  route electrified without an `.electrodb`; Uspensk not electrified.
  Dual-system engines: VL82m (`vl82m.dll`), EP20 (`Brakes.dll` only).

## 15. Exterior lights without a DLL (`0x4F07D4`, `0x4FFDF8`, `0x4FF9B8`)

- **CONFIRMED** a light definition (72 bytes, the wagon type's `+356`,
  count `+348`; loader `0x50036C`): `+0` Type, `+1` Headlight, `+2`
  Control, `+3` Service, `+4` Unit, `+5` TimeOfDay, `+6` Weather, `+8`
  FadeIn, `+12` FadeOut, `+16` Cycle, `+40` the number of States, `+44`
  the States (76 bytes each, `+4` Duration).
- **CONFIRMED** `0x4F07D4(train)` sets LightObj bit 8 ("turn on") of every
  car's lights from their conditions; it runs on events: night falling
  (`0x4F0798` from the sky update `0x417D74`), coupling, uncoupling, a
  train's type or direction changing (`0x469858`, `0x469ED8`, `0x4F549C`,
  `0x4FA374`, `0x507634`, `0x50B430`, `0x48A0F8`).
  - The player's train (type 1): an engine whose `LocoFlags & 0x88` is 0
    (not cold, not AI) is skipped: its DLL switches its lights.
  - Only cars with Wagon `Flags` bit 1 ("in sight").
  - A train of type 0 (STATIC) or 3 (LOOSE) lights nothing unless its
    activity service says so (train `+20`: `+96` bit 0x40 off, `+72` set).
  - Car 0 is tried with Unit 2 (4 when the train runs reversed, train `+4`
    bit 0x80), the last car with 3 (5); one car is both; the rest 1.
- **CONFIRMED** `0x4FFDF8(car, light, unit, train type)`:
  - Control 1: off for types 1 and 4; 2: on only for them.
  - Service 1: on for a cold engine (`IsLocomotive` and `LocoFlags` bit 3)
    or types 0 / 3; 2: off for a cold engine and types 0 / 3.
  - Unit: must equal the place given.
  - TimeOfDay 1: off when `is_night` and the light level
    (`dword_61D60C`, the sun's ambient and direct colours averaged over
    six channels, `0x408574`) is below 0.5; 2: off when not night and the
    level is at least 0.5.
  - Weather (`byte_61D62D`: 0 clear, 1 snow, 2 rain): 1 clear, 2 rain, 3
    snow.
  - Headlight: the phase is 1 (day), 2 (level below 0.5), 3 (night), and
    1 for a cold engine; 4 wants 2 or 3, the others their phase.
- **CONFIRMED** `0x4FF9B8(car, dt)`: a light turned on fades in over FadeIn
  (none: at once), off over FadeOut; with more than one State it shows
  them by their Durations (Cycle 0) or Cycle spread evenly
  (`0x4FF5E0` picks the State).
- openRTS: `simulation/train_lights`, every frame for every train
  (`docs/train-systems.md`).

## 16. Pantographs and sand without a DLL (`0x424488`)

- **CONFIRMED** the `.eng` (`0x44247C`, engine type `+593..+632`):
  `ExcludePantograph` bits (`+593`, repeatable, 1..8),
  `AlwaysUsePanto` (`+594`), `LowerPantoOnStop` (`+595`),
  `RaiseSecondPantoWhenSpeedBelow` (`+632`, a speed); the activity's
  `TrainAlwaysUsePanto`, `TrainLowerPantoOnStop`,
  `TrainRaiseSecondPantoWhenSpeedBelow` override them (`0x4F072C`).
  `PantographRaiseTime` is the wagon's (`+276`, default 3 s, 0 or less is
  3).
- **CONFIRMED** an AI train (train `+4` bit 8), each electric engine not
  cold: the raised bits `|=` 1 when `(Flags & 1) != (train +4 & 0x80)` is
  false, else 2 (the comparison of bit 0 with bit 0x80 is true unless both
  are clear: a turned car in a reversed train gets 2 as well); the second
  goes up below `RaiseSecondPantoWhenSpeedBelow` (0: never) and down
  otherwise; `LowerPantoOnStop`'s bits go down while the train's `+5` bit
  8 is set (PROBABLY standing at a stop); `AlwaysUsePanto`'s go up;
  `LocoFlags` bits 0x400 / 0x800 put the first / second down (PROBABLY
  damaged).
- **CONFIRMED** sand: adhesion is `Adheasion`'s second value; while
  `Engine::Sanding` it is times the third (`+208`), `SandLeft` (`+408`)
  runs down and stops the sanding at 0; after it stops the effect lasts
  while the engine's run since then (`+412`) is within its length. The
  first value (`+200`) multiplies it after a slip. `MaxSandingTime`
  (`+520`, default −1).

## 17. The line current, the wire and the substations (`0x503CE4`, `0x5B6030`)

- **CONFIRMED** `0x503CE4(train, loco, dt)` works on the cars between the
  engine's `+436` and `+440`, which `0x4F9FB4` sets to the run of cars
  joined by the high-voltage bus (Wagon `Flags` 0x40000 front, 0x80000
  rear) around each engine that is not cold (an engine without a bus: only
  itself). It is called from the `Run` dispatch while `LineVoltage` is 0,
  so once per group and frame.
- **CONFIRMED** for each electric engine of the group:
  - with a burn time (`+608` = `MaxLineCurrent` x
    `MaxLineCurrentCatenaryBurnTime` when both are positive, else 0) and
    not burnt: the heat (`+416`) cools by (`MLC_A` x |speed| + `MLC_B`) x
    dt (`MaxLineCurrentVelocityIncrease ( MLC_B MLC_A )`, `+600` / `+604`;
    negative values drop both), not below 0;
  - pantographs down, burnt (`LocoFlags` 0x200) or car flag `+32` 0x20:
    no line from it; its power (`+484` `+212`, `PowerConsuming`) still
    counts; a burnt one counts 60 s in `+416`, then the flag clears;
  - else its line (`0x5B53A8`), the number of raised pantographs, and the
    wire's burn factor: of the type-1 catenary pieces over it the BurnQ (0
    if any has 0, else the largest; −1 without one; 1 without an
    `.electrodb` piece).
- **CONFIRMED** then the highest voltage goes to every electric engine of
  the group with the frequency of the last one found; `LineCurrent` = the
  group's power / voltage x this engine's raised pantographs / all raised.
  Per pantograph: without a burn time, above `MaxLineCurrent` it burns at
  once; with one, the heat grows by current / BurnQ x dt (only BurnQ > 0)
  and burns at the limit: heat 0, `LocoFlags |= 0x200`, a message
  (`off_5F81D0`).
- **CONFIRMED** `0x5B6030(substation, phase, power, dt)`: unless the
  substation is off (`+44` bit 0) its phase record (20 bytes, `+56`)
  sums power x dt; after 2000 ms (`dword_5E04E8`) the average (`+12`) is
  sent to the server as package `0x4649` (substation, phase, energy,
  seconds; `0x4CF464`), or in single player simply reset. The engine's
  share: the group's power x its pantographs / all; two sources (in
  multiplayer only) split it by the electrosection's shares. The server
  (`rts_server.exe` `0x4436C8`) relays `0x4649` to the dispatchers; it
  reads `PowerDecline` (`0x437D44`) but nothing lowers the voltage by the
  load.

## 18. ALSN's arrays and taking another cab

- **CONFIRMED** (`0x508520`) the front pass collects up to 16 signal
  objects ahead of the train front (56-byte chain nodes), the rear pass up
  to 16 behind its rear; those within the passing distance (speed x dt x
  1.5) count as passed (`v100` at the front, `v101` at the rear).
  `ALSN(loco, A, nA, B, nB, C, nC, limit, x, y, reversed)`: A = the front
  objects not passed, B = the rear ones not passed, C = the passed ones
  (front first). Each `SignalsInfo`: `Distance`; `Aspect[0]` the highest
  state of its NORMAL heads (flag 1), `[1]` DISTANCE (2), `[2]` SHUNTING
  (4), `[3]` INFO (8); flag 0x10 beyond an obstruction; `SpeedLimit` the
  node's (−1 with train flag `+6` bit 3); `RTSSignalData*` the first
  head's `+124`. `limit` = the lowest of the train's limits; `x` / `y` =
  the next speed post's limit and distance (train `+120` / `+116`,
  floats); `reversed` = the car faces against the train's direction
  (`+4` bit 0x40). Auxiliary handlers get the same (`0x56EF60`).
- **CONFIRMED** walking through a door whose target is a cab (`-3`,
  `0x435654`) into another engine calls `0x436734`, which enters it as
  the player's (`0x468C74`: bind, `Init`, `ChangeLoco`, grouping) and makes
  its cab current. The `-7` target is a salon.

## 19. Track items, named triggers and the signal's data (2026-11-04)

### `RTSGetTrackItems` (`0x4805D8`)

- **CONFIRMED** the call: `(dll, loco, code, distance, items&, count&)`.
  - A DLL whose `TrackItem` size differs from RTS's is told so once; one
    whose `TrackItemsItem` size differs throws "Critical DLL structure
    mismatch" (the module's flags from `DLLVersion(20 / 21)`, `0x5364D0`).
  - `distance` <= 0 means 5000 m.
  - A locomotive facing the train's rear (Wagon `Flags` bit 0) has bits 1
    and 2 of `code` swapped, and in what it gets ahead and behind swapped
    and `Z` negated.
- **CONFIRMED** the list is the train's (`+392`, 28 bytes):
  - a count, 16-byte `TrackItemsItem` entries and an output copy;
  - how far it reaches ahead and behind, and whether the items under the
    train are in.
- **CONFIRMED** it is gathered when the request goes further (`0x4808DC`
  → `0x480A08`).
  - The walk starts at the train's rear traveller (`+232`).
  - Forward it covers the train's length (car 0's `ZOrd` + half its
    length, `0x4FB6BC`) plus `distance`. Backward it covers `distance`.
  - Each item found (`0x47F214`, any type) is an entry: `Distance` =
    from the front ahead, negative under the train; from the rear
    behind. `Z` = the signed distance from the rear. `Pos` = 1 ahead, 2
    behind, 4 under the train.
- **CONFIRMED** the per-train signal pass (`0x508520`) clears the list
  every frame the train moves; standing still, it is kept.
- **CONFIRMED** returned: the entries with `Pos & code`, within `distance`
  (under the train: always), leaving out signals (type 1: `ALSN` gives
  them).
- **CONFIRMED** the items (`0x482680`): `TrackItem` (44 bytes: `Type`,
  `ID`, `TrItemSData` offset and flags, `TrItemPData` x z tile,
  `TrItemRData` x y z tile, x negated) and per type:
  - 2 speed post (64 bytes): `Flags`, `Value` (with flag 2 the limit, with
    1 the route's as it is, else km/h or with 0x100 mph made m/s), `Off`
    (the sign's rotation), `KM` (without flag 2), `Number` (flag 0x10);
  - 3 platform (76): flags, the other item, platform and station names
    (`wchar_t*`), `PlatformMinWaitingTime`, `PlatformNumPassengersWaiting`;
  - 4 siding (76): flags, the other item, its name;
  - 5 level crossing (52): `Time` and `Distance` not read at load;
  - 6 crossover (52): the other item and the section;
  - 7 pickup (56): amount and flags;
  - 8 car spawner (48);
  - 1 signal (128);
  - anything else 0 (44).
- **openRTS** gives every step the list from the player's train's rear,
  forward over its length and back, as far as the DLLs asked (5000 m at
  least).
  - The walk follows the set junctions and ends at an end of track or at
    a trailing junction set against the way.
  - The train's length is from its rear end to its front end.

  Both points are PROBABLE: which `0x47ECD0` mode `0x47F214` walks with
  here, and where RTS's rear traveller stands, are not traced.
- **Content:** the ED9M asks for code 1 and code 4 at 4000 m
  (`OPENRTS_DLLHOST_TRACE`).

### Named triggers and trigger pairs (`0x4394F4`, `0x439548`)

- **CONFIRMED** `RTSGetNamedInteger(SMSObject::sms, 5, name)` looks the
  name up in the script's `NamedTriggersTable` without case and returns
  its code. Any other code, or no match, gives 0.
  - The table (`0x4DD0EC`) is the file's first `NamedTriggersTable` block;
    every group gets it.
  - Its tokens are pairs, a name and its code (`_wtoi`). An empty name
    stops it with a message.
  - Installed: `Usavp.sms` (7955 entries: a train's stops and their
    announcements per route) and `vl80k_aux.sms`.
- **CONFIRMED** `RTSIsSMSTriggerOn(SMSObject*, code)`:
  - −3 without a script or without trigger pairs;
  - −2 before any pair code (no `PairedFlags`);
  - for an On code of a pair, that pair's state (`0x4A8A58`): 1 on, 0
    off, −1 not known yet;
  - −4 for any other code.
  - ts.h's `IsTriggerOn` gives −5 itself without `PairedFlags`.
- **CONFIRMED** `PairedFlags` (32 bytes, `0x4A87C8`, allocated at the
  first pair code): four words of pairs on, four of pairs known.
  - An On code is dropped while its pair is known to be on; an Off code
    while it is known to be off. Before a pair's first code both pass.
  - A Halt code sets its pair off and passes.
  - Pairs past the 128th keep no state.
- **openRTS:**
  - the host gets each script's table and its group's pairs at setup
    (once per file);
  - every step it gets the states the game's sound objects keep;
  - the sound objects drop and pass codes as above. Until now an Off
    code before any On was dropped.

### `RTSSignalData` (`0x4903DC`)

- **CONFIRMED** the route database's `SignalUnit` (20 bytes: tile x z,
  `Position` x y z with x negated, the name or null).
  - Each unit is hung (`+124`) on every TDB signal item still without
    one, on the same tile within 0.2 m in x, y and z. Named or not.
  - `ALSN` gets the first head's; `Cabin::Signal` the next signal's.
- **openRTS:** `formats::rts_signal_units`, sent to the host once per
  unit. On Kuban the signals ahead at the start all have one.

## Open questions

- Train types 4 and 5 (`State` bits `0x200` / `0x400`): PROBABLY type 4 is
  the engineer's train on its helper's client (it asks for the cab, its cab
  sound takes `0x463A`) and type 5 a dispatcher-controlled one. A remote
  player's train is type 2 with flag `0x10000` (`reverse/multiplayer.md`).
- Section 14: the `Electricity` flags' high byte (`+11`, a distance limit
  in `0x5B53A8`, 0 in the installed routes); `sub_45F570` (the distance:
  3-D or plane); the global `dword_61D31C` `+36`, PROBABLY "playing
  through the network thread" (`reverse/multiplayer.md`): why the line
  voltage reads it is not traced.
- The auxiliary entry layout (`0x54` bytes) and whether entries are sorted
  by order or kept in declaration order.
- The FPU control word at run time.
- `LowerPantoOnStop`'s train flag (`+5` bit 8) and `LocoFlags` 0x400 / 0x800
  (§16) were not traced to where they are set.
- The display fade (`DisplayFade` first value, `0x4B355C`): how the node is
  faded.
