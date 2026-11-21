# The openRTS server

`openrts-server` is openRTS's own game server (roadmap step 6, stage 3).
This is its first mode, **Legacy on**: it speaks the RTS protocol, so the
original `rts1.exe` and openRTS clients log in to it as they log in to
`rts_server.exe`. How RTS's server behaves: `reverse/multiplayer.md`,
`reverse/multiplayer-protocol.md`. The client side of the same protocol:
`net.md`.

Not yet:
- the openRTS protocol (Legacy off) and its settings for everyone;
- trains the server drives itself (TRAFFIC, step 7);
- `rts_server_DSP.exe` consoles: they would get the player's login reply,
  not the `0x4501`.. they expect (not read yet).

## Running it

To update openRTS on the server's computer without the game:
`openrts-updater.exe` beside it (a window, no graphics card needed), or
`openrts-updater.exe --yes` from a script; then start the server again
and put the placement `autosave-previous` back (INSTALL_EN.md,
"Without the game").

```
openrts-server --route Kuban_RTS --root D:\Games\MDDRTS --port 800 --season summer --weather clear --time 10:00
```

| Flag | |
|---|---|
| `--route <folder>` | the route, a folder under `ROUTES` (case ignored) |
| `--root <folder>` | the RTS installation (the current folder by default) |
| `--port <n>` | 800 by default, as `rts_server.exe` |
| `--season`, `--weather` | `spring`..`winter` or 0..3; `clear`, `snow`, `rain` or 0..2, `storm` or 3: openRTS's thunderstorm (rain to RTS players, docs/weather.md) |
| `--time HH:MM` | the time of day at the start (12:00) |
| `--no-admit` | take no new players |
| `--max-players <n>` | places: a new player beyond so many connected is refused (`0x4702`, as with `--no-admit`); one coming back under his nick and the dispatcher's consoles are not counted against it; 0 (the default): no limit |
| `--autosave <minutes>` | every train on the track saved as the placement `autosave` so often and at the stop (2 by default, 0 never); at the start the last run's kept as `autosave-previous` (Placements) |
| `--discord-join`, `--public-address <host:port>` | openRTS players' Discord profiles get "Join" with that address (none given: the one each player connected to); net.md, "Joining from Discord" |
| `--dispatcher-password <text>` | the dispatcher's console from another computer logs in with it (none: only from this one; "The dispatcher's console (in the game)" below) |
| `--data <folder>` | where the server keeps its files (the route database it saves); openRTS's user data folder by default (`%LOCALAPPDATA%\openRTS`, or next to the program with a `portable.txt`), never the RTS installation |
| `--command "<line>"` | a console line run at the start (repeatable) |
| `--run <s>` | stop after so many seconds (tests); runs at the console |
| `--no-window` | run at the console, without the window (`--route` needed) |
| `--capture <file.ppm>`, `--capture-at <s>` | the window's picture `s` seconds after the server started (3 by default), then it closes (a check) |

The start menu starts it too ("Сетевая игра", "Свой сервер": "Открыть
окно запуска сервера") with the RTS folder, its window (the port is set
there) and its log (`openrts-server.log` beside the settings) of its own;
"Подключиться к своему серверу" then joins 127.0.0.1 at the port its
window was last started with (`server_window.ini`), with the nick and the
role given in the menu.

It loads the route in about a second (Kuban: 1653 track nodes, 1951 signal
heads, 0.9 s) and then listens. Players connect with the start menu's
"Сетевая игра" or `openrts_viewer --mp-connect host[:port]`, or with
`rts1.exe`. Arguments and console lines are read as UTF-16 on Windows, so
Russian path names and messages work.

## The window

Without `--no-window` (or `--run`) the server opens its window, as
`rts_server.exe` does. The game steps on a thread of its own, every 20 ms
as without the window; the window reads and changes it under a lock, its
drawing outside it (2026-11-21: until then the game stepped once a frame,
and a computer without a video card, drawing the window in software, sent
the players' world seconds apart). On such a computer `--no-window` is
lighter still.

- **The start form:** the RTS folder (with "Обновить" to list its
  routes again), the route, season, weather, time, port (0: any free
  one), whether new players are taken, the places ("Мест в игре", 16 at
  first, 0 no limit), the dispatcher password (with it
  the dispatcher's console may join from another computer), "Вход через
  Discord" with the address for the players (net.md, "Joining from
  Discord"), and the server's folder. The arguments fill it in. Started without `--route`, it
  is as it was last started (`server_window.ini` in the server's folder,
  the password too), but for the folder, port and password the command
  line gives.
- **Then the console on the route's map** (the same map as the viewer's
  Track control, `trackmap/`): the track, junctions, signals with their
  names, platforms, sidings, stations, the trains in the colour of their
  kind (player's, traffic, standing) with their numbers, owners and speed
  and an arrow ahead of the head the way each faces, each train's route and the station routes' segments (waiting grey, set
  green, the train on it dark green). The wheel zooms, a right drag moves
  the map, the first train placed brings the map to it. A right click
  opens what can be done there:
  - a train: give it to a player or take it back, cancel its route,
    remove it;
  - a junction: a branch, lock against players or unlock;
  - a signal (its unit's report on top): set the aspect, open,
    automatic block, close, back to its default;
  - the track: "Поставить поезд" (the consist picked in "Поезда" first,
    then any found by part of its name), a route for the selected train
    to here (from its head, or its rear when the place is behind it).
- **Placing a train** is seen first: the consist stands on the map
  outlined, its first car orange with a big arrow the way it faces, red
  where it does not fit or runs into a train; a bar over the map says
  which and how long. Drag it along the track, click another track to
  move it there, Left / Right nudge it 1 m (Shift: 10 m); "Поставить
  (Enter)" puts it (the server still refuses it on another train),
  "Развернуть (Пробел)" turns it, "Отмена (Esc)" drops it.
- **The tabs:**
  - "Поезда": the trains (number, id, kind, driver, cars, speed; a click
    selects one and shows it), the selected one's actions (a train gone
    is unselected), a new train: a consist (found by part of its name,
    Russian or Latin; a click on the picked one, or "Снять выбор",
    unpicks it) shown at a path's start to be put from there.
  - "Станционные маршруты", as `rts_server.exe`'s "Собрать маршрут": the
    train (picked here, in "Поезда" or on the map), its queue with each
    segment's state and cancel, and the routes it can take next from the
    end of its last segment, else from its head (a blue mark with an
    arrow on the map); "Направление" turns that way (it reverses there, or
    starts rear first). Main tracks first in green, onto a wrong track last
    in lilac, "[П]" and the length of a station track it goes onto, how far
    its start is. The one picked is drawn on the map (thick, with an arrow
    at its end and its name; the way to it thin) and the map shows all of
    it; the one under the mouse in orange. "Задать" (or a double click)
    queues it with "держать следующий" and the priority, and the list goes
    on from its end: to the next station and further. "Все маршруты
    станции" lists any of a station's routes; at the bottom every train's
    queue.
  - "Сигналы": every signal of a type to manual / open / automatic block,
    the type shown as a train signal, restore the defaults, set as the
    defaults, save the route database (into the server's folder).
  - "Игроки": nick, state, trains, address; a message to the player or to
    everyone, drop the player.
  - "Журнал": the server's log and a line for the console's commands
    (`help`), the same as at the console.
- **The bar on top:** the route and port, the server's time and setting
  it, pause, taking new players, how many play (of how many places), where the dispatcher may
  join from ("только с этого компьютера" or "откуда угодно, по паролю"),
  stopping the server.
  Under it what was last done, for 5 s (an error 10 s).

The log also goes to the console behind the window. Not in the window yet:
starting and stopping a train on its own (the server drives no train
before step 7, TRAFFIC).

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
| `weather [clear\|snow\|rain\|storm\|dynamic [seed]]` | the weather while the game runs, to openRTS players and consoles (`0x45FA`, docs/weather.md); also the window's top bar ("Динамическая погода", "Другая": another seed) |
| `number <train> <number>` | its train number (`0x4605` to everyone, as `rts_server.exe` sends it when the console changes it: RTS numbers its cars by it); not one another train has |
| `move <train> <node> <offset> <0\|1>`, `chocks <train> none\|head\|tail\|both` | a standing train put again: elsewhere, or with chocks under its ends (Placements) |
| `name <train> [text]`, `notes <train> [text]` | its name (the console's map shows it after the number) and its notes, one line here (the console's train form takes several); `0x461A` to everyone and in the snapshot; no text: cleared |
| `say <nick\|*> <text>` | a message (`0x4611`) |
| `pause`, `resume` | `0x4616`; nothing goes out while paused, as RTS |
| `time [HH:MM[:SS]]` | shown or set (`0x460D` to everyone) |
| `route <train> <signal> [signal...]` | a train's route, as the console's assembly: from the train's front (or its rear, running rear first) to the signal named so in the route database (its "литера"), through the others in turn, the shortest way; of several signals with one name the nearest along the way. Every junction on it is set, facing and trailing, and held: players cannot throw it until the train's rear has passed it (and 30 m). Its manual signals facing the train open ("Signals" below). A TDB item number works too |
| `route <train> cancel` | the route's junctions freed (they stay as they are), the signals it opened closed |
| `heads` | every signal head as the players get it: TDB item, aspect, draw state, mode (to compare with `openrts-net-dump --heads`) |
| `signals [text]` | the signals' names and how many stand under each |
| `stations [text]` | the stations of the route database's station routes, with how many routes each |
| `routes <station> [text]` | a station's routes: ID, name, from, to, flags, whether its way is laid |
| `next <train> [back]` | the station routes the train can take next, as "Собрать маршрут": from its last segment's end, else its head (back: the other way); ID, station, name, "[П]" and the track's length, main track, onto a wrong track, how far; with `back` or `rear` when they need it |
| `segment <train> <route ID> [back] [hold] [rear] [priority N]` | a segment of the train's route, the station route by its ID (back: from its end; hold: the train's next segment waits; rear: the train runs it rear first): checked and queued, set when its turn comes ("Station routes" below) |
| `segment cancel <id>` | cancelled: the train's route goes back to its previous segment's end |
| `segments` | the queue: id, train, station route, state, junctions set |
| `junction <node> [branch] [lock\|unlock]` | thrown (refused while occupied or held by a route) or locked against players |
| `signal <item\|name>` | the signal's unit: its mode, aspect, whether the dispatcher picked it, its defaults, its heads' types; by name when only one signal has it |
| `signal <item\|name> <aspect>` | held at that aspect (manual, the dispatcher's: a route leaves it so), as the console's "set aspect" |
| `signal <item\|name> open\|auto\|close\|default` | open (its script runs until a train passes it), automatic block, closed (manual at STOP_AND_PROCEED, as unticking the console's "Открыт" / "Автоблокировка"), back to its defaults |
| `signal type <type> <manual\|open\|auto>` | every signal with a head of that sigcfg type, as the console's switch by type |
| `signal defaults` | every signal back to its defaults from the route database |
| `signal set-defaults` | every signal's defaults become its mode and aspect now (the console's "Задать как умолчания") |
| `signal type <type> normal on\|off` | the type shown as normal (its heads count for their signal's aspect; the console's "Отображать как поездной") |
| `signal save` | the defaults and the types shown as normal into the server's copy of the route database (`<data>/server/<route>/<file>.rtsdb`, the one before kept as `.bak`): the console's "Сохранить всё" for the signals; the server reads that copy first from then on |
| `couple <train> <train>` | the second joins the first where they stand end to end (the nearest ends within 50 m), as `rts_server.exe` couples a LOOSE train run into another: `0x460F` to everyone, the first's owner too |
| `cut <train> <car>` | cut before car `car` (from 0): the cars from it on are a train of their own, kind and owner as the train's; `0x460E` to everyone, the owner too |
| `helper <nick> <train\|off>` | the player is the helper (помощник машиниста) on the train's cab, or no more (openRTS players: "Helpers" below) |
| `kick <nick>`, `admit on\|off`, `quit` | |

## The dispatcher's console (in the game)

The remote dispatcher (from 2026-11-21; hidden from 2026-11-09 until the
server was done, the user's call): the start menu's "Сетевая игра", "Войти
как: Диспетчер (пульт)", with the server's dispatcher password.

openRTS itself is the console (the user's choice, 2026-11-09):
`openrts_viewer --mp-connect host --mp-nick <nick> --mp-console
[--mp-password <text>]` (the start menu passes the password in
`OPENRTS_MP_PASSWORD`, off the command line and the log).
- **Who is let in** (`ServerOptions::consoles`, on; `dispatcher_password`):
  from the server's own computer (127.0.0.1) any nick, no password; from
  another one only with the dispatcher password set at the server (its
  window's start form or `--dispatcher-password`): a wrong or missing one
  gets `0x4707` (openRTS's own code), a server without a password `0x4702`.
  Until 2026-11-21 a nick given with `--dispatcher` let a console in from
  anywhere without a password; such nicks now only narrow who may (when
  given, the nick must be one of them as well). The password goes in
  plain text, as the whole protocol does.
- It logs in with `0x4500` (openRTS's own: the password, `u16` length and
  UTF-16 units with a zero, after the nick's zero; such a login may be 256
  bytes, a player's 64) and gets the world at once, no train needed.
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
  head. RTS's other way, the station routes of the route database, is the
  window's "Станционные маршруты" below.
- The window "Пульт ДСП":
  - the time, the pause, setting the time;
  - the trains (number, kind, driver, speed, route): picking one centres
    the map on it; give it to a player, take it back, remove it, set or
    cancel its route to a signal (литера);
  - "Станционные маршруты", as the server window's: the picked train, its
    queue with each segment's state and cancel, the routes it can take next
    (asked from the server every 3 s and when its queue changes: from the
    end of its last segment, else its head; "Направление" turns that way),
    main tracks first in green, onto a wrong track last in lilac, "[П]" and
    the track's length, how far; "держать следующий", the priority, "Задать"
    or a double click queues it (the `segment` line); every train's queue.
    The picked route is not drawn on the map yet;
  - "Сигналы": every signal of a type (the route's types, read here) to
    manual / open / automatic block, shown as a train signal or not, back
    to the defaults, set as the defaults, the route database saved (on the
    server, into its folder): the `signal` lines;
  - the train picked: its number ("Задать номер"), its name and notes
    (several lines; "Сохранить название и заметки"), as RTS's console's
    train form: `0x461A` to the server, which sends them to everyone; the
    map shows "№ name" on the train and the notes when it is picked. A
    form being edited keeps what is typed; another dispatcher's change
    comes in where it is not (2026-11-21, the testers);
  - a new train: a consist (filtered) and a path, put on the track, with
    its number (empty: the lowest free), name, kind (STATIC, PLAYER,
    LOOSE, TRAFFIC) and state (Холодный, Включен, На ходу), STATIC and
    Холодный by default (2026-11-21, the organiser); the same on the map's
    "Put a consist here", and in the server window's "Новый поезд" and its
    map. `0x45F3` carries the name, kind and state after the cars (an older
    console's place has none: STATIC, cold). A TRAFFIC train stands: the
    server drives none yet;
  - a train's menu on the map (the console's and the server window's):
    "Номер и название", changed in place;
  - Track control in the console: the camera does not follow the trains
    put on the track (with none of its own it followed the first one, so
    each train put took the dispatcher to it), no car view (F3); "К себе"
    centres the map on the camera, Ctrl+T teleports to the cursor (the
    menu's "Телепорт сюда" stays);
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
    cars' files and makes them its train. A .con from anywhere on the
    dispatcher's computer too (the map's "Browse..."): read there, by its
    path (2026-11-21: until then "consist not found" for those, the game's
    folders only, the testers' freight consists among them). RTS's console does the same in
    its own packages (live capture 2026-11-05): `0x4607` with the cars'
    records, the server's id back in `0x4507`, then `0x4603` / `0x4606` to
    place it; the server will take that sequence too when
    `rts_server_DSP.exe` is let in;
  - `0x45F2` server → consoles, every second: the time, the pause,
    whether new players are taken, the users (id, nick, connected, playing,
    console, the camera's place, their trains) and the trains (id, number,
    kind, subtype, state, owner, the junctions their route holds, speed,
    standing, cars, the first car); since 2026-11-21 then the station
    routes' segments (id, train, the route's ID, station and name, from its
    end / holds the next / rear first, priority, state, junctions thrown of
    how many), which a reader without them stops before;
  - `0x45F8` console → server: what a train can take next (`u32` train,
    `u8` 1 the other way); `0x45F9` the answer (`net::ConsoleNext`): the
    train, the way, whether it is on the server's track, rear first, the
    segment it follows (id, name), then the routes (ID, from its end, main
    track, onto a wrong track, a platform, the track's length, how far,
    station, name), as `GameServer::next_routes` ranks them.
- A hand switch a player throws on foot (`track.md`) goes to the server
  as `0x4642`, as `rts1.exe` sends it; the server's answer is what
  stands. A player has no other way to throw a junction in a network
  game, as in RTS.

## Placements

Every train on the server's track, saved under a name and put back
(2026-11-21, the organiser): its cars (file, flipped, cold, handbrake,
chocks), the front of its first car (TDB node, offset, the way it faces),
its number, name, notes, kind (STATIC, PLAYER, TRAFFIC, LOOSE; a PLAYER
train comes back nobody's) and state. The file is the one the single
player and the dispatcher's console write too (docs/placements.md, "The
file"), in `<data>/server/<route>/placements/<name>.placement` on the
server and `placements/<route>/` in the user data folder of the
dispatcher's openRTS (the single player's placements of the route among
them: one a player sends is put there, or on the server, as it is). A
placement includes others (`include` lines): they are put with it.

- The server: `placement save <name>`, `placement load <name> [add]` (the
  trains on the track removed first, unless `add`; the included
  placements' trains with them), `placement list`, `placement get [name]`
  (its includes and every train, the included ones marked `from`; no
  name: the trains now), `placement include|exclude <name> <other>`,
  `placement delete <name>`, `placement clear` (every train removed),
  `placement upload <name> <part> <parts> <text>` (a console's file in
  parts: a console command's package holds 0x3C00 bytes; written whole when
  the last comes). The window's "Поезда" tab, "Расстановки": save every
  train, put (the trains now removed), add, delete; "Открыть: вложения,
  поезда" lists its includes (taken out, another added) and its trains
  with theirs (number, name, kind and state, cars and chocks, which
  placement), the picked put or saved as another placement (one train's
  out of a station's).
- The dispatcher's console, "Расстановки": save every train on the server
  or here; the server's placements listed, shown or kept here (as one of
  its own: the included trains in it, no includes); the ones kept here
  shown (with their includes' trains), saved on the server (uploaded, the
  trains on its track left as they are) or deleted; a placement shown
  lists its includes (taken out, another added: on the server by
  `placement include|exclude`) and its trains (number, name, kind and
  state, cars and chocks, which placement; the notes on hover), the ones
  picked put on the track (`console_place` each: its number, name, notes,
  kind, state, chocks, the place), every train on the track removed first
  if ticked, or saved as another placement here or on the server.

**A standing train put again** (2026-11-21): `move <train> <node>
<offset> <0|1>` (its front there, facing along the node with 1) and
`chocks <train> none|head|tail|both`; the window's train menu and the
dispatcher's Track control ("Переставить (на карте)": the preview of its
cars dragged and turned, then put; "Башмаки"). The server removes it and
places it anew with its number, names, notes, kind, state and cars (a new
id: RTS's clients see a train go and one come); refused where it does not
fit (then it is put back where it stood) and for a train a player has.
The chocks hold it on the server too (10 kN each, as RTS's PlaceDrag).

**The autosave**: every train on the track saved as the placement
`autosave` every 2 minutes (`--autosave <minutes>`, 0 never) and when the
server stops; none on the track, the last one is kept. At the start the
last run's is kept as `autosave-previous`: after a crash or a restart
"Put" it (or `--command "placement load autosave-previous"`) and the
trains stand as they stood. Written whole, then put in place of the old
file: a crash while saving leaves the last one.

Checked: `openrts.content.server_kuban` saves Kuban's train named and
numbered, clears the track, puts it back (number, name, notes, kind,
state, cars, its place within 0.5 m) and reads the file as a console
does; puts it again with chocks under both ends (in its records, 0x100
and 0x200) and 60 m further (its number, notes and chocks kept), refuses
a place off the track (the train kept); a placement including it (and
not itself) puts it, `placement get` marks it `from`; an upload of 41
trains in several parts written as sent; the autosave and the last run's
kept. `openrts.server` reads the single player's older file, writes and
reads the new fields, resolves includes (a loop, a missing one) and the
upload's lines back to the text.

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
- The signals are run as `rts_server.exe` runs them ("Signals" below), every
  0.05 s, with every train on the server's network. A head that changes
  goes out as `0x460C`.

**Signals** (`simulation::SignalSystem::set_dispatched`; how RTS does it:
`reverse/dispatcher.md`, "Signal modes on the server"):
- The server's signal units are `rts_server.exe`'s signal objects: the TDB
  signal items at one place of a vector node facing one way, in its
  table's order (the route database's `SignalUnit`s are in that order).
- Each has a mode: **manual** (its heads hold what they were given and run
  no script), **open** (they run until a train's leading end reaches the
  signal, then it is manual again: mostly STOP), **automatic block** (they
  always run). A running head sees `enabled` = the mode and the units ahead
  as the junctions stand (renewed when one is thrown), so a block signal
  shows the next one's aspect. Trains' chains and free-running heads, the
  game's way, are not used.
- At the load every unit is manual and runs once with `enabled` 0, then
  the route database (`<RouteID>.rtsdb`) gives its defaults: `UnitFlags`
  bit 2 open, else bit 4 automatic block, else manual, and the unit runs
  once. A manual one is not held at its `DefaultAspect` (`rts_server.exe`
  holds it on the console's selected signal, none at the load): it shows
  what its scripts give with `enabled` 0 — on Kuban mostly
  STOP_AND_PROCEED, red, as the live capture's first snapshot; held at
  STOP its `-also` heads would be dark ("No"). Kuban (2026-11 build): 1405
  units, 503 automatic block, 902 manual; with no train its block signals
  show CLEAR_2 389, APPROACH_1 40, APPROACH_3 14, RESTRICTING 44,
  STOP_AND_PROCEED 16. So the stations' home, exit and shunting signals
  stand red until the dispatcher opens them or sets a route; the line's
  block signals work by themselves. "Restore the defaults" does the same.
  A `SignalUnit` finds its unit as `rts_server.exe` finds it (its own
  index first, then by its first head's place, which for a signal facing
  the other way at a shared place is moved 20 m: `reverse/dispatcher.md`),
  so every one is used and the modes are the file's.
- A route (`route`, the console's `0x4619`) opens its manual signals facing
  the train, but not those whose aspect the dispatcher picked; its end or
  cancellation closes the ones still open.
- `0x460C`'s field `+96` is the head's `enabled`, so it carries the unit's
  mode (0 manual, 1 open, 2 automatic block), as the RTS server sends it.
- **Consoles** (Legacy, `0x4500`; taken when `ServerOptions::consoles` is
  on, as the session has it since 2026-11-21): after the world a console gets every unit's `0x4618` (unit index,
  aspect, mode, the dispatcher's flag), then each unit whose aspect, mode
  or flag changes. A console's `0x4618` sets the unit: mode 1 or 2 that
  mode, mode 0 manual at the aspect given (the dispatcher's when its flag
  is set); a unit the server has not is answered with an error.
- **Types shown as normal** (`SignalTypeShownAsNormal` in the route
  database, `signal type <type> normal on|off`): their heads count for
  their unit's aspect besides those of its lowest function, at the step's
  runs.
- **Defaults**: `signal set-defaults` takes every unit's mode and aspect
  as its defaults; `signal save` writes them into the server's copy of
  the route database: the file as RTS's "save all" writes it (every unit
  in order with its Position, NumSigItems, Name, UnitFlags with bits 2 and
  4 from its default and the others as they were, DefaultAspect; the types
  after the header), the rest of the file (route points, routes, paths) as
  it was. A unit has one name, as RTS writes it: on Kuban 901 names became
  896 (a few units the old database named twice). The installation's file
  is not touched; copying the saved one there gives `rts_server.exe` the
  same defaults.
- The station routes: below.

**Station routes** (`server::StationRoutes`, `GameServer::add_segment`;
how RTS does it: `reverse/dispatcher.md`, "Station routes and segments"):
- At the load the route database's route points are put on the track
  (within 3 m) and each station route's path is laid from its start point
  to its end point through the junctions it lists, in order and at their
  branches, and back; a route onto a station track runs on to its far
  point (by the exit signal ahead, the guide's "точка окончания маршрута").
  Kuban: 182 points (all on the track), 828 routes, 828 paths, every one
  laid both ways.
- What a train can take next (`StationRoutes::available`,
  `GameServer::next_from`): from its last segment's end (else its head,
  and the point it stands at within 300 m back) the way chosen, through
  junctions any way, to the first route point on each way; the routes
  starting there that way. An exit with nothing that way and a point
  nothing starts from are passed, so after a route onto a stretch come the
  next station's (Kuban from Краснодар-1's track 4: Краснодар-2,
  Витаминкомбинат, Титаровка). One-way routes only from their start.
- A train's route is a chain of segments, each a station route (from its
  start, or from its end). A segment is checked when it comes (the train;
  no oncoming train's route waiting at the far end of the stretch its end
  leads onto; a way on from the train's previous segment's end) and queued
  by its priority.
- Each step sets those whose turn it is (the train's previous segment set
  and not holding the next): an end onto a stretch waits for an oncoming
  route there or a train coming this way on it; a point to start from waits
  for another driven train between; then its junctions must be free (none
  locked or held for another train's route, with the checks on no other
  train on the track between them). They are thrown, the train's route is
  laid from its head to the segment's end (held, its manual signals open,
  `0x4619`): state 1. A route with flag 8 may be set part of the way
  (state 2). The train on it: 3; past it: 4, and it goes at the next step.
  A junction of it thrown elsewhere while the train's route holds it sends
  it back to waiting.
- Cancelled: the train's route goes (laid again to its previous segment's
  end when that one is set).
- Consoles get `0x450B` when a segment is queued and in their world,
  `0x450E` with each change of its state, `0x450D` when it goes; a
  console's `0x450B` (checked by its station and its start point's tile)
  and `0x450D` are answered with `0x450C`.

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

## Helpers

- **openRTS players** say `0x45F4` after their login; the server answers.
  Such a player may get the world without a train (an `rts1.exe` without one
  still waits in the menu, `0x4703`): only when he asks (the menu's "Войти
  без поезда", `--mp-no-train`); otherwise he waits for the dispatcher's
  train as in RTS (since 2026-11-21).
- **Asking** (`0x45F5`): a player asks to help on a train. The server asks
  its owner (an openRTS player in play) and tells the helper "waiting"; the
  owner takes him or refuses. Not possible: no such train, nobody drives
  it, he drives it himself, or one side is not openRTS.
- **The dispatcher** makes a player a helper with `helper <nick> <train>`,
  as RTS's dispatcher assigns helpers, without asking; `helper <nick> off`
  ends it. `users` shows who helps where, and who is an openRTS player.
- **The streams:** the owner's `0x45F6` (his cab) goes to his helpers on
  that train, a helper's `0x45F7` (his hand) to the train's owner. Nothing
  else takes them.
- **It ends** when the helper or the engineer leaves, the train is given to
  another, taken back or removed, or either side says so; both are told.
- `docs/net.md`, "Players and helpers", has the client's side.

## The code

| | |
|---|---|
| `server/src/server_route.cpp` | the route: network, signals with their scripts and world placements, paths, consists made into cars and records, the counts |
| `server/src/game_server.cpp` | users, login, snapshot, the relays, junctions, signals, trains, couplings, the dispatcher's actions; no sockets |
| `server/src/tcp_server.cpp` | TCP: an accepting thread, a reader and a sender per connection; RTS's limits (a login of 64 bytes, packages of `0x3C00`) |
| `server/src/console.cpp` | the commands |
| `server/src/session.cpp` | a running server: the route loaded, the port open, the game, console lines; the program and the window run one |
| `server/src/server_window.cpp` | the window: the start form and the console on the map (built when ImGui, GLFW and WebGPU are; `wgpu_native.dll` is loaded only when the window opens) |
| `trackmap/src/track_map.cpp` | the route's map, shared with the viewer's Track control: track, junctions, signals, places, stations, trains, lines and marks of its owner, picking, tooltips |
| `ui/src/text.cpp`, `ui/src/fonts.cpp` | the Russian texts (`tr`) and the fonts, shared with the viewer |
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
    (4 472 m, 11 junctions held) whose junction a player cannot throw;
    the signal units (2026-11 build: 1 405, 503 automatic block, 902
    manual, as its route database), the route opening its 5 manual
    signals facing the train (APPROACH_2, CLEAR_2, APPROACH_1 and two
    shunting ones at STOP_AND_PROCEED; `0x460C` with mode 1) and closing
    them when cancelled, a block signal held at STOP (`0x460C` mode 0),
    open / automatic block / close / defaults on a manual signal, a whole
    type under automatic block and every signal back to its defaults;
    a console getting every unit's `0x4618` after its world and its own
    `0x4618` holding a unit at STOP (sent back) and putting it under
    automatic block; a type shown as normal, "set as defaults", the save
    into a temporary data folder and the route loaded again from it (the
    same modes, the type, 896 of 901 names).
  - `tests/server_test.cpp` (`route_database`): a route database read,
    written with other units and types and read back, the rest of the
    file as it was.
  - Kuban's station routes (182 points, 828 routes and paths laid both
    ways), a segment from the ED9M's track set (552 "НМ4БП => Н1": 10 of 10
    junctions at their branches, the train's route holding them, its
    signals open, the console's `0x450B` and `0x450E`), the next one from
    its end queued, one elsewhere refused ("no way"), a console's `0x450B`
    answered and one of another database refused, the cancels (the train's
    route and signals gone).
  - At the load no head dark and most of Kuban's manual `-also` heads at
    STOP_AND_PROCEED (red); the console's `heads` against
    `openrts-net-dump --heads` of the live capture's first snapshot: all
    1951 heads the same in aspect, draw state and mode on the 2025 build's
    Kuban after 40 s; every SignalUnit of both builds on its unit, the
    modes as the files give them.
  - Kuban's "Собрать маршрут": from the ED9M's head the three routes from
    its track 4БП (552, 555, 558) past half a track nothing starts from,
    then on from the end of a route onto the stretch, station after
    station (Краснодар-2, Витаминкомбинат, Титаровка); `next` at the
    console from the train and from its segment's end.
  - `tests/server_test.cpp` (`station_segments`): on the switch track a
    station route through the junction set (thrown, held), the train on it
    (3) and past it (4, then gone), the console told each; an unknown route
    or train refused. (`route_database`: route points, routes and paths read
    and kept by the save.)
  - `tests/server_test.cpp` (`session`, with an installation): a session
    refusing a route that is not there; Kuban's started on a free port,
    a console line placing the ED9M, its cars on the map, segment 552's
    route leg for the map, no route to the place its head stands on,
    `quit`.
  - The window by hand and by its picture (`--capture`): the start form
    (filled in from the arguments, as last started without them), Kuban's
    console with the ED9M placed and its segment set.
  - `tests/signal_test.cpp` (`test_dispatched`): units on a synthetic
    track, `enabled` = the mode, automatic block reading the unit ahead,
    an aspect the type lacks, a train closing the open signal it reaches.
- By hand, 2026-11-09: two openRTS clients on `openrts-server` with Kuban.
  Each got an ED9M at a path's start, the world (555 junctions, 1951 heads), the
  other's train in its place, its lights, sounds and look.
- Not yet checked: `rts1.exe` on it. The login, the counts, the `0x4601`
  layout (13 bytes, in the menu too) and the snapshot follow the live
  capture byte for byte.
