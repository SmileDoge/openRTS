# Legacy locomotive DLL ABI

Status: static, read-only analysis of the existing `rts1.exe.i64` IDA database.
No runtime test has been performed.  Addresses refer to the analysed `rts1.exe`.

The public ABI source is the original `ts.h` distributed with RTS content.  The
PDF documentation describes `LibraryModule` as the primary locomotive module
and `AuxLibraries` / `AuxCall` as its auxiliary-module mechanism.  The sections
below distinguish facts corroborated by code and ABI headers from inferences.

## Main locomotive DLL lifecycle

### CONFIRMED

The primary DLL flow is:

```text
parsed ENG module name
  -> sub_5026FC
  -> cached library descriptor (sub_4389E8)
  -> path lookup and sub_56D9C8(..., 7)
  -> LoadLibraryW in sub_56DBE4
  -> InitRTSLibrary / DLLVersion in sub_5364D0
  -> GetProcAddress callbacks in sub_5026FC
  -> primary Init, then auxiliary Init
  -> primary Run between auxiliary pre/post dispatch in sub_5031D8
```

`0x5026FC sub_5026FC` obtains the module-name string from an already parsed
ENG/WAG-side record, obtains a cached descriptor, and loads it with context
argument `7`.  It resolves the primary callbacks `_CanWorkWith`, `_Init`,
`_ChangeLoco`, `_LostMaster`, `_CanSwitch`, `_Switched`, `_ALSN`, `_SpeedLimit`
and `_Run` from the loaded module.

Before primary `Init`, this function allocates and initialises a runtime engine
object through `0x4FE6C8 sub_4FE6C8` or `0x4FE734 sub_4FE734`.  It obtains the
objects passed to the plugin through `_RTSGetLocoObject` and then calls the
stored `_Init`.  It invokes `0x56EE38 sub_56EE38` for auxiliary `Init` after
the primary call.

`0x5031D8 sub_5031D8` is the direct observed call site of the primary stored
`Run` pointer.  It dispatches auxiliary `Run` callbacks before the primary
callback, invokes primary `Run`, then dispatches auxiliary callbacks after it.
Its only direct caller found in this pass is `0x424488 sub_424488`.

### PROBABLE

The parsed string consumed by `sub_5026FC` is the runtime representation of
ENG `LibraryModule`.  The documentation names that field, while this pass did
not yet locate the parser that writes the raw field.

`0x424488 sub_424488` is likely a large train/simulation update routine rather
than the top-level application loop.  Only its direct call to `sub_5031D8` is
established here.

## Library descriptor (`LibraryRecord`, candidate name)

This record has no current user-defined IDB type.  `0x4389E8 sub_4389E8`
deduplicates records by case-insensitive module name in the global pool
`dword_61D53C`; `0x56DC14 sub_56DC14` decrements its reference count and
releases it at zero.

| Offset | Type / candidate name | Confidence | Evidence |
|---:|---|---|---|
| `+0x00` | `wchar_t *moduleName` | CONFIRMED | Allocated and used for module/path lookup. |
| `+0x04` | `HMODULE module` | CONFIRMED | Written by `LoadLibraryW` in `sub_56DBE4`; read by all subsequent `GetProcAddress` calls. |
| `+0x08` | `loaded` flag | CONFIRMED | Tested by `sub_56D9C8` before a repeat load. |
| `+0x09` | `loadFailed` flag | CONFIRMED | Set after a failed `LoadLibraryW`; tested on later attempts. |
| `+0x0C` | reference count | CONFIRMED | Incremented by primary/aux paths; decremented by `sub_56DC14`. |
| `+0x10` | adapter-table pointer | CONFIRMED | Assigned by `sub_537DC8` for legacy DLL versions and consumed by object resolvers. |
| `+0x14` | DLL variable-stack size | CONFIRMED | Receives `DLLVersion(DLLV_STACKSIZE)`. |
| `+0x18` | ABI/layout flags | PROBABLE | Bits are set for two observed layout-size mismatches; precise consumers remain unexamined. |

## Module path lookup and loading

### CONFIRMED

`0x56D9C8 sub_56D9C8` calls `0x56DBE4 sub_56DBE4`, then invokes
`0x5364D0 sub_5364D0` only after a successful load.  For context argument `7`,
the observed lookup tries paths under `TRAINS\\TRAINSET` and `GLOBAL\\LIB`, with
fallbacks using the supplied module name.  `sub_56DBE4` calls `LoadLibraryW`
and stores the returned `HMODULE` at descriptor offset `+0x04`.

### PROBABLE

Context `7` identifies the main locomotive-library lookup context.  Primary
loading demonstrably uses it; the semantic label is inferred from that use.

## `InitRTSLibrary` and RTS-side API exports

### CONFIRMED

`0x5364D0 sub_5364D0` resolves `_InitRTSLibrary` from the plugin DLL and calls:

```cpp
InitRTSLibrary(GetModuleHandleA(nullptr), libraryRecord);
```

Thus `Module` is the HINSTANCE of `rts1.exe`, while `DLL` is the RTS internal
library descriptor, not the plugin `HMODULE`.  This matches `ts.h`, whose
`InitRTSLibrary(HINSTANCE Module, void *DLL)` stores its second argument in the
DLL global `DLLHandle` and resolves RTS API functions from `Module`.

The following RTS-side exported functions were located:

| Export | Address | Notes |
|---|---:|---|
| `_RTSGetInteger` | `0x43929C` | Integer accessor. |
| `_RTSSetIntegerG` | `0x43939C` | Integer setter. |
| `_RTSGetNamedInteger` | `0x4394F4` | Named integer accessor. |
| `_RTSIsSMSTriggerOn` | `0x439548` | Trigger query. |
| `_RTSGetTrackItems` | `0x4805D8` | Track-item accessor. |
| `_RTSGetLocoObject` | `0x536650` | Locomotive/engine object resolver. |
| `_RTSGetLocoObjectByName` | `0x536920` | Named locomotive-object resolver. |
| `_RTSGetCabObject` | `0x536B14` | Cabin-object resolver. |
| `_RTSGetCabObjectByName` | `0x536F08` | Named cabin-object resolver. |
| `_RTSGetObject` | `0x537590` | Routes generic object requests. |
| `_RTSGetObjectByName` | `0x5375F8` | Routes named generic requests. |

`_RTSGetObject` routes requests to the loco/cabin resolvers.  The loco and
cabin resolvers conditionally use an adapter table before yielding an object.

## `DLLVersion` and ABI checks

### CONFIRMED

After a successful `InitRTSLibrary`, `0x5364D0 sub_5364D0` resolves
`_DLLVersion` and:

- requires `DLLVersion(DLLV_VERSION) >= 101`;
- saves `DLLVersion(DLLV_STACKSIZE)` at descriptor `+0x14`;
- checks the returned sizes for `TrackItem` and `TrackItemsItem`, recording
  mismatch bits in descriptor `+0x18`;
- when `DLLVersion(DLLV_ADAPTER_COMPLIANCE) < 39`, compares a set of legacy
  ABI object sizes (including switch, display, screen, locomotive, engine,
  light, class-subobject and object);
- accepts version `104` directly when adapter compliance is at least `39`;
- invokes `0x537DC8 sub_537DC8` for supported non-current versions.

The original `ts.h` defines `DLLV_ADAPTER_COMPLIANCE` as query `18`,
`DLLV_STACKSIZE` as query `19`, and emits compliance value `39` when the
corresponding adapter-compliance macro is enabled.

## Primary callbacks

### CONFIRMED

`0x5026FC sub_5026FC` stores the primary callback addresses after resolving
them from descriptor `+0x04`.  Its callers observed in this pass include
`0x468C74`, `0x4F995C`, and `0x57CD78`.

`0x468C74 sub_468C74` calls primary `ChangeLoco` and the auxiliary dispatch
for that event.  `0x528540 sub_528540` invokes primary `CanSwitch` with
auxiliary pre/post handling; `0x5288CC sub_5288CC` does the corresponding
`Switched` dispatch.  These assignments are based on resolved export strings
and the observed saved function-pointer calls.

## Auxiliary DLL loading and dispatch

### CONFIRMED

When parsed auxiliary data is present, `0x5026FC sub_5026FC` creates an
auxiliary stack and calls:

- `0x56DC3C sub_56DC3C` — creates auxiliary entries from parsed data and
  builds default callback configuration if none is present;
- `0x56E970 sub_56E970` — loads each auxiliary module through `sub_56D9C8`,
  resolves `_Init`, `_ChangeLoco`, `_LostMaster`, `_ALSN`, `_SpeedLimit`,
  `_Run`, `_CanSwitch`, and `_Switched`, and allocates its variable stack;
- `0x56EE38 sub_56EE38` — invokes auxiliary `Init` after primary `Init`;
- `0x56EADC sub_56EADC` — dispatches Run, ChangeLoco, LostMaster, CanSwitch,
  and Switched according to configured ordering;
- `0x56EF60 sub_56EF60` — dispatches ALSN;
- `0x56F068 sub_56F068` — dispatches SpeedLimit.

No auxiliary `_CanWorkWith` resolution was observed, matching the documented
auxiliary callback set.  `sub_5031D8` provides confirmed pre-main/post-main
ordering for `Run`.

### PROBABLE

Each auxiliary entry occupies `0x54` bytes in the created array.  Its complete
field layout has not been recovered, so this document intentionally does not
assign field names beyond the callback and descriptor accesses above.

## ABI adapters for legacy versions

### CONFIRMED

`0x537DC8 sub_537DC8` recognises DLL ABI versions `101`, `102`, and `103` and
stores a version-specific table pointer at descriptor `+0x10`.  The current
version is `104`.

The object resolvers `_RTSGetLocoObject` and `_RTSGetCabObject` use that table
to choose an adapter/wrapper path.  `0x53782C sub_53782C` and
`0x538548 sub_538548` perform observed deferred adapter-update processing after
plugin callback paths.  Therefore a version-specific adapter layer exists for
ABI versions 101–103.

## IDA function index

| Address | Existing IDA name | Role | Confidence |
|---:|---|---|---|
| `0x4389E8` | `sub_4389E8` | Create/find cached library descriptor. | CONFIRMED |
| `0x4FE6C8` | `sub_4FE6C8` | Allocate/init one runtime engine variant. | PROBABLE |
| `0x4FE734` | `sub_4FE734` | Allocate/init another runtime engine variant. | PROBABLE |
| `0x5026FC` | `sub_5026FC` | Primary DLL bind, callbacks, Init, aux setup. | CONFIRMED |
| `0x5031D8` | `sub_5031D8` | Primary Run and aux pre/post Run dispatch. | CONFIRMED |
| `0x5364D0` | `sub_5364D0` | InitRTSLibrary and DLLVersion handshake. | CONFIRMED |
| `0x537DC8` | `sub_537DC8` | Select legacy ABI adapter table. | CONFIRMED |
| `0x53782C` | `sub_53782C` | Initiate deferred adapter updates. | CONFIRMED |
| `0x538548` | `sub_538548` | Perform deferred adapter updates. | CONFIRMED |
| `0x56D9C8` | `sub_56D9C8` | Path lookup, load lifecycle and handshake entry. | CONFIRMED |
| `0x56DBE4` | `sub_56DBE4` | LoadLibraryW wrapper. | CONFIRMED |
| `0x56DC3C` | `sub_56DC3C` | Build auxiliary-library stack. | CONFIRMED |
| `0x56E970` | `sub_56E970` | Load/resolve auxiliary callbacks. | CONFIRMED |
| `0x56EADC` | `sub_56EADC` | General auxiliary callback dispatcher. | CONFIRMED |
| `0x56EE38` | `sub_56EE38` | Auxiliary Init dispatcher. | CONFIRMED |
| `0x56EF60` | `sub_56EF60` | Auxiliary ALSN dispatcher. | CONFIRMED |
| `0x56F068` | `sub_56F068` | Auxiliary SpeedLimit dispatcher. | CONFIRMED |

## UNKNOWN / open questions

- The exact parser and raw parsed-record offsets for `LibraryModule`,
  `VariableStack`, `AuxLibraries`, and `AuxCall` have not been located.
- The full `LibraryRecord+0x18` flag semantics and consumers are unknown.
- The layouts and precise object-conversion rules of the adapter tables selected
  for versions 101–103 are unknown.
- The exact concrete types represented by the two engine allocations at
  `sub_4FE6C8` and `sub_4FE734` are not established solely by this pass.
- The complete `0x54`-byte auxiliary-entry layout is unknown.
- `sub_424488` is the direct caller of Run dispatch, but whether it is the
  top-level main loop, a train update phase, or a nested simulation routine
  requires further caller/context analysis.
- All results are static evidence only; ordering and failure handling have not
  been verified with a running original RTS instance.
