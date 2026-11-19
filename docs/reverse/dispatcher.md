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

- The network side (logins `0x4500` / `0x4600`, the server's threads,
  rights and routing, the mini-server in `rts1.exe`) is in
  `multiplayer.md`.
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

## The train form ("Редактирование поезда", `rts_server.exe`) — CONFIRMED (static, 2026-11-06)

`TfTrains::btEditClick` (`0x44019C`) fills `TfTrainEdit` from the train
(`0x4140A0`), shows it, and applies it on OK (`0x41448C`). The form's
published fields: `cbType` `+0x30C`, `edName` `+0x310`, `cbPlayer`
`+0x314`, `mmNotes` `+0x328`, `edNumber` `+0x330`, `cbService` `+0x334`,
`cbTrainState` `+0x338`, `panRestr` `+0x33C`, `cbEType` `+0x364`.
What the server's train record decides (kind `+296`, state `+300`, the
player `+328`, the speed `+332` as the owner's `0x4603` gave it, flags
`+316`):
- **State:** the train's; a PLAYER train whose speed is not exactly 0
  shows "Движется" and the box is disabled.
- **Player:** enabled only for a PLAYER train with flag `+316` bit 0 that
  stands (speed 0) or has no player.
- **Type:** disabled for a PLAYER train with a player, and for any train
  whose speed is not 0.

So a client that stops but sends a last position with a few mm/s left
keeps its train "moving" for the console: openRTS did until 2026-11-06
(the snapshot held 0.01 m/s), and the ChS4z's state and player could not
be changed. `rts1.exe` and now openRTS send 0 for a train that stands.

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

## Signal modes on the server (`rts_server.exe`) — CONFIRMED (static, 2026-11-19)

**Signal objects** (`0x402F80`, the table `Src`, `dword_54C5BC` entries of
56 bytes): per vector node in TDB order, per signal item in its
`TrItemRefs` order, an object for the first item of each place (`+8`, the
offset) and direction (`+48`), holding every item of the node there. The
route database's `SignalUnit`s are written in this order, and `0x4618`'s
index is into it. Fields: `+0` its aspect (u16, the strongest of its heads
of its lowest function), `+2` that lowest function, `+4` its mode (f32),
`+8` the distance to its next object, `+17` the dispatcher picked its
aspect (u8), `+20` / `+24` its heads, `+48` its next object, `+52` the one
whose next it is.

| Mode | | Its heads' scripts |
|---|---|---|
| 0 manual | holds what it was given | not run |
| 1 open | the console's "Открыт", a route | run every step (`0x40CBB4`); manual again once a train reaches it (`0x45A1DC`) or its route ends or is cancelled (`0x4098F4`) |
| 2 automatic block | the console's "Автоблокировка" | run every step |

**A run** (`0x40CD40`, also `0x402F80` and `0x40CBB4`): each head's
`enabled` (`+96`) is set to the object's mode, its script runs
(`0x43B9F0`), and `+0` becomes the strongest state of its heads of the
lowest function (`0x40CBB4` also counts a type marked
`SignalTypeShownAsNormal`, flag `0x100` of the type). So a manual object's
script sees `enabled` 0, which most scripts answer with STOP; one that
does not read it (Kuban's `T_Head_G`) gives what it gives.
`next_sig_lr` / `next_sig_mr` walk the objects' `+48` chain.

**The next object** (`0x40D1D8`): the next signal item ahead of the
object's first head, the way it faces, as the junctions stand, within
50 km (`0x42A8D8`); `+8` its distance. A junction thrown (`0x40C52C` →
`0x40CE20`) renews it for the objects behind the junction.

**At the route's load** (`0x402F80`) every object is manual and runs once.
Then the route database's `SignalUnit`s (`0x449C68`, `<route>.rtsdb`) are
matched to the objects (the same index first, else by position) and give
their defaults (`0x41F39C`): `UnitFlags` bit 2 → open, else bit 4 →
automatic block, else manual; the object runs once (`0x40CD40`). A manual
one's `DefaultAspect` goes to `0x40C714`, but that holds an aspect on the
object the console has selected (`dword_54C70C`), not on the one given:
at the load none is selected and nothing is held, so a manual signal shows
what its scripts give with `enabled` 0 — CONFIRMED by the live capture
of 2026-11-05: its first snapshot, 40 s after the start (12:01), has no
head dark (−1) and 932 at STOP_AND_PROCEED (Kuban's red), though every
`DefaultAspect` is STOP; openRTS on the same route matches 1949 of its
1951 heads. (Kuban's `-also` heads map STOP to the draw state "No", no
light: held at STOP they would be dark.) `DefaultAspect` is read through
`0x426AC0` (STOP 0 … CLEAR_2 7, 8, 9; anything else 0). Bit 1
only marks the signal red on the console's map in one of its modes
(`byte_54C85C`); no logic of it found. An object without a `SignalUnit`
keeps what its run at the load gave. The console's "restore defaults"
(`TfEditor_btSigRestoreDefClick`) and "set as defaults" (`0x4499F0`,
written into the `.rtsdb` by "save all") work on the same table.

**Holding an aspect** (`0x40C714`): the object made manual and run once;
`+0` = the aspect, `+17` = the flag given; the first head of the lowest
function whose type has the aspect shows it (its draw state and speed),
the other heads go dark (draw state −1); when none has it, every head
takes the aspect with the type's draw state for it, else for STOP, else
for STOP_AND_PROCEED. The console's menu:
- "Установить показание" (`TfPult_mnSigSetStopClick`): aspects 0..7, the
  flag 1;
- "Открыт" / "Автоблокировка" (`TfPult_mnSigOpenClick`,
  `TfPult_mnSigAutoClick` → `0x40C648`): toggles; ticked, mode 1 or 2;
  unticked, manual at aspect 1 (STOP_AND_PROCEED), the flag 0 — the live
  capture's "close gives 1";
- by type (`TfPult_btSigTurnOnClick` → `0x40CAF0`): every object with a
  head of that type to the mode chosen, and the type's "shown as normal"
  flag.

**Routes.** A train's route (`0x40918C`, from `0x4619` and the station
routes) opens every object along it whose items face the train, that is
manual and whose `+17` is clear (mode 1). Cancelling (`0x4619` with a
length below 0) or ending it (`0x4098F4`) closes the open ones along what
is left. A train closes an open object as its first car reaches it
(`0x45A1DC`, from `0x40DCDC`: the objects within the first car's
`+188` + half its length ahead of it).

**`0x4618`** from a console (`0x40FB90`): the object index, `+0` (u16),
`+4` (f32), `+17` (u8); a mode 0 holds that aspect (`0x40C714`), a mode
above 0 runs from the next step. The server sends it back to its consoles
(`0x451F3C`) with every change of `+0` or the mode; the players get each
head's `0x460C` (`0x451D5C`), whose `+96` is `enabled`, that is the mode.

**The installed routes' `.rtsdb`** (`UnitFlags` counts): Kuban 503 × 4
(automatic block), 846 × 0, 56 × 1, `DefaultAspect` STOP for all but one;
Pavlovsk 496 × 4, 1610 × 0, 92 × 1, 5 × 5; Chernyshevsk 377 × 4, 3122 × 0,
97 × 1; Novokuznetsk 222 × 4, 3136 × 0, 147 × 1; Stepy Priazovya 184 × 4,
1705 × 0, 108 × 1; DZHD 6 × 4, 103 × 0, 8 × 1; Lviv 1661 × 0 with
DefaultAspects STOP_AND_PROCEED, RESTRICTING, CLEAR_2, STOP and APPROACH_1.
In the old install only: Kievskij hod 1112 × 4, 1947 × 0, 177 × 1, 72 × 5;
Uspensk 214 × 4, 709 × 0, 134 × 2 (open), 48 × 5. The new build ships no
`.rtsdb` for these two.

**So on an RTS server:** the line's block signals work by themselves,
clear over free blocks whatever the station's signals show; the station's
home, exit and shunting signals stand at STOP until the dispatcher opens
them (and close again as the train reaches them) or sets a route. The
server throws no junction by itself: players throw hand switches
(`track.md`), the dispatcher assembles routes. Single player has no
dispatcher (PROBABLE, not read in `rts1.exe` for this: it reads the
`.rtsdb` for the signals' names, `0x4903DC`): every signal runs its script
there, so an exit signal clears by itself when the way ahead is free.

**openRTS's server** does this since 2026-11-19 (`server.md`, "Signals";
`simulation::SignalSystem::set_dispatched`), the consoles' `0x4618`,
`SignalTypeShownAsNormal`, "set as defaults" and the signals' part of
"save all" included; the station routes too (below).

**"Save all" for the signals** (`0x448914`, also asked at closing): the
file is written whole, `.bak` keeping the one before: first
`SignalTypeShownAsNormal ( "<type>" )` for every type with flag `0x100`
(the type's name as sigcfg writes it), `JunctionDefaultStates`, the route
points, routes and paths, then `SignalUnits ( n` with a `SignalUnit` per
object in table order: `Position ( tile_x tile_z x y z )` of its first
head (x as the TDB writes it: RTS keeps it negated), `NumSigItems`,
`Name ( "…")` when it has one, `UnitFlags ( %08x )` and
`DefaultAspect ( NAME )` from the defaults table. The installed files
have no `SignalTypeShownAsNormal`.

## Station routes and segments (`rts_server.exe`) — CONFIRMED (static, 2026-11-19)

The route database's other half (`0x449C68` reads it after the signal units):

- **`RoutePoint`** (`0x4640F0`): `Name`, `PointType` (1 a point, 2 a track),
  `Flags`, `StationName`, `Point1 ( tile_x tile_z x y z dir )` placed on the
  track (`0x428790`, "Failed to place point1 in route point"), for a track
  `Point2` too and the distance between them along the track (`+152`, "Failed
  to make way for track"), and an optional `Junctions ( n Point ( … branch ) )`
  within it. Point flags: bit 2 it leads onto a stretch (`0x469804`,
  `0x46A348`), bit 4 no occupancy checks; bit 1 a wrong-way exit and bit 8
  a passenger platform (PROBABLE, below). Kuban: 182 points, 52 points and
  130 tracks.
- **`TrainRoute`** (`0x464674`): `ID`, `Name`, `Flags`, `StationName`,
  `Start` and `End` (point names of the station, `0x449BF8`). Route flags:
  bit 4 no occupancy checks, bit 8 it may be set part of the way; bit 1
  the main track and bit 2 one way (PROBABLE, below). Kuban: 828 routes.
- **`TrainPath`** (`0x464890`): `RouteID`, `Name`, `Priority`, `Start`
  (bit 1 the direction, bit 2 from the start point's Point2) and
  `Points ( n Point ( tile_x tile_z x y z branch ) )`: every junction the
  route passes, in order, with the branch it is set to. `0x4361F4` walks
  them from the start point: each point must be the next junction, the
  branch leads on, the last one to the end point's node ("Failed to load
  route" otherwise). Kuban: 828 paths, one per route.

**The guide** `Поездные маршруты.pdf` (next to `rts_server.exe`, 4 pages)
says how the operator uses them:

- The route form ("Маршруты поездов") per station: points are its tracks
  and its exits (set before the home signals); a track's two points stand
  by its exit signals, one in each direction, "чтобы корректно расположить
  точку окончания маршрута для трафика" — PROBABLE: a route onto a track
  ends at its far point. An exit's properties, in this order: "Неправильный
  путь" (routes onto it last in the list, lilac), "Проверять встречный
  маршрут" (the oncoming route check: bit 2, CONFIRMED by the code),
  "Маневровый"; the forms table adds a passenger platform. A route's, in
  this order: "Главный путь" (first in the list, green), "В одну сторону"
  (only from its start), "Маневровый" (no occupancy checks: bit 4,
  CONFIRMED), "Составной" (part of the way: bit 8, CONFIRMED). The order
  of the two confirmed bits gives the rest: PROBABLE route bit 1 main
  track, 2 one way; point bit 1 wrong way, 8 a platform (Kuban: its
  station tracks with platforms). Kuban's route 552 "НМ4БП => Н1" (flags 3)
  runs straight onto main track 1; most have bit 2.
- Several paths per route: the free one of the smallest priority is set.
- **"Собрать маршрут"** (`TfrmAssembleRoute`, a train's menu): the upper
  list is the train's queue; the routes list shows "все маршруты в
  выбранном направлении, доступные от текущей точки: последнего элемента
  очереди или от самого поезда"; "Направление" turns the search (and the
  train's way on) at that point; a route onto a station track shows "[П]"
  for a platform and the track's length; a priority (the smaller first);
  the lower list is every train's routes at the station. How it finds the
  routes from the point is not read from the code: openRTS walks on from
  the point the way chosen, through junctions any way, to the first route
  point on each way and takes the routes starting there that way (an exit
  with nothing that way, and a point nothing starts from, are passed) —
  so after a route onto a stretch the next station's routes follow.

**A segment** (52 bytes; the server's list `dword_54C630`, linked by
`+44` / `+48`): `+0` the point it starts at, `+4` the one it ends at (swapped
when run from its end), `+8` the route, `+12` run from its end, `+13` hold
(the train's next segment waits), `+14` the train runs it rear first,
`+16` its length, `+20` the train, `+24` the priority, `+28` the state,
`+32` its id, `+36` the path it was set by, `+40` its junctions set.

- **Added** (`0x450B` from a console, `0x454194` reads it: the route by ID,
  refused when its station or its start's `Point1` tile differ; then
  `0x469804`): the train exists; an end point leading onto a stretch: no
  segment queued that ends at the stretch's far end (`0x436510`, "На
  перегон задан маршрут встречному поезду"); a track at the end: the
  operator asked when it is occupied (not from a console); a way from the
  train's previous segment's end to the new start (`0x435278`, "Не найден
  путь от предыдущей точки пути до нового маршрута"). Then queued
  (`0x41F520`): a new id (the largest + 1), after the last segment, moved
  before other trains' segments of a larger priority number; `0x450B` to
  the consoles.
- **Each step** (`0x46A348`), in the list's order:
  - 0 waiting: its turn when the train has no earlier segment or that one
    is set (1 or 3) and does not hold. An end leading onto a stretch waits
    while a route for an oncoming train waits there or a train on the
    stretch comes this way; a point to start from waits while another
    driven train (kind 1 or 2) stands between the train and it. Then the
    path (`0x46A10C`): no junction locked or held for another train, and,
    with the checks on (neither the end point nor the route has bit 4), no
    other train on the track after each junction but the last. Its
    junctions are thrown for the train (`0x40C4AC`) and the train's route
    laid to the segment's end (`0x40918C`, which opens its signals): 1.
    Not possible and route bit 8: as many junctions as are free, the route
    to the last route point reached, state 2, on next steps further.
  - 1 / 3: a junction of the path the train's route holds thrown
    elsewhere: back to 0. The train on it (its junctions behind and ahead
    of the train's front, or one of them within half its first car + 80 m
    ahead): 3; off it after 3: 4.
- **Cancelled** (`0x450D`, `0x469C68`): for a train the server drives, its
  route closed and laid again to its previous segment's end; `0x450D` to
  the consoles, the segment freed.
- **Packages:** `0x450B` (`0x453F80`): id, train, route ID, station
  (`u16 n`, the UTF-16 and a zero), the start point's `Point1` tile (two
  `u16`), `+12`, `+13`, `+14`, length, priority, state, the path (index in
  `RouteTrainPaths` + 1), junctions set. `0x450E` (`0x454600`): id, state,
  path, priority, length, junctions set. `0x450C` (`0x454484`): the
  request's id, the segment's, ok, the reason (UTF-16 to a zero). A
  console's world brings every segment's `0x450B`.
- UNKNOWN: what drops a segment at state 4 on the server (the console
  drops it; no code found that frees it); openRTS drops it at the next
  step with `0x450D`.

openRTS: `server::StationRoutes` (the points on the track, each path's way
both ways through its junctions) and `GameServer::add_segment` and its step
(`server.md`, "Station routes").

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
| F6 | `byte_61D55E` | the train's timetable when it has one: station, arrival, actual, departure, actual, distance (`0x466F70`, columns at 25/40/55/70/85 % of its width; `docs/activities.md`, "The timetable"); else a panel of the stations and stops ahead with distances; openRTS adds the sidings ahead in yellow, a name starting with `%` (a mark for the announcer, САВПЭ: "%3 мост", "%A ...") only with its "marks (%n ...)" box |
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
