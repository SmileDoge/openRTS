# Locomotive DLLs: legacy bridge and the openRTS train plugin API

Status: plan, deferred until trains are implemented (roadmap step 5, cab
and on-board systems). Nothing here is implemented yet. Sources:
- `TRAINS/TRAINSET/tsrLoco_VL80k-125/CAB/ts.h`, the header RTS ships for DLL
  authors (TS_VERSION 104);
- the VL10k source (`tsrLoco_VL10k-1461/cab/vl10k_main.cpp`);
- `docs/export.pdf` ("Создание кабин для RTrainSim", v0.43, chapter "DLL-модуль");
- `docs/ext.pdf` ("Расширенные настройки дополнений RTS 0.43", "DLL-библиотеки
  локомотивов", AuxLibraries);
- the exports of `rts1.exe` and of the installed DLLs.

Goal: every installed locomotive DLL works in openRTS unchanged, and new
content gets a clean 64-bit interface.

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

### UNKNOWN (IDA, targeted)

- What the `time` argument of `Init` / `Run` is: the frame step or game time.
  export.pdf says "step"; check against `vl10k_main.cpp` and the RTS call
  site.
- The exact call order per frame (main DLL, aux libraries with negative and
  positive `AuxCall` order, slave locomotives) and when
  `ChangeLoco` / `LostMaster` fire.
- How RTS consumes the outputs: the units and sign of `Force`, the rate
  semantics, `ALSNOn` encoding. These are in physics, `@@Physics`.
- **Adapters:** RTS refuses a DLL whose sizes differ (see the MSVC case above).
  Still to trace: which codes it compares, and what the adapters do for older
  `TS_VERSION`s and compliance levels.
- `RTSGetInteger` / `RTSSetIntegerG` codes beyond the 7 in ts.h.
- The FPU control word RTS runs DLL code with. Borland code may expect
  unmasked exceptions or 64-bit precision.

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
- **Save / load:** an explicit `save` / `load` is needed for savegames and
  for joining a multiplayer session mid-run. For legacy DLLs the bridge
  saves `Engine::var` and the mirror state.
- **Later:** the same API can be offered to a scripting runtime (for example
  WASM) without changing the simulation side.

## Order

| Step | Content | Depends on |
|---|---|---|
| 1 | `openrts-dllhost32 --probe`: load every installed DLL, `DLLVersion` table | — |
| 2 | Reverse notes `docs/reverse/train-dll.md`: call order, `time`, adapters, output consumption, FPU | IDA |
| 3 | Mirror layouts (`pack(4)`, size asserts against step 1) and the 11 host functions | 1 |
| 4 | Host process, shared memory protocol, `--run` harness, VL10k regression | 3 |
| 5 | `train_plugin.h` and the legacy bridge plugin on the openRTS side | 4 |
| 6 | Wiring into the train simulation and cab (roadmap step 2) | 5, train |

Steps 1–4 need no train simulation in openRTS and prove compatibility on
their own. Step 6 comes with the train and cab work.

## Out of scope here

`RTS/MODS/*.dll` (RTrainSimPBR, Normals, HTML, Radiostation, Utils) and
`RTrainSimPlugins.dll` are game plugins with a different interface, not
locomotive DLLs.
