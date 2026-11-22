# Replacing Brakes.dll (plan)

Status: under way (2026-11-19): steps 0, 1 and 2 done; step 2's devices
(`simulation/cab_brakes.hpp`) in the game as an experimental setting
(docs/train.md, "openRTS's cab brakes over Brakes.dll"), Brakes.dll by
default. Step 3: 3.1 .. 3.4 done, the КЛУБ-У (`simulation/klub.hpp`)
step for step with Brakes.dll's, in the game as an experimental setting
(docs/train.md, "openRTS's КЛУБ-У over Brakes.dll"). The developer's
request:
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
   - 2.3 (done): the host and the controller: the devices work the
     realistic air instead of Brakes.dll's valve; the train's DLL's
     `var + 32` bits before Brakes.dll's `Run` reach them (protocol 14,
     `AuxExchange`); the cab's gauges and lamps (`BR_MAN*`, `BR_LAMP*`)
     show theirs; Brakes.dll's ЭПК trip opens the pipe. The springs and
     the air sounds: "The rest", stage 1 (2026-11-20).
   - 2.4 (done): the harness (`--brakes openrts [--numbers books]`) and
     `tools/reverse/golden_compare.py` against the golden runs. The ЭД9М
     and the ЭП20 match them in the УР, the independent brake and the
     ЭПТ; the pipe and the cylinders differ by the air (RTS's in the
     golden runs). In III the УР follows the realistic pipe down as the
     ЭПТ's reservoirs refill from it (the book: the УР follows the pipe
     in III). In VI Brakes.dll applies the ЭПТ only once the pipe is under
     0.1 atm; openRTS's devices not at all (the pneumatic emergency brakes
     fully).
   - 2.5 (done): the two settings in the Train window; a car with a
     `BR_334ID`, `BR_292CUSTOM` or a parameter not known keeps Brakes.dll.
   - Left for testers: the ЭД9М and the ЭП20 in the game with
     "Brakes: openRTS"; the differences below.
3. **КЛУБ-У on one locomotive** (the ЭД9М), as its own module:
   - the map from the track items;
   - the permitted speed and the braking curve;
   - the warnings, vigilance and the ЭПК;
   - modes, keys, screens, sounds.

   Compared to the golden runs and testers' recordings. The +3 km/h
   option becomes the КЛУБ's own setting. The developer's choice
   (2026-11-19): first the КЛУБ as Brakes.dll has it, the real КЛУБ-У's
   features (the ЭК by track numbers, the ALSN's frequency, the shunting
   codes) later as openRTS's settings.
   - 3.1 (done): `_ALSN`, `updateKLUB`, `SwitchKLUB` and the КЛУБ's parts
     of `_Run` and `Switched` read: docs/reverse/brakes-dll.md §11. Brakes.dll
     keeps no map and no track numbers: a number entered only switches the
     "ЭК" mode on, and the КЛУБ follows what RTS's ALSN gives along the
     set path. openRTS needs none either.
   - 3.2 (done): the harness's line (`line_signal`, `line_post`,
     `line_aspect`, `when` / `then` by the front's metres, `press` for the
     keys; the record gains the cab's aspect and the screens' texts) and
     two golden scripts: `tests/data/golden/ed9m_klub.script` (the ЭК, the
     train's data, the curves, a КЖ stopped at, the red passed with the
     ВК, an overspeed that vents the ЭПК) and `ed9m_klub_modes.script`
     (commands, 799 / 800, the МП's shunting mode, the ЭПК key, the
     КЛУБ's power, "РЕЖИМ БЕЗ ЭК", КОН).
   - 3.3 (done): `simulation/klub.hpp` (`step_klub`), its tests
     (`tests/klub_test.cpp`) and `openrts-train-check --klub shadow`:
     openRTS's КЛУБ beside Brakes.dll's on the same inputs, every step
     set against Brakes.dll's variables (the aspect, the limits, the
     target, the permitted speed, the vigilance timer, the flags, the
     track number, the entry, the ЭПК, the КЛУБ's sounds). Both scripts:
     every step the same (openrts.content.klub_ed9m,
     openrts.content.klub_ed9m_modes). Brakes.dll's arithmetic matters
     there: its km/h are a double product kept in a float (the own limit
     at a signal is let go past it only so).
   - 3.4 (done): in the game: the Train window's "КЛУБ: Brakes.dll /
     openRTS".
     - Brakes.dll's КЛУБ is switched off by its `PB_SETUP` without 2
       (protocol 15, `AuxParameterSet`).
     - The module's ЭПК and КОН vent the pipe.
     - The screen (`KLUB_DBIL`) and the lamps are drawn from the module's
       state (`simulation/klub_screen.hpp`); the shadow sets them against
       Brakes.dll's too, every object the same on both scripts.
     - Its sounds go on Brakes.sms.
     - What the train's DLL reads of a КЛУБ is written into Brakes.dll's
       variables (protocol 15, `AuxVarWrite`).
     - Both scripts pass that way too (openrts.content.klub_ed9m_own,
       openrts.content.klub_ed9m_modes_own).
     - The +3 km/h option stays the game's own (the limits reach the КЛУБ
       through it).
   - 3.5 (in part, 2026-11-19): what the testers told of the real
     КЛУБ-У set against Brakes.dll's ("The real КЛУБ-У against
     Brakes.dll's" below); their recordings (the approach to a signal,
     К282) not posted yet.
   - 2026-11-19: every cab of an engine (the ЭП1, ЭП2К, ЭП20: the working
     cab's keys and devices, each cab's screen), the БЛОК's screen (the
     ЭП20, 2ЭС6) and `KLUB_BILTYPE` 2's (the ЭП2К), the assistant's ALSN
     lamps (`KLUB_B<n>POM<cab>`, read by a wrong name length until then),
     and what Brakes.dll's other parts keep, read back from its variables
     every step (protocol: `want_aux_vars` while openRTS's КЛУБ works):
     САУТ's "on" (`+4` bit `0x40`) and permitted speed (`+256`) in the
     game too, its coefficient (`Engine +120`) and distance (`+308`),
     ТСКБМ's timer (`+12`). The limit and the next target go into
     Brakes.dll's variables after its `Run` (`AuxVarWrite::after_run`):
     САУТ reads `_ALSN`'s there before the КЛУБ. Its sounds go on the
     locomotive's SMS with `TRANS_SOUND` 1 as Brakes.dll's do. Set against
     Brakes.dll's step by step: `openrts.content.klub_2es6`, `_ep20`,
     `_ep2k`, `_ep1` (standing: the ЭК, К799, the aspects).
   - Not in it yet: САУТ and ТСКБМ of openRTS's own (Brakes.dll's run on),
     `Engine::CurrentMilepost` (the coordinate).
4. **The other cabs**, by the `KLUB_*` and `BR_*` ids of each family; then
   the recorder, САУТ, ТСКБМ.
5. **Brakes.dll not loaded** for a locomotive switched to openRTS's.

Each step keeps the switch back to Brakes.dll.

### The rest (2026-11-20)

The testers asked for openRTS's own devices in place of Brakes.dll's
altogether (the request accepted 2026-11-20): ТСКБМ, САУТ and the 3СЛ-2М
behave as the content expects them to, as the КЛУБ does. Brakes.dll is
MaDaDev's library; openRTS neither ships nor changes it, and with
"Brakes.dll" chosen everything works as before. Each stage below keeps the
switch back to it. Compatibility is checked as for the КЛУБ: the same
scenarios run with Brakes.dll and with openRTS side by side
(`openrts-train-check --klub shadow`), and their results compared.

1. **The springs and the air sounds** (done 2026-11-20). openRTS's cab
   brake devices play the content's air sounds (Brakes.sms, on the
   locomotive's SMS too with `TRANS_SOUND`; the 31xx forms with
   `BR_130MODE` / `BR_224MODE`): the valve's I and II feeding, V, the pipe
   vented, II at the charge, the ЭПТ's apply and release, the ТМ needle
   over the pipe, the 254's and 4ВК's filling and letting out
   (`LegacyConsist::own_air_after`). The valve's feed is the realistic
   air's (`CarState::valve_feed`, the 395 relay's flow into the pipe). The
   ТМ needle lags by the handle and the train's length. The springs
   (`trainplugin::brake_springs`): the 254's, 254Д's and КПТ's last
   position back once neither its key nor the mouse holds it. The 4ВК's
   direction now follows `BR_FLIPCABS`. Against the golden runs
   (`ed9m_brakes`, `ep20_brakes`, `--brakes openrts`): the same sounds
   phase by phase; the times differ by the air (in VI the realistic pipe is
   empty in under a second). In VI openRTS's ЭПТ does nothing.
2. **ТСКБМ** (done 2026-11-20): with "КЛУБ: openRTS" ТСКБМ is openRTS's
   too, part of the КЛУБ's step: its timer for the screens and the train's
   DLLs, its display and lamps (`TSKBM_DISP`, `PIT`, `L1` .. `L3`, L2's
   blink), its whistle, request, warning and the ЭПК at 0; any switch
   starts it again. Checked: `openrts.content.klub_ed9m_tskbm` (a new
   script) and the ЭП20's (the БЛОК), with the КЛУБ's own runs
   (`..._own`).
3. **САУТ** (done 2026-11-20): with "КЛУБ: openRTS" САУТ is openRTS's too
   (`simulation/src/saut.cpp`), part of the КЛУБ's step, for the КЛУБ's
   screens and the train's DLLs: its keys in the working cab, its modes
   (ГР, УК, КОМ, ПОД, К20, ОТП), its target and distance, its braking (the
   DLLs cut traction), its displays (`SAUT_DBID`, `SAUT_DFID`), its sounds
   (on the locomotive's SMS with `TRANS_SOUND`), its announcements from the
   track items (platforms, crossings and the routes' `%n` sidings:
   Новокузнецк, Чернышевск, Павловск, Кубань and Степи Приазовья have
   them); the КЛУБ's screen shows "СРЫВ ЭПК САУТ". Checked: every КЛУБ
   script (the ЭД9М's, 2ЭС6, ЭП20, ЭП2К, ЭП1) and a new one,
   `openrts.content.klub_ed9m_saut` (the modes, the aspects, a red at 90 ..
   100 m), with its own run (`..._own`). Not in the checks: the ЭД9М's
   braking coefficient (it brakes with its ЭПТ), the announcements (the
   check runs on a line without them).
4. **The speed recorder** (done 2026-11-20 for the 3СЛ-2М): with "КЛУБ:
   openRTS" the cabs without the КЛУБ that have `SL_SPEED1` take openRTS's
   speed recorder and ALSN vigilance (`simulation/recorder.cpp`): the
   needle, the clock wound by its key, the rolling odometer, the periodic
   check by the aspect and the speed with its whistle, the requests, the
   ЭПК venting when overdue or when the train runs with the ALSN off, the
   lamps (`SL_PSS`, `SL_HL`), the ВК on К, the РБ, the РБС, the ВК and the
   clock's key, its sounds. Checked on the ВЛ10
   (`openrts.content.recorder_vl10`: the КЖ's check and its venting, the
   ALSN off on the move, the РБ and the РБС, the clock), with its own run
   (`..._own`). Not openRTS's yet: the КПД-3 (`SL_KPDSPD1`: ВЛ60к, ВЛ60пк,
   2ВЛ60к, ВЛ80С), the УКБМ (`SL_SETUP` 8: 2ТЭ10У, 2ТЭ10В, 3ТЭ10У, 2М62У),
   the МУ blocks (`SL_SETUP` `0x10`: no content of the installation); the
   recorder's state in a saved game (it starts afresh); openRTS's tape
   (docs/tape.md) from it.
5. **The rest of the cab** (done 2026-11-20): the coordinate
   (`Engine::CurrentMilepost`) as RTS works it out
   (docs/reverse/train-dll.md §21; `simulation/milepost.cpp`), given to
   every engine of the player's train before its DLLs run
   (`CarInput::current_milepost`): the КЛУБ's coordinate lines and САУТ's
   modes read it (on Kuban: 1338 km at Albashi, moving 0.46 .. 0.55 km in
   500 m by the posts' spacing). The `RC_*` parameters are the
   RTSCONNECTOR library's (MaDaDev's link to its programs): nothing to take
   over there; that library stays as it is.
6. **The cars Brakes.dll keeps whole** (the list made 2026-11-20, over the
   installation's 2517 `.eng` with Brakes.dll in 52 families):
   - Taken now: the cars with `BR_KID` and the ВЛ82м's `BR_OTID` take
     openRTS's cab brakes (223 + 16 `.eng`: the 2ТЭ10М, 2ТЭ10Л, 2ТЭ10У,
     2ТЭ10В, 3ТЭ10М, 3ТЭ10У, ТЭП10Л, ВЛ80С, ВЛ82м). Of 2517, 263 keep
     Brakes.dll's brakes.
   - Brakes.dll's still, each its own port: the 334 (`BR_334ID`, the ЭР9's,
     ЭР9П's, ЭР9ПК's valve: 51 `.eng`) and the custom 292 (`BR_292CUSTOM`:
     170, the same families); the ДТ116's `BR_AUTO` (5); the КПД-3, the
     УКБМ, the МУ blocks (stage 4).
   - 2026-11-22: the ЭП2Д's ППТ (`BR_PPTID`, 26 `.eng`) is openRTS's: at
     0, with the ЭПТ on, its КПТ commands the ЭПТ by itself (VI, V, VЭ
     hold and apply, IV and III hold; PROBABLE, from Brakes.dll's `_Run`).
     The ЧС2's `BR_OTIDID1` (a typo, 23 `.eng`) is a name Brakes.dll never
     reads and no longer keeps it. `openrts-train-check <root>
     --brakes-survey` lists every family and why "without Brakes.dll"
     leaves it loaded (`trainplugin::brakes_dll_kept_reason`).
   - Every other parameter the installation gives Brakes.dll is read by
     openRTS's devices (the brakes' `understood()` rules over every
     `.eng`).
7. **Brakes.dll not loaded** (done 2026-11-20 for the КЛУБ's cabs): with
   "Without Brakes.dll where all of it is openRTS's" (the Train window, from
   the next train; `LegacyConsist::set_without_brakes_dll`, train-check
   `--brakes-dll none`) a car whose every part openRTS has — its brakes
   understood, a КЛУБ openRTS takes (with ТСКБМ and САУТ), no speed
   recorder — gets a `BRAKES` library without code
   (`AuxLibrarySpec::stub`: no DLL loaded; its parameters and Brakes.sms
   kept). Not in the host's process at all (2026-11-21): until then the
   host loaded it at its start to report it, mapped with nothing of it
   called; the Cab window says "не загружена (приборы openRTS)". The first
   train of a run too: it is made before the first frame, which read the
   box only then (until 2026-11-21 it always had Brakes.dll loaded, the
   testers' ВЛ10У-053 with 75 cars; a train loaded later had not). Checked
   on the stand with MaDaDev's Brakes.dll, its modules listed
   (`EnumProcessModulesEx`): with the box on, the ВЛ10У-053's host has
   vl10.dll and RTSConnector.dll, not Brakes.dll. openRTS's КЛУБ and cab brakes stay on for that train on the
   realistic air (to go back, load the train again with the box off). What
   the train's DLL reads from the brakes there (that they run, the
   distance, the ЭПТ's pressure, the 418, a braking position, the ALSN, the
   switches' sounds) openRTS gives. Without it every КЛУБ script passes (the
   ЭД9М's КЛУБ, modes, ТСКБМ, САУТ; the 2ЭС6, ЭП20, ЭП2К, ЭП1); tests
   `openrts.content.klub_ed9m_no_brakes_dll`, `klub_ep20_no_brakes_dll`. A
   multiple unit's other cars (the ЭД9М's motor and trailer cars, no КЛУБ)
   keep Brakes.dll. Not yet without it: the speed recorder's cabs (their
   ALSN delay), the 334 / 292 / ППТ, the КПД-3, УКБМ, МУ; the 367's lock
   while moving and the cassette's state; the cab's aspect as the train's
   DLL reads it (openRTS's КЛУБ keeps its own); a saved game without it
   not checked.

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

The ЭПТ's level for cars on RTS's air (`ep_value`) fills at 1 atm/s and
lets out 0.65 atm/s, as Brakes.dll's on the ЭД9М (its golden run); on the
realistic air the cars' ЭВР take only the command and fill and let out at
their own rates (docs/reverse/air-brakes-real.md).

The 418 (2026-11-20, the testers' request; Афонин и др. §4.7, Асадченко
§4.3.5) works as the real one with both sets of numbers under the realistic
air, on a locomotive with the ВР 483 only (`simulation::sensor418_fitted`):
the testers: the 418 is a freight locomotive's, a 292's (the ЭП1's, a ЧС's,
a multiple unit's) has none. openRTS's stock rules give freight
locomotives the 483 (`packs/stock/freight_locomotives.luau`,
docs/scripting.md), and the car menu takes the 418 off or the 483 away
(docs/train.md). Its ДДР closes when that 483 starts an application with
its additional discharge (its quick service, an emergency); with its ДТЦ
still closed (the distributor's ТЦ channel under 0.5 kgf/cm²: it opens at
0.4-0.7, set to 0.5, by МПС's ЦТ-533 and the ВЛ11's signaller; РЖД's: out
over 0.07 MPa; the testers) the relay picks up and holds itself: the lamp ТМ and no traction (for the
train's DLL as well), until that channel opens the ДТЦ. The channel is the pressure the
distributor works its cylinder to (`DistributorState::channel`), not the
cylinder: the engine's own brake let off (the 254) while the train brakes
puts the lamp out all the same. A service application: a flash (some 2.5 s
on the ВЛ10 from V); a break: lit until the channel fills (the testers: it
gets there), an emergency the same in under a second; a channel held short
of the ДТЦ would keep it lit. An overcharge going (a slow fall) and a
release do not set it off, nor the ЭПТ braking the train by wire with the
pipe standing (the 483 starts nothing). Where Brakes.dll's brakes work,
its own 418 stays.

## The real КЛУБ-У against Brakes.dll's (step 3.5)

What the testers, drivers of real КЛУБ-У, told (the Discord thread,
2026-11-18 .. 19) against what Brakes.dll's КЛУБ does
(docs/reverse/brakes-dll.md §11), and so openRTS's (step 3 repeats it).
Their recordings (the approach to a signal, К282) are not posted yet.

| The real КЛУБ-У (testers) | Brakes.dll's (and openRTS's) |
|---|---|
| The position by GPS against the electronic map (ЭК) of its section; "ЭК" in the ALSN line | no map: a track number entered switches the "ЭК" mode on |
| The target comes from the map by the track number entered, even when the train is on another track | the next signal on the path the train takes (its switches as set); the number chooses nothing |
| Entered before the entry signal: the target is the entry Н, then the track's signal (Н3, НМ3); after the exit, the first block signal | the next signal ahead: the same where the number matches the route |
| НМ21, НМ31, НМ41 along the main track need no new number; a switch onto another track does | no chains: always the next signal |
| A red at the target: the braking curve to the signal of the track entered | a straight curve, 0.08 km/h a metre (12.5 m a km/h) to the next signal; the real curve's shape not told |
| Without the ЭК (no number, or not in the map): no target, no distance, the aspect's top speed | `KLUB_GRSPD` (the ЭД9М's 120); after a yellow `KLUB_YLSPD` (60), after КЖ 20, at `KLUB_BLUCH` (1000 m); "РЕЖИМ БЕЗ ЭК" |
| Back to the ЭК by itself once the number entered is on the map ahead (track 3 at the next station) | the mode stays as long as a number is entered |
| The +3 km/h depends on the depot; without it the speed blinks from 97, sounds from 98, whistles over 100 | the same with round limits (the routes' +3 taken off by the game's option) |
| The ALSN's frequency: no codes off the section's | "50ГЦ" / "25ГЦ" shown by the line's current (DC / AC); no frequency logic |
| The shunting mode: the signal ahead keeps giving КЖ (the testers' wish) | the shunting mode (МП on Б) shows Б at 60, whatever the codes |
| К282 | not in Brakes.dll |
| К799: on white only once the speed on white is entered and confirmed | on white at once, at the speed before, while the speed is entered |

openRTS's КЛУБ does К799 the real КЛУБ-У's way (2026-11-19, the
developer's choice; `KlubState::white_799_entry`); К809 and К800 as
Brakes.dll's. The shadow sets only the entry against Brakes.dll's from К799
until its screen redraws after the speed is entered.

So the real КЛУБ-У's map (a target by the track number) and the ALSN's
frequency are openRTS's to add later as settings (the developer's choice,
2026-11-19). Open with the testers: the frequency of a section; the
shunting mode (how it comes on, its speed, vigilance, the way out); К282.

### openRTS's electronic map (ЭК), first version

With openRTS's КЛУБ the Train window's "Electronic map (ЭК) from the route
database" (on by default, remembered as `klub_map`) gives the leading cab
the map's target (`simulation/electronic_map.hpp`, the testers' rules of
2026-11-19 .. 20). Off, or on a route whose database has no station tracks:
Brakes.dll's way above.

- **The map** is made when the route loads, from its `.rtsdb`: its station
  tracks (RoutePoints of PointType 2: a station, a name such as `3`, `1А`,
  `2БП`, `6ч`, the track's two ends; the number is the name's leading
  digits) laid on the track from one end to the other, and its stations'
  ends at the lines (PointType 1 with flag 2, where the entry signals
  stand). Not the signals' letters: they are filled on some routes only
  (Kuban 900 of 1406), the station tracks on all (Kuban: 130, every one
  placed, in 0.1 s). A train signal (a NORMAL head) on a track or within
  60 m past its end is that track's (its exit or route signal); one within
  40 m of a station's end, its entry.
- **The target:** from the front, the way to the nearest track of the
  entered number ahead within 10 km, every branch looked at, then on along
  the switches' MainRoute; the target is the first train signal along it,
  its distance along that way. So a number entered before the entry gives
  the entry Н, then the track's own signal (Н3, НМ3), after the exit the
  first block signal; a wrong number gives the distance to the entered
  track's signal, whatever way the switches lie. The aspect stays the
  codes' (ALSN of the track the train is on). The screen names the target
  by its letter where the database has one.
- **A side track is taken in front of its turn** (2026-11-21, the testers:
  "the ЭК sees the tracks only in front of the signal the turns to the
  side lie beyond"): a track whose way turns off MainRoute (a facing
  junction's other branch) is taken, the ЭК not yet on, only where at most
  one train signal stands before that turn. An entry Н with a route signal
  НМ past it and the switches past НМ: "без ЭК" up to НМ, then НМ and the
  track's signal; where the switches lie right past Н, from in front of Н
  as before. Once taken it keeps it (out of its station onto the line);
  the main track (no turn) at once. Kuban, 2 km out before Албаши with 3
  entered: "без ЭК" up to the block signal, then the entry and track 3's
  signal.
- **In the station** (2026-11-20, the testers' request: on track 6 with 1
  entered the КЛУБ drew ЧМ6, on 1 with 3 НГ1; the way-to search had found
  the next station's track 1, through ЧМ6): the front on another track of a
  station with the entered one, or in its throat past its entry signal
  until it passes a track's signal, takes the entered track's signal the
  way it runs, as the real КЛУБ-У's map by its coordinate. Measured along
  that track from its point abreast of the front (in the throat: the
  metres up to its near end, straight on, added); of two tracks of the
  number ("2", "2А"; "6н", "6ч") the nearest beside the front with a
  signal ahead. Past the entered track's far end (its signal nearer than
  the train's own) the metres the front is on past it count: that signal
  passed, the next one on along its way (to 2026-11-21 the distance stood
  at the metres from the track's end to its exit, 2, and past 50 m the
  way-to search took it). However far aside the track is (a park of the
  station's; the testers' 2026-11-21: "wherever the park is"; to then not
  for a track more than 500 m off). Over every station track with another
  of its station's numbers entered, both ways (measured with the 500 m
  limit): the entered track's signal in 76 % (Чернышевск), 84 %
  (Новокузнецк), 87 % (Kuban).
- **On the line** (2026-11-21, the testers' request: an odd train on track
  1 with 2 entered follows track 2's block signals): the lines' tracks
  carry the numbers of the station main tracks (1, 2) they go on from
  (a side track's way out made the throat a "line" of its number: with 6
  entered at Деревянковка the distance stood at 548 m), along MainRoute
  from each end up to the next station's track, 40 km at most (not on
  where a side track joins: its number stops there; a node led onto by two
  numbers, a single line, gets none). The front on one of another number
  than the entered, not in a station with the entered track nor left off
  its way, takes the entered one's next train signal abreast (within
  200 m) the way the train runs: those facing as most of that node's
  signals do, so on a double line its block signals with their backs to
  the train. Measured along that track from its point abreast of the
  front (where it begins ahead, past a station's throat, the metres up to
  it, straight on, added). Kuban, 2 km out before Албаши on track 1 with 2 entered: track
  2's ALSO block signals one after another over 4 km, a back-facing head
  (`T_Head_YGR_Back`) among them left out. With no such track abreast (a
  side track's number on the line) as before: on along MainRoute.
- **The track entered** (2026-11-21, the testers' request): П, the number,
  ВВОД, then the screen's "ПРИЗН. ПРАВ." (1 the right track, "ПР"; 0 the
  wrong one, "НП"), ВВОД. The number, and the ЭК's target and distance
  with it, change only at the second ВВОД, as with command 799's speed
  (Brakes.dll's at the first); 0, no track, at once
  (`KlubState::pending_track`). Both fields show the last ones (the track
  number, its sign) until a digit replaces them (Brakes.dll's 0). The
  shadow sets nothing against Brakes.dll's meanwhile, and until the screen
  redraws. Each entry, the
  same number too, starts the ЭК afresh (`KlubState::track_entries`): left
  off its way ("без ЭК"), the number entered again takes the map again.
- **The next object line** (2026-11-21, the testers: checking the ЭК they
  could not see which signal it took): the Train window's "КЛУБ:
  platforms, crossings and limits in the next object line", off by
  default (remembered as `klub_objects`). Off, the line (objects 21, 22,
  the distance 20) always shows the target signal; on, as Brakes.dll's: a
  platform, a crossing or a limit within 500 m in its place
  (`KlubScreenInputs::objects_line`). Later, perhaps, the entered track's
  objects.
- **"Без ЭК":** no number, 99 (the instructions' way into it), a number no
  station of the route has; the entry signal of a station without the
  entered track passed; or, a track other than the main ones (1, 2)
  entered, past the first signal on past its own along its way
  (2026-11-21, the testers): past its route signal НМ3 the exit Н1 after
  the crossover, past its exit Н3 the first block signal. The train on
  another track (its switches for its own): its station's exits passed
  on (several heads of one too), "без ЭК" past the first block signal; once
  the entered track abreast is behind (its signal passed), the target that
  block signal along the train's way, the junctions as set (else the next
  station's entry), not another park's track of the number further on
  (Новокузнецк-Восточный, 1Г with 7: 7Г's signal, then the first block
  signal 5.8 km on; until then 7А's, 3.9 km on, and "без ЭК" at the second
  head of 1Г's exit). The main tracks' numbers stay the ЭК's out of the
  station by the other main track: the line's track of the number (above).
  To 2026-11-21 at any other track's signal passed. No target then, the
  aspect's speeds (`KLUB_GRSPD`, `KLUB_YLSPD`); the screen shows "РЕЖИМ БЕЗ
  ЭК" and the number entered. Without a station track ahead the ЭК goes on
  along MainRoute while the train is on the entered track's way (out of its
  station onto the line).
- **Back by itself** before the entry signal of another station with the
  track: the target is that entry (where the entries are not known, one
  train signal or none before the track). Not back in the station it was
  left in (Albashi's `2А` after its track 1 signal).
- Not yet: the ALSN's frequency from the map, its limits, switches and
  crossings, К280 / К282, manual tables for stations the database names
  otherwise. The log says each change (`КЛУБ ЭК, track 3: target НМ3 in
  1234 m`, `… без ЭК`).
- Tests: `tests/electronic_map_test.cpp` (Kuban, content: Albashi's tracks
  entered from Староминская, driven through on track 1 to Деревянковка;
  standing on its track 1 with each other track entered, both ways),
  `tests/klub_test.cpp` (`test_electronic_map`).

## Open

- **Licence:** Brakes.dll is MaDaDev's, under its own licence
  (`mdd_Libraries/License_for_RTrainSim_Add-ons_MaDaDev.pdf`).
  openRTS's is written anew from behaviour; the content's `.eng`
  parameters are read as data.
- **Content:** other authors' content may use Brakes.dll in ways the
  installation's does not show.
- **The 2ЭС4К with the ЭК** (found 2026-11-19, the shadow): with a track
  number Brakes.dll keeps the limit at `KLUB_GRSPD` (120) and the target
  at 120 / 10 000 m, as without the ЭК, where openRTS's takes the line's
  100: its `_ALSN` seems to see another limit there (the 2ЭС4К's DLL?).
  Not traced.
