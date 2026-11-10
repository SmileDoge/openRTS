# The openRTS server

`openrts-server` is openRTS's own game server (roadmap step 6, stage 3).
This is its first mode, **Legacy on**: it speaks the RTS protocol, so the
original `rts1.exe` and openRTS clients log in to it as they log in to
`rts_server.exe`. How RTS's server behaves: `reverse/multiplayer.md`,
`reverse/multiplayer-protocol.md`. The client side of the same protocol:
`net.md`.

Not yet:
- the openRTS protocol (Legacy off) and its settings for everyone;
- helpers (stage 4);
- trains the server drives itself (TRAFFIC, step 7);
- `rts_server_DSP.exe` consoles: they would get the player's login reply,
  not the `0x4501`.. they expect (not read yet).

## Running it

```
openrts-server --route Kuban_RTS --root D:\Games\MDDRTS --port 800 --season summer --weather clear --time 10:00
```

| Flag | |
|---|---|
| `--route <folder>` | the route, a folder under `ROUTES` (case ignored) |
| `--root <folder>` | the RTS installation (the current folder by default) |
| `--port <n>` | 800 by default, as `rts_server.exe` |
| `--season`, `--weather` | `spring`..`winter` or 0..3; `clear`, `snow`, `rain` or 0..2 |
| `--time HH:MM` | the time of day at the start (12:00) |
| `--no-admit` | take no new players |
| `--command "<line>"` | a console line run at the start (repeatable) |
| `--run <s>` | stop after so many seconds (tests) |

It loads the route in about a second (Kuban: 1653 track nodes, 1951 signal
heads, 0.9 s) and then listens. Players connect with the start menu's
"Сетевая игра" or `openrts_viewer --mp-connect host[:port]`, or with
`rts1.exe`. Arguments and console lines are read as UTF-16 on Windows, so
Russian path names and messages work.

## The dispatcher's console

| Command | |
|---|---|
| `status` | route, time, pause, players, trains |
| `users` | players: id, connected or away, playing or in the menu, trains, the last place |
| `trains` | trains: id, number, kind, owner, cars, where, speed |
| `paths [text]`, `consists [text]` | what trains can be made from |
| `add "<consist>" <path> [number]` | a train standing at the path's start (STATIC); refused where another stands ("occupied by train N") |
| `place "<consist>" <node> <offset> <forward\|back> [number]` | a train (STATIC) with the front of its first car at a place of the track, facing along the node or against it |
| `assign <train> <nick>` | the player drives it (`0x4601`; the others get `0x4605` PLAYER) |
| `unassign <train>` | taken back: it stands as STATIC |
| `remove <train>` | deleted (`0x4602` 1); its owner is told first |
| `say <nick\|*> <text>` | a message (`0x4611`) |
| `pause`, `resume` | `0x4616`; nothing goes out while paused, as RTS |
| `time [HH:MM[:SS]]` | shown or set (`0x460D` to everyone) |
| `route <train> <signal> [signal...]` | a train's route, as the console's assembly: from the train's front (or its rear, running rear first) to the signal named so in the route database (its "литера"), through the others in turn, the shortest way; of several signals with one name the nearest along the way. Every junction on it is set, facing and trailing, and held: players cannot throw it until the train's rear has passed it (and 30 m). The signals open by their scripts. A TDB item number works too |
| `route <train> cancel` | the route's junctions freed (they stay as they are) |
| `signals [text]` | the signals' names and how many stand under each |
| `junction <node> [branch] [lock\|unlock]` | thrown (refused while occupied or held by a route) or locked against players |
| `signal <item> <aspect\|auto>` | the heads of a TDB signal item held at an aspect, or back to their scripts |
| `kick <nick>`, `admit on\|off`, `quit` | |

## The dispatcher's console (in the game) — hidden for now

**Hidden until the server is done** (the user's call, 2026-11-09): the
server comes first, Legacy and as `rts_server.exe`, with its own console;
remote consoles (`0x4500`: `rts_server_DSP.exe`, this one) after it. The
start menu no longer offers "Войти как: Диспетчер", and the server refuses
`0x4500` with `0x4702` (`ServerOptions::consoles`, off). The code stays and
the tests run it; what is below is how it works.

openRTS itself is the console (the user's choice, 2026-11-09):
`openrts_viewer --mp-connect host --mp-nick <nick> --mp-console`.
- It logs in with `0x4500` and gets the world at once, no train needed.
- Track control is its map, open at the start. A junction thrown there or
  a signal's aspect chosen ("Показание …" in the signal's menu, or "by the
  script") goes to the server as a command. "Put a consist here" (dragged
  and turned as in single player) puts nothing on the console's own track:
  the consist goes to the server, which places it nobody's (STATIC) to be
  given to a player. A train's menu has "Jump to train", "Cancel the route"
  and "Remove train" (on the server); who drives it is the console
  window's. Picking a train on the map picks it in the window.
- **A train's route from the map, as RTS's console sets it (Legacy):**
  pick the train, right-click the track, "Route for the selected train to
  here". The console finds the way from the train's head (or its rear,
  when the place is behind it) on its copy of the network, sends each
  junction to throw as `0x460A`, then the route's length as `0x4619`
  (train, metres, the flag 1 when laid from the rear), as RTS's console
  does (live capture 2026-11-05). The server lays that length from the
  head along the junctions as they stand, holds them, and sends the
  distance left (`0x4619`) to everyone, players too, every 0.5 s while it
  changes; `-1` when the train has run through it, or when the console
  cancels it (`0x4619` -1). The map draws the route green from the train's
  head. RTS's other way, the station routes assembled from the route
  database (`0x450B`), is not there yet.
- The window "Пульт ДСП":
  - the time, the pause, setting the time;
  - the trains (number, kind, driver, speed, route): picking one centres
    the map on it; give it to a player, take it back, remove it, set or
    cancel its route to a signal (литера);
  - a new train: a consist (filtered) and a path, put on the track;
  - the players (dispatcher, away, in the menu, playing; their trains):
    messages to one or everyone, dropping a player, taking new players or
    not;
  - the log: the commands sent and what the server answered; any server
    command typed.
- The packages are openRTS's own (no RTS program has them):
  - `0x45F0` console → server: a command line, as typed at the server
    (all but `quit`);
  - `0x45F1` the answer, what the server printed;
  - `0x45F3` console → server: a consist to put on the track, by its
    cars (each car's folder and file, engine or wagon, flipped, cold, the
    handbrake), its MaxVelocity, an optional number, and where: a path's
    start or the front of its first car at a node, an offset and a way.
    The consist is the dispatcher's own file; the server needs only the
    cars' files and makes them its train. RTS's console does the same in
    its own packages (live capture 2026-11-05): `0x4607` with the cars'
    records, the server's id back in `0x4507`, then `0x4603` / `0x4606` to
    place it; the server will take that sequence too when
    `rts_server_DSP.exe` is let in;
  - `0x45F2` server → consoles, every second: the time, the pause,
    whether new players are taken, the users (id, nick, connected, playing,
    console, the camera's place, their trains) and the trains (id, number,
    kind, subtype, state, owner, the junctions their route holds, speed,
    standing, cars, the first car).
- A player's junction thrown with the key (or in the cab's Track control)
  in a network game now goes to the server as `0x460A`, as `rts1.exe`
  sends it; the server's answer is what stands.

## What it does

**Login** (`rts_server.exe` `0x4436C8`):
- the first package fixes the kind: `0x4600` is a player, `0x4500` a
  console (refused with `0x4702` for now), anything else `0x4704`;
- protocol 101, else `0x4705`; a nickname of 1..15 characters;
- the same nickname again takes the user over: the old connection is
  closed, its trains stay theirs (as the RTS server's reconnect);
- the reply: `0x4600`, 101, "Маршрут: <RouteID>\r\nСезон: <season>", the
  season and weather, the RouteID in lower case, and the four counts
  `rts1.exe` checks against its own route (TDB nodes, track items,
  substations, electrosections: `reverse/multiplayer.md`, "Login"). Kuban
  gives 1653, 4074, 3, 3, as `rts_server.exe` did;
- then a `0x4601` for each train the user owns: the menu waits for one.

**The startup data** (`0x4608`): only for a user with a train or one who
played before, else `0x4703` (a code alone, as RTS). The snapshot, in the
live server's order (`net::write_snapshot`):
- the root, the time;
- every junction (index = TDB node − 1, the branch, the lock);
- every signal head (its TDB item, the aspect, the draw state, mode 1 or 0
  when the dispatcher holds it, the speed);
- the trains: `0x4607`, `0x4603`, the name, `0x4606`, cargo, derailment,
  `0x4601` for the user's own;
- the other players' places (`0x462D`, 32-byte names), the pause;
- `0x4608`.

No electrification goes out: the clients run their own route's.

**In play**, by `rts_server.exe`'s rules:
- `0x4603` of a PLAYER train from any sender (RTS checks no owner) is kept
  and relayed to everyone else; `0x4606` too, with the train's place;
- `0x4617`, `0x4639`, `0x463A`, `0x463C`, `0x463D` (lights, sounds, looks)
  go to players within 3 km of the train. A player's place is its camera's
  (`0x462D`), not its train's. openRTS: for 15 s after a player asked the
  owner for the train's look (`0x4623` mode 2, forwarded to the owner), the
  answer reaches them wherever their camera is: a client asks when it loads
  the train, its camera may still be kilometres off then (where it
  started), and the answer was lost;
- `0x461B`, `0x461D`, `0x463F`, `0x4640` are relayed; `0x461C`, `0x4636`
  are relayed and kept; `0x4641` makes a STATIC train LOOSE;
- `0x462D` is kept and relayed with the user's id in front and the name
  after, 84 bytes of it (the live capture);
- dropped, as from RTS players: `0x4602`, `0x4605`..`0x4607`, `0x4613`,
  `0x461A`, the electrification, `0x4649` (no dispatchers yet).
- Flooding (over 5 000 packages or 2 500 sound packages in 5 s): `0x4706`
  and the user is dropped.
- When a player leaves, their moving trains are announced as standing.

**Routes.** Station routes of the route database (the console's
"Сборка маршрута поезда", `0x450B`) are not read yet: `route` runs to
named signals instead, which is what a station route joins. Both it and
the console's hand route send the distance left (`0x4619`) as
rts_server.exe does; RTS clients have no case for it, openRTS's consoles
draw it.

**Junctions and signals** are the server's:
- A player's `0x460A` / `0x4642` / `0x4609` is carried out on the
  server's network and sent to everyone. A locked junction or one a train
  stands on is refused, and the sender gets its state back.
- The signals run their scripts on the server (`simulation::SignalSystem`),
  every 0.05 s, with every train on the server's network. A head that
  changes goes out as `0x460C`.
- The players' trains run the heads ahead of both their ends, as RTS's
  player trains.

**The server's trains.** Each train has a copy on the server's network
(`simulation::Train`): its cars from the consist's files, placed where the
last `0x4603` says and moved on at that speed for up to 5 s, as `rts1.exe`
extrapolates. They occupy the track for the signals and the junctions, and
make couplings and cuts exact:
- `0x460F` (A took B in): B's owner, if another player, first gets
  `0x4601` "not yours"; then the others get `0x460F`, B's owner `0x4610`
  (B → A) and everyone the merged train's `0x4603`, as the live capture
  showed. A second coupling naming a train gone does nothing;
- `0x460E` (a cut): the part gets the lowest free id, keeps the kind, owner,
  number and state; the sender gets `0x4610` (its local id → the server's),
  the others `0x460E` with the new id and the part's `0x4603`.
- Standing trains grow cold as on RTS's server: `0x4605` "Включен" 480 s
  after they stopped, "Холодный" 1 200 s after.

**Time** runs at 1×; `0x460D` goes out every 300 s (as `rts_server.exe`)
and when the dispatcher sets it.

## The code

| | |
|---|---|
| `server/src/server_route.cpp` | the route: network, signals with their scripts and world placements, paths, consists made into cars and records, the counts |
| `server/src/game_server.cpp` | users, login, snapshot, the relays, junctions, signals, trains, couplings, the dispatcher's actions; no sockets |
| `server/src/tcp_server.cpp` | TCP: an accepting thread, a reader and a sender per connection; RTS's limits (a login of 64 bytes, packages of `0x3C00`) |
| `server/src/console.cpp` | the commands |
| `server/src/main.cpp` | `openrts-server` |

## Checked

- `tests/server_test.cpp`:
  - on a synthetic track: the refusals, the reply, the snapshot, the
    relays, junctions and locks, a coupling and a cut with the live
    capture's order of packages, pause and time, a reconnect, a takeover,
    flooding;
  - over TCP: openRTS's own client (`net::RtsConnection`) logs in, gets
    the train the dispatcher gives it, and the world;
  - with an installation (`openrts.content.server_kuban`): on the 2025
    build's Kuban (TDB 1653 nodes) its counts 1653 / 4074 / 3 / 3 and
    1951 heads as the live server's; on another (the 2026-11 build redrew
    its track: 1647 / 4241 / 3 / 3, 1953 heads) the counts as its TDB writes
    them and a snapshot with every junction and head; an ED9M at a
    path's start, a signal held at stop and a junction thrown from the
    console reaching the player; a route from the ED9M to the signal "1"
    (4 472 m, 11 junctions held) whose junction a player cannot throw.
- By hand, 2026-11-09: two openRTS clients on `openrts-server` with Kuban.
  Each got an ED9M at a path's start, the world (555 junctions, 1951 heads), the
  other's train in its place, its lights, sounds and look.
- Not yet checked: `rts1.exe` on it. The login, the counts, the `0x4601`
  layout (13 bytes, in the menu too) and the snapshot follow the live
  capture byte for byte.
