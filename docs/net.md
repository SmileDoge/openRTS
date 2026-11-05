# The RTS network protocol in openRTS

Stage 1 of the multiplayer plan (`roadmap.md`, "6. Multiplayer"): the RTS
protocol without a network. The packages are catalogued in
`reverse/multiplayer-protocol.md`; this is what openRTS does with them.

## The libraries

| Target | What it holds |
|---|---|
| `openrts_net` | the wire format, the package names, the startup snapshot. No simulation, no sockets |
| `openrts_net_world` | a snapshot in openRTS terms: junctions, signal overrides, where trains stand |

### The wire format (`openrts/net/wire.hpp`)

- A package is a `u16` id, a `u32` length that counts these six bytes, then
  its body. Packages nest inside bodies. Fields are little-endian.
- `split_frames()` cuts data into packages and stops at the first header
  that does not fit, saying where.
- `WireReader` reads fields in order; reading past the end sets `ok()`
  false and returns zeros, so a parser checks once at the end.
- `WireWriter` writes fields; `begin()` / `end()` wrap a package and fill in
  its length when it closes.
- **Strings.** A length in UTF-16 units (`u32` or `u16`), the units, then a
  zero unit. A length of 0 has neither: an empty second name in
  `0x461A` is four zero bytes.

### Package names (`openrts/net/packages.hpp`)

`enum class Package` with the names of the catalogue
(`reverse/multiplayer-protocol.md`, "Package names"). `package_name(id,
parent)` gives a name for printing; the parent tells `train_cars` /
`train_car` inside `train_add` from `uncouple` / `couple`.

### The startup snapshot (`openrts/net/snapshot.hpp`)

What a server sends a player after its login, and what the RTS client saves
as `RTS/multdata` and replays from.

- `read_snapshot()` takes the packages one by one into a `Snapshot`: time of
  day, junctions, signal heads, substations and phases, section sources,
  piece supplies, trains (cars, position, standing, assignment, names,
  brakes, route length, cargo, derailment), user records and player
  positions. Packages it does not model stay in `other`, in order.
- A package that does not parse, or a position for a train the snapshot
  has not got, is a problem; the package is kept in `other`.
- `write_snapshot()` writes it in the order the RTS server sends it. The
  `multdata` of the live session of 2026-11-05 writes back byte for byte.

**How the snapshot names things** (CONFIRMED against Kuban: all 555
junctions, all 1 951 signal heads and the three trains fit):

| Package | Field | In openRTS |
|---|---|---|
| `0x460A` junction | index | the TDB node number − 1 (the client reads its node table at the index; `0x4603` reads it at node − 1) |
| `0x460C` signal head | index | the head's TDB `SignalItem` (`TrItemId`); openRTS heads carry it as `track_item` |
| `0x4603` train position | node, offset, direction | the train's **rear** end on a TDB vector node; direction 0 faces along the node (the offset grows ahead), 1 against it |
| `0x4604` car position | offset | the car's middle, metres ahead of the rear end; car 0 has the largest |

The front of car 0, where openRTS places a train, is the rear end moved
ahead by car 0's offset and half its length (`front_distance()`; RTS does
the same in `0x4606`, `0x4C3DE0`).

### The snapshot in openRTS terms (`openrts/net/world.hpp`)

- `apply_junctions()` throws each junction the snapshot names; one that is
  no junction node or has no such branch is a problem.
- `apply_signals()` gives each head the snapshot names its aspect, drawing
  state and speed as an override: the server decides signals, an RTS
  client only shows them.
- `train_front()` walks from the rear end to the front of car 0 along the
  junctions as they stand, so junctions go first.
- `car_file()` turns a car's `folder\name.eng` into a consist entry's
  folder, name and kind.

## The tool: `openrts-net-dump`

```text
openrts-net-dump <snapshot> [--packages] [--root <game folder> --route <name>]
```

- Prints the snapshot as a world: time, junctions, aspects, electrification,
  trains with their cars and positions, players. `--packages` lists the
  packages first.
- Writes it back and says whether the bytes are the same.
- With `--route` checks it against the route: every junction, every head
  (a TDB signal item that a world signal shows), every train's front on the
  track, every car file installed, every piece supply's node.
- Exits with 1 when anything does not fit.

## The viewer: `--mp-snapshot <file>`

```text
openrts_viewer --window --route Kuban_RTS --root <game folder> --mp-snapshot <game folder>/RTS/multdata
```

- The time of day comes from the snapshot unless `--time` is given.
- The junctions are thrown, the trains put on the track (as a situation
  puts them back), the client's own train driven; the rest stand.
- The heads get the snapshot's aspects once the signals have loaded. They
  are overrides: the heads keep them, as an RTS client keeps what its
  server sends.

**Not applied yet:** electrification (substations, section sources, piece
supplies), chocks and handbrakes from the car flags, cargo, derailments,
track objects, weather. The snapshot reads them; the viewer leaves them.

## Watching a game: `--mp-connect` (stage 2, the observer)

```text
openrts_viewer --window --route Kuban_RTS --root <game folder> --mp-connect 127.0.0.1:800 --mp-nick openRTS
```

**The connection** (`openrts/net/connection.hpp`, `RtsConnection`):
- TCP to "host:port" (port 800 when none is given); the login `0x4600` with
  the protocol version 101 and the nick (1 to 15 characters).
- The reply has no length field: `0x4600`, the version, the description,
  season and weather, the route id and four counts; or a code alone
  (`0x4701` nick in use, `0x4702` no new players, `0x4705` another
  version).
- Then a thread reads whole packages into a queue the main loop takes.
  When the socket closes it queues a `server_down` of its own, as
  `rts1.exe` does.

**Joining:**
1. The viewer logs in and says on the console that it waits for a train.
2. **The dispatcher assigns it one** on the console (ТЧД). The server sends
   the world only to a player with a train (reverse: `0x44ECAC` sets the
   user's `+41`; without it `0x4608` gets `0x4703`).
3. On the assignment (`train_assign`, mine) it asks for the snapshot
   (`0x4608`) and takes the packages from the empty `0x4600` to `0x4608`.
4. The route scene comes up with the snapshot's time, junctions, signals
   and trains.

**In play** (`viewer/src/net_session.cpp`, the packages from
`openrts/net/play.hpp`):
- the time of day (`time`), junctions (`junction`, set the server's way even
  where a train here holds them), signal heads (`signal_head`, overrides);
- trains: `train_add`, `train_pos` / `train_pos_forced`, `train_stands`,
  `train_remove` (off the track or deleted), `train_info`;
- the dispatcher's messages, pause and `server_down` on the console.

**Remote trains** (`net::RemoteMotion`): every train is another client's
or the server's. It is put on the track as a train that is never
simulated (`TrainView::add_remote`) and drawn one package interval behind
(measured: 1.5 s for players, 0.5 s for TRAFFIC), interpolated along the
track between its positions; past the last one it goes on at its speed for
one more interval, and stops where `train_stands` says. A position the
track does not join to the last one puts the train there when it is due.

**Not yet:** nothing is sent back, the assigned train is not driven
(stage 2, "own train"); a coupling or a split is not repeated, so a
train whose car count changed stays where it was (RTS drops such a
position too); lights, pantographs and sounds of remote trains;
electrification; the server's pause does not stop the clock.

## Tests

- `openrts.net`: fields and strings both ways, nested packages, broken
  lengths, package names, a snapshot with every modelled package written,
  read and written again byte for byte, a stray position; the packages of
  play; a connection to a fake server on the loopback (the login, its reply
  without a length, an assignment, the snapshot, the hang-up).
- `openrts.net_world`: on a synthetic switch, distances along the track,
  junctions set the server's way, a train's front from its rear end, a
  remote train interpolated, run on, stopped and put across a jump.
- `openrts.content.net_multdata` (with `OPENRTS_TEST_ROOT`): `RTS/multdata`
  reads without a problem and writes back byte for byte. It is skipped when
  the file is not there. The file stays in the game folder: it holds the
  players' names.
