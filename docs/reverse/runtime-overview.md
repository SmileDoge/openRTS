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

### Force bookkeeping before the solver (`0x424488`)

#### CONFIRMED (decompiled)

Per car, each frame:

| Field | Meaning |
|---|---|
| `+0x54` | active force (traction, from the DLL) |
| `+0x50` | grade force |
| `+0x4C` | rolling and curve resistance |
| `+0x2C` | brake force |

Then:
1. `active −= grade`: the grade joins the active force.
2. `dissipative = brake + resistance`.
3. **Pre-cancel.** If `|active| ≤ dissipative`, the dissipative force eats
   the active force: `dissipative −= |active|`, `active = 0`. Otherwise
   `active −= dissipative · sign(velocity)` (sign of the velocity, not of the
   force) and `dissipative = 0`.
   - A standing train therefore does not move until traction exceeds brakes
     plus resistance.
4. **Gaps.** For every coupler, the current gap is computed from the
   offsets, half lengths and `r0` (doubled for rigid ends).
5. **Sleep.** The solver runs only if some car has an active force, a
   non-zero gap or a non-zero speed (values below 1e-6 are zeroed first).

### Coupler solver (`0x505414`)

#### CONFIRMED (decompiled)

Arguments are per-car arrays: dissipative force, active force, velocity,
gap and coupler force, plus the frame `dt`.

The frame `dt` is cut into 0.05 s sub-steps, the last one shortened. Each
sub-step:

1. **Restore forces.** The active and dissipative forces are restored from
   the frame values.
2. **Coupler springs:**
   - `gap = (offset_j + moved_j) − (offset_{j+1} + moved_{j+1}) − (L_j + L_{j+1})/2 − (r0_a + r0_b)/2`,
     minus `r0/2` again for a rigid end;
   - `F = gap · (k_tension_a + k_tension_b)` when `gap ≥ 0`, else with the
     compression stiffnesses. It is linear, with zero force at gap 0 (no
     dead band);
   - **half** of F goes to each car: `active_j −= F/2`,
     `active_{j+1} += F/2`;
   - decoupled cars (flag `0x40` in byte `+0x20`) get no force;
   - the force of the first sub-step is stored for break checks.
3. **Integration:** `v += active/m · h` (semi-implicit Euler: velocity
   first, then position in step 5).
4. **Static friction:** the dissipative force reduces |v| by `diss/m · h`.
   If that would pass zero, `v = 0` and the unused part stays as
   "remaining friction" for step 5 of this sub-step.
5. **Velocity equalisation** (`0x504648`), for every pair of neighbours
   whose velocities differ (and are coupled):
   - the relative displacement `(v_j − v_{j+1}) · h` is resolved through the
     coupler chain **towards the front** (car j, then j−1, … to 0);
   - at each coupler, part of it is taken up by the coupler: the slack
     `r0/2` of non-rigid ends, in proportion `k / (c · Δv/h)` capped at 1,
     where k is the stiffness sum and c the damping sum for the direction;
   - the rest is a **plastic momentum exchange** between car j and the
     group behind it (mass-weighted);
   - remaining static friction of braked cars absorbs part of it;
   - **the damping is not an explicit `c·v` force:** it only sets how much
     of a velocity difference becomes coupler movement versus immediate
     momentum exchange.
6. **Displacement:** `moved += v · h`.

After the frame:
- **Velocity and acceleration:**
  - `v = moved / dt` (the average over the frame);
  - `a = (v − v_old)/dt`;
  - `0x424488` sets `v = v_old + a·dt` (the same average), zeroes
    `|v| < 0.002·dt` and `|a| < 0.002`.
- **Offsets:** `offset += v·dt`, rebased so the last car stays the
  reference. A coupler gap below `0.001·dt` is snapped to exactly 0 by
  shifting all cars ahead.
- **Uncoupling** (`0x469858(train, index, close_cocks, keep_open)`), CONFIRMED:
  - the cars from `index` form a new train (type 3, standing);
  - car flags `+28`: the cut's rear car gets `0x2A00` set (hose, second
    pipe and EP line rear disconnected; byte `+29` 0x02/0x08/0x20), the cut's
    front car `0x1500` (the same at the front); the end cocks stay as they
    were;
  - `close_cocks` (or a train of type 2) runs `0x4FECB0`: rear `0x8200`,
    front `0x4100` — the end cocks closed (and EP / second pipe when both
    cars have them);
  - `keep_open` sets car `+14` bits 2 / 1 (couplers left open);
  - callers: coupler break with (0, 0) after "Train damaged"; the uncouple
    menu `0x54F288` with (1, 0); multiplayer and requests by car flags.
- **Trains meeting** (`0x424488`), CONFIRMED: neighbouring trains keep the
  gap between their ends. When a step closes it, the impact is
  `−gap_after / dt · (m1 + m2)` of the two end cars. Above either car's
  `DerailBufferForce` (`+0xE4`), or for a derailed car, `0x58A07C` handles a
  collision (impulse, damage flags 16/32 above the coupler threshold). Else
  the trains couple (`0x469ED8`) unless a coupler is not automatic (and the
  train is not type 2), a contact end is left open (`+14` 0x11 / 0x22), or
  the closing speed is below the coupling `Velocity` (`+36` of the
  coupling block); then they only push (`0x4FB8CC`: the overlap split by
  the trains' masses).
- **Coupler break** (player train only, unless switched off in the config):
  - the stored coupler force is compared with the weaker end's `Break`
    value — tension or compression by the force sign, and tension for
    rigid ends;
  - breaking uncouples (`0x469858`) and shows "Train damaged";
  - an activity counts it as failure 3.

#### Consequences for openRTS

- **The step size matters** in the springs (explicit), in the static
  friction cut-off and in the equalisation, which works per sub-step.
- **The frame-rate dependence** in RTS comes from the shortened last
  sub-step and from frame-averaged velocities.

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

## Derailment (`0x5059A8` → `0x580B30`; per frame `0x581BC4`, `0x581694`, `0x586E6C`, `0x58AB30`)

A derailed car gets a body (208 bytes, list at train +384, next at +204) —
CONFIRMED for the parts below, the rest of the module (~40 KB: a solver for
derailed trains `0x586E6C`/`0x588184`, ground contact `0x58A390`/`0x58A630`,
impacts `0x589A14`, the 19 KB `0x581ECC`) is not ported.
- **Overturning** (curve overspeed `m·v²/R > DerailRailForce`, or a tipped
  neighbour): target roll `0.1 + 0.04·|v|` rad (neighbour: 0.75 of its), at
  most 1.8; rate `|v|/33` rad/s (neighbour: 0.8 of its); drag at full roll
  `m·10` (times `0.637·target` below 1.57); the roll grows by
  `rate·dt·(|target − roll|/|target| + 0.2)` (`0x581BC4`); outwards of the
  curve or as the neighbour rolls.
- **Fallen** past 0.8 rad: car flags `0x3F00` (hose, second pipe and EP
  lines both ends) and the neighbours' facing ends.
- **Neighbours** (`0x581694`): `0.637·|roll|·m` above a neighbour's
  `DerailBufferForce` derails it; past 0.9 rad the coupler is damaged
  (flags 16/32); farther apart than the half lengths + 0.5 m, the hoses part.
- **Sliding** (a failed advance, e.g. the end of the track): an on-ground
  body moving along the car's heading with the car's speed, drag `m·5`.

- **Ground contact** (`0x58A390`, CONFIRMED structure): the body's
  corners (±width/2, height) are tested against nearby world objects
  (`0x58A630`: objects with `CollideFlags` non-zero and bit 2 clear, sphere
  ×1.42 prefilter, then their triangles); contacts sum a support force
  (contact depth × 1 170 000 for the ground kind 1, × 3000 otherwise) and
  limit the body's descent.
- **Hits** (`0x589A14`): a hit near the middle of a derailed body rolls it
  (target `F/m·t·0.04 + 0.1`, rate `|F/m·t/width|`), towards one end turns it;
  on the rails it adds to the body's motion.

openRTS: the overturn and sliding rules above; hits (`Train::hit`: on the
rails above DerailBufferForce the car comes off sliding, below it the body
takes the shove; a derailed car hit from the side rolls by the formula
above, a sliding one hit along folds); a train running off the end of the
track or into an obstacle folds: each next car derails while the stopping
force of the cars behind it (m·v over 0.5 s, PROBABLE stand-in for the
`0x586E6C` solver) is above its DerailBufferForce, folding to alternate sides
by `0.1 + 0.02·v` rad (at most 0.6); buffer collisions between trains hit
both end cars. The viewer puts slid cars on the terrain (bilinear heights,
never above the rails, at most 6 m down), lowers an overturning car onto
lower ground beside the track, and stops a sliding car at a world object
with CollideFlags (bit 2 clear) ahead within half its bounding radius.
openRTS: the overturn and sliding rules above; a fallen car's hoses are
torn (the pipe vents); past 0.9 rad the coupler breaks (the viewer uncouples
there); a sliding car slows at 5 m/s² (its drag over its mass); the viewer
rolls an overturning body about the rail on its falling side and moves a
sliding one along the line it left by, 0.3 m down.
