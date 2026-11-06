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
simulated (`TrainView::add_remote`) and moved along the track
(`Train::shift`, which turns the wheels too):
- **Drawn behind:** one package interval (measured: 1.5 s for players,
  0.5 s for TRAFFIC) and a margin of 0.25 s for a package that comes late.
  Positions RTS sends out of turn (stopping, starting: 0.06 s after the
  last) do not count for the interval.
- **Between two positions** the train follows a curve through the speeds
  the packages give (Hermite), not a straight line: the speed runs on from
  one package to the next instead of jumping at each. Replayed from the
  live capture of 2026-11-05 at 60 Hz, the largest acceleration drawn fell
  from 27-62 m/s² (the straight line's speed steps) to 0.5-1.2 m/s².
- **The drawing clock** catches up a delay that changed at most 10 %
  faster or slower than real time, instead of jumping.
- Past the last position the train goes on at its speed for one more
  interval and stops where `train_stands` says. A position the track does
  not join to the last one puts the train there when it is due.

**Checked against a live server** (2026-11-06: `rts_server.exe` with
Kuban, one `rts1.exe` driving an ED9M, the observer as openRTS):
- the login, the wait for a train, the snapshot over the network (it
  writes back byte for byte and fits Kuban whole), a reconnect under the
  same nick (the server sends the assignment again by itself);
- junctions and signals as the route assembly and its cancelling set them:
  the exit signal white on a route given by hand, green on one from the
  assembler, as in RTS;
- the ED9M moving, smooth since the curve and the clock.

**The look of remote trains** (as RTS draws another player's train: its
own light conditions and pantograph rules are not worked out, `0x4F07D4`):
- when a train comes on the track the observer asks its owner for its
  whole look, `state_request` 2 (as `rts1.exe`, `0x4D0228`);
- `car_light` switches a light of the car's `Lights`, by its index;
- `car_state` gives the raised pantographs and, for a diesel, whether its
  engine runs (`TrainView::set_remote_light` / `set_remote_look`).

**The observer's place:** it sends the camera's tile and position as
`player_pos` four times a second while the camera moves and every 5 s
anyway, as `rts1.exe` does. The server relays a train's lights, look and
sounds only to players within 3 km of it, and the dispatcher's map shows
the person; its players table's "Отклик" (seconds since the server last
heard the player) no longer grows.

**Own trains** (stage 2, step 2): the trains the dispatcher assigned are
simulated here, one of them driven with its DLLs, cab and keyboard, and the
others see them:
- **Several trains can be ours.** The first in the snapshot is placed as the
  driven train (`TrainView::place_driven`), the other ones after it as
  trains standing here (`TrainView::add_local`), then the remote trains. On
  foot, Enter at an exterior cab door of another own train takes it
  (`TrainView::board`); the Train panel lists them with "Drive" as a
  reserve. Own trains placed while the player's DLLs run join their host, as
  a part cut off does: a DLL is bound when the player first gets in and goes
  on after he leaves. Track control is not there in a network game: the
  server and its dispatchers own the track. Each own train sends its
  position, look and sound by itself. Its DLLs start in the snapshot's
  state: "Холодный" 0 (cold, the handbrake on), "Включен" 1, "Движется"
  running, as `rts1.exe` (`0x5026FC` reads the train byte `+2`). The server
  changes the state by itself: a train left "Движется" comes back running.
- **Its position** (`train_pos`, from the rear end as the snapshot gives
  it, `net::position_of`): at once when it starts, every 1.5 s while it
  moves, and a last one with `train_stands` when it stops. The server keeps
  it: the next login puts the train where it stopped.
- **Its look:** a light's `car_light` when it switches; `car_state` (the
  raised pantographs, "running") at most every 250 ms when it changed. A
  client that loads the train asks with `state_request` 2; the answer is
  the lit lights (every light starts off there; `rts1.exe` sends all of
  them, 275 a car on an ED9M) and every car's `car_state`.
- **Its sound:** every discrete trigger of the exterior `.sms`
  (`sound_trigger`, slot 3), except the group's
  `NonDistributableTriggers`; Var1..Var8 and the distance travelled
  (`sound_values`) at most every 200 ms when they changed. The volume is
  sent as 1: it is the DLL's `SMSObject` volume (`+308`), which the DLL host
  does not read yet.
- `player_pos` carries the driven train's id (0 on foot); `train_pos` for
  an own train is ignored (`rts1.exe` keeps its own trains' position),
  `train_pos_forced` puts it there.
- **Taken off the track** (`train_remove` 0, the console's "Удалить из
  мира"): the train leaves the track here and stays ours; if the player
  drove it, he is let out where he was (`TrainView::drop`: no train, the free
  camera). The server puts it back with its position and `train_stands`
  (`rts1.exe` takes `0x4603` for a train out of play, then `0x4606` puts it
  in play, `0x4C3DE0`); the player takes it again when he has no train.
  RTS crashed its owner here (`reverse/multiplayer-protocol.md`).
- **Deleted** (`train_remove` 1): gone, the player let out as above.
- **Assigned during play** (`train_assign`): a train given to us becomes
  ours where it stands (driven when the player has no train, else
  standing); one taken away is drawn as another client's from where it
  stood.
- A train taken away leaves its sounds stopped (`TrainView` stops a removed
  train's voices at the next sound update; before, its loops played on).

**Remote sound:** `sound_trigger` and `sound_values` for slot 3 play the
car's exterior sound with the owner's triggers and variables
(`TrainView::remote_sound_trigger` / `set_remote_sound`). Its cars get the
group `rts1.exe` gives another player's train in a network game
(`docs/sound.md`, "Groups by owner"): the ED9M's head cars have only group
7 and were silent before.

**Checked against a live server** (2026-11-06, the ChS4z "1337" driven in
openRTS, an ED9M in `rts1.exe`): the ChS4z moves, stops, raises its
pantographs and lights up the same in both; the ED9M's horn, whistle,
traction, brake shoes and couplers sound in openRTS.

**Not yet:** a coupling or a split is not repeated, so a train whose car
count changed stays where it was (RTS drops such a position too); the whole
sound state (`sound_state`): a client that loads the train hears a loop that
already runs only from its next trigger; an own train put on the track while
the player has no DLL host runs its DLLs only once driven; substation loads;
electrification; the server's pause does not stop the clock.

## Tests

- `openrts.net`: fields and strings both ways, nested packages, broken
  lengths, package names, a snapshot with every modelled package written,
  read and written again byte for byte, a stray position; the packages of
  play, the own train's (a sound trigger as `rts1.exe` sent it, read and
  written back); a connection to a fake server on the loopback (the login, its reply
  without a length, an assignment, the snapshot, the hang-up).
- `openrts.net_world`: on a synthetic switch, distances along the track,
  junctions set the server's way, a train's front from its rear end, a
  remote train interpolated, run on, stopped and put across a jump.
- `openrts.content.net_multdata` (with `OPENRTS_TEST_ROOT`): `RTS/multdata`
  reads without a problem and writes back byte for byte. It is skipped when
  the file is not there. The file stays in the game folder: it holds the
  players' names.
