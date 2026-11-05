# Locomotive DLLs: legacy bridge and the openRTS train plugin API

Status (2026-11-01): the legacy bridge works — `openrts-dllhost32.exe`, the
mirrors, the 11 host functions, the adapters for TS_VERSION 101–103, the
call sequence of RTS, the game side (`trainplugin/`), the viewer's cab
window, keys and 3-D cab. All 54 installed DLLs load (53 accepted, one
refused as RTS refuses it), one consist per DLL runs through the host
without a crash (29 consists), the VL10k drives the train simulation in a
regression test. The native plugin API exists (`train_plugin.h`, C++
layer, loader, a sample, a parity test against a legacy DLL), the game
runs `ORTSTrainPlugin` engines, and the VL10k logic is a native example
(2026-11-04, `docs/train-systems.md`). The "how it works in RTS" notes
are in `docs/reverse/train-dll.md`. Sources:
- `TRAINS/TRAINSET/tsrLoco_VL80k-125/CAB/ts.h`, the header RTS ships for DLL
  authors (TS_VERSION 104);
- the VL10k source (`tsrLoco_VL10k-1461/cab/vl10k_main.cpp`);
- `docs/export.pdf` ("Создание кабин для RTrainSim", v0.43, chapter "DLL-модуль");
- `docs/ext.pdf` ("Расширенные настройки дополнений RTS 0.43", "DLL-библиотеки
  локомотивов", AuxLibraries);
- the exports of `rts1.exe` and of the installed DLLs.

Goal: every installed locomotive DLL works in openRTS unchanged, and new
content gets a clean 64-bit interface.

## What a locomotive DLL is

A locomotive DLL is not the car's behaviour as a whole. It is the
locomotive's onboard equipment. RTS simulates the vehicle and what the
train shares: mass, couplers, rails, the air along the train, the
catenary, sound, drawing. The DLL simulates what stands between the
driver's handle and the wheels on a real locomotive:
- the electrical circuits: contactors, relays, traction motors, the main
  breaker, protections;
- the driver's brake valve;
- the safety devices (КЛУБ, САУТ, ЭПК);
- the gauges and lamps.

```text
driver → cab (the switches' SetState)
           ↓ CanSwitch / Switched
      DLL: the locomotive's circuits
           ↓ Force, TrainPipeRate, lights, sound triggers, gauges
      RTS: the vehicle — mass, couplers, air along the train, rails, catenary
           ↑ speed, pressures, line voltage
```

In control terms RTS is the plant, the DLL the control system and the cab
the operator interface.

**Similar designs elsewhere:**
- **Flight simulators.** Add-on aircraft for FSX / P3D bring their own
  DLL with the aircraft's systems (hydraulics, electrics, flight
  management, instruments) on top of the simulator's flight model. That is
  the same role.
- **The Quake 2 game DLL.** It uses the same mechanism:
  - the engine and `gamex86.dll` share structures with a fixed layout
    (`edict_t`);
  - the engine hands the DLL a table of its functions (`game_import_t`);
  - the DLL returns its callbacks (`game_export_t`);
  - the API version is checked on load.

  RTS has the `ts.h` structures, the 11 `_RTS*` functions, `Init` / `Run`
  / `Switched` and `DLLVersion`. Only the scope differs: one locomotive
  type, not the whole game.
- **In entity-component terms**, a DLL is a system attached to entities by
  type (`LibraryModule` in the `.eng`).
  - Its private component is the `VariableStack`, an unnamed float array
    (`var[5]`, `var[12]`).
  - Auxiliary libraries are further systems, ordered before or after it.

**Where it departs from an entity-component design:**
- **The line runs across the entity, not around it.** One car has two
  writers: RTS writes speeds and pressures, the DLL forces and rates.
  Nothing declares which field is whose; it had to be found by reverse
  engineering (what RTS fills before a call and reads after).
- **No API, only the engine's memory.** The DLL gets pointers to RTS's
  own objects and may keep them. Hence one address space per consist in
  the host.
- **Entities write into each other.** A master writes into its slaves'
  stacks through pointers.
- **Logic and presentation are one.** The same DLL computes the traction
  current, moves the ammeter needle, lights the lamp and posts the sound.
- **Part of the state is global.** It lives in the DLL's globals, and
  locomotives of one series share it because the module is loaded once.

**Three approaches to locomotive logic:**

| Simulator | A locomotive is defined by | Logic |
|---|---|---|
| MSTS | data | the engine's built-in model, parameters from the `.eng` |
| RTS | code | the DLL replaces the built-in model; RTS keeps the vehicle physics |
| Open Rails | data plus scripts | the built-in model with scripts for chosen parts: brake controller, circuit breaker, power supply, train control system |

**In openRTS** the DLL fills the slot of the locomotive's controller. The
keyboard driver fills the same slot when there is no DLL. The native
plugin API (below) turns the Quake 2 style module into a component with
declared inputs and outputs:
- typed `inputs` / `outputs` instead of shared structures;
- cab controls by name;
- a context per instance;
- explicit `save` / `load`.

The cab belongs to `CabModel`, not to the plugin.

## What the legacy DLLs are

### CONFIRMED

- **Binaries:** 32-bit x86 DLLs, built with C++ Builder (`___CPPdebugHook`,
  Borland name mangling). There are about 40 in TRAINSET:
  - main locomotive DLLs (`vl10k.dll`, `ep1m.dll`, `te10.dll`, ...);
  - auxiliary libraries in `mdd_Libraries`:
    - `Brakes`, `Usavp`, `Usavpl` — safety devices and brakes;
    - `RTSConnector`, which uses `WSOCK32`.
- **Linking in the .eng file** (`EngExtended`):
  - `LibraryModule ( "CAB/vl10k.dll" param )`;
  - `VariableStack ( n )`;
  - `EngineParameters ( ... )`;
  - `AuxLibraries ( AuxLibrary ( AuxLibraryName AuxLibraryModule
    AuxVariableStack AuxLibraryParameters AuxCall ( "fn" order ) AuxLibrarySound… ) )`.
- **Exports**, resolved by name with a leading underscore, all `cdecl`:
  - `_Init`, `_Run`, `_ChangeLoco`, `_CanWorkWith`, `_CanSwitch`, `_Switched`;
  - `_LostMaster`, `_ALSN`, `_SpeedLimit`;
  - `_InitRTSLibrary(HINSTANCE module, void* dll)`;
  - `_DLLVersion(code)`.

  Prototypes are in ext.pdf p.35–36. Auxiliary libraries get an `AuxLibrary*`
  where the main DLL gets `Locomotive*` / `Engine*`.
- **Host API:**
  - `InitRTSLibrary` calls `GetProcAddress(module, …)` for 11 functions:
    `_RTSGetInteger`, `_RTSSetIntegerG`, `_RTSGetObject`, `_RTSGetObjectByName`,
    `_RTSGetLocoObject`, `_RTSGetLocoObjectByName`, `_RTSGetCabObject`,
    `_RTSGetCabObjectByName`, `_RTSGetTrackItems`, `_RTSGetNamedInteger`,
    `_RTSIsSMSTriggerOn`.
  - `rts1.exe` exports exactly these, so `module` is the RTS executable.
  - `dll` is an opaque handle that the DLL passes back.
- **Data exchange is by direct memory access.** The DLL reads and writes
  fields of `Locomotive`, `Engine`, `Cabin`, `ActSwitch`, `ActDisplay`,
  `SMSObject`, `LightObj`, ... through pointers. It also writes into object
  arrays the host returns. Examples:
  - the sound trigger queue;
  - `Engine::GlobalMsg`, which stores a raw `wchar_t*` into DLL memory for
    `ShowMessage`;
  - `TrackItemsItem` arrays.
- **Layout rules:**
  - 4-byte alignment, so `double` members are 4-aligned (`#pragma pack(4)`).
    The user's experience confirms it: an MSVC-built DLL with default packing
    aligned `Wagon::ZOrd` and `Engine::BatteryCharge` to 8 bytes. The structs
    grew, and RTS refused to run the DLL because the `DLLVersion` sizes did
    not match. MSVC needs `#pragma pack(push, 4)` around `ts.h` (or `/Zp4`).
    It follows that RTS checks the sizes on load;
  - Borland `bool` is 1 byte, `enum` is `int`;
  - `wchar_t` is 2 bytes.
- **Memory:** the DLL never allocates objects for the host. `Engine::var`, the
  float stack of `VariableStack` entries, is the DLL's per-locomotive storage.
- **Version check:** `DLLVersion(code)` returns `TS_VERSION`, the `sizeof` of
  every shared struct as the DLL was compiled, `RTS_ADAPTER_COMPLIANCE` (39)
  and `RTS_STACKSIZE`. RTS has `Rts_adapters` / `Rts_adapters_stock` units, and
  every shared object starts with `DLLInterfaceObject { adaptLink,
  adaptStatus, adapters }`. So RTS adapts DLLs compiled against older layouts.
- **When DLLs are loaded:** only when the player enters a drivable
  locomotive — one in a PLAYER train that is not cold or AI.
- **Semantics** (export.pdf p.23–33):
  - `State`: byte 0 is the initial state (0 cold, no cars, … 3 running with
    cars); byte 1 bit 0 means the player is in this locomotive;
  - `LocoFlags` bit 0 marks the master locomotive;
  - the DLL computes `Force`, `BrakeForce`, `TrainPipeRate`, `MainResRate`,
    `IndependentBrakeValue`, `EPTvalue`, `Sanding`, ... and updates the cab
    switches, displays and lights.

### Answered by the reverse notes

The questions this section listed (what `time` is, the call order, when
`ChangeLoco` / `LostMaster` fire, how outputs are read, the adapters, the
host function codes, the FPU) are answered in `docs/reverse/train-dll.md`
(sections 2–7 and 10). In short: `time` is the frame step in seconds; `Run`
goes in car order, auxiliary libraries around it by their `AuxCall` order;
RTS never touches the FPU control word; DLLs of TS_VERSION 101–103 get
shadow objects without the fields their layout lacks.

## Why a proxy

A 64-bit process cannot load a 32-bit DLL. The alternatives:
- **openRTS as a 32-bit build:** rejected — memory and modern dependencies.
- **x86 emulation inside the game** (an emulator plus Win32 API shims):
  rejected. The DLLs import KERNEL32, USER32, ADVAPI32, SHELL32, WSOCK32 and
  the Borland RTL, so that would mean a Win32 reimplementation.

The plan is an **out-of-process 32-bit host**: `openrts-dllhost32.exe`, built
by our own CMake as a Win32 target with MSVC. It is installed next to the
game, not in RTS/.

## Legacy bridge design

```text
openRTS (x64)                         openrts-dllhost32.exe (x86)
  train simulation                      loads the train's DLLs (main + aux)
  LegacyDllPlugin  ── shared memory ──  mirror objects in ts.h layout
   (an openRTS train plugin)   + events  Locomotive / Engine / Cabin / ...
                                         exports the 11 _RTS* host functions
```

### Host process

One host per player train consist. All its locomotives and auxiliary
libraries load in one process, so a DLL used by two locomotives shares its
globals, as in RTS.

#### One address space for the whole consist (requirement)

A legacy master DLL can change a slave's fields directly through a pointer
(from `_RTSGetLocoObject` / `_RTSGetLocoObjectByName`, or pointers inside the
shared objects), and a DLL can keep such pointers between frames. So every
DLL of a consist must see the same, consistent objects in one 32-bit address
space; per-DLL commands over IPC are not enough. Consequences:

- **Mirror objects for every car of the consist** live in the host, not only
  for the locomotives with a DLL, with the pointers between them that RTS
  sets (which fields carry pointers, and to what, is a question for step 1).
  `_RTSGetLocoObject` and the other resolvers return these mirrors.
- **Stable addresses.** A mirror never moves while its car is in the
  consist (no reallocation); a DLL may hold a pointer to it.
- **One snapshot in, one round of calls, everything out.** Per frame the game
  writes the inputs of all cars, the host runs every DLL in RTS's order
  (masters, slaves, auxiliary libraries, wagons with DLLs) inside the same
  frame, so a slave's DLL sees what the master wrote a moment earlier, as in
  RTS. Then the host reads back the outputs of **all** mirrors, not only
  those of the DLL that ran: a master's write into a slave's `Engine` is a
  normal output of that slave.
- **Field ownership.** Each field is either written by the game (inputs:
  speeds, pressures, voltages, signals) or by DLLs (outputs: forces, rates,
  switches, lights). The game never overwrites DLL-owned fields with its
  snapshot, so values a DLL wrote into another car survive to the next
  frame. Which fields are which comes from step 1 (what RTS fills before a
  call and what it reads after).
- **The consist changes in place.** Coupling, uncoupling and changing cabs
  add or remove mirrors and DLL instances in the same host, with
  `ChangeLoco` / `LostMaster` as RTS calls them; a DLL module used by
  several locomotives stays loaded once.
- **Native plugins get the same power without raw pointers:** multiple-unit
  control goes through host calls (train lines such as `SMECable` / `HVBus`
  as named channels a master writes and slaves read), so a native master can
  drive native or legacy slaves and the other way round, with the bridge
  copying between the channels and the slaves' mirror fields.

- **Exports:** the host exports the 11 functions under their exact names
  (`.def` file, `_RTSGetObject` …). It passes its own `HMODULE` to
  `InitRTSLibrary`.
- **Mirror objects:**
  - they are allocated and owned by the host in the ts.h layout:
    `Wagon`, `Locomotive`, `ElectricLocomotive` / `DieselLocomotive`,
    `Engine` and its variants, `Cabin` with arrays of `ActSwitch`,
    `ActDisplay`, `CloseUpSubObject`, `InternalLight`, `TexChange`,
    `FreeAnimation`;
  - also `LightObj` (exterior lights), `SMSObject` (engine and cab),
    `EngineParam` lists, `AuxLibrary`, `SignalsInfo`, `TrackItem`
    variants;
  - layouts are written from the documented field lists with
    `#pragma pack(4)` and `static_assert` on the sizes; ts.h itself is not
    copied into the repository.
- **Layout check:** on load the host calls `DLLVersion` for every code and
  compares the result with its own `sizeof`.
  - Matching DLLs run directly.
  - Others go through the adapter layer, once the IDA notes on
    `Rts_adapters` are written.
  - Until then such a DLL is refused with a clear message instead of
    corrupting memory.
- **Per frame (one round trip):**
  1. The game writes an input snapshot into shared memory: velocity,
     acceleration, pressures, line voltage and frequency, coupling and
     cable state, switch states the player set, signals and speed limits
     (`ALSN` / `SpeedLimit` input), time and day/night, track items within a
     window ahead and behind.
  2. The host copies the snapshot into the mirrors and calls the DLL
     functions in RTS order.
  3. It collects the outputs into shared memory:
     - forces, rates and pressures, panto and main switch;
     - display values and states, light flags;
     - texture changes and animations;
     - drained sound triggers from the `SMSObject` queues;
     - `GlobalMsg` entries, with `ShowMessage` text copied out of DLL
       memory.
  4. The host signals the game.
- **Synchronous calls:** `CanSwitch` / `Switched` use the same channel when
  the player operates a control.
- **Callbacks inside a call** (`RTSGetTrackItems`, `RTSGetNamedInteger`) are
  answered by the host from data sent beforehand: the track item window and
  the SMS trigger name tables at load. No nested IPC.
- **Isolation:** a crashing DLL kills only the host. The game reports it and
  the locomotive loses its control logic; the game keeps running. The host
  sets the FPU control word as RTS does around every call.
- **Cost:** one shared-memory exchange per frame is a few microseconds.

### Test harness

`openrts-dllhost32 --probe <dll>` and `--run <eng> <script>` run without the
game:
- **probe:** prints `DLLVersion` (TS version, struct sizes, compliance, stack
  size) for every installed DLL;
- **run:** loads a locomotive from its .eng, applies a scripted input
  sequence (raise pantograph, main switch on, throttle up) and logs the
  outputs.

These give regression tests for compatibility. For example: VL10k gives
`Force > 0` after the throttle steps; its brake rates follow the brake handle.

## openRTS train plugin API (new)

A C ABI (`include/openrts/plugin/train_plugin.h`), 64-bit, versioned. It is
the only way locomotive logic talks to openRTS. The legacy bridge is one
plugin implementing it, so the simulation knows one interface.

### Principles

- **No shared memory layouts.** The plugin sees opaque handles and calls host
  functions. Data structs start with `uint32_t size` and grow only by
  appending, so old plugins keep working with a newer host and the other way
  round.
- **Per-instance state.** `create` returns a plugin context per locomotive;
  there are no implicit globals.
- **Deterministic step.** `step(ctx, dt, inputs, outputs)` gets everything it
  needs in `inputs` and writes `outputs`, with no hidden reads of the world.
  In multiplayer the owner's machine runs it and the outputs are what gets
  synchronised.
- **Named controls and indicators, not indices.** The cab's controls are
  declared by name in the cab files and resolved to handles once.

### Sketch

```c
#define ORTS_TRAIN_API_VERSION 1

typedef struct OrtsTrainInputs {      /* size first; append-only */
    uint32_t size;
    double   time;                    /* simulation time, s */
    float    speed, acceleration;     /* m/s, m/s^2, along the loco */
    float    brake_pipe, brake_cylinder, main_reservoir, equalising; /* bar */
    float    line_voltage, line_frequency;
    uint32_t flags;                   /* master, player inside, reversed, ... */
} OrtsTrainInputs;

typedef struct OrtsTrainOutputs {
    uint32_t size;
    float    traction_force, dynamic_brake_force;   /* N */
    float    brake_pipe_rate, main_reservoir_rate, independent_brake; /* ... */
    float    sanding;
    uint32_t pantographs, main_switch;
} OrtsTrainOutputs;

typedef struct OrtsHost {             /* functions openRTS gives the plugin */
    uint32_t size;
    OrtsHandle (*find_control)(void* host, const char* name);
    float      (*control_value)(void* host, OrtsHandle control);
    void       (*set_control)(void* host, OrtsHandle control, float value);
    void       (*set_indicator)(void* host, OrtsHandle indicator, float value, uint32_t state);
    void       (*sound_trigger)(void* host, OrtsHandle sound, uint32_t trigger);
    float      (*parameter)(void* host, const char* name, float fallback);
    uint32_t   (*track_items)(void* host, uint32_t kinds, float distance,
                              OrtsTrackItem* out, uint32_t capacity);
    void       (*message)(void* host, uint32_t kind, const char* utf8);
    void       (*log)(void* host, uint32_t level, const char* utf8);
} OrtsHost;

typedef struct OrtsTrainPlugin {      /* what the plugin exports */
    uint32_t size, api_version;
    void* (*create)(const OrtsHost* host, void* host_ctx, const OrtsTrainSetup* setup);
    void  (*destroy)(void* ctx);
    void  (*step)(void* ctx, double dt, const OrtsTrainInputs* in, OrtsTrainOutputs* out);
    int   (*control_request)(void* ctx, OrtsHandle control, float from, float to); /* CanSwitch */
    void  (*control_changed)(void* ctx, OrtsHandle control, float previous);     /* Switched */
    void  (*consist_changed)(void* ctx, const OrtsConsistInfo* info); /* ChangeLoco / LostMaster */
    void  (*signals)(void* ctx, const OrtsSignalView* view);          /* ALSN / SpeedLimit */
    int   (*save)(void* ctx, OrtsWriter* w);                           /* state for saves and MP join */
    int   (*load)(void* ctx, OrtsReader* r);
} OrtsTrainPlugin;

/* exported entry point */
const OrtsTrainPlugin* orts_train_plugin(uint32_t host_api_version);
```

- **New content in the .eng:** declared as `ORTSTrainPlugin ( "file.dll" )`.
  The `LibraryModule` path keeps meaning a legacy DLL.

### One cab for both kinds of DLL (the user's requirement, 2026-11-01)

Legacy RTS DLLs must keep working unchanged, and native openRTS DLLs must
manage at least as well. So the cab is not part of either plugin kind:

```text
                cab files (.s + RTS cab .sd: Switch, Display, lights, ...)
                                   │
                              CabModel (openRTS)
     controls: value, states, limits, frames, click areas, hints, keys
     indicators: displays, gauges, lamps, texture changes, animations
     sounds: cab SMS with SoundSources
         ▲            ▲                 ▲                    ▲
   mouse / keys    renderer        LegacyDllPlugin       native plugin
   (CanSwitch →  (draws the cab   (ActSwitch / ActDisplay   (find_control by
    Switched)     from the model)  arrays by .sd ID,         name → handle,
                                   via the x86 host)         in process, x64)
```

- **CabModel is the only owner of cab state.** Input changes controls
  through it, the renderer only reads it, and both plugin kinds read and
  write it through the plugin interface. A cab works the same whichever kind
  drives it, and works (switches move, gauges read the simulation) with no
  DLL at all.
- **Addressing:** legacy DLLs see the RTS arrays indexed as the `.sd` IDs
  give them (the bridge copies between the arrays and the model each frame);
  native plugins resolve `.sd` `Name`s once (`find_control`), and openRTS cab
  files may add names where RTS ones have only IDs.
- **What native plugins get beyond the legacy interface:** no fixed struct
  layouts (size-first, append-only structs), several plugins per locomotive
  without globals, explicit save / load (savegames, joining a multiplayer
  session), UTF-8 messages, typed parameters from the `.eng`, track items
  and signals through host calls rather than raw arrays.
- **Equal footing, checked:** an SDK (`train_plugin.h`, a small C++
  wrapper over the C ABI, a sample plugin) and a parity test. The VL10k's
  published logic (`vl10k_main.cpp`) is ported to a native sample plugin;
  the harness runs one scripted sequence through the legacy DLL in the x86
  host and through the native port, and their outputs must agree. That
  proves the native API is enough for real locomotive logic.
- **Native plugins run in the game process** (64-bit, no IPC); a crash there
  is the plugin's bug, reported with the plugin's name. Legacy DLLs stay
  isolated in the host process.
- **Save / load:** an explicit `save` / `load` is needed for savegames and
  for joining a multiplayer session mid-run. For legacy DLLs the bridge
  saves `Engine::var` and the mirror state.
- **Later:** the same API can be offered to a scripting runtime without
  changing the simulation side. The plan is Lua (LuaJIT) with a PascalCase
  API in the style of `ts.h`: `roadmap.md`, "Train plugins in Lua and
  replaceable train systems".

## As built

```text
openRTS (x64)                                    openrts-dllhost32.exe (x86, static CRT)
  viewer / train-check                             --serve <shared memory> --parent <pid>
  trainplugin::LegacyConsist  -- shared memory --  dllhost::Host
    (.eng/.wag/.sd -> SetupRequest)  8 MB, 2 events   mirrors (ts.h layout, stable addresses)
    LegacyController per DLL engine                   modules (LoadLibraryEx, DLLVersion, adapters)
    LegacyHost (start, timeouts, crash)               the 11 _RTS* exports (exports.def)
```

- **Protocol** (`include/openrts/dllhost/protocol.hpp`): `hello` (versions,
  sizes of the shared structs), `setup` (cars, DLL paths, parameters,
  auxiliary libraries, cabs), `enter` (the player's engine: bind, `Init`,
  `ChangeLoco`, grouping by cables), `step` (the cars' state in; engine
  outputs, switches, displays, lights, SMS triggers and Var1..8, GlobalMsg
  texts out), `can_switch`, `shutdown`. A step is one round trip; a step
  that takes longer than 3 s counts as a hung DLL and the host is killed.
- **The host owns the legacy cab state**: switches move to `SetState` over
  their `AnimTime` and fire `Switched` when they get there (RTS's cab
  update); the game shows what the replies say and asks with `can_switch`.
- **Isolation**: every DLL call runs under SEH; a crash switches that DLL
  off and the log names the module, the offset and the bad address
  (`ep2d.dll+0x7624, reading 0x0000002C`). A host that ends or hangs is
  reported; the game keeps running.
- **Paths**: DLLs are loaded from their real files (`Vfs::real_path`, so
  packs are extracted to the cache); the RTS program folder is the DLL
  directory (for `bass.dll` and `CC3260MT.DLL`).
- **Tools**: `openrts-dllhost32 --probe <folder>` (versions and layouts of
  every DLL, calls only `DLLVersion` and `InitRTSLibrary`);
  `openrts-train-check <root> --dll <folder> <eng>... | consist <name>
  [--script f] [--seconds s] [--cars]` (runs the DLLs on a test track and
  checks a script); `openrts-train-check <root> --cab <folder> <eng>` (how
  the `.sd` binds to the cab shape).
- **Tests**: `openrts.dllhost` (the fake DLL `dllhost/test/fake_loco.cpp`,
  no content needed: Init, master / slave, CanSwitch, Switched, sounds,
  messages, the flip), `openrts.plugin_parity` (the same logic as a legacy
  DLL and as a native plugin), and with `-DOPENRTS_TEST_ROOT=<RTS>`:
  `openrts.content.dll_probe`, `openrts.content.dll_vl10k`
  (`tests/data/vl10k.script`).

### Native plugins (as built)

- `include/openrts/plugin/train_plugin.h`: the C ABI (size-first structs,
  one context per locomotive, named controls); `train_plugin.hpp`: a C++
  base class and `ORTS_TRAIN_PLUGIN(Class, "name")` for the export.
- `trainplugin::NativeLocomotive` loads one (`orts_train_plugin`), keeps a
  `CabModel` from the cab `.sd` (controls and indicators by name, RTS's
  movement and `Switched` order) and is the simulation's controller.
- `sdk/samples/fake_native`: the fake DLL's logic on the native API; the
  parity test runs both through one script and compares every step.
- The viewer runs the driven engine's `ORTSTrainPlugin ( "x.dll" )`
  (relative to the `.eng` folder): its controller, its cab (`.sd` keys,
  the mouse, RTS's step rules), gauges, lamps, sounds and messages; the
  3-D cab shows its state as a DLL's.
- `sdk/samples/vl10k`: the VL10k's logic as an example (one section; no
  multiple units, regenerative braking, КЛУБ / САУТ).

### Remaining

- The parity run of the VL10k example against `vl10k.dll` (not wanted for
  now: the port stays an example), native plugins on the other engines of
  a multiple unit.
- Doors by name, close-up screens. (Track items for `RTSGetTrackItems`,
  named SMS triggers and trigger pairs, `RTSSignalData` for `ALSN`: done
  2026-11-04, `reverse/train-dll.md` §19. Coupling and uncoupling keep the
  DLLs' state since 2026-11-04; a part cut off runs its DLLs and its
  library sounds and can be taken over since 2026-11-05, §3,
  "Regrouping".)

## Order

Start with understanding the layer (the user's decision, 2026-11-01): how
RTS and the DLLs talk — when each call happens, what is passed and read
back, and why — before writing the host. See roadmap step 5, item 0.

| Step | Content | Depends on |
|---|---|---|
| 1 | Reverse notes `docs/reverse/train-dll.md`: the RTS ↔ DLL layer — load and init, per-frame fill / call order / read-back, `time`, event calls and their triggers, masters and slaves, aux libraries and wagons with DLLs, adapters, FPU, and why | IDA, `vl10k_main.cpp` |
| 2 | `openrts-dllhost32 --probe`: load every installed DLL, `DLLVersion` table | 1 |
| 3 | Mirror layouts (`pack(4)`, size asserts against step 1) and the 11 host functions | 1 |
| 4 | Host process, shared memory protocol, `--run` harness, VL10k regression | 3 |
| 5 | `train_plugin.h` and the legacy bridge plugin on the openRTS side | 4 |
| 6 | CabModel from the cab `.sd` and wiring into the train simulation and the 3-D cab | 5, train |
| 7 | SDK: the C++ wrapper, the native VL10k sample plugin, the parity test against the legacy VL10k | 5, 6 |

Steps 1–4 need no train simulation in openRTS and prove compatibility on
their own. Step 6 comes with the train and cab work.

## Out of scope here

`RTS/MODS/*.dll` (RTrainSimPBR, Normals, HTML, Radiostation, Utils) and
`RTrainSimPlugins.dll` are game plugins with a different interface, not
locomotive DLLs.
