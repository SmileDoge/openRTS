# RTS signalling: sigcfg.dat, sigscr.dat and the signal VM

Status: static analysis of `rts1.exe` (Hex-Rays), a survey of the
`sigscr.dat`/`sigcfg.dat` files of 7 installed routes and RTS's compiler
output (`sigscr.dat.asm` of TestRoute_RTS). The details openRTS follows are
in "Compiler, VM and evaluation in detail" at the end.

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
`SignalScriptsGenerateAsm = 1` in `[TSconfig]` of the RTS config writes the
text assembly to `<script file>.asm`. That is the script path with `.asm`
appended, e.g. `ROUTES/<route>/sigscr.dat.asm`, UTF-16 LE without a BOM,
overwritten when the route loads (`0x496DB0`) — CONFIRMED. It is the easiest
way to see what RTS made of a script.

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
- **Observed in RTS (2026-11-01, Kuban_RTS, ЗИП, M10 = UiD 12924, INFO type
  `T_Head_WB-new`), compared with openRTS by the user:**
  - its script (the first of two for the type; RTS: "Multiple signal
    scripts for %s, only first one is used") shows white when `next_sig_*`
    gives STOP or the block is occupied, blue otherwise;
  - approaching with the player's train: blue; passing it: white (behind
    the front, free-running); backing off a little: **blue again, and it
    stays blue** moving forward or back;
  - openRTS showed white after backing off: it built only the chain ahead of
    the end the train last moved towards. Fixed by the rule below; the user
    checked M10 again in openRTS and it now behaves as in RTS.
- **Both ends for the player's train (CONFIRMED, `0x508520`):**
  - the front pass (from the front traveller `train+0xA8`) runs when
    `speed (+92) > 0`, or the train type is 1 or 4, or flag `+4` bit 0x80
    is clear;
  - the rear pass (from `train+0xE8`, backwards) runs when `speed < 0`, or
    the type is 1 or 4, or that bit is set;
  - so the player's train (type 1; 4 PROBABLE a remote player) has the
    signals ahead of both ends evaluated every frame, also standing;
  - UNKNOWN: what bit 0x80 of `+4` is. openRTS keeps other trains on the
    end they last moved towards.
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

## Look-ahead on closed loops

### CONFIRMED (code reading, not run)

- **`0x47F214` (find the next track item along the path)** loops over nodes
  through `0x47ECD0`. It stops only when it finds a matching item or the
  transition returns no node (end node). Its distance limit is compared only
  when an item is found, and `0x508520` calls it with no limit (0.0).
- **`0x48106C` (`block_state`)** loops until a signal of the mask or an end
  node.
- **Neither keeps visited nodes.**

### PROBABLE

On a closed loop with no matching signal in the search direction, both loop
forever. The game hangs, which matches reports of MSTS/RTS "crashing" on
looped track. Not reproduced at runtime.

MSTS documentation agrees that this is old MSTS behaviour.
"Step-by-Step Guide to Building Routes for Microsoft Train Simulator", §1.3
"MSTS limitations" (p. 19):

> you cannot make a closed circuit (like a model railroad that allows running
> round and round forever): if you do, MSTS will freeze up. Loops and wyes can
> create similar problems. What matters is the "organization" (or
> "connectivity") of your route [...]: the order in which switches occur is the
> only important consideration here.

§3.4.4 (pp. 132–133) adds:
- the guide's author knows no general rule for placing loops and wyes;
- a mainline branching into a loop or wye "is usually fatal";
- every way to "drive out of the curved exit of a switch and somehow come
  back to the straight exit of that same switch" is a loop.

A freeze that depends only on connectivity, not on geometry, fits an
unbounded graph walk like the one above. RTS inherits it.


## Compiler, VM and evaluation in detail

Code reading (Hex-Rays), CONFIRMED unless marked. The compiler part is also
checked against RTS's own `sigscr.dat.asm` of TestRoute_RTS.

### Loading (`0x496DB0`)

- **Decoding:** the file is ANSI (`MultiByteToWideChar(CP_ACP)`).
- **Comments (`0x459D3C`):**
  - `/* */` is blanked;
  - `/*RTS# … */` keeps its body (only the markers are blanked);
  - `//` is blanked to the CRLF;
  - both scans start at the second character.
- **Scripts:**
  - each `SCRIPT` runs to the next `SCRIPT`;
  - the name is the next token, lower-cased, and matched against the
    signal types;
  - the body is **not** lower-cased: identifiers and built-in names are
    case-sensitive (`lstrcmpW`).
- **Constants (`0x4598E8`):** substituted per script, in this order,
  case-sensitive, as substrings without a word boundary. Each replacement
  is padded with spaces to the name's length:
  - `SIGASP_STOP_AND_PROCEED` before `SIGASP_STOP`, then the other
    aspects;
  - `SIGFN_NORMAL`, `_DISTANCE`, `_SHUNTING`, `_INFO`, `_REPEATER`;
  - `BLOCK_CLEAR`, `_OCCUPIED`, `_JN_OBSTRUCTED`;
  - `SIGFEAT_USER` (blanks, so `SIGFEAT_USER3` becomes `3`),
    `SIGFEAT_NUMBER_PLATE` 32, `SIGFEAT_GRADIENT_PLATE` 33;
  - `SIGMODE_NORMAL/SHUNT/FAILED/EXCL` 0–3.

  There are no constants for aspects 8 and 9.

### Statements (`0x497ED8`)

- **Words (`0x458D98`):**
  - a word is a run of characters in `-`..`9` or ≥ `A` (this includes `{`,
    `}`, `|`);
  - a `(` starts a bracketed group;
  - everything else is skipped.
- **`extern`** marks the next declaration as a built-in.
- **`float name`** declares a local, or, after `extern`, a built-in, by
  name:

  | Built-in | Id |
  |---|---|
  | `block_state` | 1 |
  | `route_set` | 2 |
  | `next_sig_lr` | 3 |
  | `next_sig_mr` | 4 |
  | `def_draw_state` | 5 |
  | `sig_feature` | 6 |
  | `state` | 7 |
  | `draw_state` | 8 |
  | `enabled` | 9 |
  | `dist_multi_sig_mr` | 10 |
  | `this_sig_mr` | 11 |
  | `this_sig_lr` | 12 |
  | `signal_mode` | 13 |

  `float name = expr` compiles `expr` into AX and emits `MOV name,AX`.
- **`if`:**
  - The condition (the bracketed group) compiles into AX, then `JZ if<d>`.
  - The body runs to the first `;` at brace depth 0, or to the closing `}`,
    and compiles at depth d+1. Then `JMP else<d>`, `:if<d>`.
  - An `else` body (extended over `else if` chains) compiles at d+1, then
    `:else<d>`.
  - **d is the nesting depth, so labels repeat.** The assembler (`0x498964`)
    resolves jumps in order: each jump takes the first label of its name
    not yet taken.
- **`return`** emits `RET`.
- **Any other word whose second character is ≥ `A`** starts a statement
  that is compiled up to the `;`, as an expression at AX. A one-letter
  first word is skipped (a quirk of the check).
- **Output (`0x49722C`):** `SCRIPT <name>` + CRLF CRLF, the code, then CRLF
  CRLF.

### Expressions (`0x45911C` tokens, `0x497334` code)

- **Tokens:**
  - a number (digits and `.`);
  - an identifier (`A`..`z`, digits, `_`). Followed by `(`, it is a call
    whose arguments run to the matching `)`;
  - a bracketed group;
  - an operator, matched 3-, 2-, then 1-character:

  | Text | Op | Prec | Mnemonic |
  |---|---|---|---|
  | `==# >=# <=# !=#` | 1 5 3 6 | 2 | `CMPE CMPGE CMPLE CMPNE` |
  | `># <#` | 4 2 | 2 | `CMPG CMPL` |
  | `== >= <= !=` | 13 17 15 18 | 2 | `CMPEF CMPGEF CMPLEF CMPNEF` |
  | `> <` | 16 14 | 2 | `CMPGF CMPLF` |
  | `&&` `\|\|` | 7 8 | 1 | `AND OR` |
  | `=` | 10 | 0 | `MOV name,…` |
  | `! - #` (unary) | 12 11 19 | 3 | `NOT NEG ROUND` |

  A lone `&` is an error. `^` is never tokenised.
- **Code at register r** (0 = AX … 7 = HX):
  - **No operator:**
    - a number or identifier: `MOV r,<text>`;
    - a call: each argument compiles at r and is pushed (`PUSH r`), then
      `CALL name,r`. The arguments are split at every comma, with no
      bracket depth;
    - a group: its inside compiles at r.
  - **Operators:** the lowest precedence present is taken, and its
    operators are applied left to right.
    - The left operand compiles once, at r, before the first operator,
      unless that operator is `=` or unary.
    - Each right operand (up to the next operator of that precedence)
      compiles at r+1. Then a binary operator emits `OP r,r+1`, a unary one
      `OP r+1` and `MOV r,r+1`, and `=` emits `MOV <left identifier>,r+1`.
    - So `a || b && c` is `(a || b) && c`, and nothing short-circuits.

### VM (`0x499948`, operands `0x49ACB0`)

- **Operands:** a local (index), a head field (`state` +84, `draw_state`
  +88, `enabled` +96, `signal_mode` +104), an immediate, or a register (16
  floats; AX..HX used).
- **Opcodes:**

  | Code | Op |
  |---|---|
  | 1 | MOV |
  | 3 | CALL |
  | 4 | PUSH |
  | 5 | JMP |
  | 6 | JZ (AX = 0) |
  | 0x0A–0x11 | CMPE, CMPNE, CMPG, CMPGE, CMPL, CMPLE |
  | 0x12 | AND |
  | 0x13 | OR |
  | 0x14 | XOR |
  | 0x15 | NOT |
  | 0x16 | NEG |
  | 0x17–0x1C | the float compares |
  | 0x1D | RET |
  | 0x1E | ROUND |

  - **Integer compares** truncate both sides toward 0.
  - **ROUND:** a non-zero value is kept when its fraction is below 0.5,
    else rounded up.
- **Locals are not reset between runs (PROBABLE).** They live in the
  compiled script, so all heads of a signal type share them, and a run sees
  the previous run's values. A script that reads a local before writing it
  depends on evaluation order.
- **Before a run (`0x499948`, CONFIRMED by code reading):**
  - the speed limit is set to −1;
  - in a chain, unless the head is FAILED: the object's first head takes
    the next (farther) object's mode when that is neither SHUNT nor FAILED;
    the object's other heads take the object's own mode (chain node +12);
  - overrides (the `SignalOverride` list, head +120, every record in turn)
    set the mode (+8 unless 0xFFFF), the speed (+12 when above −2), the
    aspect (+6 when not negative) and the draw state (+4 unless 0xFFFF; −2
    means the type's draw state for the aspect).
- **FAILED / EXCL mode:** the script is skipped.
  - aspect: the override, else 9 (FAILED);
  - speed: the override, else a global default (`dword_61D7D4`, value
    UNKNOWN);
  - draw state: the override (−2: the type's for the aspect, `0x47A138`),
    else −1: no draw state, the head is dark. (Earlier notes said the
    type's default; the code gives −1.)
- **After a run (in a chain):**
  - the object's mode becomes the head's;
  - with no override, a mode other than NORMAL that differs from the next
    object's spreads to every object beyond (`0x50B6F4`: object and heads,
    FAILED heads excepted);
  - overrides apply (aspect, speed, draw state as above);
  - SHUNT mode (unless `RTS_ShuntingAutoMode`) turns aspects 3..7 into 2
    (RESTRICTING), with that aspect's draw state and speed.
- **`block_state(mask)` and `route_set(mask)`:** the argument selects which
  signal functions end the walk:

  | Argument | Functions that end the walk |
  |---|---|
  | none, 0 | NORMAL only |
  | 1 | own + NORMAL |
  | 2 | own only |
  | 3 | any |
  | 4 | own and the lower function numbers |
  | 5 | as 1 plus the lower function numbers |

- **`block_state`** walks from the head's offset, in its direction, to the
  first such signal facing the same way.
  - **1:** a train found. On the head's node and on the end node, the train
    must be between the bounds; on the nodes in between, anywhere.
  - **2:** a trailing junction on the walk set against. This wins over an
    occupation found earlier.
  - **0:** otherwise. An end node ends the walk.
  - With the shunting flag, only trains flagged for it count.
- **`route_set`** walks the same way and counts the head's linked
  junctions (`TrSignalDirs`: node, branch) passed with that branch
  selected.
  - With no links: 1 when a signal or an end is reached.
  - With links: the count once a signal is reached and the count is not 0.
    With links but no match, it walks on past the signal.
  - 0 at a trailing junction set against.
- **`next_sig_lr(fn)` / `next_sig_mr(fn)`:** along the chain from the next
  object on.
  - For NORMAL, an object whose head-type minimum is NORMAL gives its cached
    obeyed aspect.
  - Otherwise, the max or min over that object's heads of type `fn`.
  - The first object that has one gives the result; 0 if none does.
- **`def_draw_state(asp)`:** the aspect in the type's aspect table. Returns
  its draw state (0 when the aspect is not listed) and sets the head's speed
  limit.
- **`sig_feature(f)`:**
  - f = 32 / 33 test head flags 0x10 / 0x20;
  - any other f ≥ 32 gives 1;
  - f < 32 tests bit f+5 of the head flags (from the world
    `SignalSubObj`).
- **`dist_multi_sig_mr(fn…)`:** the min over the listed types on the first
  object that has one. Starts at 7; 0 if none.
- **`this_sig_mr(fn)`:** the min over heads of type `fn` before this one on
  the object. −1 outside a chain, 0 with none.
- **`this_sig_lr(fn)`:** the max over the other heads of type `fn`. −1 if
  none or outside a chain.

### Free-running heads (`0x479AFC`)

- **Runs every frame**, from the draw loop. A head whose `enabled` is 0 (no
  train chain this frame) resets its mode to NORMAL (unless FAILED/EXCL)
  and runs with no chain. A global flag skips this (UNKNOWN which; the same
  one stops `0x508520`).
- **Lights:**
  - The draw state selects lights. A light marked flashing is lit while
    the head's timer (+108, plus the frame time, wrapping at `on + off`) is
    below `on`.
  - Each light is an RTS light object switched on and off (`0x449740` /
    `0x4497A4`). Signal lights are drawn by the light system, not by the
    shape.
  - Semaphores (type flag 1) set the arm position from the draw state.
- **`enabled` is cleared** at the end of the frame.
- PROBABLE: heads outside the drawn area do not update. That is visible
  only on screen.

### Per-train evaluation (`0x508520`, from the train update `0x424488`)

- **Collect** signal objects ahead of the front traveller (`0x47F214`):
  - up to 16;
  - skip objects whose first head is EXCL;
  - until the count of objects with a NORMAL head reaches the first NORMAL
    type's `SignalNumClearAhead` + 2.
- **A chain node** holds:
  - the obeyed aspect and the minimum head function;
  - the distance;
  - the mode from the train's flags: `+7` bit 0 gives SHUNT, bit 1 NORMAL,
    to the object and to each of its heads that is not FAILED (none set:
    the heads keep theirs);
  - the heads, next and previous.
- **Evaluate from the farthest to the nearest.**
  - `enabled` is 2 for the run, except 0:
    - for the farthest NORMAL object when the count was reached;
    - beyond the train's path or reservation end;
    - where a facing junction is set against the path end.
  - Objects beyond the movement authority, or with the global flag set, do
    not run.
  - After the run, an object farther than `speed · frame time · 1.5` keeps
    `enabled = 2` for the frame, so the free run skips it. A nearer one
    ("being passed now") is reset to 0.
- **Obeyed aspect:** the train obeys the max aspect over the object's
  NORMAL heads. With the train flags `+6`, other heads count too when their
  function is the object's minimum: bit 0 DISTANCE (also needs single
  player or the player's train), bit 1 SHUNTING, bit 2 INFO. The
  speed limit is the lowest positive one.
- **Passing:** the first object within the passing distance sets the
  train's speed limit (+108). For the player train in an activity:
  - flag 1 with aspect 1 (STOP_AND_PROCEED) fails with "passed red signal"
    (`0x41B4F4` code 1);
  - flag 8 with aspect 0 (STOP) fails with "passed stop signal" (code 4).
- **Backwards:** a mirrored pass runs from the rear (`train+0xE8`). Which
  passes run: see "Both ends for the player's train" above.
- **Occupancy for `block_state`:** per node, a list of train records with an
  offset (`node+52`, entries with an offset at +40). openRTS uses the whole
  train's extent per node instead (`docs/signals.md`). That covers every
  car, including cars on a junction or beyond it.

### Signal lights (`0x4491E8`)

When a world signal is loaded, every head gets one RTS light object per
`SignalLight` of its type (164-byte entries, head +112):
- **position:** the light's `Position`. If the shape has its flag at +13 set
  (UNKNOWN which), the translation of the shape matrix named after the head's
  `SignalSubObj` is added: x − tx, y + ty, z + tz in RTS space. Only the
  translation counts, not the matrix's rotation — CONFIRMED;
- **size:** `Radius`; **texture:** the type's `LightTex` (name, UV rectangle);
- **colour:** the `LightsTab` entry (ARGB).

The head update `0x479AFC` switches these lights on and off from the draw
state (flashing included). The lights are drawn by the light system, not as
part of the shape. RTS space has Z flipped against the MSTS shape space:
TestRoute_RTS's mast signals have their lenses towards −Z in the shape, while
`Position` puts the lights at +Z (0.65 m). openRTS therefore places a light at
(tx − x, y + ty, tz − z) in shape space: Position is turned 180° about the
vertical against the shape. The X sign is checked on Kuban's
T_Small_YGW_Rx-new (red lens column at shape x +0.28 for Position x −0.28),
the Z sign on TestRoute's mast heads — CONFIRMED by geometry.

### Drawing a light (`0x408574` light loop, `0x449818` quad)

Lights are drawn after the objects: depth test on, depth write off, no
lighting or fog, under the owning object's transform. Each light is **one flat
quad**, not a camera-facing billboard: corners at (+-s, +-s, 0) rotated by the
light's quaternion (identity for signal lights, set in `0x4491E8`) around
Position, UV from LightTex, tinted by the LightsTab colour. So the quad lies
in the signal's XY plane, flat on the lens: head-on it is the lit lens, from
the side it is edge-on, from behind the housing hides it - CONFIRMED.

Half size s = Radius * fov/45 * max(1, d * light_scale * |u1 - u0| * k / Radius),
where d is the camera distance, light_scale = 0.012 * LightsScale (rts.ini)
and k = texture width / 512 (integer division) when the texture flag +13 is
set. Near it is the lens's size, far it keeps a constant angular size -
CONFIRMED. The fov is RTS's camera field of view, config.ini `FOV` (65 in
this installation).

openRTS's view zoom (Ctrl and the wheel, outside the cab; not RTS): the far
term is divided by the zoom for signal lights (`SignalView::collect_lights`),
so a far light keeps the size it has unzoomed on the screen; zoomed in, it
was as many times bigger, a red light at a kilometre filling the view. Near,
the lens's Radius (a real size) is seen zoomed like the signal. The trains'
lights keep the rule as it is (2026-11-08).

**Quirk — a light's size follows the field of view.** The factor fov/45 is
computed by `rts1.exe` and handed to `lamp.vert.shader` in `Size` (the
shader only draws a quad of `Size.x * Size.y` around the light). It keeps a
glow's size on the screen when the FOV changes, which suits a halo, but RTS
applies it to every light quad, also to those that are part of a model:
lamp faces, and the ED9M's searchlight cone built of six glow quads
(train-dll.md, exterior lights). Changing the FOV in RTS grows and shrinks
them around their centres: lamp faces overflow or shrink inside their
housings, the cone moves away from or into its lamp. Content is made at
the FOV its author used (the ED9M cone sits at its lamp at 65). openRTS
sizes every light (signals and trains) with the factor at 65 degrees,
fixed, whatever the field of view (`GlowSpriteRenderer`). The lens geometry itself is part of the shape; nothing else lights it.

**PROBABLE** blending: the light quad (`0x449818`) enables `GL_BLEND` and
disables the alpha test but sets no blend function of its own, and the
light loop of `0x408574` sets none before it; the frame's standing one is
`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` (set back there after its additive
part). So a light is laid over the scene by its alpha (`lamp.frag`:
texture x LightColor), after every object, the blended glass included.
The loop sits between object passes 2 and 3 (shape-rendering.md, Passes):
after the glass, before the cab. openRTS draws train and signal lights
there, laid over by their alpha in the RTS path (`GlowBlend::over`) and
added in the experimental path (`GlowBlend::added`, emissive in the HDR
frame).

### SignalSubObjs and shape parts (`0x4491E8`, parser `0x483694`)

SigSubType numbers: SIGNAL_HEAD 1, DECOR 2, NUMBER_PLATE 4. When a world
signal is loaded, every sub-object of its shape that is **DECOR** and whose bit
is clear in the world's SignalSubObjs mask sets flag 1 (hidden) on the shape
matrix of the same name. Nothing else is hidden: heads stay drawn whatever the
mask or OPTIONAL say (many shapes name every head "SIGNAL", the matrix that
holds the whole mast). A SIGNAL_HEAD whose type has flag +6 bit 1 sets matrix
flag 2 and gets an animation (semaphore arms) - CONFIRMED.

### Lights seen from behind

RTS enables `GL_CULL_FACE` once (`create_game_window`, `0x40564D`) with the
default back-face culling and only disables it briefly elsewhere; the light
loop does not. The one-sided light quad is therefore invisible from behind
the lens plane — CONFIRMED. openRTS skips a light whose lens faces away from
the camera, which also hides the far-grown quad around a head seen from
behind.

### A light behind its lens's glass (openRTS)

RTS draws the quad at the light's Position, depth tested against the
signal: the depth test stays on from `0x409A0D`, and the light loop only
turns depth writes off (`glDepthMask(0)` at `0x40C097`). It changes neither
the depth function nor the polygon offset — CONFIRMED (code). Where a
Position lies inside the lens, the lens's glass hides the quad. Then only
the glow's edge past the housing shows, and only far off, where the quad has
grown.

On Novokuznetsk this is so for every `-new` head: 219 lights, 43–49 mm
behind the glass. Other heads there have 425 lights 3–10 mm behind a surface
(`openrts-signal-check <root> --lenses <route>`). By its code RTS shows these
lenses unlit as well. UNKNOWN whether it does in practice; not compared at
runtime.

openRTS casts a ray from the light along the way its lens faces (shape
space, the nearest distance level) and finds the frontmost surface it meets
within 15 cm. The quad goes 1 cm in front of that surface, or 1 cm in front
of Position if nothing is met (`SignalView::lens_front`, once per shape,
type and light). Since 2026-11-08; before, the quad was only lifted 1 cm.
