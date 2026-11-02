# Signals

Status: implemented (see "State" below). Roadmap step 3: the signals of a route show the
aspects their scripts compute. They react to the player's train, to
occupied blocks and to switch positions. The train sees the aspect and the
speed limit ahead, and passing a stop signal is detected. Paths with
reservation, the dispatcher console and AI trains are step 4.

## State (2026-10-31)

Steps 1–6 are implemented. What is in the code:

- **Reverse** (`docs/reverse/signals.md`, "Compiler, VM and evaluation in
  detail"):
  - the compiler, statement by statement, with its quirks;
  - the VM opcodes;
  - `block_state` / `route_set` walks and masks;
  - free-running heads (every frame);
  - the per-train chain, the passing check, the light switching.
- **Formats:**
  - `formats/signal_config` (`sigcfg.dat`);
  - world `SignalSubObj` / `SignalUnits` in `route_world`;
  - `formats/signal_script`: a port of the RTS compiler. Its text assembly
    matches RTS's own `sigscr.dat.asm` for TestRoute_RTS **line for line
    (17583 lines, 0 differences)**. An assembler resolves repeated labels
    the RTS way.
- **Simulation** (`simulation/signals`):
  - `SignalSystem`: heads from the world signals, placed by their TDB items;
  - the register VM with every built-in;
  - occupancy from each train's whole extent (rear to front, through the
    junctions it lies on);
  - train chains far to near;
  - free-running heads;
  - `SignalAhead` for the train, `SignalPass` events.
- **Update cost:**
  - Train chains run every step.
  - A free-running head reruns when its walk's nodes change occupancy, its
    junctions change, or it leaves a chain.
  - RTS keeps script locals between runs. A run that read a local before
    writing it is detected at run time, and that head reruns every step.
    Static analysis would mark 40 % of the scripts (`T_Head_YGR` reads
    `next_state` only when the state set in the same branch says it was
    written). At run time 5–45 heads per route remain.
- **Viewer** (`SignalView`):
  - loads in the background;
  - steps with the train;
  - lit lights as glow sprites (`renderer/glow_sprites`), at the
    `SignalLight` position in the head sub-object's frame, with the
    `LightsTab` colour and flashing;
  - the Signals panel: next signal, passes, nearest head with
    `block_state`, `route_set` and the script's locals;
  - optional aspect posts.
- **Checks:**
  - `tests/signal_test` (compiler output, label order, precedence, the
    system on a synthetic track with a train);
  - `openrts-signal-check`:
    - all 7 routes: 1697 scripts, 0 errors, 19575 heads, all placed;
    - `--run`: changed-only against every-step evaluation, 0 mismatches
      on every route; ≤ 0.33 ms per step with a train.

Not done from the plan:
- **Lights** are glow sprites, not `LightTex` textures.
- **Optional decor parts** of a signal shape (`SignalSubObj` mask) are all
  drawn.
- **Semaphore arms**: no content has them.
- **Path ends and reservations** do not limit the chain (step 4).

## What exists

- **Reverse notes** (`docs/reverse/signals.md`):
  - `sigcfg.dat` keywords, aspect and function numbering;
  - head fields;
  - the `sigscr` language as RTS compiles it: lexing, `/*RTS#`, constants,
    operators and their RTS quirks, the register VM;
  - the built-ins, the per-train evaluation order (`0x508520`), modes and
    overrides;
  - the look-ahead hang on closed loops.
- **Track:**
  - TDB `SignalItem` with flags, direction, angle and `TrSignalDirs`, read
    into `TrackDatabase`;
  - travellers with look-ahead bounded by distance (no endless loop,
    `track.md`);
  - junction state and occupancy;
  - the track overlay already marks signal items.
- **World:** `Signal` objects are read as an object kind and drawn as static
  shapes. Their `SignalSubObj` mask and `SignalUnits` (head index →
  `TrItemId`) are not read yet.
- **Trains:**
  - `Train` with front and rear travellers, `TrainEvent`s and the fixed
    0.05 s step;
  - the viewer drives one train.

## Content (installed)

| Route | Signal types | Shapes | LightTex | World signals | Heads |
|---|---:|---:|---:|---:|---:|
| Chernyshevsk_RTS | 241 | 151 | 17 | 1466 | 2280 |
| Kievskij_hod_RTS | 168 | 110 | 1 | 2904 | 5068 |
| Kuban_RTS | 267 | 163 | 17 | 1091 | 1951 |
| Novokuznetsk | 307 | 190 | 2 | 3246 | 4978 |
| Pavlovsk_RTS | 238 | 150 | 17 | 1894 | 3441 |
| TestRoute_RTS | 241 | 151 | 17 | 135 | 191 |
| Uspensk_RTS | 242 | 152 | 17 | 1010 | 1666 |

- One `sigscr.dat` per route, one script per signal type (about 2100 in
  total).
- No semaphores in the installed content (no `SEMAPHORE` flag). Semaphore
  arms are planned, but only checked on synthetic data.
- Built-ins used by the content: `block_state`, `route_set`,
  `next_sig_lr/mr`, `def_draw_state`, `sig_feature`, `enabled`, `state`,
  `draw_state`. The other built-ins are implemented as reversed but are
  not exercised by content.

## 1. Reverse notes (targeted, IDA on the copy)

Open questions the implementation depends on:
- **Lights on screen:** how RTS draws a lit head. Sprite or billboard size
  from `Radius`, texture from `LightTex`, colour from `LightsTab`, blending,
  depth test, distance scaling, day/night brightness, fog. Entry: the
  per-frame head update `0x479AFC` and its draw call.
- **When signals update:**
  - how often `0x508520` and the free-running head scripts run (every
    frame, a timer, on events);
  - what order is used when several trains approach one signal.

  openRTS runs them on the 0.05 s step (deterministic, multiplayer); the
  reverse decides whether that differs from RTS anywhere observable.
- **Occupancy for `block_state`:** the train occupancy records
  (`train+0x128/+0x148`, `0x490324`):
  - whether a train occupies whole vector nodes or offsets;
  - what counts as occupied ahead of a facing junction;
  - the PROBABLE shunting exception.
- **Passing a stop signal:** the exact condition of `0x41B4F4`: which end
  of the train, which heads, at what aspect, outside activities.
- **`route_set`** (PROBABLE in the notes): confirm the walk to the next
  signal.
- **Signals without a script** (unknown type, compile error): their default
  aspect and draw state.

Each answer goes into `docs/reverse/signals.md` with its confidence.

## 2. Formats

- **`sigcfg.dat`** (`formats/signal_config`) with everything in the reverse
  list:
  - light textures, light table, signal types (function, flags, lights,
    draw states with flashing, aspects with speeds, `SignalNumClearAhead`,
    `SemaphoreInfo`, `SigFlashDuration`);
  - signal shapes with sub-objects;
  - `ScriptFiles`, `RTS_ShuntingAutoMode`.

  Read with RTS semantics: keywords case-insensitive, unknown blocks
  skipped and reported.
- **World `Signal`:** `SignalSubObj` mask and `SignalUnits`. A head is its
  sub-object index plus a `TrItemId`.
- **`sigscr` compiler** (`formats/signal_script`): source → instructions for
  the RTS register machine (8 float registers, 30 opcodes, `JZ`/`JMP`/`RET`,
  `PUSH`/`CALL`), following the reverse:
  - lexing with `/*RTS#`, textual constant substitution padded to keep
    offsets;
  - `extern` and local declarations, `if/else`, `return`;
  - RTS operator precedence: `&&` and `||` equal, left to right; integer
    compares `==#` etc.; unary `-`, `!` and `#`; no arithmetic; no
    short-circuit;
  - errors with line numbers. A script for an unknown type is reported; a
    second script for the same type is an error, as in RTS.
- **Tool `openrts-signal-check`:** compiles every installed `sigscr.dat`,
  checks the types against `sigcfg.dat`, and lists errors and unused
  built-ins.

## 3. Simulation

- **`SignalSystem`** (`simulation/signals`), built from the network, the
  route's `sigcfg`, the compiled scripts and the world signals:
  - **Heads:** type, function, TDB item, the vector node, offset and
    direction it faces, linked junctions. State: `state`, `draw_state`,
    `enabled`, speed limit, `signal_mode`, flash timer.
  - **Objects:** heads grouped by world signal, in head order (for
    `this_sig_*`).
- **VM** (`simulation/signal_vm`): runs one head's bytecode with locals
  zeroed, head fields as operands and built-ins through an interface.
- **Built-ins on the network:**
  - **`block_state`:** walk from the head along its direction to the next
    signal of its own function or a NORMAL one. 0 clear, 1 occupied, 2 a
    junction set against (the traveller's `probe` mode gives that already).
    Bounded by distance.
  - **`route_set`:** the head's linked junctions (`TrSignalDirs`) set for
    it.
  - **`next_sig_lr` / `next_sig_mr`, `dist_multi_sig_mr`:** along the
    evaluating train's path (the chain built in the per-train pass). STOP
    without a chain.
  - **`this_sig_lr` / `this_sig_mr`, `def_draw_state`** (also sets the
    head's speed limit from the aspect), **`sig_feature`** (from the world
    sub-object mask).
- **Occupancy: the whole train, not its front.** The network keeps, per
  train, the list of node intervals the train covers. Each step a walk goes
  from the rear traveller to the front one, along the junctions the train
  lies on. This covers:
  - **every car between the ends**, also those standing on a junction or
    beyond it on a branch. The walk goes through each junction the way the
    train went. Junctions under a train are locked (occupied), so the walk
    cannot take a branch the cars are not on;
  - **the clearance** behind the tail: a junction the tail passed facing
    stays occupied for its `ClearanceDist` (already in the simulation);
  - **trains in either direction**: a block is occupied whichever way the
    train faces or moves.

  When the list changes (an interval entered or freed, which is the head
  and the tail crossing node boundaries), the heads whose `block_state`
  covered those intervals are marked to rerun. `block_state` asks this
  structure. One train now; it holds many (AI and remote trains in step 4
  and multiplayer).
- **Signal passes per train end.** The front passing a head gives
  `signal_passed` (and SPAD at STOP). The rear passing it frees the block
  behind. Both come from the travellers' moves, not from a car's centre.
- **Evaluation** on the 0.05 s step, after the trains moved:
  - **Per train:** collect up to 16 signal objects ahead along the set
    route. Continue until `SignalNumClearAhead` of the first NORMAL head + 2
    NORMAL signals. Evaluate from far to near with `enabled` set, as
    `0x508520`.
  - **Which end:** the end the train last moved towards (it stays so while
    the train stands). The player's train (`SignalSystem::set_player`, the
    keyboard-driven one in the viewer) also runs the pass ahead of its other
    end every step, as RTS does for train types 1 and 4; with a path, that
    pass runs with `enabled` 0. What the train obeys and passes comes from
    the end it moves towards.
  - **Free-running:** heads no train approaches run with no chain
    (`next_sig_*` gives STOP). They do not run every step: see
    "Update cost" below.
- **Update cost: only what changed runs.** Up to 5068 heads per route, and
  `block_state` walks the track, so running everything every 0.05 s is
  wasted work. A head's result depends only on its inputs. The rule:
  - **Train chains run every step.** They read the moving trains: up to 16
    objects per train.
  - **Every other head reruns only when an input changes:**
    - the occupancy of a node interval its `block_state` walk covered;
    - a junction on that walk or among its `TrSignalDirs`;
    - its mode or override;
    - the aspect of a head it reads (`this_sig_*`).
  - The walk records what it covered, so these dependencies are known per
    head.
  - Everything runs once when the route loads.
  - **Flashing** is a renderer timer, not a script run.
  - A head that leaves a train's chain runs once more with no chain.

  The result is the same as running all heads every step, because a head
  whose inputs did not change computes the same aspect.
  `tests/signal_test` checks this: a run where everything is evaluated
  every step must match the dirty-tracking run. How often RTS itself
  updates (step 1) only matters if RTS's results differ observably, for
  example a free-running head lagging behind.
  - **Modes:** FAILED/EXCL heads skip the script. SHUNT forces aspects 3..7
    to RESTRICTING unless `RTS_ShuntingAutoMode`.
  - **Overrides** (`SignalSystem::set_override`, RTS `SignalOverride`): a
    mode, an aspect, a draw state (or the type's for the aspect) and a speed
    in place of the script's; unset fields are left to the script. A FAILED
    or EXCL head without an override shows FAILED and lights nothing.
  - **Train flags** (`set_train_settings`, RTS train +7 / +6): a train's
    chain gives its heads SHUNT or NORMAL (FAILED heads keep theirs), and
    distance, shunting or info heads count for the aspect it obeys when they
    are an object's lowest function.
  - **Along a chain:** an object's first head takes the next object's mode
    unless that is SHUNT or FAILED, its other heads their object's; a mode a
    script sets spreads to the objects beyond, unless an override holds the
    head. The details: `docs/reverse/signals.md`.
  - **Track control** sets both for testing (a signal's "Override" menus, a
    train's "Signals for this train"); activities and the dispatcher console
    will use the same API.
- **For the train** (`Train` gets a look-ahead, no new physics):
  - the next signal object ahead: distance, the aspect it obeys (least
    restrictive NORMAL head; distance, shunting and info heads by the train
    flags), the speed limit (lowest aspect speed);
  - a `TrainEvent::signal_passed` with the aspect. Passing at STOP is
    reported as SPAD (exact condition from step 1).
- **Loops:** every walk is bounded by distance, so a closed loop without
  signals ends instead of hanging. Legacy keeps the RTS result where one
  exists (a found signal); where RTS would hang, openRTS stops and logs it.

## 4. Rendering

- **Lights:** each head's `draw_state` gives its lit lights (`DrawLights`).
  Each light is drawn at its `Position` in the signal shape frame, with
  `Radius`, `LightTex` and the `LightsTab` colour, the way the reverse in
  step 1 finds RTS draws them. Flashing uses `SigFlashDuration` and the
  head's timer. A renderer API for lit sprites (`SignalLightRenderer` or
  part of the model renderer): one batch per frame, depth-tested.
- **Heads in shapes:** `SignalSubObj` and `SIGNAL_HEAD` sub-objects decide
  which parts of a shared signal shape are visible (the world mask).
- **Semaphores:** dropped (the user's decision, 2026-11-01). For the
  record: `SemaphorePos` per draw state gives the arm's animation frame, and
  RTS animates the head's matrix when the type has flag +6 bit 1
  (`0x4491E8`).
- **Track overlay:** signal markers coloured by the head's aspect. Option:
  the head's script state in a tooltip for debugging.

## 5. Viewer

- **Train panel:** next signal, distance, aspect, speed limit, and the SPAD
  counter. The cab's ALSN comes with step 5.
- **Signals panel:** counts by aspect, heads without a script, scripts
  with errors. A pick (nearest head to the camera) shows its type, script
  locals, `block_state` and `route_set`.
- **`--signals off`** for comparisons. Debug views (Ctrl+F6) record the signal
  states (optional).

## 6. Checks

- **Compiler tests** (`tests/signal_test.cpp`): lexing, constants,
  precedence quirks (`a || b && c`), integer compares, `#x` rounding, `if`
  chains, errors; the VM on synthetic heads.
- **Built-ins on a synthetic network:**
  - a block with and without a train gives 0/1;
  - a junction set against gives 2;
  - `route_set` both ways;
  - `next_sig_*` along a chain;
  - a loop without signals ends.
- **Content:** `openrts-signal-check` compiles all 7 routes with no errors
  (or with the same errors RTS reports). Every head runs once with no train,
  with no VM fault.
- **Train runs:** the test track with three signals and a train: aspects
  step down behind the train and clear as it leaves. `TrainEvent`s of
  passing signals, SPAD at a forced STOP.
- **Viewer runs**, silent: TestRoute and Kuban with a train, screenshots of
  lit signals ahead and behind it.
- **Optional, needs you:** RTS writes its compiled scripts to `<file>.asm`
  with `SignalScriptsGenerateAsm` in its config. Running RTS once with that
  set (I may not change `RTS/` myself) gives a reference to diff the
  openRTS compiler against.

## Order and size

| Step | Content | Depends on | Size |
|---|---|---|---|
| 1 | reverse: lights, timing, occupancy, SPAD, defaults | — | medium |
| 2 | formats: sigcfg, world heads, compiler, signal-check | 1 (defaults) | large |
| 3 | simulation: system, VM, built-ins, occupancy, evaluation, train look-ahead | 2 | large |
| 4 | rendering: lights, flashing, sub-objects, semaphores | 1, 3 | medium |
| 5 | viewer: panels, pick, overlay colours | 3, 4 | small |
| 6 | checks over all routes, viewer runs | all | small |

Milestone: after steps 1–3 the signals compute their aspects; step 4
makes them visible.

## Legacy and multiplayer

- Scripts compile and run exactly as RTS does: the operator quirks, no
  short-circuit, the same evaluation order. With Legacy on, this is the only
  path.
- Bounded walks are the one deliberate difference (a hang in RTS). They do
  not change a result RTS produces.
- Signal states are a function of trains, switches and overrides, all on
  the shared 0.05 s step. A server can therefore send switches and overrides
  only, and every client computes the same aspects. Whether RTS's protocol
  sends aspects is a question for the multiplayer step.

## Not in this step

- Path reservation and automatic switch setting, signal modes driven by
  the dispatcher, speed posts (step 4).
- ALSN and cab signalling (step 5, with the DLL `ALSN` callback).
- Activities' `SignalOverride` and `SetSignalMode` events (step 7; the
  override API is ready for them).

## Paths and the reserved way (2026-11-01)

- A train with a path has a movement authority (its reserved way,
  `paths.md`); heads beyond it run with `enabled` 0 (RTS 0x508520).
- A pass is judged by the aspect shown while the train was still beyond the
  passing distance (RTS no longer runs an object being passed for the
  train); the train's own front reaching the signal no longer turns the pass
  into one at danger.
- `block_state` counts a train only when it is strictly inside the bounds
  (a front exactly at the signal is not in its block yet).
