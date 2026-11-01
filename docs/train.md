# Train on the track (plan)

Status: done (2026-10-31), except the items under "Not in this step".
Roadmap step 2: a real consist from the content stands on a
route and can be driven with the keyboard. It must pass junctions, sound
like a train and behave like RTS where RTS defines the behaviour. Cab, DLLs,
signals and AI trains come later. The interfaces are laid out so that they
plug in without rework.

## What exists

- **Track** (`docs/track.md`):
  - track network, travellers with the RTS junction modes;
  - look-ahead, locating a position on the track.
- **Rendering:**
  - shapes with node animation (wheels, bogies, pantographs use the same
    mechanism as switch blades);
  - instanced drawing.
- **Sound** (`docs/sound.md`):
  - `.sms` runner, group choice by owner (player train → group 7);
  - joint and junction events, distance travelled, speed, acceleration,
    radius variables.
- **RTS reverse notes** (`docs/reverse/runtime-overview.md`):
  - simulation loop `0x424488`, car layout, forces;
  - coupler solver `0x505414` with 0.05 s sub-steps and static-friction
    brakes;
  - car movement by travellers `0x4F5B34`;
  - wagon type parser `0x44247C` with the coupling block and defaults.
- **Schema:** `docs/RTSFileFormat.dat`, the RTS file schema: 803 node types
  with parents, parameters and descriptions, including Coupling, Friction,
  brakes, TrainCfg, EngineData. It is the field reference for the parsers.
  It has no `MaxPower`: RTS leaves traction to the locomotive DLL.

## Content (installed)

| What | Count |
|---|---|
| `TRAINS/CONSISTS_RTS` | 1418 `.con` |
| `TRAINS/CONSISTS` | 924 `.con`, in subfolders by train type |
| `.eng` / `.rtseng` | 2247 / 724 |
| `.wag` / `.rtswag` | 6427 / 4833 |
| `LibraryModule` (DLL) | 655 engines; DLLs are deferred (`train-dll.md`) |

- **Values carry units and arithmetic:**
  - units: `2.02m`, `75t`, `1e6N/m`, `2cm`, `6mph`, `730kN`;
  - expressions: `"2.5*100t"`.
- **Text:** Cyrillic folder names, UTF-16 and 8-bit files.

## 1. Reverse notes (targeted, IDA)

Extend `docs/reverse/runtime-overview.md`, or a new `docs/reverse/train.md`:
- **Value parser:** units and `*` expressions, exactly as RTS evaluates
  them.
- **`.con` loader:**
  - `Engine` / `Wagon`, `UiD`, `EngineData` / `WagonData` (file, folder);
  - `Flip`;
  - where it looks: `CONSISTS_RTS` vs `CONSISTS`.
- **`.rtseng` / `.rtswag`:** whether they overlay or replace the
  `.eng`/`.wag` blocks, and which keys they may carry.
- **Brakes in the engine:** done, see `docs/reverse/brakes.md` (pipes and
  segments, venting, triple valve with graduated release, EP, independent
  brake, main reservoir, brake force). Left: initial pressures, the
  emergency flag source, `TrainPipeLeakRate`.
- **Trains without a DLL:** how RTS drives AI trains (force and braking
  model). It is the closest RTS reference for the keyboard controller.
- **Car geometry and animation:**
  - bogie positions and pitch from bogie heights;
  - wheel and bogie node animation (`0x4F0BC8`: `wheels*`, `bogie*`),
    wheel radius;
  - flipped cars.
- **Leftovers:**
  - the coupler damping step `0x504648`;
  - the rebasing of offsets;
  - when RTS calls the derailment `0x5059A8`.

## 2. Formats

In `formats`, all with byte and VFS entry points:
- **Values:** a parser for quantities with units and `*` (the RTS rules
  from step 1), with SI results and tests per unit.
- **Consists:** `.con` → `ConsistDefinition` with a list of
  `{engine|wagon, file, folder, flipped, uid}`.
- **Wagons and engines:** `.wag` / `.eng` → `WagonDefinition`:
  - shape, size, mass, wheel radius, number of wheels;
  - couplings (both ends: type, stiffness, damping, break, `r0`, rigid);
  - friction, adhesion, derailment values;
  - brake equipment and its values;
  - sounds (`Sound`, `RTSExteriorSound`, `RTSCabinSound`), lights
    (parsed, used later).
- **Engine part:** `EngineDefinition`:
  - type (electric, diesel, steam);
  - `MaxForce`, `MaxPower`, `MaxVelocity`, `MaxCurrent`;
  - brake controllers, dynamic brake values;
  - `EngExtended` (`LibraryModule`, `VariableStack`, `NumEngines`, cabin,
    and the like) — parsed and kept for the DLL step.
- **RTS files:** `.rtseng` / `.rtswag` applied as step 1 finds.
- **`openrts-train-check`:**
  - parses every `.con`, `.eng`, `.wag` of the installation;
  - resolves every consist entry to its file;
  - reports unknown keys and missing files (as the sound and track checks
    do).

## 3. Simulation

In `openrts_simulation`, no rendering.

### Model

- **`Train`:** cars in order, a direction, a reference traveller.
- **`Car`:**
  - definition, flipped flag;
  - offset in the train (`double`, m), velocity, acceleration;
  - forces;
  - brake state;
  - two bogie travellers (front and rear).

### Placement

- A consist is put on the track at a `TrackNetwork::Location` (the probe,
  or a point on the track near the camera) facing a direction.
- The cars are laid back along the track through junctions as they are set.
- Placing a train again first frees the junctions it held (`vacate()`): the
  junctions under it and those its tail still kept for ClearanceDist.
- Changing ends (`turn_around()`, viewer key X): only while standing. The
  old rear becomes the train front facing the other way, the car order
  reverses and every car's `flipped` toggles (each car stays as it is on the
  track); controllers move with their cars. The viewer then puts the driver
  on the new front engine with the old handles, throttle 0 and reverser
  neutral. How RTS changes cabs (ChangeLoco and the master) is not traced.

### Movement

As `0x4F5B34`:
- every bogie traveller is the reference traveller advanced by the car's
  offset;
- the head uses junction mode `occupy`, the tail `release`. So trailing
  run-throughs, occupancy and clearance work as in `docs/track.md`;
- running off the end of the track stops the train (derailment is step 6).

### Forces

The RTS rules from the notes:
- rolling resistance by the MSTS `Friction` law;
- overspeed penalty above `MaxVelocity`;
- curve resistance `7/R · m`;
- grade `-sin(pitch) · m · 10`.

### Couplers

The RTS solver:
- springs per coupler with tension and compression stiffness and `r0`;
- dissipative forces as static friction;
- fixed 0.05 s sub-steps.

### Brakes

As `docs/reverse/brakes.md`:
- connected pipe segments, venting at open ends and in an emergency;
- charging from the main reservoir with distance attenuation `min(1, 30/L)`;
- equalisation between cars;
- triple valve with control chamber, graduated release and the
  auxiliary-reservoir limit;
- EP brake, twin (charging) pipe;
- independent brake, handbrake, brake skids;
- brake force from the cylinder ratio.

**Keyboard brake valve: the 395 of the ВЛ10к DLL.** The cars keep the RTS
triple valve, the only brake model now. The keyboard controller's valve is
the one the ВЛ10к DLL (`tsrLoco_VL10k-1461/cab/vl10k_main.cpp`) runs on it, with the
same constants. It keeps the equalising reservoir in
`DriverHandles::equalising` (`Engine::UR`):

| Position | Key | Action |
|---|---|---|
| I | hold `[` past II (springs back to II) | УР +0.25 atm/s up to the main reservoir; pipe fed at `(УР − p)/1.8` |
| II | | УР up to the charging pressure (5.0) at ≤0.25 atm/s. Stabiliser: УР −0.003 atm/s while above the pipe. While the locomotive is braked, УР +0.15 atm/s (the DLL's own overcharge to release through the RTS valve). Pipe follows the УР (venting ≤0.5 atm/s) |
| III | | lap: vents down to the УР, else −0.005 atm/s (leaks) |
| IV | | lap with feed: +0.05 when the УР is 0.1 above the pipe |
| VЭ | | electro-pneumatic application (ЭПТ on); the pipe only vents down to the УР. Without ЭПТ it is a lap |
| V | `]` | УР −0.2 atm/s, pipe −0.25 atm/s |
| VI | `]` | emergency: pipe −1.2 atm/s |

**ЭПТ** (key K; the ЭР9Т DLL's layer):
- The demand (`DriverHandles::ep_target`, `Engine::EPTvalue`) rises at
  0.5 atm/s in VЭ and V, holds in III/IV and falls at 0.8 (I) or 0.5 (II)
  atm/s, at most 6.
- VI is pneumatic.
- As in RTS, the simulation spreads the demand from the engine to the
  neighbouring cars with EP brakes, as far as the EP line runs. Those cars
  follow it within their auxiliary reservoir and ignore the pipe.

**As in RTS:**
- RTS does not use `TrainPipeLeakRate` (no reader in `rts1.exe`). So the
  stabiliser only works while the reservoirs draw air. An overcharge from I
  then stays until the next application.
- It does not brake the train.
- The earlier `distributor_stability` setting was removed.

#### Proposal: realistic pneumatics (openRTS path)

Not implemented; to be agreed first. The RTS model (`docs/reverse/brakes.md`)
stays as it is and remains the only path with Legacy on.

**Setting.** `TrainSettings::brake_model`: `rts` (the default and the Legacy
path) or `realistic`, and the server
fixes it like the coupler sub-steps.

**Structure.** The `realistic` path has three parts:
- **Pipe** (`BrakePipe`): per-car segments with a volume `V_i` (pipe length ×
  area; the area is set from the pipe bore, the 34.3 mm in one source is
  unverified). Flow between neighbours is `q = G · (p_i − p_{i+1})`, with `G`
  from the hose and pipe resistance. Each car leaks at `TrainPipeLeakRate`.
  Explicit integration needs `Δt < V/G`, so the pipe gets its own sub-steps
  inside the 0.05 s step (as the coupler solver does). `G` is calibrated so
  that an emergency reaches the tail of a 100-car train in about 5–6 s and the
  release wave is 50–70 m/s (§6 of the research).
- **Distributor per car**, behind an interface: `step(dt, pipe&, state&)`
  takes air from the pipe and fills or vents the cylinder. Types:
  - `rts`: the current RTS triple valve, used for Legacy and for content
    that sets nothing else;
  - `vr483` (freight): modes empty / medium / loaded and flat / mountain;
  - `vr292` (passenger): modes К / Д / УВ;
  - `evr305` (ЭПТ, with a `vr292` as the pneumatic fallback).
  The type comes from a new openRTS key in the `.wag` (MSTS/RTS ignore
  unknown blocks). Without it, the type is inferred from `Type` (Freight →
  483, Carriage → 292, EP flag → 305 + 292). This inference needs your
  decision.
- **Driver's valve 395** (`DriverValve395`, keyboard controller): the УР
  volume, the relay and the positions I–VI. The pipe at the locomotive
  follows the УР through the relay: `q = G_relay · (p_УР − p_0)`, capped by the
  valve's capacity. I is held, not latched; the overcharge is whatever the УР
  reached while it was held.

**Equations (per car, `p` = pipe at the car).** Numbers from the research
table, marked there with their confidence.
- **Charging (all types):** `dZR/dt = (p − ZR)/τ_ZR` while `p > ZR`. The time
  constant τ_ZR is set by the charge times (483: 78 l to 4.8 in about 4.5 min;
  292: 130–180 s). The air taken is subtracted from the pipe segment
  (`ΔV·ΔZR`, volume-weighted).
- **ВР 483:**
  - РК charges from the pipe with τ_RK (flat 3 min, mountain 4 min to 4.6).
  - Stability: while the pipe falls slower than `s₄₈₃ = 0.3 atm/min`, РК and
    ЗК follow it: `RK = max(p, RK − s₄₈₃·dt)`.
  - Application: when `RK − p ≥ δ = 0.15 atm`, the quick-service valve
    takes `ΔДР` (UNKNOWN for 483, to be calibrated, of the order of 0.2–0.4)
    from the local pipe segment. The cylinder target is then
    `TC* = min(TC_max(mode), k_mode · (RK − p))`, where `k_mode` puts full
    service (`TC_max`) at a drop of about 1.5 atm. The cylinder moves to
    `TC*` at the fill rate (90 % in 15–22 s), drawing from the ЗР
    (`ΔZR = −ΔTC·V_TC/V_ZR`). The cylinder never exceeds the ЗР–ТЦ
    equalisation.
  - Release, flat: when `p ≥ RK − 0.025`, the cylinder vents fully at the
    release rate (head 35–40 s, tail 20–25 s), and a lap rise below that does
    nothing.
  - Release, mountain: `TC* = k_mode · (RK − p)` follows the pipe both ways
    (graduated).
  - Emergency: a pipe rate ≥ 0.8 atm/s gives TC_max at the emergency fill
    rate.
- **ВР 292:**
  - No РК: the reference is the ЗР. Stability `s₂₉₂ = 0.5 atm/min`: the ЗР
    flows back to the pipe as fast as the pipe falls.
  - Application: quick-service takes the КДР volume (1 l) from the local
    segment. `TC = 3 · (ZR₀ − p)` (the ЗР : ТЦ ratio; from content
    `TripleValveRatio` where it is set) up to equalisation at a drop of
    1.3–1.5 atm.
  - Release: a pipe rise of 0.1–0.15 above the ЗР vents the cylinder fully
    (К 9–12 s, Д/УВ 19–24 s).
  - Emergency: the accelerator vents the local pipe segment, and the cylinder
    fills to 3.5 in 6–9 s (К) or 12–16 s (Д/УВ).
- **ЭВР 305:**
  - РК (1.5 l) is filled from the ЗР or vented by the controller (ЭПТ wire
    state: apply / hold / release). The cylinder follows РК through the relay
    at about 1 atm/s, minimum step 0.2.
  - Release, full or graduated: 3.5 → 0.4 in 8–10 s.
  - The ЗР refills from the pipe during braking (inexhaustible).
  - No ЭПТ (wire loss, emergency): the 292 works on the pipe.
- **Driver's valve 395:**
  - I: УР from the main reservoir through 1.6 mm. Pipe from the main
    reservoir at full valve capacity.
  - II: the reducer holds the charging pressure in the УР (freight
    4.8–5.0 / 5.3–5.5, EMU 4.5–4.8: per train, set in the panel). Above it,
    the stabiliser removes the overcharge at 0.2 atm in 80–120 s
    (≈0.12 atm/min), which is below both stabilities.
  - III and IV: lap, without and with feed.
  - V: the УР vents at 0.2–0.25 atm/s. VA: 0.5 atm in 15–20 s. VI: emergency.
  - The pipe follows the УР in every position except VI.

**Test scenarios** (`tests/train_test.cpp`, synthetic stock, plus scratch
runs on real consists):
1. Charging from empty: ЗР and РК reach the charging pressure in the stated
   times, ±20 %.
2. Stability: a pipe drop of `0.8 · s` for 3 min brakes nothing. A drop of
   `1.5 · s` brakes. Per type.
3. Sensitivity: the minimum service step (passenger 0.3, freight УР 0.6–0.7)
   applies every car of a 100-car train within the wave time. The first-step
   cylinder is in the range expected from the ratio, not zero.
4. Full service: a drop of 1.5 atm gives TC_max of the mode, and a further
   drop adds nothing.
5. Overcharge: I until the УР is at 6.0, then II. 6.0 → 5.8 takes 80–120 s,
   and no car of a 100-car freight train applies.
6. Release: 483 flat releases fully, head 35–40 s and tail 20–25 s; 483
   mountain releases in steps; 292 releases fully on +0.15; 305 releases
   graduated.
7. Emergency: the tail of a 100-car train responds in about 5–6 s; fill times
   per type.
8. ЭПТ: TC 3 atm in 2.5–3.5 s in every car at once. The pipe stays charged.
   A wire loss falls back to the 292.
9. Legacy: the RTS model is unchanged (the existing tests, `train-check
   --run`, the service-step table of the research measured with
   `brake_model = rts`).

**What changes against today:**
- Per-type distributor values replace the RTS triple valve on this path.
- The keyboard controller's brake handle becomes the 395 valve with an УР
  (held I instead of a latched release).
- The pipe becomes a flow model with sub-steps on the realistic path. The RTS
  path keeps `min(1, 30/L)`.
- Release becomes per type (full for 292 and 483 flat).
- Quick-service, load and length modes, and the emergency accelerator are
  added.

**Constraint from RTS (§8 of the research).** In RTS the driver's valves
live in the locomotive DLLs on the `ts.h` interface (sources: ВЛ10к, ЭР9Т;
the MaDaDev add-on `mdd_Libraries/Brakes/Brakes.dll` is another such valve).
They are tuned against the RTS triple valve; the ВЛ10к overcharges by itself
in II to release. A locomotive run by a legacy DLL therefore keeps
`brake_model = rts` for its train. `realistic` goes with the openRTS valve
(keyboard controller or an openRTS plugin). The VL10k source values (УР
−0.2 atm/s in V, stabiliser 0.003 atm/s, I +0.25 atm/s) are a second
reference for the 395 model.

**Open questions:**
- **Brakes.dll** (MaDaDev add-on): optional to reverse, only to match that
  stock closely.
- **Type selection:** a new openRTS `.wag` key versus inference from `Type`.
- **Charging pressure:** the panel only, or the consist and activity too.
- **Unknown numbers:** quick-service volume of the 483, pipe bore and hose
  resistance, numeric leak gradients. To be calibrated against the wave and
  release times above, unless better sources turn up (the scbist.com chapters
  of Асадченко were unreachable).

The controller (keyboard now, DLL later) only sets the `Engine` demands:
`TrainPipeRate`, `IndependentBrakeValue`, `MainResRate`, `EPTvalue`, flags.

### Time step

- **One fixed simulation step for everything: 0.05 s (20 Hz)**, the coupler
  sub-step of RTS itself. It is also the network tick, the same under any
  setting.
- **Coupler sub-steps: a setting with two paths** (like the RTS and
  experimental shading paths):
  - **RTS-compatible (default): 1 sub-step**, the coupler solver once per
    0.05 s step.
  - **Experimental: 2, 3 or 4 sub-steps** (0.025 / 0.0167 / 0.0125 s), the
    coupler solver only.
  - **Why only the couplers.** RTS integrates the coupler springs explicitly
    (semi-implicit Euler: `v += F/m·h`, then `x += v·h`), so their accuracy
    depends on h where the system is stiff:
    - a 75 t flat car with 1.5·10⁶ N/m couplers gives ω ≈ 9 rad/s,
      ωh ≈ 0.45 (under 1 % frequency error);
    - the RTS default stiffness `mass·100` (about 10⁷ N/m) gives
      ω ≈ 20 rad/s, ωh ≈ 1 (about 5 %), closer to the stability limit.
    - **Damping is not an explicit force in RTS.** The velocity
      equalisation `0x504648` resolves velocity differences once per
      sub-step as coupler movement plus plastic momentum exchange, with the
      share set by `k / (c · Δv/h)`. It depends on h directly.
    - **Slack and static friction:** `r0` gaps and static-friction brakes
      are cut per sub-step.
  - Traction, resistance, grade, brake pipe and traveller movement change
    smoothly. They gain nothing from a smaller step and run once per step.
- **Rendering** interpolates car poses between the last two steps.
- **Input** (handles, keys) applies at the next step, at most 50 ms later.
- **Difference from RTS:** RTS splits the frame `dt` (clamped to 8–1000 ms)
  into 0.05 s sub-steps plus a shortened last one, so its result depends on
  the frame rate. openRTS uses whole steps only: the same result at any
  frame rate. That is required for multiplayer and replays.
- **Multiplayer:** the server fixes the coupler sub-steps. With Legacy on
  (see `roadmap.md`) it is 1.
- **Reverse-check:** recorded runs compare against RTS behaviour (stopping
  distances, acceleration, slack run-in).

### Locomotive controller interface

- **Interface:** a locomotive gets its forces and brake demands from a
  `LocomotiveController`. Inputs and outputs are as in `docs/train-dll.md`:
  - inputs: speed, pressures, line voltage, flags;
  - outputs: traction force, dynamic brake, brake pipe rate, main reservoir
    rate, independent brake, sanding.
- **Built-in keyboard controller for this step:**
  - throttle notches and reverser;
  - train brake: release / lap / service / emergency;
  - independent brake.
- **Traction model:** openRTS's own, from the `.eng` values (`MaxForce`,
  `MaxPower`, `MaxVelocity`, dynamic brake values), documented as such.
- **Later:** the legacy DLL bridge and the new plugin API become further
  controllers; nothing else changes.

### Train events

The train emits events for sound and, later, signals:
- joint passed (section boundary) and junction passed, per bogie;
- traveller node change (occupancy);
- end of track.

## 4. Viewer

- **Placing a train:**
  - `--consist <name|file>` (searched in `CONSISTS_RTS`, then `CONSISTS`);
  - a "Train" panel with a consist list.
  - The train is placed on a route path start (`PATHS/*.pat`, `--path`;
    rear at the start, as MSTS explore mode) or with C on the track nearest
    to the camera.
- **Controls:**
  - keys for throttle, reverser, train and independent brakes;
  - the panel shows speed, notch, pipe and cylinder pressures, forces, and
    coupler forces for the worst coupler.
- **Camera:**
  - follows the train (V);
  - free fly stays;
  - cab view is the cab step.
- **Debug views:** saved views (F6) also record the train: consist, position
  and speed. A run can then be restarted exactly.

## 5. Rendering the train

- **Car pose:**
  - position = midpoint of the bogie travellers;
  - heading and pitch from the vector between them, as RTS.
- **Submission:** car shapes are submitted every frame as dynamic instances,
  not streamed world objects. They batch with identical wagons (instancing).
- **Animation:**
  - wheel nodes rotate by distance / wheel radius;
  - bogie nodes turn to their traveller's heading relative to the car body;
  - freight animation (`FreightAnim`) is shown static.
- **Not yet:** lights and pantographs come with the cab and DLL steps
  (panto state comes from there).

## 6. Train sounds

- **Wagon and engine sounds:**
  - `Sound` / `RTSExteriorSound` per car;
  - group 7 for the player's train, 5 or 0 for others (`docs/sound.md`);
  - positioned at the car.
- **Variables:** speed, acceleration, distance to the listener, distance
  travelled, curve radius of the car's section.
- **Events:** joint and junction events per bogie (`WagonOnJoint`,
  `WagonOnJunction`, `WagonOnAnyJoint`).
- **Not yet:** cab sounds and `VariableN` from DLLs are the cab step. The
  built-in controller may set the MSTS meaning of Variable1..3 (throttle,
  brake, dynamic brake) so engine sounds react.

## 7. Checks

- **Unit tests:**
  - value units and expressions;
  - consist and wagon parsing on synthetic and real files;
  - forces (friction law, overspeed, curve, grade);
  - coupler solver: two cars settle at `r0`; static friction never reverses
    motion; one solve per 0.05 s step on the RTS path, n on the
    experimental one;
  - brake pipe propagation;
  - a train across a junction (trailing run-through, release behind the
    tail);
  - a fixed step gives the same result at different frame times.
- **`openrts-train-check`:**
  - every consist parses and resolves;
  - every consist runs 60 s on a straight test track with full throttle,
    then with the brake: no NaN, finite speeds, it stops.
- **Viewer:** a real consist (VL10k with freight cars; an ED4M EMU) on
  TestRoute and Kuban:
  - accelerates, brakes, passes switches both ways;
  - wheels turn;
  - joint sounds play.
  - Checked by screenshots, and by `--volume 0` runs for sound counters.

## Order and size

| Step | Content | Depends on | Size |
|---|---|---|---|
| 1 | reverse notes: values, .con, .rtseng, brakes, AI force, bogies | — | medium |
| 2 | formats + `openrts-train-check` (parse/resolve) | 1 (values) | medium |
| 3 | simulation: placement, movement, forces, couplers, brakes, controller, events | 2 | large |
| 4 | viewer: placing, controls, panel, follow camera, saved views | 3 | small |
| 5 | rendering: car poses, wheel and bogie animation | 3 | medium |
| 6 | train sounds | 3, 5 | small |
| 7 | checks over all consists, viewer runs | all | small |

Milestone: steps 1–5 give a train that can be driven over a route; step 6
makes it audible.

**State:** all seven steps are done.
- Reverse notes: `docs/reverse/rolling-stock.md` and `brakes.md`.
- Formats: `formats/rolling_stock`, `train_path`.
- Simulation: `simulation/train`, with the 395 valve and ЭПТ.
- Viewer: `TrainView`, path and consist choice.
- Rendering: posed bogies and wheels.
- Sounds: positional.
- Checks: `train-check --run` passes 2209 of 2209; `tests/train_test`.

## Not in this step

- **Cab view** and the locomotive DLLs (`docs/train-dll.md`).
- **Signals** (roadmap step 3): the train's events and look-ahead are ready
  for them.
- **AI trains, activities, timetables.**
- **Consist changes:** coupling and uncoupling on the move, train-to-train
  collisions.
- **Derailment:** detection is computed and reported, but there are no
  visuals or recovery yet.
- **Lights, pantographs, doors, multiple-unit control.**
