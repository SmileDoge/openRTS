# rts1.exe runtime overview

Status: static, read-only analysis of a copy of `rts1.exe.i64` (headless IDA 9.0
+ Hex-Rays), cross-checked against `ts.h` (TS_VERSION 104), `export.pdf` v0.43
and `ext.pdf`.  No runtime test has been performed.  Addresses refer to the
analysed `rts1.exe` (PE32, linked 2021-08-06, image base `0x400000`).

All structure offsets below are offsets observed in code.  Field names are
candidate names unless a primary source (ts.h, ENG/WAG token) names them.

## Binary

### CONFIRMED

- 32-bit Borland C++ build (`CC3260MT.DLL` RTL, Borland mangled CRT symbols,
  entry at `0x401218`).  No VCL; plain Win32 windows and message loop.
- ~3470 functions, the core ones very large (up to 44 KB each), little C++
  class structure visible: data is plain structs reached through raw offsets.
- Imports: OpenGL 1.x + `wglGetProcAddress` extensions (`init_open_gl_funcs`
  `0x4A5D10`), GLU, DirectSound (single `DirectSoundCreate`; `bass.dll` is
  separate), WinSock 2 (multiplayer), `timeGetTime`.
- Text formats are parsed by one generic token scanner (`0x4578C0`, IDB name
  `parser_maybe`) that looks up a lower-cased token in a UTF-16 buffer and
  returns the `( ... )` range.  Every format loader is a long straight-line
  sequence of such lookups, so the token string tables in `.data` are a good
  index of what each loader understands.
- MSTS install detection reads
  `HKLM\SOFTWARE\Microsoft\Microsoft Games\Train Simulator\1.0\Path`
  (`0x401C3C`); missing key is a fatal "MSTS not installed".

## Process start (`WinMain` `0x401368`)

### CONFIRMED

```text
WinMain
 -> 0x401C3C  register window classes, check MSTS registry key
     -> 0x401E7C  build temp "page_consists"/"page_engines" indexes from
                  TRAINS\CONSISTS(_RTS) and TRAINS\TRAINSET (engextended, cabin)
     -> 0x405D2C  read config.ini (viewdistance, auxthreads, usevbo, ...)
 -> 0x5B71A8  menu window (GUI\SCREENS\*.ace, GDI drawn)          | one of,
 -> 0x54AEEC  activity/editor/IPC startup (/startIPC, .act, saves) | by command
 -> 0x40521C  game window: pixel format, GL context, shaders,       | line
              texture/shape/SMS pools, then 0x44D280 main loop
 -> 0x402C2C  cleanup
```

## Main loop (`0x44D280`)

### CONFIRMED

Loading order: shaders (`0x56F1E0`: shape/shell/shadows/satellite GLSL),
trains (`0x42F4A4`: activity, savegame, multiplayer data, path, consist,
placement), world tiles (`StartAddress` `0x42188C` run synchronously once),
textures.

Per frame:

1. `PeekMessage` / `DispatchMessage`; custom thread messages carry path/consist
   load requests and network events.
2. If the camera is more than 1024 m from the current tile origin in X or Z, a
   background loader thread (`StartAddress`) is (re)started.  It loads world
   files (`WORLD\w%+07i%+07i`, `0x41B6D0`), terrain (`TILES\`, `0x49B088`) and
   grass/dynamic terrain (`0x538C14`).  Shared pools are guarded by Win32
   semaphores (`sObjA`, `sTerrain`, `sTrain`, `sSoundObj`, `TextureArr`,
   `ShapeArr`, `SMSFiles`).
3. Pending texture uploads are drained (`0x44150C`).
4. AI/static trains are (re)spawned when the camera moved more than
   `updatetrains` distance (`0x4232A8`).
5. `dt = timeGetTime() delta`, clamped to **8..1000 ms**; game clock advances
   by `dt` (wraps at 86400 s; `+`/`-` keys shift time).
6. Network tick (`0x52CD20`), activity tick (`0x530D4C`).
7. Input (`0x4121BC`).
8. **Simulation**: `0x424488(dt)` then `0x44AB7C(dt)`.
9. Render (`0x408574`), optional 30 FPS limiter.

The simulation runs on the render thread with the frame's variable `dt`; there
is no fixed outer simulation step.  Fixed sub-stepping exists only inside the
coupler solver (below).

## Train simulation (`0x424488`)

### CONFIRMED (control flow)

Iterates the train list (`dword_61D794`, linked through `train+0x18C`).  For
each train and each car (`train+0x1C` count, `train+0x20` car pointer array):

- sound/environment and tape playback housekeeping;
- signal / track-item look-ahead in both directions (`0x508520`), which also
  raises activity failures through `0x41B4F4`: 1 = passed red signal,
  3 = coupling broken, 4 = forced junction;
- locomotive DLL `Run` via `0x5031D8` (see `legacy-dll-abi.md`);
- a queue of up to 16 `{short action, short id}` pairs at `+0x90` of a
  per-car object is consumed here (PROBABLE: `Engine::GlobalMsg` written by
  DLL `PostGlobalMessage`, which has the same shape in `ts.h`): light
  group toggles (`0x438430`), door/animation/texture changes, on-screen
  messages (`0x414684`, `0x41463C`, `0x41477C`);
- brake pipe equalisation between neighbouring cars;
- per-car resistance, grade and curve forces;
- coupler solve and integration (`0x505414`);
- coupler break, derailment and uncoupling (`0x469858`);
- movement of every car along the track (`0x4F5B34`);
- train-to-train collision/coupling (`0x58A07C`, `0x469ED8`, `0x4FB8CC`).

### Car state (candidate layout)

| Offset | Meaning | Confidence |
|---:|---|---|
| `+0x0C` | engine kind (1 = electric, 2 = diesel per DLL dispatch) | PROBABLE |
| `+0x1C` | flags; bit 0 = car is flipped (velocity sign and coupler ends swapped) | CONFIRMED |
| `+0x24` | mass, kg | CONFIRMED (used as `F/m`) |
| `+0x2C` | dissipative force accumulator (brakes + resistance), N | CONFIRMED |
| `+0x44` | velocity along the train, m/s | CONFIRMED |
| `+0x48` | acceleration, m/s² | CONFIRMED |
| `+0x4C` | rolling resistance, N | CONFIRMED |
| `+0x50` | grade force, then reused as coupler force for break checks | CONFIRMED |
| `+0x54` | active (propulsive) force, N | PROBABLE |
| `+0x58` | `double` offset of the car inside the train, m | CONFIRMED |
| `+0x64`, `+0x6C` | brake pipe / second pipe pressure | PROBABLE |
| `+0x98` | 64-byte track traveller (tile X/Z `int16`, position, track node...) | CONFIRMED |
| `+0xD8` | previous traveller (copy of `+0x98`) | CONFIRMED |
| `+0xC4` | current curve radius (0 = straight) | CONFIRMED |
| `+0xC8` | pitch angle (grade) | CONFIRMED |
| `+0x138` | wagon type (parsed ENG/WAG data, shared) | CONFIRMED |
| `+0x174` | exterior equipment / light groups (116-byte records at `+0x38`) | PROBABLE |
| `+0x1A4` | cabin object | PROBABLE |
| `+0x1EC` | primary DLL `Run` pointer | CONFIRMED |

Wagon type (parser `0x44247C`): `Size` → `+0x24/+0x28/+0x2C` (W, H, **L**),
`Mass` (t × 1000) → `+0x30`, front coupling block `+0x54`, rear `+0x7C`
(copied from front if absent), `DerailRailHeight` `+0xD8`,
`DerailRailForce` `+0xDC` (default 240000 N), `DerailBufferForce` `+0xE4`,
`NumWheels` byte `+0x1D`, MSTS `Friction` ten values `+0xE8..+0x10C`,
`MaxVelocity` (inside the brake block) `+0x1B4`.

Coupling block (40 bytes): type byte (`+0`), rigid flag (`+1`), stiffness
tension/compression (`+4/+8`, defaults `mass*100`, `mass*120`), damping
(`+0xC/+0x10`, defaults `mass*200`, `mass*250`), break force
tension/compression (`+0x14/+0x18`, defaults `mass*100`, `mass*200`),
`r0` slack (`+0x1C/+0x20`).

### Forces

#### CONFIRMED

- **Rolling resistance** follows the MSTS `Friction` piecewise power law: below
  the transition speed `c1 * v^e1` (with `v <= 1` treated as 1), above it a
  second power term in `(v - v_t)`.
- **Overspeed penalty**: above wagon `MaxVelocity` resistance is scaled up to
  10× at 1.2 × `MaxVelocity`, then grows quadratically.  This is RTS-specific.
- **Curve resistance**: `+= 7.0 / R * mass`.
- **Grade**: `F = -sin(pitch) * mass * 10` (g = 10).
- **Derailment** (unless disabled in config): when `mass * v² / R` exceeds
  `DerailRailForce`, `0x5059A8` derails the car.

### Coupler solver (`0x505414`)

#### CONFIRMED

Called once per frame with `dt`.  Integrates in **fixed 0.05 s sub-steps**
(last one shortened) with explicit Euler:

1. For each coupler between cars *j* and *j+1*: gap = difference of car
   offsets − half lengths − coupler lengths (`r0`, doubled for rigid ends).
   Force = gap × (tension stiffness sum if gap ≥ 0, else compression
   stiffness sum).  Applied ±½ to both cars.  Cars flagged `0x40` in byte
   `+0x20` are decoupled from this (independent movement).
2. `v += F / m * h`.
3. Dissipative forces (brakes, resistance) are applied as **static friction**:
   they reduce |v| but never reverse it; if they would, `v = 0` and the
   remainder is kept for the next sub-step.
4. Neighbours with different velocities go through `0x504648` (PROBABLE:
   coupler damping / slack impulse).
5. Offsets advance by `v * h`.

After the loop it writes the average velocity and `a = Δv / dt` back to the
cars.  `0x424488` then re-integrates `v += a * dt`, zeroes tiny values
(|v| < 0.002·dt), and rebases offsets to the last car.  Coupler forces are
compared against the break forces; breaking uncouples the train and posts
"Train damaged" (unless `disablecouplingbreak`).

### Moving along the track (`0x4F5B34`)

#### CONFIRMED

The train keeps front/rear travellers at `train+0xA8` and `train+0xE8`.  Each
car's traveller is a copy of the reference car's traveller advanced by the
car's offset difference through `0x481440(traveller, distance, mode, train)`;
a failed advance (end of track) derails/stops the car via `0x5059A8`.  When a
traveller crosses into another track node the occupancy records
(`train+0x128/+0x148`) are updated (`0x490324`).  World position is rebuilt
as `(tile - camera tile) * 2048 + local position`; pitch is `atan` of the
height difference between bogies.

So positions are 1-D (distance along the train/track); 3-D placement is a
projection of that.

## Cab and interior

### CONFIRMED

There is no MSTS `.cvf` 2D cab path in the strings; RTS cabs are 3-D
interiors described by an RTS extension of the shape description:

- `0x51CD88`: cabin description loader — `interiorgeometryinfo`,
  `interiorchairs`, `interiorwindows` (`headoutwindow`), `switches`,
  `decorativeswitches`, `displays` (arrow / lamp / display / screen types),
  `closeups` (2-D close-up screens with `backgrounds`, `camerafov`, ...),
  `locomotivesignalization`, `signalaspects`, `lightsources`, `smstriggers`,
  `animations`.  Called from the cabin loader `0x435654` ("Failed to load
  cabin") which is triggered from the train update when the player enters.
- `0x515434`: passenger-car interior (`Interior ( InteriorFile ... )`):
  areas, doors, windows, seats, `togglelights`, `unlockdoors`.
- `0x4F0BC8`: `esd_interior_parts` of the exterior model: exterior doors,
  ladders, ledges, pantograph parts, wheels, bogies, `wagon_front/back`.

The runtime objects are the ones in `ts.h` (`ActSwitch`, `ActDisplay`,
`ActChair`, `ActWindow`, `ActDoor`, `CloseUpSubObject`, `FreeAnimation`,
`TexChange`): every switch has `State` and `SetState`, the engine animates
`State` towards `SetState` over `AnimTime`, and both player input and the DLL
only write `SetState`.

### Control contract (from `export.pdf` + `ts.h`, matches the call sites)

```text
player click/key on switch -> DLL CanSwitch(id)        (veto + sound trigger)
switch reaches new position -> DLL Switched(id)        (synchronous logic)
every frame                 -> DLL Run(eng, loco, State, dt, AirTemperature)
                               -> Engine::Force, BrakeForce, TrainPipeRate,
                                  AuxilaryRate, MainResRate, UR, sanding ...
                               -> SetState of switches/displays, lights,
                                  SMS triggers, PostGlobalMessage queue
enter/leave loco            -> ChangeLoco / LostMaster, multiple-unit grouping
                               via SMECable / HVBus (ext.pdf)
```

The engine contains no built-in locomotive logic for RTS cabs: traction,
brakes valve logic, safety devices and instruments all live in the DLL.  The
engine supplies the car/train physics, brake pipe propagation between cars,
sound, animation and the object model the DLL edits.

## Open questions

- Exact semantics of `0x504648` (damping vs. slack).
- Where `Engine::Force` / `BrakeForce` are folded into car `+0x54` / `+0x2C`
  (probably `0x503CE4` or the code right after the DLL dispatch).
- Brake cylinder model per car (`0x50D3B4`, `0x514630`) — not read yet.
- Switch animation stepper and mouse picking in the cab (`0x4121BC` and the
  renderer) — not traced; behaviour above is from `ts.h`/`export.pdf`.
