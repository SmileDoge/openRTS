# RTS signalling: sigcfg.dat, sigscr.dat and the signal VM

Status: static analysis of `rts1.exe` (Hex-Rays) plus a survey of the
`sigscr.dat`/`sigcfg.dat` files of 7 installed routes. Not implemented in
openRTS yet.

| Address | Role |
|---|---|
| `0x483694` | route loader: tsection, TDB, `sigcfg.dat` (signal types, shapes, `ScriptFiles`) |
| `0x482680` | TDB `TrItemTable` (`SignalItem`, `TrSignalDirs`, speed posts, ...) |
| `0x496DB0` | loads one script file, splits it on `SCRIPT`, compiles each script |
| `0x497ED8` | statement compiler (source → text assembly) |
| `0x45911C` / `0x497334` / `0x497E14` | expression tokenizer / code generator |
| `0x498964`, `0x499664` | assembler (text assembly → dword bytecode), operand decoder |
| `0x499948` | bytecode interpreter: evaluates one signal head |
| `0x49ACB0` | operand → storage (local / head field / immediate / register) |
| `0x48106C` | `block_state()` |
| `0x480CCC` | `route_set()` |
| `0x479AFC` | per-frame head update: free-running script run, lights, flashing |
| `0x508520` | per-train evaluation of the signals ahead/behind the train |
| `0x5C09BC`, `0x5BD8FC` | activity outcomes `SetSignalMode`, `SignalOverride` |
| `0x41B4F4` | "passed red/stop signal" activity failure |

## Data model

### CONFIRMED

- `sigcfg.dat` (UTF-16, `SIMISA@@@@@@@@@@JINX0G0t______`) keywords handled:
  `LightTextures/LightTex`, `LightsTab/colour`, `SignalTypes/SignalType`,
  `SignalFnType`, `SignalLightTex`, `SignalFlags` (`SEMAPHORE`), `SemaphoreInfo`,
  `SignalLights/SignalLight/Position/Radius`, `SignalDrawStates/
  SignalDrawState/DrawLights/DrawLight` (+ `SignalFlags(FLASHING)`),
  `SemaphorePos`, `SignalAspects/SignalAspect` (+ `SpeedMPH`/`SpeedKPH`),
  `SigFlashDuration(on off)` (seconds), `SignalShapes/SignalShape/
  SignalSubObjs/SignalSubObj` (`SIGNAL_HEAD`, `DECOR`, `NUMBER_PLATE`,
  `USERn`, `SignalFlags`), `ScriptFiles/ScriptFile` (path relative to the
  route). `RTS_ShuntingAutoMode` is also read there.
- Aspect names (`0x45B290`): STOP 0, STOP_AND_PROCEED 1, RESTRICTING 2,
  APPROACH_1 3, APPROACH_2 4, APPROACH_3 5, CLEAR_1 6, CLEAR_2 7,
  BLOCK_OBSTRUCTED 8, FAILED 9. Larger number = less restrictive.
- `SignalFnType`: NORMAL 1, SHUNTING 2, DISTANCE 3, REPEATER 4, INFO 5
  (numbering differs from MSTS/Open Rails documentation; use these values).
- A world `Signal` object holds several *heads*; each head links to a TDB
  `SignalItem` and has the fields the script sees:

  | Head offset | Script name |
  |---|---|
  | +84 | `state` (aspect) |
  | +88 | `draw_state` |
  | +96 | `enabled` |
  | +100 | aspect speed limit (m/s, −1 = none) |
  | +104 | `signal_mode` (RTS) |

- Flashing: a draw-state light with `FLASHING` is lit while the head's timer
  is below `on`; the timer wraps at `on + off` (`0x479AFC`).

## The `sigscr.dat` language

It is the MSTS signal-script language: C-like syntax, every value is a
`float`, one `SCRIPT <SignalTypeName>` section per signal type (matched
case-insensitively; a script for an unknown type is reported, a second script
for the same type is an error). The file is read as ANSI (code page of the
system).

RTS does not interpret the source. It compiles each script into text
assembly for a small register machine and then into dword bytecode.
`SignalScriptsGenerateAsm` in the RTS config writes the text assembly to
`<file>.asm`, which is the easiest way to see what RTS made of a script.

### Lexing (CONFIRMED)

- `//` to end of line and `/* */` are blanked. `/*RTS# ... */` is
  **RTS-only code**: the markers are removed and the body is compiled, while
  MSTS/Open Rails see a comment. No installed route uses it.
- Named constants are replaced textually before compiling, padded with
  spaces so offsets stay unchanged:

  | Name | Value |
  |---|---|
  | `SIGASP_STOP` … `SIGASP_CLEAR_2` | 0 … 7 (as above) |
  | `SIGFN_NORMAL/SHUNTING/DISTANCE/REPEATER/INFO` | 1/2/3/4/5 |
  | `BLOCK_CLEAR/OCCUPIED/JN_OBSTRUCTED` | 0/1/2 |
  | `SIGFEAT_USERn` | n |
  | `SIGFEAT_NUMBER_PLATE`, `SIGFEAT_GRADIENT_PLATE` | 32, 33 |
  | `SIGMODE_NORMAL/SHUNT/FAILED/EXCL` (RTS) | 0/1/2/3 |

### Statements (CONFIRMED)

- `extern float name;` / `extern float name ();` declare built-ins;
  `float name;` declares a local. Locals start at 0 each run.
- `x = expr;` assignment; a bare `call (...);` expression statement.
- `if (cond) stmt [else stmt]`, with `{ }` blocks; `else if` chains work.
- `return;` (`RET`).
- No loops, no user functions, no `switch`.

### Expressions

Operators recognised (`0x45911C`). The precedence column is RTS's own
binding-strength number:

| Operator | Meaning | Precedence | Opcode |
|---|---|---|---|
| `==# !=# ># >=# <# <=#` | compare as integers (both sides truncated toward 0) | 2 | `CMPE`… |
| `== != > >= < <=` | compare as floats | 2 | `CMPEF`… |
| `&&`, `\|\|` | logical and/or (any non-zero = true, result 0/1) | 1 | `AND`, `OR` |
| `=` | assignment | 0 | `MOV` |
| `!x` | logical not | 3 | `NOT` |
| `-x` | negate | 3 | `NEG` |
| `#x` | round to nearest integer (half up) | 3 | `ROUND` |

- CONFIRMED: there are **no arithmetic binary operators** (`+ * /` are not
  tokens and `-` is always unary).
- CONFIRMED: `&&` and `||` have the **same** precedence.
- PROBABLE: operators of equal precedence are applied left to right, so
  `a || b && c` means `(a || b) && c`, not C's `a || (b && c)`. Scripts in
  the installed routes always parenthesise.
- CONFIRMED: `^` (XOR) has an opcode but the tokenizer never accepts `^`, so
  it is unusable.
- CONFIRMED: there are no short-circuit evaluations. Every call in a
  condition runs.

### Virtual machine (CONFIRMED)

- 8 float registers `AX`…`HX`. Operands are locals, head fields (`state`,
  `draw_state`, `enabled`, `signal_mode`), registers or immediates.
- 30 opcodes. Control flow is `JZ` (jump if `AX` is zero), `JMP` and `RET`.
  Built-in calls use `PUSH` (argument) and `CALL`.

### Built-ins

| Name | Result |
|---|---|
| `block_state()` | walks the track from the head to the next signal of its own type or a NORMAL signal: 0 clear, 1 occupied by a train, 2 a junction is set against the route |
| `route_set()` | 1 if the junctions linked to the head (`TrSignalDirs` / `JN_LINK`) are set for it |
| `next_sig_lr(fn)` | least restrictive aspect of the heads of type `fn` on the next signal object of that type along the train's path; 0 (STOP) if none |
| `next_sig_mr(fn)` | same, most restrictive |
| `def_draw_state(asp)` | draw state from `SignalAspect`; also sets the head's speed limit from that aspect |
| `sig_feature(f)` | 1 if the shape has sub-object `USERn` / number plate (32) / gradient plate (33) |
| `dist_multi_sig_mr(fn, ...)` | most restrictive aspect over the listed types on the first object ahead that has one; 0 if none |
| `this_sig_mr(fn)` | most restrictive head of type `fn` placed *before* this head on the same object; 0 if none, −1 outside a train chain |
| `this_sig_lr(fn)` | least restrictive other head of type `fn` on the same object, −1 if none |
| `state`, `draw_state` | the head's outputs |
| `enabled` | non-zero when a train's path is reserved through the signal |
| `signal_mode` | (RTS) 0 normal, 1 shunting, 2 failed, 3 excluded |

Only `block_state`, `route_set`, `next_sig_lr/mr`, `def_draw_state`,
`sig_feature`, `enabled`, `state` and `draw_state` occur in installed routes.
The installed routes have 168–308 scripts each, about 2100 in total.

## When scripts run

### CONFIRMED

- **Per train** (`0x508520`): RTS collects up to 16 signal objects along the
  train's path. It continues until it has seen the first NORMAL signal's
  `SignalNumClearAhead` + 2 NORMAL signals. The objects are evaluated from
  the farthest to the nearest, so `next_sig_*` reads aspects updated in the
  same pass. `enabled` is set while the script runs and cleared after. A
  signal with no train approaching runs its script with no chain, so
  `next_sig_*` returns STOP there.
- **Aspect obeyed by the train**: the least restrictive aspect over the
  object's NORMAL heads. Train flags `obey_distance`, `obey_shunting` and
  `obey_info` add the heads of that type. The speed limit is the lowest
  aspect speed among them.
- **Modes** (RTS):
  - FAILED/EXCL heads skip the script; the aspect and draw state come from
    `SignalOverride`.
  - In SHUNT mode, aspects 3..7 are forced to RESTRICTING unless
    `RTS_ShuntingAutoMode` is set.
  - A mode the script assigns spreads along the chain ahead.
- **Activity control**: `SignalOverride(Position, SignalMode, SetAspect(head
  asp), SetDrawState(head ds), SetSpeedLimit, Cancel)` overrides the script
  result; `SetSignalMode` changes the mode for a train.

### PROBABLE

- In shunting mode `block_state()` ignores some trains when checking
  occupancy (flag inside the train record).
- `route_set()` returns non-zero once the walk reaches the next signal with
  all linked junctions aligned.
