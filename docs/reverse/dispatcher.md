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
