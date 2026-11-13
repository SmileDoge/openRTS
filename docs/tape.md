# The tape

RTS's "tape roll": while it rolls, the game writes a text line about one
locomotive every game second. RTS's `lenta.xls` (an Excel book with the
macro `ImportWRLenta`, shipped in `RTS\` of RTS 0.44.1) reads such a file
into a table: the distance, the speed, the signal ahead, the pressures, the
limit, and more. openRTS writes the same file in the same format, and draws it in a window
of its own as a speed recorder's paper (the tape window, below).

Source: `rts1.exe` (functions below), and the macro in `lenta.xls`.
openRTS: `simulation/tape.hpp` (the line, the file, reading it back),
`TrainView::tape_*` (the keys, what goes into a line), the DLL host
(`RTSGetInteger` / `RTSSetIntegerG` 7), `viewer/src/tape_view.cpp` (the
window).

## Keys

| RTS keys | openRTS keys | What |
|---|---|---|
| Ctrl+Num5 | Ctrl+`` ` `` | start the tape on the locomotive driven; stop it (on whichever locomotive) |
| Num5 | — | while it rolls: which locomotive it records |
| — | Ctrl+F9 | the tape window: the tapes drawn as a speed recorder's paper (openRTS's own, below) |

RTS (CONFIRMED, `0x402C88` key 0x65, `0x414B40`, `0x414C10`):
- Ctrl+Num5 stops a tape that rolls. Otherwise it starts one on the current
  locomotive, if that is a locomotive with an `Engine` (a DLL engine): the
  file opens, `LocoFlags |= 0x10000`, the message "Tape rolling". A file
  that cannot be opened starts nothing.
- Num5 alone, while the tape rolls: "Recording loco <LocomotiveSeries>[<number>]".
- The F5 car list marks the recorded car with ` [TAPE]` (`0x408574`).
- The tape stops when the game ends (`0x40521C`) and when the locomotive is
  taken out of the game (`0x503A90`).

openRTS:
- Any locomotive he drives can be recorded (with a DLL, a native plugin, or
  the keyboard driver). What only a DLL keeps (throttle, sand, ЭПТ, the
  equalising reservoir, the consumption) comes from the DLL's `Engine`; for
  the keyboard driver from its handles; otherwise 0.
- The tape follows its car through couplings and stays on it when he drives
  another one, as in RTS.
- The F5 list marks the car with ` [ЛЕНТА]` (` [TAPE]` in English).

## The file

RTS (CONFIRMED, `0x504FA8`): `<RTS>\RTS_TAPES\YYYY.MM.DD_hh_mm_ss_<series>_trainlog_rts.txt`,
the computer's date and time at the start, `<series>` the engine's
`LocomotiveSeries` (ENG `+386`). The folder is made when missing; the file is
created anew (`CREATE_ALWAYS`) and shared for reading.

openRTS: the same name in `RTS_TAPES` of its own data folder
(`%LOCALAPPDATA%\openRTS\RTS_TAPES`), not in the RTS install. A series with
characters Windows refuses in a name gets `_` for them; an engine without a
series gives its file name.

## When a line is written

RTS (CONFIRMED, `0x424488`): the frame time is added up; at 1 s or more a
line is written and the count goes back to 0.

openRTS: each game second, in the fixed simulation step; the second is taken
off the count instead of setting it to 0. RTS's lines come a frame late each
(a second and a frame apart), and `lenta.xls` takes each line for one
second when it adds the speeds up into the distance.

## The line

ASCII, fields separated by `#`, ending in CR LF (`0x5050B0`). Pressures are
in psi: RTS's atm × 14.69. 47 fields:

| # | Format | What (RTS) | `lenta.xls` column |
|---|---|---|---|
| 0 | `%0.3f` | `Wagon::Velocity`, m/s | Скорость (× 3.6); the sum of the lines: Ордината |
| 1–3 | `%i` | the game clock h, m, s (0 ≤ t ≤ 86400; negative wraps) | Время |
| 4 | `%i` | the train's signal aspect ahead (train `+0xA0`) | Сигнал |
| 5 | `0` | | |
| 6 | `%0.2f` | `TrainPipePressure` (`+0x64`) | ТЦ |
| 7 | `%0.2f` | `IndependentBrakePressure` (`+0x34`) | ТМ |
| 8 | ` %0.2f` | `BrakeCylinderPressure` (`+0x60`) | ЛТ |
| 9 | `  %0.3f` | the distance to that signal, m (train `+0x7C`) | До светофора |
| 10 | `%i` | the aspect again | |
| 11 | `%0.2f` | x in the tile (−`TrPos[0].X`, MSTS's axes) | |
| 12 | `  %0.2f` | y | Y |
| 13 | `%0.2f` | z | |
| 14, 15 | `%i` | tile x, z | |
| 16–18 | `0#0#8` | | |
| 19 | `%i` | `Engine::ThrottlePosition` | |
| 20 | `1.00` | | |
| 21, 22 | `%i` | pantograph 1, 2 up (`PantoRaised` bits 0, 1) | |
| 23–25 | `5#2#0.00` | | |
| 26 | `%0.3f` | the speed limit in force, m/s (`0x4FB630`) | |
| 27 | `0` | | |
| 28 | `   %0.2f` | `Engine::Sanding` | Песок |
| 29 | `  %0.2f` | electric: `PowerConsuming` / 1000; diesel: `FuelConsuming` | Расход энергии/топлива |
| 30–32 | `%i` | the computer's clock h, m, s | Время(реал) |
| 33 | ` %E` | the train's mass, kg (every car's `Wagon::Mass`) | Масса поезда |
| 34 | `%i` | cars | Вагонов |
| 35 | `%i` | locomotives not cold (`IsLocomotive` and not `LocoFlags & 8`) | Локомотивов |
| 36 | `1` | | |
| 37 | `  %E` | the train's length, m | Длина поезда |
| 38 | `%i` | 3 electric, 2 diesel, 0 other | |
| 39 | `%i` | the season (0 spring … 3 winter) | |
| 40 | `%i` | the weather (0 clear, 1 snow, 2 rain) | |
| 41 | `%i` | ЭПТ engaged (`BrakeSystemsEngaged & 2`) | ЭПТ |
| 42, 43 | ` 0.00#-4663` | | |
| 44 | `%0.3f` | the speed limit again | Ограничение (× 3.6) |
| 45 | `%0.3f` | `Engine::UR` (the equalising reservoir) | УР (when present) |
| 46 | `%0.3f` | the kilometre by the posts (train `+0x70`), −1 none | |

Notes (RTS, CONFIRMED unless marked):
- **The aspect** (4, 10) is the signal pass's (`0x508520`): the aspect of
  the nearest obeyed head ahead, 0 when there is none. The distance (9) is
  not cleared then and keeps the last one. RTS writes 8 instead of the
  aspect when the signal lies beyond what `0x47FE14` finds from the train's
  end (UNKNOWN what: PROBABLE the end of its path); openRTS does not.
- **The limit** (26, 44): the route's `SpeedLimit` (100 m/s without one),
  lowered by the train's track limit (`+0x68`), signal limit (`+0x6C`) and
  `+0x90`, each only when set.
- **The kilometre** (46): from the nearest kilometre post behind within
  5 km, counting up or down as the next one ahead tells (`0x508520`, the
  bit `0x800000` of the train's flags).
- **The labels** of `lenta.xls`'s columns 6–8 read ТЦ, ТМ, ЛТ, while its
  macro names them `TM`, `LM`, `TZ`: field 6 is the brake pipe. Its signal
  names (1 КЖ, 2 Б, 3 Ж, 4 Ж-Ж, 5 Ж, 6 З, 7 З, 8 К, otherwise -) are the
  macro's reading of RTS's `SignalAspect` (0 STOP … 7 CLEAR_2).
- In RTS 0.44.1 the line ended at field 44; the macro reads 45 only when it
  is there.
- The constant fields are RTS's, written as they are.

## The DLLs

RTS (CONFIRMED):
- `RTSGetInteger(loco, 7)` (`0x43929C`): 0 no tape, 1 rolling on another
  locomotive, 3 rolling on this one.
- `RTSSetIntegerG(loco, 7, value)` (`0x43939C`):
  - `value` 0: stops the tape when it rolls on this locomotive (1), else 0;
  - otherwise, with the tape on this locomotive: 2 without bit 4, with it a
    new file; on another one: 0 without bit 2, with it that one stops;
    then the file opens (0 when it cannot) and the tape rolls on this
    locomotive (1).

openRTS: the DLL host answers the same from the car the game names, and the
game opens the file after the step; the host answers 1 without knowing
whether the file opens. `LocoFlags` 0x10000 marks the car the tape rolls on.

## The tape window (openRTS)

RTS has none: its tapes are read in `lenta.xls`. openRTS draws a tape as a
speed recorder's paper, in the manner of the 3СЛ-2М (its look, not its
scales): Ctrl+F9, or "Speed recorder tape" in the game panel.

- **The tapes:** the files of `RTS_TAPES` in openRTS's folder and in the RTS
  installation's (marked "RTS"), the newest first. The one rolling is read
  as it grows and followed; a new tape is followed when the window showed
  the one rolling.
- **By the path or by the time.** By the path as the recorder's paper runs:
  each line a second at its speed (by its size, backing too), so a standing
  takes no paper. By the time: a line a second.
- **The lanes,** top to bottom:
  - the speed, 0–150 km/h (0–220 when the tape went above 145); the limit in
    force, in steps (not drawn above the scale, as RTS's 100 m/s without a
    limit); in red where the speed was more than 0.5 km/h above the limit;
    the stops of 10 s and more, with how long;
  - the time pen: across its lane in ten minutes of the game's clock, then
    back, the clock at each tooth; by the path a standing draws it up and
    down in place;
  - the cab signal: a ribbon in the lamps' colours, with `lenta.xls`'s names
    (З, Ж, Ж-Ж, КЖ, Б, К); `К/—` is 0, STOP or no signal (the line does not
    tell them apart);
  - the pressures, 0–7 kgf/cm²: the brake pipe (field 6), the brake
    cylinder (8), the independent brake's cylinder (7, where it was used:
    MaDaDev's multiple units brake their ЭПТ through it, Brakes.dll filling
    it in every car), the equalising reservoir (45, when the tape has it);
  - under the paper: the path from the tape's start, or the game clock; the
    kilometre posts passed (field 46).
- **The values** of the line under the cursor (else the last), below the
  paper: the game's and the computer's clock, the path, the kilometre, the
  speed, the limit, the signal and the distance to it, the throttle, sand,
  the pantographs, the pressures, ЭПТ, the consumption, the train.
- **The summary** above it: the path, the time, the top speed, the stops,
  how many times above the limit and the most above it. The 0.5 km/h and the
  10 s are openRTS's own, not a decoding standard's.
- The wheel scales, a drag scrolls (and stops following the end), a double
  click shows the whole tape.

Reading (`parse_tape_line`, as `ReadLenta`): fields 0..44 must be there and
be numbers (RTS 0.44.1's lines end at 44), 45 and 46 are read when there; a
line that is not a tape's is counted and left out. `TapeReader` reads the
whole lines added since the last time; the one being written waits.

## The brake log (openRTS)

Next to each tape openRTS writes a file of its own: the tape's name with
`_openrts.csv` for `_rts.txt`
(`2026.11.13_10_53_24_ed9m_trainlog_openrts.csv`), started and stopped with
the tape (`TrainView::log_brakes`). It holds what the tape has not, so a
run can be read back without being told when which handle stood where:
twice a second, and a row more at every change of a handle, the change
named in `событие` ("кран V", "ЭПТ вкл", "вспом 1.5", "экстренное",
"контроллер 3", "реверс 1"). UTF-8 with a BOM, `;` between the fields, a
decimal point; a header again when the cars change (coupled, cut).

| Column | What |
|---|---|
| `время`, `с_начала` | the game clock; s since the tape started |
| `скорость_кмч`, `км`, `сигнал`, `до_сигнала_м` | the speed, the kilometre by the posts, the signal obeyed ahead and the distance to it |
| `воздух`, `поезда_с_DLL` | RTS's air or the realistic one; with DLLs: RTS's, the DLL's valve or openRTS's valve (docs/train.md) |
| `кран`, `кран_состояние` | the 395's position (I, II, III, IV, VЭ, V, VI): the keyboard's, or the cab's control under Brakes.dll (`BR_395ID1`) as openRTS's valve reads it (the cab's StateHints: the ВЛ80С numbers them VI..I); its raw state |
| `редуктор`, `КК`, `блокировка_367` | under Brakes.dll: the reducer's charging pressure (`BR_REDID1`), the combined valve усл. 114 (`BR_KKID1`: нормальное, двойная тяга, экстренное), the 367 on or off (`BR_367ID1`, `BR_367OPPOL`) |
| `АЛСН_вкл`, `огонь_АЛСН` | the DLL's Engine::ALSNOn (0: the cab's signal lights out) and the aspect its cab signal shows (Cabin::Signal.Aspect[0], an index into the cab's SignalAspects; 8 none) |
| `вспом_цель` | the independent brake's target the controller asks, atm |
| `ЭПТ`, `цель_ЭПТ` | the ЭПТ engaged; its demand |
| `УР`, `экстренное`, `вагонов_в_экстренном` | the УР of the valve working the pipe; the emergency asked (the valve's VI, or the DLL's direct vent); cars in emergency (realistic air) |
| `ГР`, `подача_ГР` | the engine's main reservoir; the compressor's rate, atm/s |
| `тяга_кН`, `тормоз_кН`, `контроллер`, `реверс` | the train's traction and brake force; the throttle and reverser |
| `DLL_УР`, `DLL_темп_ТМ`, `DLL_ЭПТ`, `DLL_вспом`, `DLL_системы` | what the DLL gives: Engine::UR, TrainPipeRate, EPTvalue, IndependentBrakeValue, BrakeSystemsEngaged |
| `рельсы`, `тележек_в_юзе`, `колодки_влажность_ср`, `колодки_нагрев_макс`, `тележек_горит` | the rails' state (the Train window's "Rails"), the bogies sliding (юз), the shoes' mean wetness (0 .. 1) and most warmth (K above the air), the bogies whose wooden shoes are alight: docs/train.md |
| `ТЦ_мин`, `ТЦ_макс` | the least and the most cylinder in the train (each car's higher of the train and independent one) |
| `вN_ТМ`, `вN_ЗР`, `вN_РК`, `вN_ТЦ`, `вN_вспом` | car N's pipe, auxiliary reservoir, a 483's РК (realistic air), cylinder and independent cylinder: every car up to 16, else the first and the last 8 |

## Testing

- `openrts_tape_test`: the line field by field against RTS's format, the
  name, a line a game second, the file shared for reading; a line read back
  (and written again the same), RTS 0.44.1's shorter line, lines that are
  not a tape's, a file read as it grows, the path and the summary.
- `openrts_dllhost_test`: `RTSGetInteger` / `RTSSetIntegerG` 7 with the
  fake locomotive DLL.
- `--capture-tape` (the viewer's capture): the tape starts at 0.2 of
  `--capture-at` and its window opens; it rolls until the capture ends,
  when the window's tape is printed (`tape window: <file>: <lines> ...`).
