# Replacing Brakes.dll (plan)

Status: under way (2026-11-18): steps 0 and 1 done; step 2's devices
built and tested (`simulation/cab_brakes.hpp`), not yet in the game. The
developer's request:
stop patching openRTS around MaDaDev's `Brakes.dll` and replace it, after
learning how a train's DLL talks to it.

Why: every brake fix of 2026-11 was a patch around it. openRTS's valve
over its УР, the cocks it reads, the ЭП20's second cab, the КЛУБ's
thresholds that the routes' +3 km/h hides (docs/paths.md). Its air and
openRTS's realistic air keep diverging.

## What Brakes.dll is

An auxiliary library (docs/reverse/train-dll.md, "auxiliary dispatcher"),
not a locomotive's DLL. 1685 `.eng` files of the installation load it,
beside their own DLL:

```
AuxLibraries (
  AuxLibrary(
    AuxLibraryName ( "BRAKES" )
    AuxLibraryModule ( "../mdd_Libraries/Brakes/Brakes.dll" )
    AuxCall ( "CanSwitch" 0 )  AuxCall ( "Switched" 0 )
    AuxCall ( "Run" 0 )        AuxCall ( "ALSN" 0 )
    AuxLibraryParameters ( 500  BR_395ID1 ( 2 )  BR_367ID1 ( 75 )  KLUB_B0ID1 ( 413 ) ... )
    AuxLibrarySound ( "../mdd_Libraries/Brakes/Brakes.sms" ) ...
```

(the ЭД9М's; USAVP and RTSConnector are libraries of their own beside it).

**Systems**, by its parameter names:

| Prefix | Names | What |
|---|---|---|
| `BR_` | 48 | 395, 254, 367, 130 / 224 modes, cocks, ЭПТ, ЭПК mode, gauges, lamps, cabs |
| `KLUB_` | 45 | КЛУБ-У: its keys, screens, lamps |
| `SL_` | 70 | the speed recorder (КПД-3, its МУ blocks), clock |
| `SAUT_` | 16 | САУТ |
| `RC_`, `TSKBM_`, `PB_` | 8, 6, 5 | radio, ТСКБМ, others |

**Code** (IDA, 2026-11-18): 77 functions of its own, about 140 KB of x86.

| Function | Size, bytes | Role |
|---|---|---|
| `_Run` | 64 000 | everything per step |
| `updateKLUB` | 28 000 | the КЛУБ |
| `SwitchKLUB` | 5 500 | the КЛУБ |
| `sub_411458` | 11 000 | PROBABLY the recorder |
| `sub_4033D0` | 10 000 | PROBABLY the recorder |
| `sub_40F3F0` | 5 000 | PROBABLY the recorder |
| `_Init` | 3 000 | start |
| `_ALSN` | 2 500 | signals and limits |
| `_CanSwitch`, `_Switched` | small | the cab's switches |

## How a train's DLL and Brakes.dll talk

CONFIRMED where an address is given.

1. **The same objects, one after the other.**
   - RTS runs the locomotive's `Run`, then Brakes.dll's (`AuxCall` order
     0: after).
   - Both read and write the same `Engine` (docs/reverse/train-dll.md):
     `UR` `+84`, `TrainPipeRate` `+56` (−1.2 when its ЭПК trips),
     `EPTValue` `+80` and `IndependentBrakeValue` `+52` (the EP pressure it
     builds), `BrakeSystemsEngaged` `+104`, `ALSNOn` `+106`.
   - Brakes.dll writes last, so it wins.
2. **Its own memory, by name.**
   - ed9m.dll (`sub_405BD8` → `sub_403804`) calls
     `RTSGetLocoObjectByName(loco, 22, L"BRAKES")`. Object 22 is an
     auxiliary library.
   - Through its `AuxLibrary::var`, it sets bits in Brakes.dll's variables
     at `var + 32` for each car of its unit: `0x10` hold the EP brake,
     `0x20` apply it.
   - Brakes.dll's `_Run` builds the EP pressure from these bits: up to 4.3
     atm, at its rates `var + 236` / `var + 232`. It writes the pressure to
     `EPTValue` or `IndependentBrakeValue` and plays its triggers 122 / 123
     / 134.
   - So **a train's DLL commands the ЭПТ by writing into Brakes.dll's
     variable stack**. Anything replacing it must keep that object, its
     name and those offsets.
3. **The cab.**
   - The switches are shared, by the ids in `AuxLibraryParameters`.
   - Brakes.dll vetoes and answers their moves through its aux
     `CanSwitch` / `Switched`. The locomotive's DLL reads the same
     switches.
   - The КЛУБ's and the recorder's screens go through `RTSGetCabObject`.
4. **RTS's API.**
   - `RTSGetTrackItems`: the КЛУБ's map, its speed posts and the next
     object's line.
   - The aux `ALSN` call: the signals, the train's lowest limit, the next
     limit and its distance.
   - Also: `RTSGetInteger`, `RTSSetIntegerG` (the tape), `RTSGetNamedInteger`,
     `RTSIsSMSTriggerOn`, `RTSGetCabObject(ByName)`, `RTSGetLocoObject(ByName)`.
5. **Sound.**
   - `Brakes.sms` is a sound object of its own.
   - Triggers: 56 / 57 the ЭПК whistle on / off; 3196 / 3198 the КЛУБ's
     beeps; 122 / 123 / 134 the ЭПТ.
6. **Its state** lives in its variable stack. RTS saves it whole with a
   game.
   - The КЛУБ part, traced (docs/paths.md):
     - `var + 80` the limit (km/h, from `ALSN`);
     - `+140` the permitted speed;
     - `+8` flags: `0x10000000` whistle, `0x800000` the warning 2 km/h
       below, bit 8 the vigilance request;
     - `+24` the vigilance timer: whistle at 5 s, ЭПК at 12 s;
     - `+336` a limit of its own.

Step 0 (done 2026-11-18, `docs/reverse/brakes-dll.md`) answered the
rest:
- 34 of the installation's DLLs reach into `BRAKES` as ed9m.dll does.
  `var + 32` is their exchange word: they set bit 4 (the safety devices
  powered) and their ЭПТ and other commands, one step each.
- They read its bits back to cut traction: 1 the ЭПК tripped, 2 the 418,
  8 САУТ's braking. That is why the ЭД9М will not make its circuit
  without the ЭПК and the КЛУБ.
- What it writes of `Engine`, its functions by system, its parameters and
  sound triggers are mapped there; `_Run`'s brakes inside are left for
  step 2.

## The way: an openRTS "BRAKES" in the host

A train's DLL must find a `BRAKES` library where it looks, with the
variables it writes, or it stops working (the ЭД9М's ЭПТ). So the
replacement is a library of openRTS's own at that boundary. Patching
openRTS around a running Brakes.dll, as now, is what this replaces.

- **dllhost** keeps presenting an `AuxLibrary` named `BRAKES` per car,
  with a variable stack of Brakes.dll's size, but runs no Brakes.dll
  code.
  - Each step it hands openRTS what the train's DLL wrote (`var + 32`,
    the switches).
  - It writes back what the train's DLLs read: `Engine`'s fields,
    gauges, lamps, `var`.
- **simulation** does the work as openRTS's own modules, testable without
  a DLL:
  - 395 (`driver_valve`, done), 254, 367 and the cocks (the
    `BrakesLibrary` reading is done), 130 / 224, ЭПТ, ЭПК with its key
    and whistle;
  - then the КЛУБ-У, the recorder, САУТ;
  - all on the realistic air.
- **A switch per locomotive:** Brakes.dll or openRTS's. Any car whose
  parameters openRTS does not understand keeps Brakes.dll, so nothing that
  works breaks. Brakes.dll stays the default until testers have had
  openRTS's on the ЭД9М and the ЭП20.
- **A second setting, its numbers** (the user's, 2026-11-18): "as
  Brakes.dll" (the default) or "openRTS".
  - With openRTS's brakes it picks the coefficients: the rates, the
    pressures, the times.
  - "As Brakes.dll" takes them from `docs/reverse/brakes-dll.md` §10.
  - "openRTS" takes the real devices' where the books or testers' videos
    differ, each difference listed for the user to decide.
  - Brakes.dll's own numbers cannot change without changing its code.

## Steps

0. **The rest of the reverse** (`docs/reverse/brakes-dll.md`): `_Run` cut
   into its systems, its `var` mapped, every `Engine` field it writes,
   every switch parameter, every sound trigger. Also, from the other
   MaDaDev DLLs (search for `BRAKES` in each), where they reach into it.
   Notes describe behaviour and interfaces; no MaDaDev code goes into the
   repo.
1. **Golden runs** (done 2026-11-18, `docs/reverse/brakes-dll.md` §9:
   `--record`, `tests/data/golden/`, `tools/reverse/golden_report.py`).
   - Scripted drives through `openrts-train-check --dll ... --script` with
     Brakes.dll (`tests/data/*.script`, as `ep20_cabs`).
   - Each run records its `var`, `Engine`'s outputs, sounds and the
     screens' texts.
   - The replacement is compared to them step by step. Testers' videos and
     the books give the real device where Brakes.dll differs.
2. **The brakes part:** 395, 254, 367 and cocks, 130 / 224, ЭПТ (with
   `var + 32`), ЭПК, gauges and lamps.
   - In "openRTS brakes" mode Brakes.dll still runs for the КЛУБ and the
     rest, and its brake outputs are not used.
   - First on the ЭД9М and the ЭП20, whose cabs differ most.
   - 2.1 (done): `_Run`'s brakes read (`docs/reverse/brakes-dll.md` §10).
   - 2.2 (done): the devices as `simulation/cab_brakes.hpp`
     (`step_cab_brakes`, `tests/cab_brakes_test.cpp`): the 395 and 130,
     the 254, 224 and 4ВК, the cocks, the ЭПТ's commands, the gauges and
     lamps, the 418; two sets of numbers (below).
   - 2.3: the host and the controller: the devices work the realistic
     air instead of Brakes.dll's valve; the train's DLL's `var + 32` bits
     before Brakes.dll's `Run` reach them; the cab's gauges and lamps
     (`BR_MAN*`, `BR_LAMP*`) show theirs; Brakes.dll's ЭПК trip opens the
     pipe. Brakes.dll's springs (the 254's I) and its air sounds stay its
     own until step 5.
   - 2.4: the harness (`--brakes openrts`) against the golden runs.
   - 2.5: the two settings in the Train window; a car with a
     `BR_334ID`, `BR_292CUSTOM` or a parameter not known keeps Brakes.dll.
3. **КЛУБ-У on one locomotive** (the ЭД9М), as its own module:
   - the map from the track items;
   - the permitted speed and the braking curve;
   - the warnings, vigilance and the ЭПК;
   - modes, keys, screens, sounds.

   Compared to the golden runs and testers' recordings. The +3 km/h
   option becomes the КЛУБ's own setting.
4. **The other cabs**, by the `KLUB_*` and `BR_*` ids of each family; then
   the recorder, САУТ, ТСКБМ.
5. **Brakes.dll not loaded** for a locomotive switched to openRTS's.

Each step keeps the switch back to Brakes.dll.

In a network game the modules run in `simulation`, so the server can run
them later as it does the air.

## The numbers

`CabBrakeNumbers::brakes_dll()` and `openrts()`. openRTS's are Brakes.dll's
where those fall within what the books give, else the books'. Sources:
Крылов В. И., Крылов В. В., «Автоматические тормоза подвижного состава»,
1983 (the book's pages); `docs/reverse/air-brakes-real.md`.

| What | Brakes.dll | The books | openRTS |
|---|---|---|---|
| V: the УР down | 0.22 atm/s | 0.5 → 0.4 MPa in 4–6 s, 0.17–0.25 (p. 102) | 0.22 |
| VЭ (Va): the УР down | 0.03 atm/s | 0.5 → 0.45 MPa in 15–20 s, 0.025–0.033 (p. 103) | 0.03 |
| II: the stabiliser | 0.002 atm/s | 6.0 → 5.8 in 80–120 s, 0.0017–0.0025 (air-brakes-real, 9) | 0.002 |
| 254: III, VI | 1, 4 atm | 1.0–1.3, 3.8–4.0 atm (p. 115) | 1, 4 |
| I: the УР up | 0.18 atm/s at most | through a 1.6 mm hole, no time given (p. 99) | 0.18 |
| III: the УР after the pipe | 0.1 atm/s | through the 1.6 mm hole, down only (p. 100) | 0.1 |
| VI: the УР out | 0.4 atm/s | through wide channels, no time given (p. 103) | 0.4 |

**The differences** (the user decides on each):
1. **The charging pressure.** Brakes.dll holds the УР 0.1 atm below the
   one `BR_ZARPRESS` or the reducer's label names (the ЭД9М's 4.6 gives
   4.5, the ЭП20's 5.1 gives 5.0). The reducer holds what it is set to,
   within 0.005 MPa (p. 100). openRTS: the pressure named.
2. **The 254's I.** It lets the engine's own train brake off (p. 114);
   Brakes.dll does so only with `BR_BUFERKVT` (0 on the ЭД9М and the
   ЭП20). openRTS: always.
3. **The 254's II.** Brakes.dll lowers its target 0.1 a step (about
   3 atm/s); openRTS drops it at once and the cylinder lets out at the
   engine's own rate (the book: fully released within 13 s, p. 114). In
   the game they look alike.
4. **The gauges' needles.** Brakes.dll's lag: the ТЦ 0.2 atm/s within 0.5
   of the pressure and 1 atm/s beyond, the ТМ falling 0.5 atm/s. openRTS:
   the pressures as they are, as the cab gauges under openRTS's air
   already show them (testers, 2026-11-17).

**Not modelled, for a decision:** the УР warms after a braking and gains
0.005–0.01 MPa after a step and 0.02–0.03 MPa after full service (p. 103).
In IV the pipe follows it, so a 483 in flat mode may release (it releases
on a rise of 0.015–0.03 MPa).

The ЭПТ's level for cars on RTS's air (`ep_value`) fills at about 1 atm/s
and lets out 0.35 atm/s (the ЭВР 305, docs/reverse/air-brakes-real.md);
on the realistic air the cars' ЭВР take only the command. The 418's
numbers stay Brakes.dll's: the device came after the book.

## Open

- **Licence:** Brakes.dll is MaDaDev's, under its own licence
  (`mdd_Libraries/License_for_RTrainSim_Add-ons_MaDaDev.pdf`).
  openRTS's is written anew from behaviour; the content's `.eng`
  parameters are read as data.
- **Content:** other authors' content may use Brakes.dll in ways the
  installation's does not show.
