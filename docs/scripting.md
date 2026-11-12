# Scripting in Luau

openRTS's own modules can be written in [Luau](https://luau.org), Lua 5.1
with types and a few additions (`+=`, `continue`, `if … then … else`
expressions). The first module is the brake distributor (ВР) of the
realistic air model; train plugins come later in the same language
(`roadmap.md`, "Lua plugins").

Code: `script/` (the VM), `simulation/src/distributor.cpp` (the ВР),
`tools/brake-stand` (the stand), `sdk/luau/` (samples).

## The sandbox

Every script runs in a VM of its own:
- Luau's own libraries, which are safe: `math`, `string`, `table`,
  `bit32`, `utf8`, `buffer`, `vector`, `coroutine`, `debug` (`info` and
  `traceback` only), `os` (time only: `clock`, `time`, `date`). No `io`, no
  files, no processes, no `require` yet;
- the libraries' globals are frozen, the script's own globals live in a
  table of their own;
- every call has a time limit (200 ms): a loop that never ends is stopped
  with an error instead of hanging the game;
- an error names the file and the line (`vr292.luau:12: …`). The game
  writes it to its console (`openrts.log` when the menu starts it), once
  per script and error, and the car goes on with its built-in distributor.

## A brake distributor (ВР)

**Attaching.** In the `.wag` or `.eng`, inside `Wagon ( … )`:

```
ORTSBrakeDistributor ( "my_vr.luau" )
```

The file lies next to the `.wag` / `.eng`. It works with the realistic air
model (the Train window, "Realistic air (openRTS)"), which a train runs
while none of its locomotives has an RTS DLL: those DLLs' driver's valves
are tuned to RTS's own distributor.

**What the script gives.** A global function `step(c, dt)`. The model
calls it every sub-step (under 2 ms in the game, 3.3 ms on the stand) for
the car, with the car's table `c` and the sub-step's length `dt` in
seconds. A call costs well under a microsecond for a script like the
sample. The script reads and changes the table:

| Field | | What |
|---|---|---|
| `pipe` | read, write | ТМ at the car, atm. Lowering it takes air from the pipe (charging, quick service) |
| `rate` | read | how fast the pipe changes, atm/s |
| `zr` | read, write | ЗР, atm |
| `tc` | read, write | ТЦ, atm |
| `ref` | read, write | a level to remember (483: its РК; 292: the ЗР when braking began) |
| `braking` | read, write | the distributor's position: braking (applying or lap) rather than release. The game only keeps it between steps; the brake force comes from `tc` |
| `emergency` | read, write | the emergency front has reached the car; set it to start the front here |
| `ep` | read | the ЭПТ target, atm (< 0: none) |
| `ep_wired` | read | the car has the ЭПТ |
| `tc_max` | read | ТЦ of a full application (483: by the load) |
| `ratio` | read | `TripleValveRatio` |
| `v_pipe`, `v_zr`, `v_tc` | read | volumes of the pipe at the car, the ЗР and the ТЦ, m³ |
| `mass` | read | the car's mass, kg |
| `kind` | read | the built-in distributor the car would have: `"292"`, `"483"`, `"305"` |
| `load` | read | the 483's load mode: `"empty"`, `"medium"`, `"loaded"` (the car menu; by the mass until set) |
| `mountain` | read | the 483's mountain mode (the car menu) |
| `plug` | read | the 292's plug: `"short"` (К), `"long"` (Д), `"no_accelerator"` (УВ) (the car menu) |
| `state` | yours | a table kept between steps, for anything else |

Pressures that come back are taken as numbers not below 0; anything else
is ignored. The pipe's flow between cars, the driver's valve and the
emergency wave stay the model's.

**A sample.** `sdk/luau/vr292.luau` is the built-in ВР 292 written in Luau,
line for line (the test `openrts.distributor` checks that it gives the
same ТЦ and ЗР as the C++ one). Start from it.

## The stand

`openrts-brake-stand` runs a distributor through a fixed program, as a
test stand with its own driver's valve would: the pipe is held to the
program, and the distributor's answer is measured.

```
openrts-brake-stand --script my_vr.luau [--mass 70000] [--ratio 2.5] [--table]
openrts-brake-stand --kind 292|483|305
openrts-brake-stand --kind 483 --load empty|medium|loaded [--mountain]
openrts-brake-stand --kind 292 --plug К|Д|УВ
```

`--load`, `--mountain` and `--plug` set the switches as the car menu does
(`--plug` takes К / Д / УВ or K / D / UV); a script sees them in `c.load`,
`c.mountain` and `c.plug`.

The program, from an empty car:

| Time, s | Pipe | What is measured |
|---|---|---|
| 0–300 | charge to 5.0 | how long the ЗР takes to 4.9 |
| 300–360 | a step of 0.5 (4.5) | ТЦ, and how long it takes to 95 % of it |
| 360–450 | release to 5.0 | how long until ТЦ < 0.4 |
| 450–510 | full service (3.5) | ТЦ |
| 510–570 | release with an overcharge to 5.6 | |
| 570–840 | the overcharge removed at 0.15 atm a minute | stability: any rise of ТЦ (0 is right) |
| 840–900 | emergency | how long to 95 % of the full ТЦ |

`--table` adds the pressures every 10 s. A ВР that brakes during the slow
removal is marked `(ТОРМОЗИТ)`: that is the creep RTS's own distributor
has.
