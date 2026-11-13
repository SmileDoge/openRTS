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
- Consist changes (RTS `0x469858` uncouple, `0x424488` meeting,
  `0x469ED8` coupling, `0x4FB8CC` pushing apart):
  - `uncouple(index, close_cocks, keep_open)`: the cars from `index` become
    a new train where they stand; hoses at the cut are disconnected. From
    the menu RTS closes both end cocks (`0x4FECB0`) and keeps the couplers
    open; a coupler break uncouples with the cocks open, so both parts vent
    (10 atm/s at an open end) and brake ("Train damaged");
  - the brake pipe flows only through connected hoses and open cocks
    (`pipe_ends`, ts.h flags byte 2);
  - `resolve_contacts()`: ends meeting within the step couple when both
    couplers are automatic and not left open, the closing speed reaches the
    coupling `Velocity` and the impact `overlap / dt · (m1 + m2)` (RTS's
    measure, which depends on where in the step the ends touch) is at most
    both cars' `DerailBufferForce`; otherwise momentum is exchanged
    inelastically and an overlap is pushed apart by the masses;
  - the viewer keeps cuts as further trains (not driven); the driver stays
    with the part holding the master engine.
- Changing ends (`Train::turn_around()`): only while standing. The old rear
  becomes the train front facing the other way, the car order reverses and
  every car's `flipped` toggles (each car stays as it is on the track);
  controllers move with their cars. The viewer's key X for it is gone: RTS
  has no such key, its driver walks to the other cab and the DLL takes over
  through `ChangeLoco` (not traced yet).

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

**openRTS: distributor stability on the RTS model** (2026-11-13; the Train
window's "Distributor stability in the RTS model (openRTS)", off by
default, remembered; `TrainSettings::distributor_stability`). RTS's triple
valve applies on any pipe fall below the control chamber, so a stabiliser
taking an overcharge off at 0.12–0.18 atm/min creeps the cylinders on (the
ЭД9М tape of 2026-11-12: 0 → 0.35 atm while the pipe fell 5.17 → 5.09). With
the option, while a car is released, a pipe falling no faster than the
stability (483: 0.3 atm/min, 292: 0.5) takes the control chamber down with
it (single pipe: the auxiliary reservoir, which is the chamber there), so
nothing applies; a faster fall outruns it and brakes as before. Test:
`test_distributor_stability` in `tests/train_test.cpp`.

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

#### The car menu (Num3)

RTS's menu at a coupling or a bogie (`docs/reverse/car-menu.md`), in
`TrainView::car_menu_key` / `car_menu_items` / `do_car_menu_item`:
- **Key:** Num3 (RTS layout), or ` (openRTS layout), off the cab.
  - Picking: within RTS's reach (a coupling 5 m, a bogie twice the car's
    width), the one nearest to the cursor is taken, or to the screen's
    middle when the mouse looks around. RTS takes the nearest one, which by
    a joint is often the neighbour's bogie.
  - The cab car's own bogies are offered too (RTS leaves out the player's
    locomotive).
  - In the menu, the arrows and Enter (or a click) choose an item; Esc or
    the key closes it. The engineer does not walk while it is open.
- **Joint:** uncouple, end cocks, train pipe hoses, charging pipe hoses
  and EP cable.
  - Uncouple is RTS's `(0, 0)`: the cocks stay as they are and the couplers
    are not held open. The Train window's "uncouple here" stays the closing
    one.
  - The EP demand now stops at a parted EP cable. The charging pipe itself
    is not simulated yet (no `ChargingRate`), so its hoses are only state.
- **Free end:** its cock, the coupler held open ("lever on the buffer") or
  freed, and "Couple" with a train standing within the couplers' r0 —
  `simulation::end_contact`, then `Train::couple` and `merge_units` as a
  meeting does.
- **Bogie:** brake shoe (`CarState::chocks`, 10 kN each), handbrake,
  release valve, emergency valve (`CarState::valves`).
  - The release valve bleeds the cylinder at 0.5 atm/s; the emergency
    valve vents the pipe at 18 atm/s. Both act in both brake models. Under
    the realistic air a distributor would fill the cylinder again, so the
    release valve also works the distributor of a braked car as the real
    one does: a 483 vents its РК and releases at once, its ЗР kept; a 292
    vents its ЗР until the pipe stands above it.
- **Sounds:** RTS's discrete triggers go to the cars' SMS (80 air, 81 it
  stops, 82 / 83 hose, 84 / 85 EP, 86 / 87 cock, 59 / 62 coupler, 92 / 93
  release valve).
- **Since (2026-11-04, `docs/train-systems.md`):** the SME cable and the
  high-voltage bus (they regroup the DLLs), "Fix coupling", cargo
  information.
- **Not yet:** activity board events; multiplayer.
- **Tests:** `test_car_menu` in `tests/train_test.cpp`. `--capture-menu
  <item>` opens the menu where `--capture-camera` stands, picks the item and
  prints the menu and the car states. Given again, it opens the menu again
  for each next item (uncouple, hold the coupler open, couple: `1 15 16`).
  `--capture-uncouple <index>` uncouples the driven train before that car,
  as the Train window's button does (the engineer may sit in the cab);
  `--capture-drive <index>` then makes train `index` (from 0, in the trains'
  order) the driven one, as the Train window's "drive" does.

#### Realistic pneumatics (openRTS path)

**Implemented (2026-11-01)** as `BrakeModel::realistic`
(`simulation/src/realistic_air.cpp`, the Train window's "Realistic air"
checkbox, `openrts-train-check --brake ... --realistic`). Off by default;
the RTS model (`docs/reverse/brakes.md`) stays as it is and is the only path
with Legacy on (the server fixes the choice, like the coupler sub-steps). A
train with a legacy DLL keeps the RTS model
(`LocomotiveController::rts_brakes`): its valve is tuned to RTS's triple
valve; since 2026-11-13 the Train window's "Trains with DLLs" can give it
the realistic air too ("Realistic air with DLL locomotives" below). What is
built, against the proposal below:
- **Pipe:** per-car volumes (0.8 l per metre, a 32 mm bore), flow
  `q = k · V · Δp` with k = 200 1/s between joined neighbours, about 30
  sub-steps per 0.05 s, open ends venting, `TrainPipeLeakRate`.
- **Distributor types** inferred as proposed (EP equipment → 305 with the
  292 behind it, `Type ( Freight )` → 483, the rest → 292). 483: РК with its
  0.3 atm/min stability, application at 0.15, quick service, the cylinder
  by load (3.8 / 3.0 / 1.5 atm over 60 / 35 t), flat release when the pipe
  is back within 0.06 atm of the РК (0.025 in the research made a 100-car
  train never release in II). 292: the ЗР as the reference with its
  0.5 atm/min stability, full release at +0.12 above the ЗР. 305: the
  cylinder follows the EP demand at 1 atm/s.
- **Brake shoes** (`simulation/brake_shoes.hpp`, 2026-11-13): the shoes'
  friction changes with the speed and the pressing force by the formulas of
  the Russian traction rules (Правила тяговых расчётов), K the force on one
  shoe in kN, v in km/h:
  - cast iron: φ = 0.6 · (1.6K + 100) / (8K + 100) · (v + 100) / (5v + 100);
  - composite: φ = 0.44 · (0.1K + 20) / (0.4K + 20) · (v + 150) / (2v + 150);
  - disc pads: 0.35 at any speed;
  - wood (the first railways' blocks; no rule for them, a guess):
    φ = 0.45 · (v + 60) / (2v + 60).

  The content's `MaxBrakeForce` stays the force at the full cylinder
  (`BrakeCylinderPressureForMaxBrakeBrakeForce`), taken at 40 km/h; a full
  shoe presses 30 kN (cast iron) or 15 kN (composite). Cast iron then gives
  0.68 of it at 100 km/h and twice it at a stop (more at a light
  application: a lightly pressed shoe grips relatively harder); composite
  0.86 and 1.21. Each bogie has half the force on its own shoes and the
  hand brake presses the same shoes; how much of it reaches the rail is the
  next item. RTS's model keeps a force that does not depend on the speed.
  - Wooden shoes (`wood`, for fun) keep the heat at their face (half the
    heat capacity), char from +250 K (losing grip, down to 0.35 of it at
    +800 K, and wearing away over about ten minutes at +500 K), catch fire
    at +400 K and, alight, warm by 4 K a second and burn away in two
    minutes: then the bogie has no brake. A bogie dragged at 60 km/h with
    20 kN catches fire in about 15 s. F5 marks "[колодки тлеют]",
    "[КОЛОДКИ ГОРЯТ]" and "[колодки сгорели]", the bogie smokes (the
    diesels' smoke, darker alight), the brake log counts the bogies alight
    (`тележек_горит`). The bogie's car menu puts on new shoes ("Колодки
    изношены или горят (поставить новые)"), and on the whole train at once
    new shoes of each car's kind ("Новые колодки на всём поезде") or this
    bogie's kind on every braked car ("... на всём поезде (как здесь)").
  - The shoes: the car's `ORTSBrakeShoeType ( cast_iron | composite | disc | wood )`
    in its `.wag` / `.eng`, else cast iron on engines and passenger cars and
    composite on freight cars; a consist entry's
    `ORTSBrakeShoes ( front [rear] )` overrides them per bogie (by the car's
    own ends; one value for both), and the bogie's car menu switches its
    shoes cast iron, composite, wood in turn (the label says which they are;
    the menu's bogie is the one towards the train's head or tail, the shoes
    are kept by the car's own ends).
    A saved situation keeps them. No file in the MaDaDev install names its
    shoes.
  - Measured (`openrts-train-check --brake`): a 10-car ЭД9М (cast iron)
    stops from 100 km/h in 327 m (RTS's model: 225 m); a ВЛ80Т with 90 loaded
    freight cars (composite) from 60 km/h in 394 m (393 m).
- **Adhesion, wheel slide and the shoes' warmth** (`simulation/adhesion.hpp`,
  2026-11-13; the Train window's "Rails"):
  - The rails: as RTS (no weather), by the weather (rain wet, snow snow), or
    dry, wet, snow, ice, leaves. Their adhesion is a share of the dry one:
    wet 0.65, snow 0.5, ice 0.35, leaves 0.3. With anything but "as RTS" the
    traction limit (the .eng's `Adheasion`, sand) takes it too.
  - Dry adhesion by the speed (Curtius and Kniffler): 0.161 + 7.5 / (v + 44),
    0.33 standing, 0.25 at 40 km/h, 0.21 at 100.
  - Wheel slide (юз), per bogie under the realistic air: the wheels lock when
    the shoes hold them harder than the rail's adhesion turns them; a locked
    wheel holds back with 0.75 of the adhesion, and its shoe holds it with
    the standing grip, so it rolls again only once the brake is let off that
    far, or the car stops. The engine's sand raises the adhesion by a
    quarter. F5 marks "[ЮЗ]" while it slides and "[юз был: N м]" after.
  - The shoes and the wheels' treads get wet in rain and snow (towards 1 in
    rain, 0.8 in snow and ice, 0.6 on leaves, in about 1.5 min), dry in the
    air in 10 min, and the brakes warm them: the friction's power on the
    turning wheel heats a bogie's shoes and tread skin (25 kJ/K, cooling over
    200 s, at most +800 K), and above +5 K they dry fast. Wet composite shoes
    lose up to half their grip, cast iron a fifth, disc pads a tenth; dry
    treads win back 30 % of what wet rails take from the adhesion, for the
    traction too. A light application at speed for ten seconds or so dries
    the shoes (on the test train: soaked 0.96, +82 K, then 0.09), as the
    drivers' drying of the shoes does. F5 marks "[колодки мокрые]" and
    "[колодки +N °C]" above +100 K; the brake log has the rails, the bogies
    sliding, the shoes' mean wetness and most warmth.
  - Measured, emergency from 60 km/h, a ВЛ80Т with 90 loaded freight cars:
    dry 401 m, wet 477 m, snow 467 m, ice 475 m; a 10-car ЭД9М from 100 km/h:
    dry 329 m, wet 366 m, ice 439 m (every car slid).
  - The MaDaDev ВЛ80Т's own brake: its `.rtseng` gives `MaxBrakeForce 164.4kN`
    and `ScaleBrakeForce 1.5`, 246.6 kN a section (0.26 of its weight at
    40 km/h); with cast-iron shoes it outgrows dry adhesion below about
    42 km/h, so both sections slide in an emergency (157 m of 401). Its
    `AirBrakesMainResVolume 900` lies outside every section of the `.rtseng`
    and RTS's merge leaves it out: the .eng's 66 ft³ count.
  - "Limit the brake force to real stock's" (the Train window, off by
    default, `TrainSettings::limit_brake_force`): a bogie's full brake force
    is at most what real stock with its shoes gives, its stop at about half
    the dry adhesion (0.165 of the weight at no speed, as a locomotive's shoe
    pressing gives): at 40 km/h cast iron 0.077 of the weight, composite
    0.136, disc pads 0.165 (`shoe_force_limit`). For content that brakes far
    harder than the real thing, until its files are put right.
  - `ORTSBrakeForce ( 74kN )` in a `.wag` / `.eng` (or its `.rts*`
    companion): the brake force at the full cylinder
    (`BrakeCylinderPressureForMaxBrakeBrakeForce`) at 40 km/h that the
    realistic air's shoes take in place of `MaxBrakeForce`. RTS's model, and
    RTS itself, which does not read the key, keep `MaxBrakeForce`: content
    tuned for RTS's constant force stays as it was there. `ScaleBrakeForce`
    and `AddBrakeForce` do not touch it, and the limit leaves it as the
    author gives it. As a start for cast iron: 0.077 x the mass x g at
    40 km/h (a 96 t ВЛ80 section about 74 kN, a 43 t ЭД9М head car about
    33 kN). `--brake-ratio` marks such a force with *.
  - `openrts-train-check <root> --brake-ratio <folder>...` lists each engine
    of the TRAINSET folders as RTS reads it (with its `.rtseng`): its brake
    force against its weight, what its shoes make of it at a stop in an
    emergency, and whether that slides on dry and wet rails, as is and with
    the limit. The MaDaDev install, 2026-11-13 (brake force / weight at
    40 km/h; real stock with cast iron is about 0.08): ЭП20 0.067; ЭП2К
    0.119, ВЛ82м 0.120, ВЛ60 0.141, ЧС7 0.146, ВЛ10 0.155, 2ЭС4К 0.157 (all
    slide on wet rails at a stop, 2ЭС4К on dry too); ЧС4з 0.189, the ВЛ10к's
    two sections 0.100 and 0.201, ВЛ80С and ВЛ80Т 0.257; EMU cars 0.15 to
    0.28 (ЭД9М, ЭР9Т, ЭД4М, ЭР2Т: their light cars slide on dry rails). With
    the limit none slides on dry or wet rails at a stop.
  - Measured with the limit, emergency: the 10-car ЭД9М from 100 km/h in
    669 m (329 m without, five cars sliding), wet 680 m; the ВЛ80Т with 90
    loaded freight cars from 60 km/h in 432 m dry (401 m, both sections
    sliding 157 m), 498 m wet, nothing sliding.
- **Emergency:** the front starts at the driver's valve in VI or at an open
  pipe end and moves from car to car at 280 m/s (the accelerators); the
  flow model alone would carry it faster than sound. Since 2026-11-13 a
  car's accelerator vents its pipe for 3 s after the car goes into
  emergency and only then carries the front on; the car stays in emergency
  (its distributor full on) until its pipe is above 3.5 atm and rising.
  Before, the accelerators vented until then and the front was sent again
  from every car still in emergency, so after VI the pipe never charged
  again (the driver's relay could not beat them).
- **Distributor switches** (2026-11-12), set per car in the car menu at a
  bogie (Num3 outside), only with the realistic air; the item's label says
  the position. `CarState::distributor`, `Train::set_distributor_modes`:
  - 483 load mode: empty / medium / loaded (full ТЦ 1.5 / 3.0 / 3.8). Until
    a hand sets it, it is chosen by the car's mass as before;
  - 483 flat / mountain: mountain releases in steps (the ТЦ comes down to
    what the pipe's shortfall below РК gives; a rise under 0.03 releases
    nothing) and the РК charges in 4 min instead of 3;
  - 292 plug (the 305's pneumatic part too): К — fill 0.5, release 0.35,
    emergency fill 0.5 atm/s (release 9–12 s, emergency to 3.5 in 6–9 s);
    Д — 0.25 / 0.16 / 0.25 (19–24 s, 12–16 s; the slower service fill is
    PROBABLE); УВ — as Д, and the car's accelerator does not vent the pipe
    (the front still passes on). A Luau distributor gets them as `c.load`,
    `c.mountain`, `c.plug` (`docs/scripting.md`).
- **Driver's valve 395:** the pipe follows the УР through the relay
  (20 l/(s·atm), 50 l·atm/s feed, 20 vent), fed from the main reservoir;
  VI vents at 50 l/(s·atm). The keyboard controller drops RTS's forced
  overcharge in II and its stabiliser removes an overcharge at 0.12 atm/min.
- **Main reservoir:** the real volume (`AirBrakesMainResVolume` in litres
  when above 100, else cubic feet: the install has 1 .. 84 ft³ and 175 ..
  2020 l; at a border of 50 the ВЛ80Т's 66 ft³ read as 66 l and its
  compressors cycled every few seconds) and a КТ-6 compressor
  (0.088 m³·atm/s).
  Twin-pipe stock (`Air_twin_pipe`) has its feed pipe: the engines' main
  reservoirs along the cars whose second pipe is coupled are one volume (a
  multiple unit's compressors on its motor cars feed its cab's valve).

#### The driver's valve

`simulation/driver_valve.hpp`: the 395 as a module of its own
(`step_valve_395`), which the keyboard controller works, and the boundary
between a valve and the air. A valve gives the air model its УР (the relay
holds the pipe at it), an emergency opening at the engine (VI, an ЭПК, a
stop valve), the ЭПТ demand, the independent brake's target and the
release of the engine's own brake (`EngineDemands::equalising`,
`emergency`, `ep_value`, `independent_brake`, `release_train_brake`). The
RTS model takes its pipe rate instead. Positions I, II, III, IV, VЭ, V, VI
are a cab control's states 0..6. VЭ without the ЭПТ is a lap without feed
(the ЭР9Т), or with `slow_service` a Va: the УР down 0.03 atm/s, as
Brakes.dll's.

#### Realistic air with DLL locomotives

(2026-11-13; the Train window's "Trains with DLLs", remembered as
`dll_air`; works with the realistic air on, never with Legacy;
`trainplugin::DllAir`, `LegacyConsist::set_air`.) A DLL's valve is made
for RTS's triple valve, so as RTS (the default) the train keeps RTS's air.
The other two choices take the air after the valve from the model:
- **The DLL's valve** (`dll_valve`): the cab driven (LocoFlags 1) gives its
  `Engine::UR` to the model's relay; the DLL's valve, gauges, sounds and
  protections stay its own. Its `TrainPipeRate` counts only as a direct
  vent (VI, the ЭПК, a stop valve) when it is −0.6 atm/s or faster and
  either faster than its own valve would vent (that follows the pipe's
  excess over the DLL's УР about one to one: the excess + 0.3) or with its
  УР falling faster than 0.6 atm/s (its VI). On the ЭД9М's Brakes.dll V is
  −0.23 and the ЭПК about −0.9 with the УР standing at the pipe (its tape of
  2026-11-13). With openRTS's valve over it, its own УР lags after an
  overcharge and it asks to vent the pipe down to it (−0.54 at 0.54 apart
  on the stand): a plain −0.6 threshold took that for an ЭПК in the game
  (the tape of 2026-11-13 10:53). In the driven cab a DLL valve standing cut
  off (its УР below 0.5: the 367 off, a cold start) vents nothing itself,
  so any −0.6 there is a direct vent: the ВЛ80С-1886's ЭПК from a cold
  start went unseen before (2026-11-13).
- **openRTS's valve over Brakes.dll** (`own_valve`, experimental): for an
  engine whose AuxLibraries run MaDaDev's Brakes.dll, its parameters say
  where its valve is (`BR_395ID1`, the cab control), its УР gauge
  (`BR_MANU1`, a display) and the charging pressure (`BR_ZARPRESS`;
  `EngineDefinition::brakes_library`). The handle's state works openRTS's
  395 (VЭ as Va without the ЭПТ), each state as the cab's StateHints name
  it (`trainplugin::valve_states`: "[I]" .. "[VI]", "[Vа]" / "[VЭ]", or
  words such as "экстренное", "поездное", "перекрыша без питания"): cabs
  order them either way, the ЭД9М's I..VI, the ВЛ80С's VI..I (read as
  I..VI its II was a V, so the ВЛ80С braked from the start); without hints a
  7-state control is I..VI. The charging pressure is the cab's reducer
  where Brakes.dll names one (`BR_REDID1`, its StateHints "[4.0]" ..
  "[5.9]"; the ВЛ80С's starts at 5.2), else `BR_ZARPRESS`, else 5.0. The
  blocking device усл. 367 (`BR_367ID1`, a KEY switch: 0 the key out, 1
  off, 2 on) out of its operating state (`BR_367OPPOL`, 2 on the ВЛ80С)
  cuts the valve off the pipe, as it cuts the DLL's own (its УР stays at
  0; Brakes.dll lets it on only with the handles where it wants them). The
  combined valve усл. 114 (`BR_KKID1`): "Двойная тяга" cuts the valve off
  the pipe (its УР goes on), "Экстренное" vents the pipe
  (`trainplugin::brake_cab_controls`, `BrakeCabControls::read`). The УР gauge shows its УР (the cab's
  copy of the DLL's output, `TrainView::cab_state`); the other gauges
  already show the model's pressures (on the stand: the ТЦ, ТМ and main
  reservoir displays are the host's values). Brakes.dll's ЭПК, КЛУБ, САУТ
  and ЭПТ go on: its direct vents as above, its EPTvalue as it gives it.
- In both, the compressors are openRTS's (`compressor_rate`): the DLLs keep
  RTS's main reservoir (its huge volume never drains; the ЭД9М's DLLs never
  run a compressor and take their own air from it), and their
  MainResRate is not taken. An engine releasing its own brake (EngineFlags
  1, the 254 in I) vents its cylinder as in the RTS model.
- Checked on the ЭД9М (7 cars, `openrts-train-check --dll ... --air
  dll|own`): a V of 3 s then IV gives 2.15 / 1.94 atm in every cylinder
  and II releases it; VI empties the pipe in 2-3 s and fills every
  cylinder to 3.8 in 13 s; back in II the pipe charges again and every car
  releases (35 s with openRTS's valve, 65 s with the DLL's, whose II
  charges the УР slower). Test: `openrts.dll_air` (`tests/dll_air_test.cpp`,
  the controller on outputs of its own).

Measured (`--brake 60 "1,5VL10k-1654+943b" rzd_poluvag17_96vag_7906t_1322m`,
99 cars, 8182 t, 1372 m; on 2026-11-01, with the soft relay of then: since
2026-11-13 the relay is stiff (0.4 m³/(s·atm), feed 150 l·atm/s, exhaust 80),
so the pipe at the engine follows the УР within seconds and these release
times are no longer its figures; the consist is not in the installations at
hand to measure again):

| | RTS model | realistic |
|---|---|---|
| emergency from 60 km/h | 412 m, 44 s | 418 m, 45 s |
| the last cylinder above 0.1 atm | 2.0 s | 3.1 s (the accelerator front: 4.9 s) |
| head cylinder released in II | 8 s | 148 s |
| tail cylinder released in II | 11 s | about 13 min |

The slow release in II after an emergency is how a long freight train
behaves with a 483 in flat mode: drivers release with I (an overcharge).
The other way round, a long I overcharges the РК of the cars near the
engine: they stay braked in II until the pipe is back at their РК (a
92-car ВЛ80С train, 2026-11-13: two minutes in I took the head cars' РК to
about 6.6, the tail's stayed near 5). F5 shows the РК of each 483 under
the realistic air.

Charging the same 60 loaded gondolas behind a two-section ВЛ80С
(`openrts-train-check <root> --charge 60 <engine>... <wagon> [--realistic]`,
the valve in II, 5.0 atm):

| | RTS's model | realistic |
|---|---|---|
| from empty: head pipe to 4.5 | 124 s | 83 s |
| tail pipe to 4.5 / 4.8 | 157 / 207 s | 270 / 375 s |
| tail ЗР to 4.5 | 185 s | 327 s |
| after 40 s of VI: tail pipe to 4.5 / 4.8 | 57 / 108 s | 35 / 84 s |
| every cylinder below 0.4 | 9 s | 221 s |
ЭД4М (12 cars, 292 / 305) from 100 km/h: 314 m, 19 s, released in 45 s.

Tests: `test_realistic_air` in `tests/train_test.cpp` (II holds the train
released, a slow fall below the stability applies nothing, a service step
applies every car and II releases it, VI applies the whole train within
10 s, a legacy valve keeps the RTS model).

#### Proposal: realistic pneumatics (the design it was built from)

The RTS model (`docs/reverse/brakes.md`) stays as it is and remains the
only path with Legacy on.

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
- **Debug views:** saved views (Ctrl+F6) also record the train: consist, position
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
- **Lights and pantographs** came with step 5: from the DLL, else by RTS's
  default logic from the `.eng` (`docs/train-systems.md`).

## 6. Train sounds

- **Wagon and engine sounds:**
  - `Sound` / `RTSExteriorSound` per car;
  - group 7 for the player's train, 5 or 0 for others (`docs/sound.md`);
  - positioned at the car.
- **Variables:** speed, acceleration, distance to the listener, distance
  travelled, curve radius of the car's section.
- **Events:** joint and junction events per bogie (`WagonOnJoint`,
  `WagonOnJunction`, `WagonOnAnyJoint`).
- **Engine variables:** a DLL sets Var1..8 itself (`SMSObject`). Without
  a DLL the built-in controller gives the MSTS meaning of Variable1..3:
  throttle, force against the adhesion limit, brake cylinder. Cab sounds
  came with step 5 (`docs/sound.md`).

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
    then with the brake: no NaN, finite speeds, it stops;
  - `--brake <km/h> <consist>...`: one train made of the named consists
    (searched in every subfolder), full throttle to the speed, then the
    valve to VI. Prints the stopping distance and time, when the first and
    the last car's cylinder pass 0.1 atm, the largest coupler compression
    and tension (where, when), breaks, derailments and a speed profile.
    2026-11-01, VL10-1479 + 67 / 90 freight cars (6271 / 7623 t), 60 km/h,
    level: 397 / 416 m in 44 / 45 s; the last car brakes after 1.6 / 2.0 s
    (about 600 m/s — real emergency waves are 200–300 m/s,
    `reverse/air-brakes-real.md`; whether RTS's pipe is as fast is not
    checked); compression up to 390 kN near the head 5–6 s in; no breaks,
    no derailments.
  - then the valve to II (same train): the pipe 0 → 4.2 / 3.9 atm (head /
    tail) in 20 s, 4.9 at the tail after 150 s; the cylinders are empty after
    7.5 s (head) and 9.2 s (tail) while the tail pipe is still about 1.6 atm;
    the tail auxiliary reservoir 4.32 after 60 s, 4.64 after 120 s, then
    stays at 4.79 (not 4.8 within 900 s); the main reservoir 8.50 → 8.48.
  - Why (the RTS model as ported, `reverse/brakes.md`):
    - `AirBrakesMainResVolume` is used as it stands against pipe volumes in
      m³: the VL10-1479's 750 acts as 750 m³, so the air never runs short
      (content mixes units: the VL10k has 55);
    - every car is fed from the main reservoir directly (`30 / L` factor),
      not through the cars before it;
    - the triple valve releases on any rise of the pipe, not near the
      working chamber's pressure as a 483 in flat mode does.
  - Real (`reverse/air-brakes-real.md`): an emergency wave of 200–300 m/s;
    release in flat mode only with the pipe within about 0.2–0.3 atm of the
    working chamber; an auxiliary reservoir charges in about 4.5 min; the
    main reservoir and compressors limit how fast a long train recharges.
    A "realistic air" openRTS option (off in Legacy) belongs to step 5's
    "brakes in detail", after the DLL host (DLLs drive the valves).
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

- **3-D cab** (no 2-D cabs, no `.cvf`) and the locomotive DLLs (`docs/train-dll.md`).
- **Signals** (roadmap step 3): the train's events and look-ahead are ready
  for them.
- **AI trains, activities, timetables.**
- **Derailment:** overturning and sliding as RTS starts them
  (`reverse/runtime-overview.md`); RTS's solver for derailed trains, ground
  contact and impacts are not ported.
- **Coupling hoses:** RTS's hose handling when trains couple is not traced;
  openRTS joins only the couplers (since 2026-11-13; before, it connected
  the hoses and opened the cocks too): the brake pipe and charging hoses,
  the EP cable and the end cocks at the joint stay hanging and closed, and
  the driver joins them in the car menu at the joint, as a driver does
  after coupling.
- **Lights, pantographs, doors, multiple-unit control.**

## STATIC consists (2026-11-01)

RTS keeps a train type (`0x424488`): 0 STATIC, 1 player, 2 traffic, 3 LOOSE.
A STATIC train is not updated; a LOOSE one standing below 0.3 m/s for 30 s
gets its speed zeroed and turns STATIC; a static consist pushed without
coupling turns LOOSE and moves with the speed it got (server notes) —
CONFIRMED for the 30 s / 0.3 m/s rule, the rest from the code and notes.

openRTS: `Train::set_asleep` — an asleep train is not stepped (positions,
brakes and air stay as they are) but still occupies the track, meets
trains, couples and takes hits. In the viewer a train with the standing
role sleeps after 30 s below 0.3 m/s (a consist put down from Track control
as standing sleeps at once, as RTS's static consists); a meeting that gives
it speed, a role change or uncoupling wakes it. Track control lists such
trains as STATIC.
