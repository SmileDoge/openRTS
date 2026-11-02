# RTS dispatcher console (`RTS/Пульт ДСП/rts_server_DSP.exe`)

Analysed read-only on a copy (`openRTS/build/dsp`, IDA database `dsp.i64`,
forms extracted to `build/dsp/forms/*.dfm.txt`). 2026-11-01.

## The program

- PE32, linked 2021-05-10, **C++Builder** (VCL forms, `PointerArray<RTS…>`
  templates, `RTSmult_*` classes), like `rts1.exe` — CONFIRMED.
- Sockets straight through WS2_32 (`WSASocketA`, `bind`, `listen`,
  `WSAAccept`, `WSAConnect`, `WSARecv`/`WSASend`); the station map is drawn
  with OpenGL from the `.ace` icons next to the exe (`points`, `sig`,
  `platf`, `person`, `cargo`, `brakes`, `arrow`, `circle`, `finish`) —
  CONFIRMED.
- 26 forms (window caption → purpose):

| Form | Caption | What it does |
|---|---|---|
| `TfMenu` | RTS server startup | route, port (800), season, weather, time, "connect to a server", address, nickname |
| `TfMenuDSP` | RTS DSP Connect | `TfMenu` with everything but address, nickname and Start hidden |
| `TfPult` | RTS Dispatcher | the map: pause, junction exits / switching, trains, signals, save/load situation, platforms, station names, time |
| `TfTrains`, `TfTrainEdit` | Поезда | train list (STATIC / PLAYER / TRAFFIC / LOOSE; freight / passenger / other; cold / on / moving), consist, number, player, notes, current type; chocks and handbrakes from the head / tail |
| `TfrmRoutes` | Маршруты поездов | per station: route points (tracks, exits; wrong-way, check opposing route, shunting, passenger platform), routes (main track, one way, priority, composite), route generation |
| `TfrmAssembleRoute` | Сборка маршрута поезда | a train's route as a list of points, direction, priority |
| `TfSigEdit`, `TfSigTurnOn` | Сигналы | aspect (STOP … CLEAR_2), on / off, automatic block, home signal, default state; switching signals of a type on and off |
| `TfSetSpeed` | Установка скорости поезда | speed, max speed, acceleration, deceleration |
| `TfBrakes` | Тормоза | set brake pipe, auxiliary reservoirs, main reservoir pipe, cylinders of a train |
| `TfPlayers`, `TfBan`, `TfTrAssign`, `TfPlSelect`, `TfSendMsg` | Участники | players in game and queued, dispatchers and their queue, ban list, assign a train, assistant, messages |
| `TfrmElectro` | Электроснабжение | sectioning, sources, individual tracks, substations on/off and voltage |
| `TWeather`, `TWeatherEdit` | Погода | weather areas: env file, start / change time, precipitation, fog, ambient colour, temperature, adhesion, motion |
| `TfrmEvents` | События | event log by importance, jump to train |
| cargo forms | Грузы | pickups, cargo tables, leftovers, cargo turnover |
| `TfEditor` | Редактирование | default switch / signal states, edit routes, save into the route |
| `TfrmServerSettings`, `TfLog` | Server settings, Лог | sound, packet limits, server log |

Context menus of the map (`TfPult`): signal (open, automatic block, set
aspect, type, properties), train (jump, head at the rear, add to the world,
place, set / assemble / cancel route, start, stop, reverse, allow coupling,
speed, message, brakes, delete), player (assign train, message, ban,
disconnect, admit, assistant, allow train control), route end point (finish
here, open signals up to here), server (log, reset blocked addresses, data
trace, settings), pickups, retarders.

## Server or client — CONFIRMED

- `WinMain` creates `TfMenuDSP` first (the main form) and never creates
  `TfMenu`. `TfMenuDSP` derives from `TfMenu` and keeps its handlers.
- `TfMenu::btStartClick` (`0x44B570`) only connects: with a nickname it
  calls `0x42BEA0(address, nickname)` → `WSAConnect` (`0x42C20C`). There is
  no server branch in this build; the hidden "connect" box is checked.
- The server is still in the exe: `TfPult::btTurnServOnClick` ("Сервер",
  hint "Включить сервер") starts the server thread `0x420D34` →
  `0x422BB4` (`bind`, `listen`) → accept loop `0x4213B4`. A registered
  window message (`0x44A434`) also toggles it.
- After connecting, `btStartClick` hides five controls of `TfPult`
  (fields +756, +836, +920, +940, +944); +940 is `btTurnServOn` (the hook
  sets it up before calling its click handler) — PROBABLE for the others.
  So the shipped console always runs as a dispatcher client; the server
  button exists but is never shown.

## Protocol and situation files — CONFIRMED (framing), PROBABLE (meanings)

- Chunks: `u16 id, u32 length` where the length counts the whole chunk
  (id and length included); chunks nest. The same ids (`0x4600…0x464F`)
  are used by `rts1.exe`: 37 ids appear in both, more in one or the other.
- "Save situation" (`0x45D3EC`) writes a file in this format: root `0x4600`
  (u16 version 112), `0x4615` world (route name, …), then:
  - `0x460A` junction states (index, selected branch…), only those not at
    their default;
  - `0x460B` signals (count) → `0x4616` per signal object → `0x460C` per
    head;
  - `0x4614` players (32-byte nickname, …);
  - `0x4607` trains → `0x4604` per wagon (name, …), `0x461C` wagon notes,
    `0x4636` the train's route;
  - `0x4634` weather areas; `0x4646` / `0x4647` electrification (sections,
    sources); `0x4645`, `0x464B`, `0x464C`, `0x464E` pickups and global
    objects; `0x464F` cargo history.
- `RTS/multdata` (captured at login) walks with the same framing:
  `460D, 4609 (460A × n), 4646 × 3, 4648, 460B (460C × n), 4607, 4603,
  4606, 4601, 4608` — the server's snapshot of the world sent to a client.

## For openRTS

- A dispatcher in openRTS can be an HTML client of the server
  (`roadmap.md` step 4 / 6). The forms above are its feature list.
- The multiplayer protocol (step 6) can start from `0x45D3EC` (writer) and
  the client parser in `rts1.exe` (`0x4C3DE0`, `0x4CA588`): one format for
  saves, snapshots and updates.

## Test patch: show the server button

`build/dsp/patched/rts_server_DSP_server.exe` (a copy; `RTS/` untouched),
with the console's icons and sounds beside it:
- file offset `0x4ADAA` (VA `0x44B7AA`, end of `TfMenu::btStartClick`):
  `33 D2` (`xor edx, edx`) → `B2 01` (`mov dl, 1`), so `SetVisible` keeps
  `btTurnServOn` visible after connecting;
- the game root comes from the registry (MSTS key, `TfMenu_FormCreate`), so
  the copy can live anywhere.

The server serves the world the console already holds: in this build the
world (route name → `ROUTES\<name>\<name>.tdb`, trains, signals) arrives only
from the server it connected to (`0x454EE4` incoming chunks → `0x45AE7C` →
`0x45B0C0`). A standalone server with a route picker (the old `TfMenu`) was
cut from the start handler; making one needs the world fed in another way
(e.g. a situation file, "Загрузить ситуацию") — not traced yet.

## The server build (`rts_server.exe`, user's copy outside RTS/)

- Linked 2021-05-10, 31 minutes after the shipped `rts_server_DSP.exe`:
  the same source built twice. The server build has no `TfMenuDSP`; its
  main form is the full `TfMenu` ("RTS server startup": route, season,
  weather, time, port 800, optional connect). The test patch above is not
  needed to run a server — CONFIRMED.
- Its folder also holds: `version.txt` (change history 2012–2020, author
  Vladimir Skvortsov "Ted"), `readme.txt` ("test version"),
  `Поездные маршруты.pdf` (train routes), a route database
  `stepanovka.rtsdb` (UTF-16 `SIMISA@@@@@@@@@@JINX1R0t` text),
  `rts_GIDConvert.exe` (train graph logs → CSV), and server logs: a "Full
  data trace" network log (50 MB) and GID logs. The logs hold real players'
  addresses and nicknames: use them for the protocol, keep them out of the
  repository.
- `version.txt` confirms features worth matching: LOOSE consists absorbed on
  coupling, wheel chocks and handbrakes set per car and passed between
  players, drivers throwing hand-operated switches not locked by a
  dispatcher route, a static consist pushed without coupling becoming LOOSE,
  per-player packet limits.

## How the map is drawn: `rts1.exe` vs the console (2026-11-01)

Both draw the **real track geometry from above**, not a schematic — the
console's map is the route editor map of `rts1.exe`, the same code built
into a VCL form. CONFIRMED by the matching function bodies:

| | `rts1.exe` "RTS RE map" | console / server `TfPult` |
|---|---|---|
| Where | own window `RTSREMapWnd`, own thread `0x5ADAF0` and GL context (`0x5ACEE0`, 16-bit colour, no depth); opened only in the IPC route editor mode (`/startIPC`, `0x59BE80` → `0x59D6B4`), not in normal play | an OpenGL panel inside the `TfPult` form; viewport inset 5 px (`0x46476C`) |
| Paint | `0x59D8E4` (WM_PAINT) | `0x4647E4` |
| Projection | `glOrtho(-1, 1, 1, -1)`, zoom `dbl_631F20` (start 512): visible half-size `zoom·128·0.5` m, aspect from the client rect, sizes scaled by `1041 / width` | the same (`dbl_54DC70`, same formulas) |
| Background | `glClearColor(0.9, 0.85, 0.9)` | `glClearColor(0.8, 0.8, 0.8)` |
| Track | every TDB node within the view (72-byte records, world x/z = tile·2048 + offset, culled by distance − node length), black, line width 2 | the same pass, then colour per section: a train route's own colour (ARGB from the route object), alpha 0.2 where it is not set/active; the selected route green; junction branches green when set |
| Icons | textures from `GLOBAL\TEXTURES` (signal, `RTSplatf`, `person`, `RTSpaths`, `RTS_savebtn`, `RTSelectro`, `RTSGlobalObjs`, `bluediamond`) | `.ace` files beside the exe (`points`, `sig`, `platf`, `person`, `cargo`, `brakes`, `arrow`, `circle`, `finish`, `RTSGlobalObjs`) |
| Text | GL bitmap fonts: Arial 16, Times New Roman 14, Book Antiqua 16, Arial 12 bold | the same mechanism (`0x420870`, `0x420974`) |
| Extras | editor layers by a bitmask (block points, paths, platforms with names, electrification, global objects) and an edit selection | operation layers: signals as icons rotated to their heading (`glRotatef`), aspect colour; trains (`0x4782F4`) as rotated rectangles coloured by state (player, traffic, static, loose: magenta, purple, brown, olive, green) with a red outline when selected and small markers (yellow, dark red) for brakes / chocks; players' icons; platform and station names (blue / olive); route end points; the route being assembled (green points, Ctrl toggles); pickups; red "E" marks |

For openRTS the dispatcher is one map with two modes (editor layers,
operation layers) over the same geometry renderer; its look (grey
background, black track, route colours) follows the console.

## In-game "RTS track control" (`rts1.exe`) — CONFIRMED

Opened from the in-game menu (`0x462FC8`, item "Track control"; the user
reaches it with Ctrl+Shift+F7). Module `Rts_trackcontrol`; also used by the
activity viewer ("RTS Activity track control", `0x54FF80`).

- Window `RTSTrackControlWnd` "RTS track control" (`0x549A60`), 500×500 at
  50,50, its own GL context and icons (`0x549650`); background
  `glClearColor(0.8, 1.8, 0.8)` = light green; start zoom 16 (the RE map:
  512). System menu: Show sidings, Show electrosections, cargo layers.
- Paint `0x53F990`: the same map engine as the RE map and the console
  (track nodes in world coordinates, set branches green, signals, platform
  and station names blue / olive, trains, pickups, activity events
  "Event %i:%i", the selected path, coupler forces in N / kN in red, three
  buttons at the top right).
- Mouse (`0x548C8C`, `0x548E40`): right drag pans (cursor clipped to the
  window), a right click opens the context menu (`0x547F24` → `0x5439E4`),
  a left click selects; wheel zooms.
- **Names on the map (CONFIRMED, `0x53F990`; zoom = half the view / 64 m):**
  - station names: a list built when the window opens (`0x5555D8` →
    `0x555624`): every platform item counted under its station name
    (`lstrcmpiW`), the label at the running mean of the items' positions.
    Drawn black in Arial 16 at the mean plus (0.03, 0.03) of the half view
    (right and up), while zoom < 3500 and items × 300 > zoom — the big
    stations stay named farthest out;
  - platform and siding icons while zoom < 250 (flags "show platforms /
    sidings"); their names (Times New Roman 14; blue 0, 0, 0.7, sidings
    olive 0.5, 0.5, 0) only while zoom < 5;
  - fonts made at window creation (`0x549650`): Arial 16, Times New Roman 14,
    Book Antiqua 16, Arial 12 bold.
- Context menus by what is under the cursor:
  - train: Jump to train, Release block point, Show train path, No auto
    remove, Uncouple cars, Trigger board / unboard / tracking event, Edit /
    Cancel / Create route, Save route, Load path…, Save path as…, Remove
    train, Assign to player…, Load for player, Traffic options…, Cast to
    PLAYER / STATIC / TRAFFIC, Start / Cancel shunting;
  - location: Move location, Save location here, Cancel location edit;
  - junction on the edited path: Switch junction (first / n-th / last
    pass);
  - path point: Change start direction, Remove start / reverse / waiting /
    end / block point, Change waiting time, Turn to normal path / static
    placement;
  - track: Place start / waiting / reverse / end point;
  - signal: Signal info, Force signal update;
  - electrification: Switch / Reset power source;
  - cargo loader: Switch state (Off / Ready / Load / Unload), Select
    current cargo, Information.
- The command handler (`0x54478C`) edits train paths with the path module
  (`0x48B8C8`, `0x48BAF8`, `0x48BBF0`, `0x48D220`, `0x48DD94`, `0x48D0A0`,
  `0x48D470`) — the same module step 4 needs for path reservation.

So RTS has three faces of one map: the in-game track control (single
player / host: trains, paths, junctions, signals, power, cargo), the route
editor map (editing layers) and the dispatcher console (multiplayer
operation). openRTS: one map component with these modes.

## The path module and junction reservations (`rts1.exe`) — 2026-11-01

CONFIRMED from the code unless marked.

**Path points** (92 bytes, `0x4886EC` loads a .pat): +0 TrPathNode flags,
+4 next, +8 siding next, +12 TrackPDP index, +16 / +20 next / previous
pointers, +24 a 64-byte copy of the TrackPDP and its traveller: +40 kind
(1 a point on the track, 2 a junction, 5 a block point of a service), +48
the node (the junction for kind 2), +52 the branch, +64 the offset, +84 a
direction byte (for a junction: 1 = entered from a branch, leaves by the
trunk), +88 the distance from the path start.
- For a junction point the branch is the one whose track reaches the next
  point; the loop over branches does not stop at a match, so the last
  matching branch wins.
- `0x48BBF0` re-routes from an edited point to the next user point along
  the junctions' main routes (or their set branches), inserting junction
  points (Track control's path editing); `0x48BAF8` switches a junction on
  the path to its next branch and re-routes.

**Junction record:** +8 the default (MainRoute), +12 the selected branch,
+32 branch count, +36 trunk pin, +40 branch pins (8 bytes: node, end), +52
the reservation queue, +56 a lock byte (a junction with it set is never
reserved through; UNKNOWN who sets it, PROBABLE the dispatcher console).
Queue entries (32 bytes): +0 the owner (the train's path follower), +4 its
path point (whose +52 is the wanted branch), +16 / +20 links, +24 / +28 the
owner's time and distance to the junction, kept by `0x48CA28`.
- `0x48C7B8` appends the owner (queues it).
- `0x48C924` puts the owner first and sets the switch to its branch; the
  previous first owner's reservations from that point on are released
  (`0x48A8AC`).
- `0x48C85C` removes the owner; the switch goes to the new first owner's
  branch or to the default.

**Reserving ahead** (`0x48A0F8`, from the train update): from the train's
current point, junction points one after another:
- free: queue the owner, check opposing moves (`0x48C21C`: another owner
  whose direction byte differs and whose distance is within reach of the
  next junction), set the switch;
- the owner first already: the same checks, set the switch;
- held by others: queue and stop there, unless the owner's time to the
  junction (distance / speed, plus a horizon of 5000 m / speed or the
  service's own) beats the queued owners': then it takes the junction over
  (`0x48C924`), after checking the opposing moves on the stretch back to its
  own reservation (`0x48C50C`, `0x48C654`);
- it stops at a locked junction, at a waiting point not yet due, and at a
  block point whose conditions (`0x48E36C`) fail.

openRTS keeps the queue, takeover and release semantics (`docs/paths.md`)
with a simpler priority (30 s earlier and the holder beyond its stopping
distance) and a section lock for single lines instead of `0x48C21C` /
`0x48C654`.

## Signal names ("литеры") — CONFIRMED (2026-11-01)

The names live in the route database (`.rtsdb`, UTF-16
`SIMISA@@@@@@@@@@JINX1R0t`), one `SignalUnit` per signal object:
`Position ( tile_x tile_z x y z )` (the server finds the signal in the TDB by
it: "Could not find signal in tdb. Location: … Continue?"), `NumSigItems`,
an optional `Name ( "…" )`, `UnitFlags` (hex), `DefaultAspect` (aspect name)
— the writer's and reader's tokens in `rts_server.exe`. The same file holds
`SignalTypeShownAsNormal`, `JunctionDefaultStates ( Junction ( node state ) … )`,
`RoutePoints` (Name, PointType 1/2, Flags, StationName, Point1 / Point2),
`RouteTrainRoutes` (ID, Name, Flags, StationName, Start, End) and
`RouteTrainPaths` (RouteID, Priority, Start, Points). A name is set in
"Редактирование сигнала" (field "Название", `edtSigName`) and shown on the
console map with "Отображать литеры сигналов" (`btSigName`, `mnSigName`).
The user's `stepanovka.rtsdb` has 5 252 signal units and no names; the
installed routes ship `ROUTES/<route>/<RouteID>.rtsdb` with names (Kuban 900
of 1 406 signal units, Kievskij hod 2 480 of 3 308, Chernyshevsk 1 144 of
1 601, Novokuznetsk 1 584 of 3 506, Pavlovsk 1 450 of 2 203, Uspensk 797 of
1 125), e.g. "М14", "Ч2". On the shapes themselves a name exists only as a
NUMBER_PLATE sub-object (a textured plate).

**In the game too** (`rts1.exe`, single player and multiplayer):
`0x4903DC` ("Loading RTS track database") reads `<route path><RouteID>.rtsdb`;
each `SignalUnit` is matched to a TDB signal item by position (tile and
x/y/z within 0.2 m) and its name kept; `0x41B6D0` then creates a label
object (world kind 20) 3 m above each named signal.

## In-game overlay keys (`rts1.exe`, `0x402C88` toggles, `0x408574` draws) — CONFIRMED

| Keys | Flag | What is drawn |
|---|---|---|
| Ctrl+F7 | `byte_5E04BD` | signal names (kind 20 labels) within 150 m of the camera, full colour up to 30 m and fading out to 150 m, colour (1, 0.9, 0.25); the other players (person.ace icons at their trains, `dword_631E58` list) |
| F7 | `byte_5E04BC` | platform names "platform (station)" 10 m above the platform item with a line down, blue (0.1, 0.1, 1); siding names, yellow; car numbers on both sides of every car body (the number the console gives, `0x45C54C`: car +16 written "AAAA-BBBB-CCCC" with bits 18+, 10..17 and 0..9, each zero-padded to 4 digits; drawn 2.64–2.8 m from the body's rear end at half its height, both side walls) |
| Ctrl+Alt+F7 | — | the Track control menu (`0x462FC8`, mode 4) |
| F6 | `byte_61D55E` | a panel of the stations and stops ahead with distances (`0x466F70`, columns at 25/40/55/70/85 % of its width) |
| Ctrl+F5 | `byte_5E04BE` | the speed limit now and the kilometre post (ordinate) at the top, the limits ahead and their distances (`0x468544` / `0x4686F4`) |
| F5 | `byte_5E04B9` | a per-car information page, paged through the train with F5 (Shift: ...) |
| F4 | `byte_5E04BA` | performance and position statistics |

(The user's guide describes the same keys; Ctrl+F7 works in single player
too.)

### Car numbers (F7)

- **Format (CONFIRMED, `0x45C54C`):** car +16 printed as `AAAA-BBBB-CCCC`:
  `n >> 18`, `(n >> 10) & 0xFF`, `n & 0x3FF`, each padded with zeros to four
  digits, `-` between them.
- **Composition (`0x4EF740`, putting a train on the track):** bits 10..17
  from the train's record (+20, its field +52); bits 18 and up from the
  train's id record (+16), or, without one, the next value of a global
  counter (`dword_61D6CC`). PROBABLE: the low bits are the UiD of the car's
  consist entry, set when the consist loads (the ED4M consists number their
  cars 3..14). In single player the middle part is PROBABLY 0.
- **openRTS:** each car keeps its number through uncoupling and coupling,
  as the field stays with the car; the train part counts the trains put on
  the track, the middle part is 0.
