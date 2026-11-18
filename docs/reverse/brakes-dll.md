# Brakes.dll (MaDaDev's auxiliary library)

Step 0 of `docs/brakes-dll-replacement.md`: what the library is, the
objects and calls it uses, its variable stack, and how the locomotives'
own DLLs read and write it. Nothing of its code goes into openRTS; this
note gives offsets and behaviour only.

**Sources** (2026-11-18). IDA 9.0 with Hex-Rays, run headless
(`tools/reverse/`), on the installation's copies:
- `mdd_Libraries/Brakes/Brakes.dll`, 337 408 bytes;
- the 34 DLLs that look it up by name (§5).

**Status words:**
- CONFIRMED: read in the code;
- PROBABLE: from the code and the parameter names, not seen running;
- UNKNOWN.

Names like `sub_4033D0` are IDA's. `updateKLUB`, `SwitchKLUB` and the
exports are the DLL's own (Borland, with symbols).

## 1. Interface (CONFIRMED)

- **Exports:**

  | Export | Address |
  |---|---|
  | `_InitRTSLibrary`, `_DLLVersion` | |
  | `_Init` | `0x424BB0` |
  | `_Run` | `0x413EDC` |
  | `_ALSN` | `0x402828` |
  | `_CanSwitch` | `0x424A34` |
  | `_Switched` | `0x424B84` |

  No `SpeedLimit`, `ChangeLoco` or `LostMaster`. The `.eng` binds it with
  `AuxCall` `CanSwitch`, `Switched`, `Run` and `ALSN`, order 0: after the
  locomotive's DLL (docs/reverse/train-dll.md, "auxiliary dispatcher").
- **RTS's functions it resolves:** `RTSGetObject(ByName)`,
  `RTSGetLocoObject(ByName)`, `RTSGetCabObject(ByName)`, `RTSGetInteger`,
  `RTSSetIntegerG`, `RTSGetNamedInteger`, `RTSIsSMSTriggerOn`,
  `RTSGetTrackItems`.
- **Its object** is RTS's `AuxLibrary` (`dllhost/src/ts_abi.hpp`): `+16`
  the locomotive, `+20` its variable stack (`var` below), `+36` its own
  sound object (`AuxLibrarySound`, `Brakes.sms`).
- **Its wrappers:**

  | Address | What |
  |---|---|
  | `0x401778` | a parameter's value: `RTSGetObjectByName(lib, aux library 4, eng param 19, name)`, the float at `+4`, else a default |
  | `0x40173C` | the same for `NAME%d`: per cab, `BR_395ID1` .. `BR_395ID<n>` |
  | `0x4017B0` | a cab switch's state (object 1, `+44`) |
  | `0x423988` / `0x423944` | its other state (`+48`): read / set |
  | `0x4238D4` / `0x423908` | a gauge's value (object 2, `+104`): set / read |
  | `0x40D4C0` | a gauge's `+44` |
  | `0x405B90` | a screen's text (object 11, a text sub-object: `+24` buffer, `+80` length) |
  | `0x405BFC` | a screen's `+164` |
  | `0x40D4F4` | a screen's `+136` |
  | `0x405D68` | a screen's `+144` / `+148` |
  | `0x40D48C` | a texture change's `+10` (object 18) |
  | `0x403204` | the locomotive (object 3 of its own) |
  | `0x403224` | the locomotive's cab (7) |
  | `0x403244` | its `Engine` (12) |
  | `0x423894` | slave *i* (20) |
  | `0x4238B4` | an auxiliary library by name (22) |
  | `0x405D30` | `RTSGetTrackItems` |
  | `0x405C34` | an SMS trigger |

  The SMS trigger always goes to its own sound object. It also goes to the
  locomotive's exterior and cab sounds when `TRANS_SOUND` is 1 or the
  train's DLL sets `var + 32` bit `0x100` (the 2ТЭ10's do).

## 2. Parameters (`AuxLibraryParameters`)

Up to 500 per `.eng`. Most are cab object ids, numbered per cab
(`BR_NUMCABS` cabs, the working one by `BR_367ID<k>` in `BR_367OPPOL`:
docs/train.md).

| Group | Names | Read by |
|---|---|---|
| brakes | `BR_395ID`, `BR_254ID`, `BR_254RID`, `BR_254DID`, `BR_254DRID`, `BR_367ID`, `BR_367OPPOL`, `BR_130MODE`, `BR_224MODE`, `BR_292CUSTOM`, `BR_334ID`, `BR_418PID`, `BR_4VKID`, `BR_4VKRID`, `BR_RAZID`, `BR_RAZOBSHK`, `BR_KID`, `BR_KKID`, `BR_KPTID`, `BR_PPTID`, `BR_REDID`, `BR_OTID`, `BR_ETID`, `BR_EPT`, `BR_EPTAPL`, `BR_EPTREL`, `BR_ZARPRESS`, `BR_AUTO`, `BR_MODE`, `BR_FLIPCABS`, `BR_BUFERKVT`, `BR_SK`, `BR_SKNUM`, `BR_VOLT` | `_Run` |
| gauges, lamps | `BR_MANM`, `BR_MANU`, `BR_MANP`, `BR_MANC`, `BR_AMP`, `BR_AMPDBL`, `BR_LAMPO`, `BR_LAMPP`, `BR_LAMPT`, `BR_LAMPTM` | `_Run` |
| vigilance | `PB_RB`, `PB_RBS`, `PB_EPKKEY`, `PB_SETUP`, `PB_FLIP` | `_Run`, `0x4239C0` (Switched), the КЛУБ |
| КЛУБ-У | `KLUB_B0ID` .. `KLUB_B9ID`, `KLUB_BxxID` (its keys), `KLUB_BxPOM`, `KLUB_DALS`, `KLUB_DBIL`, `KLUB_DPOM` (screens), `KLUB_PIT` / `KLUB_PCOUNT` (power switches), `KLUB_KASS`, `KLUB_GRSPD`, `KLUB_YLSPD`, `KLUB_TCHNUM`, `KLUB_BILTYPE`, `KLUB_BLUCH`, `KLUB_PLAMP` | `updateKLUB`, `SwitchKLUB` |
| САУТ | `SAUT_*` (16) | `0x4033D0`, `SwitchKLUB`, `0x405DCC` |
| ТСКБМ | `TSKBM_PIT`, `TSKBM_SW`, `TSKBM_DISP`, `TSKBM_L1` .. `L3` | `0x40EAA8` |
| speed recorder | `SL_*` (70): КПД-3 (`SL_KPD*`), its МУ blocks (`SL_MU*`), clock, odometer, `SL_UKBMPW` | `_Run`, `0x411458`, `0x41076C`, `0x410F84`, `0x40F3F0` |
| others | `ALSN_DELAY`, `BLOCK_MODE`, `TRANS_SOUND`, `RC_*` | |

## 3. Its functions (PROBABLE roles, by the parameters and objects each uses)

| Function | Size, bytes | Role |
|---|---|---|
| `_Run` | 64 000 | the brakes (395, 254, 367 and cocks, 130 / 224, ЭПТ), ЭПК and vigilance, 418, gauges and lamps, the recorder's clock and odometer; calls the others |
| `updateKLUB` (`0x406658`) | 28 000 | КЛУБ-У per step (docs/paths.md: its permitted speed and warnings) |
| `SwitchKLUB` (`0x40D52C`) | 5 500 | the КЛУБ's keys and entry modes |
| `0x4033D0` | 10 000 | САУТ (sounds 3200..3223) |
| `0x411458` | 11 000 | the recorder's МУ blocks |
| `0x41076C`, `0x410F84` | 2 000, 1 200 | КПД-3's displays and keypad |
| `0x40EAA8` | 2 300 | ТСКБМ |
| `0x40F3F0` | 4 900 | УКБМ power |
| `0x405DCC` | 1 900 | `CanSwitch`'s handler (367, САУТ) |
| `0x4239C0` | 4 200 | `Switched`'s handler (РБ, РБС, ЭПК key, the recorder, САУТ, cassette, 224) |
| `_Init` | 2 900 | its rates and parameters |
| `_ALSN` | 2 500 | the signals and limits |

`_CanSwitch` (`0x424A34`) also holds the cab switches of `BR_RAZOBSHK`'s
group: one cannot leave its state while another of the group is on and
the train moves faster than 0.2 m/s (CONFIRMED).

## 4. What it writes and reads of `Engine` (CONFIRMED offsets; ts.h names)

| `Engine` | Written | Read |
|---|---|---|
| `+52` `IndependentBrakeValue` | `_Run` (the EP pressure when `BR_MODE` is 1, §6) | `_Run` |
| `+56` `TrainPipeRate` | `_Run`, `0x40EAA8`, `0x411458` (−1.2 emergency, −1.5, −0.4, 1.5 ...) | `_Run` |
| `+60` `AuxilaryRate`, `+64` `MainResRate` | `_Run` | |
| `+80` `EPTValue` | `_Run` (the EP pressure otherwise), `0x40EAA8` (−1 none) | |
| `+84` `UR` | `_Run` | `_Run`, the КЛУБ, САУТ |
| `+104` `BrakeSystemsEngaged` | `_Run` (bit 2, ЭПТ) | `_Run`, САУТ |
| `+106` `ALSNOn` | `_Run`, `0x411458` | everywhere |
| `+116` `EngineFlags` | `_Run` | |
| `+120` `Flags` | САУТ | |
| `+128`, `+132`, `+136`, `+140` | | `Reverse`, `Wheelslip`, `ThrottlePosition`, `CurrentMilepost` |

It writes after the locomotive's DLL, so where both write, its values
stand (docs/brakes-dll-replacement.md).

## 5. The locomotives' DLLs reach into it (CONFIRMED)

36 DLLs of the installation hold the UTF-16 name `BRAKES`. Besides
Brakes.dll itself, 34 analysed here. Each looks it up with
`RTSGetLocoObjectByName(lib, loco, 22, L"BRAKES", -1)` (or a slave's,
for the other cars of its unit) and goes to its variable stack.

**`var + 32` is their exchange word.** Inputs (the train's DLL sets them
before Brakes.dll's `Run`; Brakes.dll clears `0x10` .. `0x8000` at the
end of its `Run`, so they last one step):

| Bit | Set by | Meaning |
|---|---|---|
| `0x4` | nearly all, from their own switches (the ВЛ80Т: cab switches 30, 31 = 2 and 83); `updateKLUB` also, from `KLUB_PIT` | PROBABLE: the safety devices powered |
| `0x10`, `0x20` | ЭД2Т, ЭД4М(new), ЭД9М, ЭП2Д, ЭР9П, ЭР9Т | ЭПТ hold, apply: the EP pressure built in `var + 40`, up to 4.3 atm at the rates `var + 236` / `var + 232` (CONFIRMED) |
| `0x40` | the same, ЧС7, 2ЭС6 | UNKNOWN (ЭПТ) |
| `0x100` | 2ТЭ10, 2ТЭ10Л, 2ТЭ10У | its triggers to the locomotive's sounds (§1) |
| `0x200`, `0x400` | ВЛ10, ЭП2К, ВЛ85, 2ЭС6; `0x400` also ЭП1М, 2ЭС4К | UNKNOWN |
| `0x1000`, `0x2000` | ВЛ85 | UNKNOWN |
| `0x4000`, `0x8000` | 2ЭС6 | UNKNOWN |

Outputs (Brakes.dll sets them; the train's DLLs test them, mostly to cut
traction: "bits 1, 2 or 8", the 2ТЭ10's also "bit 4 off"):

| Bit | Set by | Meaning (CONFIRMED where traced) |
|---|---|---|
| `0x1` | `_Run` | the ЭПК has tripped: vigilance unanswered, the pipe vented (`TrainPipeRate` −1.2) |
| `0x2` | `_Run` | the 418 (`BR_418PID` on): the pipe falling faster than 0.0125 atm/s for over 0.5 s without the ЭПТ; cleared once the car's cylinder is over 0.3 atm or the 418 is off |
| `0x8` | САУТ (`0x4033D0`) | САУТ's braking |
| `0x4` | `updateKLUB` | see above |

**Other offsets they use:**
- **ЭП2К:** reads `var + 40` (the EP pressure over 0.3: braking); writes
  `var + 312` = 2.0.
- **ВЛ10:** writes `var + 312` = 1.3. **ВЛ85:** 2.0. **2ЭС6:** 2.5.
  Meaning UNKNOWN.
- **ЭП20, ЭП2Д:** read `var + 116`, `+120`, `+140` (the КЛУБ's next
  target, its distance, the permitted speed) for screens of their own.
  The ЭП2Д also reads `+164`, `+168`, `+224`.
- **ЭП20:** writes `var + 296`.
- **ЭП1М:** reads `+144`, `+256`, `+308`, and `+8` bit `0x400`.
  **ЭП2К, 2ЭС6:** read `+256`, `+308`.
- **ЭП20, ЭП2К, ЭР9П, ВЛ10:** test `var + 4` bit `0x01000000` (Brakes.dll
  runs, §6). **USAVPL:** `var + 4` bit `0x800`.
- **ЭП2Д:** reads `var + 224` (the distance run).
- **ТЭП70:** writes `var + 20` and `+24` (0 or 45.0: the vigilance timer).
- **RTSConnector** (MaDaDev's link to its own programs) reads `+4` ..
  `+244` and writes `+280`. Not needed by the game.

## 6. Its variable stack (`var`), as far as traced

| Offset | Owner | What |
|---|---|---|
| `+4` | `_Run` | state flags: `0x01000000` set at the start of every `Run` (the library runs; four DLLs test it), `0x10` vigilance overdue (the periodic timer past 42 s), `0x400` the ЭПК vented |
| `+8` | the КЛУБ, vigilance | flags: `0x01000000` the ЭПК vented (cleared once it recovers), `0x10000000` the ЭПК whistle, `0x800000` the warning 2 km/h below the permitted speed, `0x8` a vigilance request, `0x400000` overspeed (docs/paths.md) |
| `+12` | `_Init` | 100.0 |
| `+24` | vigilance | its timer: whistle at 5 s, ЭПК at 12 s, 45 = held |
| `+32` | all | the exchange word (§5) |
| `+40` | `_Run` | the EP pressure (to 4.3 atm) |
| `+44` | `_Run` | the periodic vigilance timer (whistle at 35 s; over 42 s `+4` bit `0x10`) |
| `+80` | `_ALSN`, the КЛУБ | the limit, km/h (`abs(limit) × 3.6`) |
| `+84` | `_ALSN` | the signal's distance − 50 m |
| `+88` | `_ALSN`, the КЛУБ | flags (`0x4000`: the signal's limit 0) |
| `+116`, `+120` | `_ALSN`, the КЛУБ | the next target speed (km/h) and its distance |
| `+140` | the КЛУБ | the permitted speed |
| `+144` | `_ALSN` | the signal's distance (whole metres) |
| `+224` | `_Run` | the distance run, m (`abs(v) · dt`) |
| `+160` .. `+184` | `SwitchKLUB` | the КЛУБ's entry: digits, flags, the numbers entered, the mode |
| `+232`, `+236` | `_Init` | the EP release and apply rates |
| `+244` | `_ALSN` | signals counted ahead (0..4) |
| `+336` | `_ALSN` | a limit of its own (km/h) |

The rest (`+16` .. `+76`, `+92` .. `+112`, `+188` .. `+220`, `+228`, `+248` ..
`+348`) is used inside one function each. UNKNOWN until step 2 reads
`_Run` system by system.

## 7. Its sound triggers (CONFIRMED codes; meanings PROBABLE)

| Codes | What | Who posts |
|---|---|---|
| 56 / 57 | the ЭПК whistle on / off | |
| 106 | | |
| 120 .. 137, 3120 .. 3137 | brakes, lamps, ЭПТ (122 / 123 its "on" / "off") | `_Run`; the 31xx forms when `KLUB_BILTYPE`-like parameters ask |
| 3192 .. 3199 | the КЛУБ's beeps | `SwitchKLUB`, `updateKLUB`, `_Run` |
| 3200 .. 3223 | САУТ | |
| 3250, 3251 | `CanSwitch`'s | |
| 171, 901 .. 903, 917 .. 919, 970, 3903, 3917 .. 3919, 3956, 3957 | | `Switched`'s handler and vigilance |

## 8. Open

- **Inside `_Run`:** the brakes system by system (395, 254, 367, 130 /
  224, ЭПТ, gauges): step 2 reads them, with golden runs (step 1).
- **The train DLLs' bits** `0x40` .. `0x8000` and `var + 312`.
- **What else** the train DLLs read of `Engine` that Brakes.dll wrote.

## 9. Golden runs (step 1, 2026-11-18)

**Recording.** `openrts-train-check <root> --dll ... --script <file>
--record <file.tsv>` records every step (20 a second). For every car with
the library (`--record-library`, `BRAKES` by default) it writes a row:
- the car's air;
- its `Engine`'s brake outputs;
- the SMS triggers of the library and the locomotive;
- the cab switches that changed;
- the library's whole variable stack as raw 32-bit words.

The host sends the stack only when asked (`StepRequest::want_aux_vars`,
protocol 13), never in the game.

`tools/reverse/golden_report.py` tells a recording phase by phase: the
air at each move's start and end, the library's sounds, chosen variables
as they change.

The recordings are made from the installation and stay out of the
repository; the scripts are `tests/data/golden/`:
- **`ed9m_brakes.script`:** a six-car ЭД9М, 430 s, its ЭПТ toggle (switch
  35, `ZT_09`, key T) on:
  - the 395 through its positions, an emergency;
  - the start on the move with the КЛУБ's request answered, then
    overspeed unanswered;
  - the ЭПК key off and on.
- **`ep20_brakes.script`:** the ЭП20 under 3 kV DC, 300 s:
  - its 130 valve through its positions (second cab);
  - the 224 through its states;
  - the ЭПК key.

**What they show** (RTS's air; CONFIRMED as measured):

| | ЭД9М | ЭП20 |
|---|---|---|
| charging pressure (УР in II) | 4.499 (`BR_ZARPRESS` 4.6) | 4.999 (5.1): PROBABLE `BR_ZARPRESS` − 0.1 |
| V: the УР | −0.22 atm/s | −0.22 atm/s |
| V: `TrainPipeRate` | −0.25 | −0.21 |
| III, IV | the pipe settles at the УР and holds | the same |
| II after a step | the УР back at 4.49 in 7.0 s | 7.3 s |
| VЭ | the ЭПТ (below), the УР −0.03 atm/s as Va | without the ЭПТ: the УР −0.03 atm/s; `EPTValue` stays −1 |
| VI | `TrainPipeRate` −1.2 | −1.2 |
| I | the УР +0.18 atm/s, up to 5.39 | |
| 224, states 0..4 | | `IndependentBrakeValue` 4, 3, 2, 1, 0 atm; state 5 springs back to 4 |

**The ЭПТ** (ЭД9М, its toggle on; CONFIRMED as measured):
- `BrakeSystemsEngaged` is 3 (the pipe and the ЭПТ).
- In V, VЭ and VI each car's EP pressure (`var + 40`) rises; in III and
  IV it holds; II and I let it out.
- **Rates.** It rises at about 1.0 atm/s and falls at 0.65 atm/s (2.85 to
  0 in 4.4 s).
- **Each car its own pace.** After the same 2.75 s in V the six cars
  stood at 2.73 .. 3.06 atm (0.99 .. 1.11 atm/s). PROBABLE: its rates
  `var + 236` / `+232`, set in `_Init`, are drawn per car.
- **Where it goes.** `BR_MODE` is 1 on the ЭД9М, so the pressure goes to
  `IndependentBrakeValue`, and RTS moves the car's independent cylinder
  after it.
- Without the toggle, VЭ is only Va and `EPTValue` stays −1.

**The КЛУБ's vigilance** (ЭД9М, the test track: no signals):
- **Starting.** 0.7 s after the train moves it asks (`var + 8` bit 8)
  and whistles (56). The РБ (switch 23) answers: whistle off (57), its
  sounds 3199 and 901 / 902.
- **Before overspeed.** At its permitted speed − 2 km/h it sets
  `0x800000` (the warning, 3199).
- **Over it.** It blinks (`0x400000`, every 0.55 s) and asks.
  Unanswered, 7.05 s after the request the ЭПК trips:
  - `var + 4` `0x400`, `var + 8` `0x01000000`, `var + 32` bit 1;
  - `TrainPipeRate` −1.2, sound 130;
  - the pipe empties and the train stops.
- **Recovery.** The ЭПК key off clears it at once and the pipe charges.
  With the key on, the КЛУБ asks again about 3 s later (the handle in
  I), and that request must be answered too.

**Sounds seen:**
- 917 / 918 / 919: the 395's, 130's and 224's handles moving;
- 120 .. 137 (31xx on the ЭП20): air and lamps;
- 3196, 3198: the КЛУБ powered;
- 903: the ЭПК key.

**Open from these runs:**
- **The ЭД9М's controller below its zero** (states 0..5) commands nothing
  in `var + 32`, and after it the train would not take traction: its
  meaning is ed9m.dll's, not traced.
- **Under 25 kV AC** the ЭП20 started "on" (its circuit DC) drops its
  main switch within 2 s, and its compressors stop (the main reservoir
  fell from 7.0 to 4.3 atm in the first recording). The script gives it
  3 kV DC: the main reservoir then cycles at 7.8 .. 8.8 atm.

## 10. Inside `_Run`: the brakes (step 2, 2026-11-18)

Read from the decompiled `_Run` (CONFIRMED where not marked) and checked
against the golden runs (§9).

**Its parts**, by the labels it traces:

| Label | Lines | What |
|---|---|---|
| (start) | | the 418, the vigilance timers |
| `EPK KLUB` | | the ЭПК and the КЛУБ |
| `BRAKES` | 4 500 | the valves, cab by cab |
| `SOUNDS` | | the air sounds |
| `Custom VR` | | a custom 292 |
| `EPT` | | the ЭПТ |
| `End Tick` | | the step's end |

**What it works from:**
- **MR:** the locomotive's main reservoir (Locomotive `+404`), or its
  first slave's.
- **k:** `RTSGetInteger(2)`, a percent, the gauges' lag.
- **The working cab:** the one whose 367 stands in `BR_367OPPOL`. The
  other cabs' handles do nothing.
- **The numbering of the handles** (checked on both `.eng` and the
  golden runs):
  - `BR_FLIPCABS` 1 (the ЭД9М): state 0 = I .. 6 = VI;
  - `BR_FLIPCABS` 0 (the default, the ЭП20's 130): 0 = VI .. 6 = I.

  The same logic serves both, mirrored. `BR_130MODE` and `BR_224MODE`
  pick only the sounds (the 31xx codes).

### The driver's valve (395, 130)

| Position | УР | TrainPipeRate | ЭПТ |
|---|---|---|---|
| I | + min(2 (MR − УР), 0.18) atm/s, at most MR | += 1.5 (УР − pipe) a step while the pipe is below | |
| II | to T at min(2 (T − УР), 0.1) atm/s, then −0.002 atm/s (the stabiliser) | += (min(max(УР, T), MR) − pipe) / 2 a step while below, within ±1.5; then −0.0024 | |
| III | −0.1 atm/s while above the pipe | += УР − pipe while the pipe is above; −0.002 | hold |
| IV | at most MR | as III, and fed while below: += (min(УР, MR) − pipe) / 2, within ±1.5 | hold |
| VЭ | −0.03 atm/s | += УР − pipe (+0.05 when the УР is 0.1 over the pipe), at least −0.4 | hold, apply |
| V | −0.22 atm/s | as VЭ | hold, apply |
| VI | −0.4 atm/s | = −1.2 | none (the code sets hold and apply, but the ЭД9М's golden run builds no EP pressure in VI) |

- **T** is the charging pressure: `BR_ZARPRESS` − 0.1 (5.0 when it is not
  given), or with a reducer switch (`BR_REDID`) 3.9 + 0.1 × its state.
- **VЭ and V lower the УР only while the handle rests there** (its target
  state, `+48`), not while passing through, and only with `var + 340` ≤
  1200 (UNKNOWN: a timer?).
- **The rate builds up.** TrainPipeRate is added to step by step, and
  RTS integrates it.
- **Its share:** rate / 6 (`BR_MODE` 1) or / 10, kept in the section's
  `var + 76`. The main reservoir pays it: `MainResRate` −= it.
- **In braking positions** `var + 4` gets `0x80 | 0x100`.
- **The ЭПТ's voltmeter and ammeter** (`BR_VOLT`, `BR_AMP`): 50 V in I
  and II; 40 V in III and below; ±4 A (`BR_AMPDBL`) in lap, 5 A in
  braking.
- **The КПТ** (`BR_KPTID`, an EMU's brake controller) combines with the
  395: at II the handle takes the КПТ's state, at III or IV the КПТ's
  when it is further. The КПТ alone gives the ЭПТ hold (its 2..3) or hold
  and apply (4..6).
- **`var + 32` bit `0x8000`:** the handle is taken as IV. **Bit `0x4000`**
  (the 2ЭС6): the УР is kept where the step began.

### The 334 (BR_334ID: the ЭР2's and ЭР9's valve, states 0..5)

| Position | What |
|---|---|
| 0 | emergency |
| 1 | braking: with the ЭПТ off, the УР −0.22 atm/s; with it on, the ЭПТ alone (hold, apply), the pipe untouched |
| 2 | lap, ЭПТ hold |
| 3, 4 | train and charge: the УР to T at 0.5 atm/s up or down, the pipe fed (±1.5); 3 also holds the ЭПТ |

Without a 334 in the cab, a КПТ commands the ЭПТ alone (with the ЭПТ on,
the ППТ `BR_PPTID` off and air in the main reservoir).

### The independent brake (254, 224, 4ВК)

| Valve, numbering | States → IndependentBrakeValue |
|---|---|
| 254, `BR_FLIPCABS` 1 | 0 → 0; 1 → −0.1 a step; 2..5 → 1, 2, 3, 4 atm (at most MR) |
| 224, `BR_FLIPCABS` 0 (the ЭП20) | 0..3 → 4, 3, 2, 1 atm; 4 → −0.1 a step; 5 → 0, springing back to 4 |
| 4ВК (`BR_4VKID`) | 2: +0.3 atm/s to min(MR, 4); 0: −0.25 atm/s; 1: holds |

- The higher of the 254 and its second (`BR_254DID`) counts, and the
  slaves' own 254s too. A handle whose cock (`BR_254RID`, `BR_254DRID`)
  is shut does not count.
- The value goes to the slaves' engines.
- The 4ВК's sounds are 120 / 121 (applying) and 122 / 123 (its cylinder
  over the car's).
- **Spring positions:** the 254's release and the КПТ's last spring back
  when neither the handle's key nor the mouse button is down, read with
  `GetAsyncKeyState`.

### Cocks, switches, emergencies

| Device | What it does |
|---|---|
| separating cock (`BR_RAZOBSHK` 1, `BR_RAZID` shut) | TrainPipeRate 0; the valve feeds nothing |
| КК (`BR_KKID`) | 0: −0.002 (the valve cut off, the leak only); 2: emergency |
| ОТ (`BR_OTID`) | EngineFlags 1: the engine's own train brake released, own and the slaves' |
| ЭТ (`BR_ETID`), stop valves (`BR_SK1..BR_SKNUM`), the ЭПК tripped (the vigilance timer ≥ 12) | TrainPipeRate −1.2, EPTValue −1 |
| `BR_BUFERKVT` with the 254 at release | EngineFlags 1 |
| ЭПТ buttons (`BR_EPTAPL`, `BR_EPTREL`) | apply; release unless the handle holds |

### The ЭПТ

- **Engaged** by its toggle (`BrakeSystemsEngaged` bit 2), or a DLL's
  EPTValue above −1 (then `var + 4` `0x80000`, dropped once the cylinder
  is under 0.1).
- **Commands:**
  - the handle (III, IV hold; VЭ, V hold and apply; VI in the code too, but no EP pressure in the golden run);
  - the КПТ, the 334, the buttons;
  - the train DLLs' bits (§5).

  With `BR_MODE` 1 they go to every slave's `BRAKES` too.
- **Pressure:** §9; into IndependentBrakeValue (`BR_MODE` 1) or EPTValue.
  With `BR_MODE` 1 and an independent brake applied, the ЭПТ's bits are
  dropped.
- **Lamps:** О whenever it is on; П in III and IV (`BR_MODE` 1: III ..
  VI); Т in VЭ, V and VI.

### The engine's own 292 (BR_292CUSTOM)

Instead of RTS's triple valve (AuxilaryRate 0, EngineFlags 1):
- **its ЗР** (`var + 324`) charges from the pipe at 0.25 /s, the pipe
  paying;
- **a smoothed pipe** (`var + 328`, 0.02 /s);
- **its cylinder** (`var + 320`) = 3 × (smoothed − pipe), up 1 /s,
  down 0.2 /s, at most the ЗР, into IndependentBrakeValue.

### Gauges and lamps

| Gauge | Shows |
|---|---|
| ТЦ (`BR_MANC`) | the higher of Wagon `+52` and `+96`; the needle 0.2 atm/s within 0.5 of it, 1 atm/s beyond |
| ТМ (`BR_MANP`) | its own `var + 28`: up with the pipe at once, down (0.5 + 0.8 k/100) atm/s (II: 0.2 + 1.2 k/100; III .. VI: 1 atm/s) |
| ГР (`BR_MANM`) | Locomotive `+404` |
| УР (`BR_MANU`) | Engine UR, the working cab only |
| lamp ТМ (`BR_LAMPTM`) | the 418 (`var + 32` bit 2) |

### Its air sounds (on / off pairs; 31xx with `BR_130MODE`)

| Codes | When |
|---|---|
| 124 / 125 | I, the valve feeding |
| 126 / 127 | II, feeding |
| 128 / 129 | V, the pipe venting |
| 130 / 131 | VI |
| 132 / 133 | II, the pipe up at its charge |
| 136 / 137 | the pipe gauge over the pipe |
| 134 / 135 | the ЭПТ |

Matches the golden runs' sequences.
