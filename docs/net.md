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
openrts_viewer --window --mp-connect 127.0.0.1:800 --mp-nick openRTS --root <game folder>
```

Or from the start menu, the "Network game" tab: the server's address and
the nick, then "Connect". `--spawn cab` (the tab's "Start at: In the cab")
seats the player at the desk when the dispatcher assigns the train;
without it the camera follows the train from above.

- **The route is the server's.** The login reply names it by its `.trk`'s
  RouteID in lower case, as `rts_server.exe` does: the folder of that name
  (its own spelling taken from the installation), else the folder whose
  RouteID it is (since 2026-11-23: a route folder "Suojarvi V2" has the
  RouteID `suojarvi`, and was not found), with its
  season and weather. `--route` is not needed; one that names another
  route is overridden, `--season` / `--weather` win over the server's. A
  route the installation does not have stops the game before the wait.
- **The start menu follows the join.** The game writes one line
  `status: <word> [detail]` to its console (`openrts.log` when the menu
  runs it) at each step, and the menu shows it:

  | Line | Step |
  |---|---|
  | `connecting <address>` | the connection and the login |
  | `waiting <route>` | logged in, waiting for a train from the dispatcher |
  | `loading` | a train assigned: the world, then the route |
  | `playing` | the window opens |
  | `unreachable` | no connection |
  | `refused nick` / `full` / `version` / `<code>` | the server refused the login (`0x4701` / `0x4702` / `0x4705` / another) |
  | `closed`, `no-world`, `timeout` | the server closed, would not send the world (`0x4703`), no train in 30 minutes |
  | `no-route <route>` | the server's route is not installed |

  A failed join ends the game with code 9 (`no-route`: 2, a content
  problem). While the game connects or waits, the menu's "Cancel" ends
  it; the server sees the connection close.

**The connection** (`openrts/net/connection.hpp`, `RtsConnection`):
- TCP to "host:port" (port 800 when none is given); the login `0x4600` with
  the protocol version 101 and the nick (1 to 15 characters).
- The reply has no length field: `0x4600`, the version, the description,
  season and weather, the route id and four counts; or a code alone
  (`0x4701` nick in use, `0x4702` no new players, `0x4705` another
  version).
- Then a thread reads whole packages into a queue the main loop takes.
  When the socket closes it queues a `server_down` of its own, as
  `rts1.exe` does. The game then shows "Сервер отключился" in the middle
  of the screen and goes back to the menu in 15 s (exit code 10, the menu
  says "Сервер остановлен: сетевая игра окончена"), at once with its
  button, or stays to look round the world.
- Closing the window closes the connection first, while the GPU is still
  up: the reader thread waits in `recv`, and a graphics layer that hooks it
  (ReShade) is unloaded with the device. Returning into it crashed the
  viewer on closing until 2026-11-06.

**Joining:**
1. The viewer logs in, opens the server's route and says on the console
   that it waits for a train.
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
anyway, as `rts1.exe` does. The player's place is the camera's, not the
train's. The server relays a train's lights, look and sounds only to
players within 3 km of it, and the dispatcher's map shows the person; its
players table's "Отклик" (seconds since the server last heard the player)
no longer grows. A remote train the camera comes back to, within 2.9 km
after being beyond 3 km, is asked for its whole look again
(`state_request` 2, as `rts1.exe` when it loads the train again): the
lights and looks it changed meanwhile never came.

**Own trains** (stage 2, step 2): the trains the dispatcher assigned are
simulated here, one of them driven with its DLLs, cab and keyboard, and the
others see them:
- **Several trains can be ours.** The first in the snapshot is placed as the
  driven train (`TrainView::place_driven`), the other ones after it as
  trains standing here (`TrainView::add_local`), then the remote trains. On
  foot, another own train is got into as the driven one: at an exterior door
  of its cab the same prompt shows, and Enter makes it the driven train
  (`TrainView::board`), then opens the door or climbs the steps as at any
  cab of his; the Train panel lists them with "Drive" as a reserve. Own
  trains placed while the player's DLLs run join their host, as a part cut
  off does: a DLL is bound when the player first gets in and goes on after
  he leaves. Track control is not there in a network game: the server and
  its dispatchers own the track; nor are the Train panel's paths, consists
  and situations. Each own train sends its position, look and sound by
  itself. Its DLLs start in the snapshot's state: "Холодный" 0 (cold, the
  handbrake on), "Включен" 1, "Движется" running, as `rts1.exe` (`0x5026FC`
  reads the train byte `+2`). The server changes the state by itself: a
  train left "Движется" comes back running.
- **Its position** (`train_pos`, from the rear end as the snapshot gives
  it, `net::position_of`): at once when it starts, every 1.5 s while it
  moves, and a last one with `train_stands` when it stops, also once when
  it is placed. A train that stands sends speed 0 exactly: the server keeps
  the last speed, and the ТЧД console holds a PLAYER train with any other
  as "Движется", its state and player locked (`reverse/dispatcher.md`).
  The server keeps the position: the next login puts the train where it
  stopped.
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
- `player_pos` carries the driven train's id (0 on foot) in its second
  field, as `rts1.exe`; the first stays 0; `train_pos` for
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
7 and were silent before. A client that loads a train asks its owner for
the whole sound too: `sound_state` (`0x463A`, a `0x463B` per stream) gives
each stream's current trigger, and a loop that already runs (fans, a
compressor) starts on it (`audio::SoundInstance::restore`; an inactive sound
keeps it for when it wakes, as RTS `0x4A8C00`). openRTS answers with its own
trains' streams the same way.

**Couplings and cuts** (`0x460F`, `0x460E`, `0x4610`):
- **Another client's:** the records follow (`net::coupled_cars`,
  `net::cut_cars`) and the trains come back with their next positions, the
  car count matching. Ends: 0 the rear, 1 the front; A's rear to B's front
  makes A's cars then B's in order (both live couplings); side 0 of a cut:
  the part is the cars from the cut on (the live cut).
- **Ours:** the driven train takes another client's or the server's train
  in as RTS does (its own train always survives; two remote trains never
  meet, `simulation::resolve_contacts`'s passive trains) and sends
  `0x460F`; a cut sends `0x460E` with a local id from 100 000, and the part
  waits, sending nothing, until `0x4610` names it (rts1.exe's flag `0x800`).
- **Taken away** (`train_assign` "not ours", also before a coupling into
  another player's train): the train stays here as another client's, its
  DLLs bound but not run (the DLL host skips trains of kind 2 and 4, as
  `rts1.exe` `0x424488`), and it goes on where it stopped when given back
  (`TrainView::give_away` / `take_back`). Own trains placed before the
  player had a DLL host join it when he gets one.
- **A DLL stays with its car**, as RTS binds it once per car (`0x5026FC`):
  every car of a network record has a key of openRTS's own
  (`net::TrainCar::key`, never on the wire) that couplings and cuts carry
  along. A car of ours that goes into another client's train leaves the
  DLL host, which keeps it parked by its key with its DLL's state; when it
  comes back (that train cut and the part given to us, or the whole train)
  its DLL goes on as it left it, without a new `Init`.

**The rest of the server's world:**
- **Pause** (`0x4616`): time and trains stand; nothing is sent, as RTS.
- **Speed posts** by the train's category from the server ("Прочий" obeys
  every post, `docs/paths.md`); in single player by the cars as before.
- **The line:** the snapshot's and play's `0x4646` (a substation off, its
  voltage), `0x4648` (phases), `0x4647` (a section's source: its own, one,
  "отключено") and `0x4645` (a piece's own supply) go into
  `simulation::Electrification`; our electric trains' draw goes to the
  dispatchers every 2 s window as `0x4649` (substation, phase, energy,
  seconds; 14 bytes as `rts1.exe` sends them).

**Checked against a live server** (2026-11-06, the ChS4z "1337" driven in
openRTS, an ED9M in `rts1.exe`): the ChS4z moves, stops, raises its
pantographs and lights up the same in both; the ED9M's horn, whistle,
traction, brake shoes and couplers sound in openRTS.

**Checked offline** with a fake server replaying the saved snapshot: a
train taken away and frozen, another client's coupling it in (9 cars) and
cutting it off again (a train of its own), and the part given back to us:
its car came back with its DLL.

**Checked against `openrts-server`** (2026-11-15, it keeps the rules of
`rts_server.exe` that matter here: a position with another car count is
dropped, the speed alone kept):
- **Our coupling with another client's or the server's train was never
  sent until 2026-11-15.** The remote train taken in leaves the track here,
  and `move_trains`, which ran first, forgot its id; `send_couplings` then
  could not name it. The server and its consoles (the ТЧД map) kept the two
  trains and dropped every position of ours with the cars taken in: the
  train stood where it had coupled, its speed going on in the console's
  list (testers: a steam engine "standing at its start" while it ran far
  away; "coupled at ours, not on the console"; giving the cars to the
  player first worked, our two trains coupling being named from our own
  list). Now the couplings go first in the frame. Checked: the VL80s-606
  coupled to 11 standing coaches, the server's train 13 cars, no position
  dropped; a cut (part 3 named by `0x4610`).
- **The server's own coupling into our train** (a LOOSE train run into
  ours, `rts_server.exe` `0x40DCDC` → `0x4501E0`: `0x460F` to everyone,
  the owner too) and **a cut of our train by someone else** (`0x460E`
  naming it) were left; the same car count trouble. Now our train takes the
  cars in (`TrainView::join_trains`, not sent back) or is cut the same way
  (`TrainView::cut_train`), the part ours under the server's id. Checked
  with the server's `couple` and `cut`.
- A position the server drops for its car count is logged by
  `openrts-server` once per train (`train N: a position with M cars …:
  dropped`); `rts_server.exe` says nothing.

**Derailments** (2026-11-07, as `rts1.exe` in the live capture):
- **Sent.** When a car of an own train comes off, and with each `0x4603`
  while the train still moves, `0x4636` goes to the server: a `u8` per car,
  then a `0x4638` per derailed car (`net::encode_derail`, the capture's
  layout byte for byte; the meanings PROBABLE): its heading, its place in
  the tile, its roll now and at rest, m·g, a state 0 as it comes off and 4
  once it rolls. Every client lets the cars fall itself from it; the server
  marks them.
- **The train stays one.** RTS damages the coupler between an overturned
  car and a neighbour that stays up (`0x581694`, flags 16/32); openRTS cut
  the train there until 2026-11-07, and its coupler-force check cut it
  between derailed cars too (an ED9M fell apart in three on a curve, three
  trains on the console). Now neither cuts.
- **Back on the rails** only by the dispatcher, as RTS: "Удалить из мира"
  and placing it again. C does nothing in a network game (it put the
  driven train at the camera, which the others never saw).
- **Not yet:** another client's `0x4636` is read and left: its cars show on
  the rails.

**Not yet:** a piece of track's `0x4645` `u8` is read and left (PROBABLE:
kept for the server's list); the console's dispatcher packages (stage 3 and
5).

## Players and helpers (openRTS's own, 2026-11-15)

Between openRTS's server and openRTS players; `rts1.exe` and
`rts_server.exe` never see these packages.

- **Hello** (`0x45F4 openrts_hello`, `u16` features, 1: players and
  helpers; then, since 2026-11-16, `u8` 1 when the game is a thunderstorm,
  the server's answer: its login said rain, docs/weather.md; a reader of
  the features alone stops before it). The client sends it right after its login. `rts_server.exe`
  leaves an id it does not know to its main thread
  (`reverse/multiplayer.md`, "Routing"). openRTS's server answers it, and
  only then does the client send the packages below. An openRTS player on
  an openRTS server does not wait in the menu for a train: it asks for the
  world at once and looks round with the free camera. The dispatcher can
  give it a train any time, and another player can take it as his helper.
- **Players.** The other players' places come as RTS relays them
  (`0x462D`: the sender's id, its place, its name). openRTS showed nothing
  of them until now. Now it shows:
  - their names over the world where their cameras are, with the distance
    (the panel's "Имена игроков над миром", on by default);
  - the panel's "Игроки": each player, his train or "on foot or the free
    camera", how far.
- **Helpers** (помощник машиниста; RTS: `0x461F` and a peer link,
  `reverse/multiplayer.md`, "Helpers"). Here everything goes over the
  normal connection, and the server forwards it by train, so nobody needs
  an open port:
  - `0x45F5 helper` (`net::HelperNote`: kind, train, user, value, nick). A
    player asks to help on a train ("Попроситься помощником" next to a
    driving player in the panel). The engineer gets a window ("X просится к
    вам помощником": "Принять" / "Отказать"). The server tells both sides
    the outcome: waiting, helping, refused, not possible, ended. The
    engineer can let a helper go, and the helper can stop.
  - `0x45F6 helper_cab`, engineer → server → his helpers. It carries the
    cab of the car the helper is in, as the engineer's DLLs show it: the
    DLL host's `CarOutput`, zlib-packed (about 370 bytes for a ВЛ80Т cab
    of 150 switches, 200 displays and 2 screens). The cab triggers posted
    since the last stream go with it; the exterior sounds, lights and
    doors do not, since the train's own packages carry those to everyone.
    It goes ten times a second while the cab changes and every second
    anyway.
  - `0x45F7 helper_input`, helper → server → engineer. "I am in car N"
    every second; a switch put at a state (CanSwitch / Switched, as the
    engineer's own hand); a drag of a lever (delta in states, let go).
- **The helper's side** (`TrainView::help`). The engineer's train, another
  client's, becomes the first train here; the helper's own driven train
  stands meanwhile, its DLLs waiting in the host, and is driven again when
  the help ends.
  - He stands by the chair of the engineer's cab and walks the train as in
    his own.
  - The cab draws the streamed state: switches, needles, lamps, screens,
    texture changes and the cab lamps' light. Its cab sound plays with the
    engineer's DLL variables and triggers.
  - His clicks, drags, wheel and cab keys run through the same switch
    logic as on his own train (`LegacyCab::remote`), and only the
    resulting switch states and drags go to the engineer. A switch he
    moved shows at once; the engineer's answer comes with the next stream.
- **The engineer's side.** A helper's hand is done by his DLLs as his own
  would be (`TrainView::helper_hand`). The helper's train stays his: it
  moves, brakes and sounds by his packages, as any other client's.
- **Not yet:** helpers with `rts1.exe` on either side (the relay of RTS's
  peer link, `roadmap.md`); the helper's ears hear the train's exterior
  sounds as another train's, through the cab.
- **Checked** (2026-11-15, one PC): `openrts-server` on TestRoute_RTS, a
  ВЛ80Т given to EngA, HelpB walked in without a train and asked to help;
  EngA took him. HelpB sat in the cab, which came in 373 bytes. HelpB's
  hand put switch 34 at 1: EngA's DLL took it ("done"), and the next
  stream showed it at 1 at HelpB's. When HelpB left, EngA was told.
  Test hooks for such runs: `OPENRTS_MP_HELP=<nick>` asks him,
  `OPENRTS_MP_TAKE_HELPERS` takes every helper, `OPENRTS_MP_HELP_SWITCH=
  <id>:<state>` works that switch once the cab is here.

## Tests

- `openrts.net`: fields and strings both ways, nested packages, broken
  lengths, package names, a snapshot with every modelled package written,
  read and written again byte for byte, a stray position; the packages of
  play, the own train's (a sound trigger as `rts1.exe` sent it, read and
  written back); a connection to a fake server on the loopback (the login, its reply
  without a length, an assignment, the snapshot, the hang-up); openRTS's
  own hello and helper packages both ways, a relayed `0x462D` with its id
  and name.
- `openrts.net_world`: on a synthetic switch, distances along the track,
  junctions set the server's way, a train's front from its rear end, a
  remote train interpolated, run on, stopped and put across a jump.
- `openrts.content.net_multdata` (with `OPENRTS_TEST_ROOT`): `RTS/multdata`
  reads without a problem and writes back byte for byte. It is skipped when
  the file is not there. The file stays in the game folder: it holds the
  players' names.
