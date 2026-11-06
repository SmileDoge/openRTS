# RTS network protocol: the package catalogue

Every package id the three programs use, with its payload and what the
receiver does. Read from copies, 2026-11-05:
- `rts1.exe` (`build/ida/rts1.i64`);
- `rts_server.exe` (`build/srv/rts_server.i64`);
- `rts_server_DSP.exe` (`build/dsp/dsp.i64`).

`multiplayer.md` holds the rest of the story:
- the transport, the framing and the login;
- the roles of the programs, coupling and helpers;
- how other players' trains look and sound.

CONFIRMED unless marked.

## How it was read

- **Tools** (`build/ida/s5/`, not in the repository):
  - `pkgscan.py` finds every constant `0x4500`..`0x47FF` in a database and
    decompiles the functions that use one: client 289 places in 98
    functions, server 162 in 67, console 160 in 65;
  - `pkgmap.py` lists, per id, the fields each sender writes and each
    receiver reads.
- **Field writers and readers:**
  - client: `0x558050` / `0x5581DC`, package object `0x4C2068`;
  - server: `0x45DBC0` / `0x45DD24`, package object `0x445990`;
  - console: `0x43BEE4` / `0x43C048`, package object `0x423F34`.
- The handlers were then read by hand.
- In the client's `0x45xx` hits there is no package code: the same numbers
  are used elsewhere.

## Package names

Short names for the packages, for talk, notes and the code of
`openrts_net` (the enum names there). RTS has none; these are openRTS's.
A name says what the package is about; the rows below hold the details.

- **Pairs.** `train_pos` and `train_pos_forced` carry the same thing; the
  second is also taken for the receiver's own trains.
- **The same id, two names.** `0x460E` and `0x460F` are `uncouple` and
  `couple` on their own, `train_cars` and `train_car` inside `train_add`.
- **The recovery file** has ids of its own (the saved game's): the same
  numbers mean other things there ("The player's recovery file").

**Session and service**

| Id | Name | What it is |
|---|---|---|
| `0x4600` | `login` | a player's login and the server's reply |
| `0x4500` | `console_login` | a console's login |
| `0x4501` | `console_login_reply` | the reply to a console's login |
| `0x4502` | `console_wait_admin` | "Waiting for admin…" |
| `0x4503` | `console_loading` | "Loading startup data…" |
| `0x4608` | `snapshot` | asks for the world; the server ends the snapshot with it |
| `0x4701` | `nick_busy` | refused: the nickname is in use |
| `0x4702` | `not_admitted` | refused: no new players or dispatchers, or not in the list |
| `0x4703` | `snapshot_again` | refused: the snapshot was sent already |
| `0x4704` | `not_a_login` | refused: the first package is no login |
| `0x4705` | `bad_version` | refused: not protocol 101 |
| `0x4706` | `flooding` | too many packages: a warning, then a disconnect |
| `0x47FD` | `drop_receive` | disconnected: a receive error |
| `0x47FE` | `drop_short` | disconnected: a body cut short |
| `0x47FF` | `drop_bad` | disconnected: a bad length or a refused package |
| `0x4611` | `message` | a dispatcher's message |
| `0x4612` | `server_down` | the connection is gone (queued by the receiver itself) |
| `0x4616` | `pause` | pause on or off |
| `0x4643` | `server_message` | a numbered server message |
| `0x4630` | `host_mark` | a mark in a mini-server host's snapshot |
| `0x462E` | `user` | a user record (in the snapshot) |
| `0x462D` | `player_pos` | where a player is: the map's person icon |
| `0x1700` | `node_hello` | the helper node's hello |
| `0x1701` | `node_text` | a text between nodes |

**World**

| Id | Name | What it is |
|---|---|---|
| `0x460D` | `time` | the time of day |
| `0x4609` | `junctions` | all junctions (snapshot) or a list of changes |
| `0x460A` | `junction` | one junction's position and lock |
| `0x4642` | `junction_request` | a player asks to throw a junction |
| `0x460B` | `signals` | all signal heads (snapshot) |
| `0x460C` | `signal_head` | one signal head's state |
| `0x4618` | `signal_control` | the dispatcher's control of a signal |
| `0x4634` | `weather_area` | a weather area made, changed or removed |
| `0x4635` | `weather_values` | a weather area's values |
| `0x461D` | `object_state` | a world object's state |
| `0x463F` | `object_sound` | a sound trigger on a world object |
| `0x464B` | `track_object_value` | a track object: a value by index |
| `0x464C` | `track_object_byte` | a track object: a byte |
| `0x464D` | `track_object_word` | a track object: a `u16` |
| `0x464E` | `track_object_list` | a track object's list |

**Electrification**

| Id | Name | What it is |
|---|---|---|
| `0x4645` | `piece_supply` | a piece of track's own supply ("Отдельные пути") |
| `0x4646` | `substation` | a substation's state and voltage |
| `0x4647` | `section_source` | a sectioning section's source |
| `0x4648` | `phase_voltages` | the substations' phase voltages |
| `0x4649` | `substation_load` | a train's load on a substation |

**Trains**

| Id | Name | What it is |
|---|---|---|
| `0x4607` | `train_add` | a train with its cars |
| `0x460E` | `train_cars` | inside `train_add`: the car list |
| `0x460F` | `train_car` | inside `train_cars`: one car |
| `0x4603` | `train_pos` | where a train is and how it moves; a client ignores it for its own trains |
| `0x4604` | `car_pos` | inside `train_pos`: one car |
| `0x462F` | `train_pos_forced` | the same, taken for the client's own trains too |
| `0x4605` | `train_info` | kind, category, number, state |
| `0x4606` | `train_stands` | the train stands; also puts it back in play |
| `0x4602` | `train_remove` | off the track (0) or deleted (1) |
| `0x4601` | `train_assign` | this train is (not) yours |
| `0x461E` | `helper_assign` | the train you help on |
| `0x4610` | `train_id` | a train's id changed |
| `0x4613` | `train_brakes` | the "Тормоза" form: pressures for every car |
| `0x4508` | `train_speed` | speed, top speed, acceleration, deceleration |
| `0x4619` | `route_left` | the distance left to the end of the route |
| `0x461A` | `train_names` | the name and the notes |
| `0x4641` | `train_wake` | a standing STATIC or LOOSE train set moving |
| `0x4636` | `derail` | derailed cars |
| `0x4638` | `derailed_car` | inside `derail`: one car's placement |
| `0x4637` | `rerail` | back on the rails (nobody sends it) |
| `0x461C` | `car_cargo` | cargo on a car |
| `0x460E` | `uncouple` | an uncoupling |
| `0x460F` | `couple` | a coupling |
| `0x461B` | `joint_action` | a car menu action between two cars |
| `0x4640` | `car_action` | a car menu action on one car |
| `0x4633` | `seat_player` | put the player in that car (mini-server host) |

**Other players' trains**

| Id | Name | What it is |
|---|---|---|
| `0x4617` | `car_light` | one car light |
| `0x463C` | `car_state` | a car's look: flags, pantographs |
| `0x4639` | `sound_trigger` | a discrete sound trigger |
| `0x463D` | `sound_values` | a sound's variables |
| `0x463A` | `sound_state` | a car sound's whole state |
| `0x463B` | `sound_stream` | inside `sound_state`: one stream |
| `0x4623` | `state_request` | asks for a cab (1) or a train's look (2) |

**Helpers**

| Id | Name | What it is |
|---|---|---|
| `0x461F` | `helper_link` | setting up and dropping the helper link |
| `0x4620` | `train_stream` | engineer → helper: the train, twice a second |
| `0x4621` | `car_stream` | inside `train_stream`: a car |
| `0x4628` | `engine_stream` | inside `car_stream`: an engine |
| `0x4629` | `electric_stream` | inside `engine_stream`: electric values |
| `0x462A` | `diesel_stream` | inside `engine_stream`: diesel values |
| `0x4622` | `cab_control` | a cab control moved |
| `0x4624` | `cab_state` | the whole cab |
| `0x4625` | `dll_state` | inside `cab_state`: the DLL's state |
| `0x4626` | `cab_controls` | inside `cab_state`: the controls |
| `0x4631` | `cab_display` | inside `cab_controls`: a display's values |
| `0x463E` | `aux_library_state` | inside `cab_state`: an auxiliary library |
| `0x4632` | `cab_drag` | a cab item dragged |

**Consoles**

| Id | Name | What it is |
|---|---|---|
| `0x4504` | `console_pause` | the pause button |
| `0x4505` | `train_go` | "Начать движение" / "Остановить" |
| `0x4506` | `train_reverse` | "Изменить направление" |
| `0x4509` | `train_allow_coupling` | "Разрешить прицепку" |
| `0x4507` | `train_add_reply` | the server's id for a console's new train |
| `0x450A` | `event_line` | a line of the event log |
| `0x450B` | `route_segment` | a segment of a train's route |
| `0x450C` | `route_segment_reply` | accepted or refused, with the text |
| `0x450D` | `route_segment_cancel` | cancel a segment |
| `0x450E` | `route_segment_state` | a segment's state |

**Situation files**

| Id | Name | What it is |
|---|---|---|
| `0x4614` | `situation_user` | a user record |
| `0x4615` | `situation_route` | the route |

## Programs, links and handlers

| Link | Sent by | Received by |
|---|---|---|
| player → server | the client's network thread (`dword_61D31C+36` set, PROBABLE "playing through the network thread"), or `0x52CE9C` on its own node | server network thread `0x4436C8` (limits, relay), main thread `0x40FB90` |
| server → player | server network thread | client `0x4C3DE0` |
| engineer ↔ helper | each side's NetworkNode | `0x4C9120`; the client's `0x4C3DE0` also hands it `0x4620`, `0x4622`..`0x4624`, `0x4632` |
| player → mini-server host | the client's network thread | `0x4CA588`, then `0x4C9120` / `0x4C3DE0` |
| console ↔ server | the same code as the server | `0x40FB90` in both programs |
| snapshot file `multdata` | — | `0x4CAEAC` replays it through `0x4C3DE0` |
| situation file | server `0x418474` writes | server `0x41A650` reads |
| recovery file `multRecoveryData.dat` | client `0x47B09C` writes | client `0x47B414` reads during the snapshot replay |

- **The console.** `rts_server_DSP.exe` is built from the server's code:
  - the same functions, field for field;
  - in `0x40FB90` the console's branches test `byte_54C83B` ("I am the
    console");
  - `byte_54C83D` is "the console has loaded";
  - `byte_54C838` is "a server with clients": it then forwards what it
    takes.
- **Client globals:**
  - `dword_61D31C+37`: playing on a player's mini-server. Only the host
    sends it, in `0x4630` with value 1. It enables `0x4622` and the
    `0x4623` mode-1 request;
  - `+48`: the player's train id from the snapshot's `0x462E`;
  - `+56` (`u16`): PROBABLE the network role, 2 the mini-server host.
- **Client train kinds:**
  - 0 STATIC;
  - 1 the client's own PLAYER train;
  - 2 TRAFFIC; with train flag `0x10000` it is another player's train;
  - 3 LOOSE;
  - 4 PROBABLE: the engineer's train on its helper's client.
- **Client train flags (`+4`):**
  - `2` loaded;
  - `4` the DLLs run (flag 4);
  - `0x10` standing (`0x4606`);
  - `0x20` the player is in it;
  - `0x1000` derailed cars;
  - `0x10000` another player's.

**Who simulates what.**
- In a network game a client simulates and sends only its kind-1 trains
  (`0x424488`):
  - `0x4603` at most every 1.5 s while the train moves;
  - one more when it stops, if train flag `+6 & 0x20` is set.
- Every other train is extrapolated from `0x4603`. Its speed and
  acceleration drop to 0 after 5 s without one.
- The mini-server host (role 2) also sends `0x462F` + `0x4606` for a train
  that has stood over 5 s, and the same with `0x4607` for a train it
  creates.

## Session

| Id | Direction | Payload | What happens |
|---|---|---|---|
| `0x4600` | player → server, server → player | login, login reply | `multiplayer.md`, "Login" |
| `0x4500` | console → server | login | the same layout as `0x4600` |
| `0x4608` | player → server; server → player | empty | the request for the startup data; the end of the snapshot. A second request gets `0x4703` |
| `0x4701`..`0x4706`, `0x47FD`..`0x47FF` | server → player | `u16` code only | `multiplayer.md`, "Login" |
| `0x4611` | server → player | `u32 n`, UTF-16 text | a dispatcher message box, also kept in the message list for 15 min; to its addressee only |
| `0x4612` | local | empty | "Server down": the client shows it and leaves; the console closes. Nobody sends it: the receiver's own listener queues it when its socket closes (client `0x4C351C` → `0x4CB390`; server and console `0x44DCC8`, `0x444B74`) |
| `0x4616` | server → player, console | `u8` suspended | pause; on the console, the pause button. Nothing at all is sent while paused |
| `0x4643` | server → player, console | `u32` message | `0x4706` shows the flood warning, anything else a numbered "Server message" |
| `0x4630` | mini-server host → player | `u8` 1 | in the host's snapshot only: sets `dword_61D31C+37` |
| `0x462E` | server → player | user id, train id, `u32`, `u32` + 3 × `f32`, `u32` + 3 × `f32`, name (32 bytes) | a user record, in the snapshot; the client takes the train id as its own train (`+48`) |
| `0x462D` | player → server → others | the client's position fields (as `0x462E` from the train id on): `u32` 0 (`rts1.exe` always sends 0 in the record's train field), the train the player is in `u32` (0 none), `u32` (UNKNOWN, 0 or 1), tile x, z (`i16`), x, y, z, four `f32` (live capture 2026-11-05) | the server stores them in the user record and relays them to everyone else (audience 2), with the sender's id in front and its name (32 bytes) after: the map's person icon |
| `0x1700` | helper → engineer | node id, session | the NetworkNode hello (`multiplayer.md`, "Helpers") |
| `0x1701` | node → node | text | shown as a "Multiplayer" message (`0x52DE78`) |

## The world

| Id | Direction | Payload | What happens |
|---|---|---|---|
| `0x460D` | server → player, console | `f32` time of day | |
| `0x4609` | snapshot; player → server | container | in the snapshot it holds the `0x460A` entries; from a player, a list of nested `0x460A` changes (`0x40C52C` each) |
| `0x460A` | both ways | junction index (the TDB node number − 1: the client reads its node table at the index, CONFIRMED against Kuban, `net.md`), branch `u32`, lock `u8` | a junction's state; from a player, a change the server decides at once (`0x40C52C`) |
| `0x4642` | player → server or host | junction index, `u32` | a junction request, done when the junction is neither locked nor reserved (`0x40C5A4`) |
| `0x460B` | snapshot | container | holds the `0x460C` entries |
| `0x460C` | server → player | the head's TDB `SignalItem` (`TrItemId`: the client reads its track item table at it, CONFIRMED against Kuban), `u16` (`+78`: the semaphore arm's position, the drawing state's `SemaphorePos`; 0 on colour lights, as on Kuban; client `0x479AFC`), state `f32` (`+84`), drawing `f32` (`+88`), `f32` `+92`, mode `f32` (`+96`), speed limit `f32` (`+100`, m/s, −1 none) | a signal head's state ("Live capture"); the client skips `+92` / `+96` |
| `0x4634` | server → player | area id, remove `u8`, `u32 n` + data | a weather area made, changed or removed (network thread only) |
| `0x4635` | server → player | area id, `u32` (`+32`), 8 bytes (`+12`) | a weather area's values |
| `0x461D` | player → server → others | object (`u32`, 3 × `f32` position, `u32` id), then `u8` `+65`, `u8` `+64`, `f32` | a world object found within 50 m (`0x4E0E4C`) gets two bytes and, unless the float is −1, a value at its `+40 → +44`. PROBABLE: crossings and animated objects |
| `0x463F` | player → server → others | the same object key, then `u16` code | a sound trigger on that object's sound (`0x4A9F48`). The data channel of `multiplayer.md` uses this one |
| `0x4618` | console ↔ server | signal object index, state `u16`, mode `f32`, `u8` | the dispatcher's control of a signal: mode 0 manual, 2 automatic block, 1 PROBABLE open by a route; the server applies a manual state at once and sends `0x460C` ("Live capture"). The table (`dword_54C5BC` entries of 56 bytes) holds the signal objects |
| `0x464B` | both ways | object id, index `u32`, `f32` | a track object of kind 2 (`0x4C90E8`; client `0x4ECC6C`) |
| `0x464C` | both ways | object id, `u8` (`+64`) | a track object of kind 1 (`0x42139C`) |
| `0x464D` | both ways | object id, `u16` (`+14`) | a track object of kind 1 (`0x4214F4`) |
| `0x464E` | both ways | object id, `u32 n` (−1 clears), then per entry `u16`, `u16`, `u16` name length + name, `u32`, `u32` | a track object of kind 1: its list. PROBABLE: pickups and the route's global objects |

- `0x464B`..`0x464E` from a player go to the others; the server keeps them.

**Electrification:**

| Id | Direction | Payload | What happens |
|---|---|---|---|
| `0x4645` | console → server → players | TDB node (1-based), the piece's index in the node's `Electricity` list `u32`, source `i16`, `u8` | a piece of track's own supply: the console's "Отдельные пути" (server `0x454E7C`, client `0x5B5F1C`). Source ≥ 0: that source of the piece's electrosection; −1 the section's own choice; −2 / −3 no source. A dead piece (a neutral section) can be fed from a neighbour: 1 the piece before, 2 the piece after. The `u8` (clients skip it) PROBABLE keeps the setting in the server's list, which the snapshot sends again: the reconnect's snapshot had node 1592, piece 0, −1, 1 |
| `0x4646` | console → server → players | substation id (the `.electrotype`'s `ID`), flags `u32` (`+44`), voltage `f32` (`+52`, V), `u32` 0 | a substation's state. Live capture: bit 0 is "отключена" (ТПС Староминская off 1, on 0); 4 came after the dispatcher set ТПС Краснодар's voltage (PROBABLE: a set voltage); the voltage was 27 500, then the set 10 500 |
| `0x4647` | console or player → server → others | section id, source `i16` | a sectioning section's source (`0x455270` / `0x5B5E5C`): −1 its own, −2 "отключено" (live capture: the train in it lost line voltage and its `0x4649` stopped); PROBABLE another value names a source |
| `0x4648` | server → player | substation count, then per substation id, `u16` phases, `f32` per phase | the phases' voltages (27 500 V on Kuban), sent again a few seconds after a sectioning change. The client checks the order: "Substation order or phase num do not match the one on server" |
| `0x4649` | player → dispatchers | substation `u32`, phase `u16`, energy `f32` (the power summed x s), window length `f32` (s); 14 bytes (live capture: 3, 1, 41.8, 2.016) | `multiplayer.md` |

- From a user whose role (`+32`) is not 1 (PROBABLE: a dispatcher),
  `0x4645`..`0x4647`, `0x464A` and `0x464B` are dropped.
- `0x4648` from a player is always dropped.

## Trains

| Id | Direction | Payload | What happens |
|---|---|---|---|
| `0x4607` | server → player; console or host → server | the train: id, kind `u16`, subtype `u16`, `f32` (`+80`, PROBABLE top speed m/s), start state `u8`, 4 bytes, the cars (`0x460E`), then `f32` `+84`, `f32` `+88` (PROBABLE acceleration and deceleration; 1.0 by default) | a new train. Another player's PLAYER train becomes kind 2 with `0x10000`. When it comes in, the server assigns the id, checks the train, answers `0x4507` to the sender and sends `0x4607` to the others. A player's is dropped (below) |
| `0x460E` inside `0x4607` | | `u32` car count, then a `0x460F` per car | the car list |
| `0x460F` inside `0x460E` | | kind `u16` (0 wagon, 1 electric, 2 diesel), `u32 n` + UTF-16 file name, uid `u32` (number `<< 18` + car index), flags `u32`, mass `f32` (`+36`, kg), width, length, height (3 × `f32`), optional handbrake `f32` (`+112`, 0..1) and `MaxHandbrakeForce` `f32` (N; the client skips it) | a car. Flags: 1 flipped, 2 cold (`LocoFlags` `\|= 8`), `0x100` a chock at the head of the train, `0x200` one at the tail (car `Flags` `0x1000000` / `0x2000000`) |
| `0x4603` | player → server → others | train id, speed `f32`, acceleration `f32`, track node `u32` (the TDB number), offset `f32`, direction `u8` (0 facing along the node, 1 against it), car count `u32`, then a `0x4604` per car | where a train is: its **rear** end (RTS train `+232`); a moving train (below) |
| `0x4604` inside `0x4603` | | offset in the train `f64` (the car's middle, metres ahead of the rear end; car 0 has the largest), speed `f32`, acceleration `f32` | a car |
| `0x462F` | server → player | as `0x4603` | the same, forced: it also moves the client's own train. The snapshot replays every `0x4603` as `0x462F` |
| `0x4605` | both ways | train id, kind `u16`, subtype `u16` (the console's category: 0 "Прочий", 1 "Пассажирский", 2 "Грузовой", the train form's list), number `u32`, state `u8` (the console's: 0 "Холодный", 1 "Включен", 2 "Движется"), path id `u32` | a train's kind and number. The server also sends it by itself when it lowers a standing train's state (live capture): "Включен" 480 s after the stop, "Холодный" 1 200 s after (`0x40DCDC`). The client sets kind 2 + `0x10000` for another player's PLAYER train, and the car numbers (`number << 18`) |
| `0x4606` | both ways | train id | the train stands: no extrapolation on clients (`+4 \|= 0x10`); the server sets `+79` bit 0 and sends `0x4606` and `0x4603` to everyone |
| `0x4602` | both ways | train id, remove `u8` | from a player: release (0) or remove (1) the train. A PLAYER train with an owner is not removed: the server sends `0x4606` instead. To a player: the standing flag clears; remove deletes the train |
| `0x4601` | server → player | train id, mine `u8`, number `u32`, subtype `u16`, kind `u16` | an assignment, only to the user it concerns. The in-game handler reads it too (`0x4C3DE0`; the menu reads another layout, `0x461F00`): mine → kind 1; another player's PLAYER train → kind 2 + `0x10000` |
| `0x461E` | server → player | as `0x4601` | a helper's train: helper → kind 4 |
| `0x4610` | server → player | train id, new id | a train's id changed: after a coupling, and as the answer to a player's uncoupling (its local id → the server's) |
| `0x4613` | console → server → players | train id, brake pipe, main reservoir pipe ("напорная магистраль"), auxiliary reservoirs, brake cylinders (`f32` each) | the "Тормоза" form: every car gets them (`+100`, `+108`, `+56`, `+96`). The packet's order is not the form's (live capture: the form's 1 / 2 / 3 / 4 came as 1, 3, 2, 4). Players' are dropped |
| `0x4619` | console ↔ server → everyone | train id, length `f32` (`+224`, m), flag `u8` (`+316` bit 2) | the distance left to the end of the route the dispatcher gave the train, laid from its head along the current junctions; −1 cancels. While the train moves the server recomputes it and sends it every 0.5 s. Clients have no case for it |
| `0x461A` | console ↔ server → everyone | train id, two `u32 n` + UTF-16 strings | the train's names (`+80`, `+81`): the first is the console's free text ("Название"); the second is the "Заметки" memo of the console's train form (`TfTrainEdit`, server `0x458F18`), empty in the live capture. Clients have no case for it |
| `0x4641` | player → server → others | train id, speed `f32` | a STATIC or LOOSE train set moving: STATIC becomes LOOSE (`0x4605` to all). A client applies it only to its own PLAYER train |
| `0x4508` | server → player | train id, `f32` speed (skipped), `f32` `+80`, `f32` `+84`, `f32` `+88` | values the console sets (server `+332`, `+260`, `+252`, `+256`). PROBABLE: the current speed, the top speed, acceleration and deceleration ("Live capture") |
| `0x4636` | player → server → others | train id, car count, `u8` per car, then a `0x4638` per marked car | derailed cars: car `+32 \|= 0x20`, train `+4 \|= 0x1000` |
| `0x4638` inside `0x4636` | | car index, 3 × `f32`, 4 × `f32`, 3 × `f32`, 5 × `f32`, `u8`, `u32`, optional blocks | a derailed car's placement (`0x57FF64` / `0x580234`). PROBABLE from one derailment: the car's direction (a unit vector); 0 and its position in the tile; the tip angle now, at rest and a third (rad; the angle grew to the rest value as the car fell); the car's weight m·g (N) and a force rising to it; a state `u32` 0 → 4. The five values are `+132`, `+136`, `+140`, `+144` and `+136` again. Then two optional blocks, each after a `u8` 1 (0 when absent): `+196` a point, a value and a vector (UNKNOWN; made in `0x586E6C` / `0x589A14`); `+200` a pull on the car's end (`0x58186C`): the point (0, height / 4, ± length / 2), a vector, the summed force, two values. The VL80's second section had the second block, at (0, 1.31, 8.0) |
| `0x4637` | → player | train id | back on the rails: the flags clear, speeds go to 0. No program sends it: none of `rts1.exe`, `rts_server.exe` and `rts_server_DSP.exe` has the constant outside the handler (PROBABLE: unused) |
| `0x461C` | both ways | train id, car index, then the load: `u16 n` + name, 3 × `f32`, `u32`, `u32`, 3 × `u16`, `u32`, flags, optional texts and a nested block | cargo on a car (`0x4CCEBC`, `0x451690`; client `0x501120`). Relayed and kept by the server |

**`0x4603` on a client** (`0x4C3DE0`):
- It is ignored for the client's own trains (kinds 1 and 4) unless it is
  `0x462F`.
- **Smoothing.** Suppose the train is on the same track node, it moves, and
  the new offset is less than `50 × max(1, |v| / 15)` m away. Then the
  client nudges the speed by `|v| × d / (100 × max(1, |v| / 15))` instead
  of jumping.
- The car count must match, or the package is dropped.
- The time is noted for the 5-s timeout.

**`0x4603` on the server** (`0x40FB90`):
- It is taken for a PLAYER train from any sender, from the console, or from
  a user of role 1. There is no owner check.
- It is relayed to everyone but the sender.

## Couplings and the car menu

| Id | Direction | Payload | What happens |
|---|---|---|---|
| `0x460E` | player → server → others | train id, car index, the player's local id for the new part `u32`, side `u8` | an uncoupling (below). Side 0: the part is the cars from the index on (the live cut: 19 cars cut at 1 left 1 and made an 18-car train); PROBABLE 1: the cars before it |
| `0x460F` | player → server → others | train A, train B, ends `u8`, `u8` | a coupling: `multiplayer.md`, "Coupling in play". PROBABLE: an end is 0 the rear, 1 the front: both live couplings (ends 0, 1) gave A's cars then B's in their order (the car spacings of the merged `0x4603`) |
| `0x461B` | player → server → others | train, car, train 2, car 2, action `u32` | a car menu action between two cars. Every client carries it out; actions 1 and 16 are not |
| `0x4640` | player → server → others | train, car, `u32`, action `u32`, `u32` | a car menu action on one car. Clients carry out `action \| 0x1000000`; the server keeps flags `0x100` / `0x200` on its car and the last `u32` (`+84`) |

**Uncoupling on the server** (`0x44FE34`):
- A new train is made from the part (`0x456958`). It **keeps the kind and
  the owner** of the train it came from (`+296`, `+328`), and takes its
  number, its path and its speed.
- The sender gets `0x4610`: its local id → the new id.
- The others get `0x460E` (train, car, new id, side), then the part's
  `0x4603`.
- The clients cut the train with `0x469858`.

## Players and helpers

| Id | Direction | Payload | What happens |
|---|---|---|---|
| `0x4633` | mini-server host → player | train id, car index | put the player in that car (`0x4FC32C`, mode −1). Only a mini-server host (`rts1.exe` in role 2, `0x4D1358`) sends it: its Track control's command for the selected car lists the players, and the chosen one is put in that car (`0x44C9BC`, after a `0x4601`); choosing itself seats the host |
| `0x461F` | server → player; engineer → server | subtype `u16`, then by subtype | helpers, `multiplayer.md`. Type 1 to the helper: name (32 bytes), address `u32`, port `u16`, session `u32`, train `u32`, `u32`, `u16`. Type 2 to the engineer: add `u8`, helper id, name, `u32`, session. Type 3: `u8` 0 drops the link. Type 4 from the engineer: helper id, port `u16`, session `u32`, train `u32`; the server builds type 1 from it (`0x4527A0`) |

**The helper link** (`0x4C9120`):

| Id | Direction | Payload | What happens |
|---|---|---|---|
| `0x4620` | engineer → helper, every 500 ms (`0x4CF670`) | as `0x4603`, cars as `0x4621` | the train, on any train that is not the receiver's own PLAYER |
| `0x4621` inside `0x4620` | | as `0x4604`, then pantographs `u8` (`+13`), `u8` `+14`, `f32` `+52`, `+56`, `+60`, `+64`, `+96`, `+100`, `+108`, `+112`; an engine with a bound DLL adds a `0x4628` | a car: the brake pressures, handbrake |
| `0x4628` inside `0x4621` | | `LocoFlags`, main reservoir (`+404`), `+408`, heat (`+416`); then `0x4629` (electric) or `0x462A` (diesel) | an engine |
| `0x4629` / `0x462A` | | line voltage (`+504`), `+508`, `+512` / fuel (`+504`) | |
| `0x4622` | either side; a player → the mini-server host | train id, car index, the control (index `u32`, kind `u16`, owner `u16`), position (`+48`), value (`+44`), animation (two `u32`) | a cab control moved (`0x4CFC74`). The receiver calls its DLL's `CanSwitch`, and `Switched` for an instant switch |
| `0x4623` | helper → engineer; player → server | mode `u32`, train id, car index, `u32` | mode 1: send the whole cab (loading the car's cab first); mode 2: send the train's look (`multiplayer.md`, "Other players' trains") |
| `0x4624` | engineer → helper, every 60 s and on request (`0x4D03E4`) | train id, car count, then per car `0x4625`, `0x4626`, `0x463E` | the whole cab |
| `0x4625` inside `0x4624` | | car index; the DLL's `Engine`: independent brake (`+52`), handbrake (`+72`), `ur` (`+84`), battery `f64` (`+96`), `flags` (`+120`), `flags2` (`+124`), the variable stack (`u32 n` + values), reverser (`+128`), throttle (`+136`); then `0x462B` (electric: pantographs `+208`, `+209`) or `0x462C` (diesel: `+208`, `+216`) | the DLL's state, written over the helper's copy |
| `0x4626` inside `0x4624` | | car index, the switch, lever and other control positions (three counted lists), then `0x4631` blocks | the cab's controls (`0x438708` without sending) |
| `0x4631` inside `0x4626` | | display index, `u32 n`, values | a display's values |
| `0x463E` inside `0x4624` | | car index, library name, `u32 n` + data | an auxiliary library's state (`0x4CF1C8` / `0x4CF324`) |
| `0x4632` | either side (`0x4CFF5C`, from the input handler `0x43823C`) | train id, car index, item `u32`, `u32`, `u8`, `u16`, `f32` `+164`, `f32` `+160`, `+132`, `+136` | a cab item the player drags (`0x527DCC`) |

On the helper's client the DLL is bound and initialised but never run: all
of its state comes from this link (`multiplayer.md`, "Helpers").

## The dispatcher console

| Id | Direction | Payload | What happens |
|---|---|---|---|
| `0x4500` | console → server | login | |
| `0x4501` | server → console | the login data | read by the login form (`0x44E788`) |
| `0x4502` | server → console | the login reply | "Waiting for admin…" |
| `0x4503` | server → console | empty | "Loading startup data…" |
| `0x4504` | server → console | `u8` | the pause button |
| `0x4505`, `0x4506`, `0x4509` | console → server → consoles | train id, `u8` | train flags `+79` (`+316`): bit 1 "Начать движение" / "Остановить", bit 2 "Изменить направление", bit 6 "Разрешить прицепку". Players get none of them (live capture) |
| `0x4507` | server → sender | `u32` 1, the sender's local id, ok, the new id as `f32`, `u32` 0 | the answer to a `0x4607`: the server's id |
| `0x4508` | console → server → players | train id, `f32` `+332`, `+260`, `+252`, `+256` | see "Trains" |
| `0x450A` | server → console | `u8` type, `u16`, train `u32`, `u32`, `u32`, 2 × `f32` | a line of the event log (`0x45E324`) |
| `0x450B` | both ways | id, train id, station route id, station name, two check `u16`, direction `u8`, two flags `u8`, length `f32`, priority `u32`, state `u8`, junction list `u16`, junctions thrown `u16` ("Live capture") | a segment of a train's route; the server checks it (`0x469804`), answers `0x450C` and sets it at once |
| `0x450C` | server → console | request id, new id, ok `u8` (`u16 n` + text when refused) | the answer; a refused route shows the text |
| `0x450D` | both ways | route id | cancel a segment; the server echoes it and answers `0x450C` (id, 0, ok) |
| `0x450E` | server → console | segment id, state `u8`, junction list `u16` (index + 1), priority `u32`, length `f32`, junctions thrown `u16` | a segment's state (server `0x454600`, console `0x4547AC`): 0 waiting, 2 throwing junctions, 1 set, 3 the train on it, 4 passed (the console drops it); on the console 0xFE waits for the server, 0xFF was refused |

- **The train's context menu** is rebuilt on every click (`0x450E1C`) by
  the train's kind:
  - "Тормоза.." (the `0x4613` form) only for STATIC and LOOSE trains;
  - "Начать движение" / "Остановить" (by bit 1), "Изменить направление"
    and the ticked "Разрешить прицепку" (bit 6) only for TRAFFIC;
  - "Отменить маршрут" only for a train with a route.
- The console also takes `0x4605`, `0x4607`, `0x460A`, `0x460C`, `0x4613`,
  `0x4619`, `0x461A` and the electrification packages.
- It sends the same packages to change a train, a junction or a signal.
- The station route's layout is in "Live capture"; its three flag bytes
  and two `u16` are not settled.

## The server's rules (network thread `0x4436C8`)

- **Limits** (5-s windows):
  - all packages (`+136`);
  - `0x4639` / `0x463A` / `0x463D` (`+148`).

  Over a limit the server sends `0x4706` and drops the user.
- **Dropped from players:**
  - `0x4508`, `0x4509`, `0x4602`, `0x4605`..`0x4607`, `0x4613`, `0x461A`,
    unless the user's role (`+32`) is 1 with permission bit `+93 & 2`;
  - `0x4645`..`0x4647`, `0x464A`, `0x464B` unless the role is 1;
  - `0x4648` always.
- **Where a package goes:**

  | Handling | Ids |
  |---|---|
  | handled in the network thread | `0x4608` (the snapshot, `0x41733C`), `0x460A` (a junction), `0x462D` (stored, relayed) |
  | relayed only (audience 2) | `0x461B`, `0x461D`, `0x463F` |
  | relayed only to players within 3 km (audience 4) | `0x4617`, `0x4639`, `0x463A`, `0x463C`, `0x463D` |
  | relayed (audience 2) and read by the main thread | `0x461C`, `0x4636`, `0x4640`, `0x4641` |
  | to dispatchers only, and read | `0x4649` |
  | main thread only | everything else |

## The snapshot

**The server's order** (`0x41733C`), to a new user:
1. `0x4600` (the root);
2. `0x460D`;
3. `0x4609` with a `0x460A` per junction;
4. for a console: the electrification (`0x4646`, `0x4648`, `0x4647`,
   `0x4645`), the `0x4618` table, the event log and the pause (`0x4504`);
5. `0x460B` with a `0x460C` per signal;
6. per train:
   - `0x4607`, `0x4603`;
   - `0x461A` when it has names;
   - `0x4606` when it stands;
   - `0x4613`;
   - `0x461C` cargo;
   - `0x4636`;
   - `0x461E` for a helper;
   - `0x4619`;
7. the track objects `0x464C`, `0x464D`, `0x464E`, `0x464B`;
8. for a console: the station routes `0x450B`;
9. the users `0x462E`, the player's own last;
10. `0x4608`.

**The client's replay.** The client saves the snapshot as `multdata` and
plays it back (`0x4CAEAC`):
- it walks into the containers `0x4600`, `0x4608`, `0x4609`, `0x460B`;
- it hands these to `0x4C3DE0`: `0x4601`, `0x4603` (as `0x462F`), `0x4606`,
  `0x4607`, `0x460A`, `0x460C`, `0x460D`, `0x4613`, `0x4616`, `0x461C`,
  `0x461E`, `0x462D`, `0x4630`, `0x4634`..`0x4636`, `0x4645`..`0x4648`,
  `0x464B`..`0x464E`;
- it reads the player's train id from `0x462E`;
- any other id stops the load.

The mini-server host builds its own (`0x4D14F0`) and adds `0x4630`.

## Situation files

- The server saves a situation (`0x418474`) and loads one (`0x41A650`) in
  the same package format: `0x4607` with the full server train record,
  `0x460A`, `0x460B` / `0x460C`, `0x461C`, `0x4634`, `0x4636`,
  `0x4614` (a user record: id, name, flags, position), `0x4615` (the
  route) and `0x4616`.
- Not decoded field by field. openRTS needs them only to load an RTS
  situation.

## The player's recovery file

`multRecoveryData.dat`, in the client's working folder (`rts1.exe`), is
what "Restore player position" and "Restore train state" read. The server
holds nothing of it.

**Written** (`0x47B09C`):
- on F2 in multiplayer, in place of a saved game (`0x4121BC`);
- on `0x4612`, "Server down";
- by the emergency save after a crash, when the player agrees to "Do you
  want to try saving the simulation state?" (`0x408360` from `0x40521C`).

Two clients started from one folder write the same file: the last one
wins.

**Contents**, in the package format of a saved game (`u16` id, `u32`
length):

| Id | Payload |
|---|---|
| `0x4400` | the root: the whole file |
| `0x4504` | version `u16` (114) |
| `0x4510` | two `u8` (1 and 0 in the capture), the simulation time `u32` (s), the temperature `f32` |
| `0x4502` | the player's place: position in the tile 3 × `f32`, three 4-byte values, tile x and z `i16`; inside it `0x4511` when in a car: train id, car index, place `u16` (1 the cab), position in the train 3 × `f32`, two more 3 × `f32`; `0x4512` / `0x4513`: an index into the cab's camera lists (`0x41ADA8`) |
| `0x4548` | the route's `.trk` path (`u16 n` + UTF-16) |
| `0x4506` | the player's own trains (kind 1 with `+4` bit 2): a `0x4521` each, the saved-game train record with its cars (`0x57194C`) |

**Read** (`0x47B414`) while the snapshot is replayed (`0x4CB2B4`), when
either box is ticked (`+70` bits 0 and 1):
- a different route path stops it;
- a time more than 300 s off the server's asks "Time in recovery data …
  significantly differs … Try to restore train state anyway?";
- "Restore train state": each `0x4521` goes into the train with the same
  id if that train is now the player's (kind 1) and has as many cars; its
  cars take their saved state (`0x57CD78`);
- "Restore player position": the player goes into the saved train and car
  (`0x4FC32C`), then the cab camera; else to the saved spot.

So a reconnect restores only what the server kept the same: the train ids
and their owner.

## Live capture (2026-11-05)

**Setup.**
- One PC: `rts_server.exe` (the ТЧД console, port 800), `rts_server_DSP.exe`
  (the ДСП console) and two `rts1.exe` (MashFirst, MashSecond), Kuban.
- All of them talked through `tools/mp-capture/mp_proxy.py --mode helper
  --dump capture.bin`: every frame was recorded whole, about 15 000 s
  (4 h 10 min) and 99 000 frames, 30 MB. `mp_dump.py` and `parse4607.py` (same folder) read the
  capture. Captures hold players' nicknames and addresses and stay out of
  the repository.
- The server's own "Data trace" (menu `mnDataTrace`) cannot do this: its
  level (`word_53D27C`) is 0 in the exe and nothing writes it, so it never
  logs `0x4603`, the lights, the sounds or the helper link.

Confirmed by the capture unless marked; it also corrected a few readings
above.

**What the capture shows, in short.**
- **Who decides.** Each client simulates only its own trains (kind 1) and
  tells the others where they are every 1.5 s. The server relays almost
  everything and checks little: it takes a PLAYER train's `0x4603` from any
  sender. TRAFFIC trains the server drives itself, every 0.5 s, on the
  consoles' commands. Every other train a client only extrapolates.
- **What is not shared at all:** passenger doors (the DLL's), how derailed
  cars come to rest, the owners (the ДСП never learns them), a split part's
  name on the ДСП.
- **What breaks:**
  - a coupling of two players' trains stops the second player's DLL; the
    train stands still for them until a dispatcher assigns it, and the DLL
    goes on without `Init`;
  - "Удалить из мира" and placing back a player's own train crashed its
    owner;
  - after a reconnect the helper link did not come back;
  - a reconnect sends the dispatcher's old brake pressures to everyone;
  - derailed cars lie differently on each client.
- **What works and is worth keeping:** the consoles' commands (signals,
  junctions, routes, speed, direction, pause, messages, substations and
  sectioning), the server's timers on standing trains, the snapshot on
  entering, a login that pushes out the old connection of the same name.
- **What openRTS takes from it:** `multiplayer.md`, "What the live capture
  of 2026-11-05 asks of openRTS".
- **Not captured:** a pickup. Of the installed routes only Uspensk has any
  (39), so it was left out; `0x461C` stays read from the code only.

**Making a train.**
- From the ДСП:
  1. the console sends `0x4607` with its own local id;
  2. the server answers `0x4507`: `u32` 1, the local id, ok, the new id as
     `f32` (5.0), `u32` 0;
  3. it sends `0x4607` with the new id to everyone else;
  4. the console then sends `0x461A` (the name), `0x4508` and `0x4606`.
- From the ТЧД (the server itself) the same `0x4607`, `0x461A`, `0x4508`,
  `0x4606` go to everyone, the console included.
- Ids are reused: a part cut off got the ids 1 and 3 of trains gone before.
- **The car record** (`0x460F` inside `0x4607`), field by field:
  - kind, file name;
  - uid: the train number `<< 18` plus the car index;
  - flags: 1 flipped, 2 cold, `0x100` a wheel chock at the head of the
    train (on the first car), `0x200` one at the tail (on the last car);
  - mass, kg (ChS4z 129 t);
  - width, length, height, m (ChS4z 3 / 19.6 / 4.5, ED9M 22.1 m long);
  - the handbrake (`+112`, 0..1);
  - its force, N: the car's `MaxHandbrakeForce` (ED9M head car 22 kN,
    motor car 0, coaches 15 and 5.4 kN). The client reads the handbrake and
    skips the force.
- The console's "Закрепить состав" has four checkboxes (their meaning is
  in the tooltips): chocks at the head, chocks at the tail, handbrake at
  the head, handbrake at the tail. They set exactly those fields on the
  first and the last car.
- PROBABLE, the train's motion values. The `f32` after the subtype in
  `0x4607` is the top speed in m/s. The tail of `0x4607` is acceleration
  and deceleration. `0x4508` repeats them after a current speed of 0.

  | Train | Top speed | Acceleration | Deceleration |
  |---|---|---|---|
  | ChS4z | 41.67 | 1 | 2 |
  | ED9M | 36.11 (130 km/h, its `MaxVelocity`) | 1 | 2 |
  | coaches only | 1 | 0.001 | 0.002 |

  The ChS4z's `MaxVelocity` is 100 mph, so 41.67 is not taken from it.
- `0x461A`'s first string is the name the console gives, free text.

**Assignments.**
- `0x4601`'s `u32` is the train number (123, 101).
- It goes only to the user it concerns: the player who gets or loses the
  train, or the console when the console itself becomes the owner.
- So the ДСП never learns who drives what. A ДСП that logged in again got
  no `0x462E` and no `0x4601` in its snapshot either. Only the ТЧД, which
  is the server, shows owners. The ДСП does get `0x462D`, whose second
  `u32` is the train the player is in (0, 2, 3 here), but it shows them
  only on the map.
- "Assign to the dispatcher": `0x4601` mine to the console, not mine to
  the old owner, then `0x461A` and `0x4605` (a name and number 101). The
  kind stays 1. No kind 5 appeared.
- Given back to a player, the train becomes his kind 1 again. His client
  does not bind or initialise the DLL again (`LocoFlags & 0x20`): the DLL
  goes on from where it froze. Whatever another owner did meanwhile is
  lost. The pantographs MashFirst had lowered were reported raised again
  by MashSecond's client.
- The train's start state (`+2`) comes in `0x4605` (2, "Движется") and is
  the `State` of `Init` when a DLL binds. MashFirst entered the absorbed
  ED9M's cab and its DLL started "running".

**Positions.**
- `0x4603` comes every 1.5 s while the train moves. The speed is signed:
  below 0 the train runs rear first.
- The "direction" byte follows neither the sign nor the reverser. PROBABLE:
  how the train lies on its track node.
- The reverser is never sent. The console's arrow is where the head
  points.
- The console lays a route the way the train last moved. A standing train
  has speed 0 and no sign, so the dispatcher had to have it nudged back.
- The players called the remote trains' motion poor: a brake application
  shows up to 1.5 s late, while the old acceleration is still applied.

**Couplings.**
- Two players: MashFirst's ChS4z ran into MashSecond's ED9M. MashFirst's
  client sent `0x460F`, then `0x463C` for all eight cars it took. The
  server sent MashSecond, in this order:
  1. `0x4603` of A;
  2. **`0x4601` "not yours" for his own train**;
  3. `0x460F`;
  4. `0x4610` (3 → 2);
  5. `0x4603` of the merged train.

  The `0x4601` comes before the coupling. MashSecond's client therefore
  made its own train another player's (kind 2), and its DLL stopped:
  controller, doors, КЛУБ. The train then moved by extrapolation.
- The console got `0x460F` and `0x4603` only. The ДСП showed no owner, the
  ТЧД MashFirst.
- When MashFirst entered the ED9M's cab, his client bound the ED9M's DLL
  and ran `Init` from scratch: the pressures were pumped up again. Nothing
  of the DLL's state comes over.
- **The ED9M's passenger doors are not replicated.** These are the
  doors the DLL opens, not the cab doors. Nothing is sent when they open:
  - `0x463C`'s `Flags`, `+32` and `+14` did not change;
  - only the pantograph mask and `Flags` bit `0x4` (the player in the
    cab) changed.

  Each client keeps the doors it last saw. On MashFirst's client the
  ED9M's doors stayed open for good after the train left him.
- The coupler menu showed different states on the two clients after a
  coupling. MashFirst disconnected the hoses and closed the cocks
  (`0x461B` actions 3, 5, 9: explicit set or clear, `0x463F84`); on
  MashSecond the same joint stayed connected. Car `Flags` are never
  reconciled. Why the relayed actions missed there is UNKNOWN: perhaps the
  car order differs.

**The helper link** (MashSecond as MashFirst's helper):
- **Which train.** The engineer's client names the train in its
  `0x461F` type-4 answer, and the server passes it on unchecked. MashFirst's
  client named train 3, the ED9M: the train it had last been in, which the
  server had long given back to MashSecond. The helper loaded into that
  train, which was nobody's for his new session. `0x461E` (kind 4) came
  only for the engineer's own trains, 2 and 1. The first type 2 had gone
  to the ДСП and was withdrawn 10 s later.
- **Traffic over the relay:**
  - engineer → helper: `0x4620` twice a second for each of his trains,
    `0x4624` (the whole cab) every 60 s for each;
  - the cab sound `0x463A` (slot 1) goes through the server, to everyone
    near;
  - the helper asked for the cabs with `0x4623` mode 1, train by train.
- **`0x4622` both ways** (26 from the helper, 19 from the engineer):
  - kind 3, owner 1: a cab switch. The ChS4z's 100 (positions 1 / 12 / 13),
    101, 103 (0 / 1 / 2), 106, 109, 110, 119;
  - kind 1, owner 1: a cab door (`ActDoor`, `Cabin+172`), here 3 and 5;
  - kind 1, owner 3: a door of the car itself, the one walked through
    (car `+328`);
  - "position" is where the control goes (`SetState`), "value" where it was;
  - the two floats are its animation value and frame.
- Cab doors and the car's doors thus reach the helper. The passenger doors
  do not move for him:
  - the engineer's door buttons (the ED9M's cab switches 25, 11, 5, 42, 34)
    arrive as `0x4622`, so the helper's DLL gets `CanSwitch` / `Switched`
    and the buttons move;
  - the doors themselves the DLL moves in `Run`, which never runs on a
    helper's client;
  - when the helper entered the cab again, his client asked for the whole
    cab (`0x4623` mode 1, a 37 KB `0x4624`), and the doors jumped to the
    engineer's state: open;
  - the next close and open showed no animation again;
  - PROBABLE: the 60-s `0x4624` rewrites the DLL's state but does not move
    the doors by itself.

  Other players get no doors at all.
- No `0x4632` (a mouse drag) was seen.

**Uncouplings.**
- MashFirst cut his train twice:
  - `0x460E`: train, car (1, the first car of the part), local id
    (100244, 100245), side;
  - the server answered him with `0x4610` (local → 1, then → 3);
  - the others got `0x460E` with the new id, then the `0x4603`s.
- The part keeps kind PLAYER and the owner. The ТЧД showed the cut ED9M as
  MashFirst's, and MashSecond did not get it back.
- The two consoles disagreed on the part's name and number:
  - the ТЧД showed "Отцеп от 123-first-1" and no number;
  - the ДСП showed number 123 and "123-first-1", its own split of the copy;
  - after a new login the ДСП showed number 0.

  No `0x461A` or `0x4605` follows a split.
- The client keeps a part it cut off as its own train: after the cuts
  MashFirst's client kept sending the parts' lights (`0x4617`) and their
  positions when they stopped (`0x4603`).

**Car menu.**
- A chock on another player's train works: `0x4640` (train, car, `u32` 2,
  flags `0x100`, `f32` 1.0) is relayed with no owner check.

**Signals and junctions from the console.**
- A junction is `0x460A` from the console, relayed to players.
- **Setting a signal is `0x4618`**, not `0x460C`. The fields:
  - the object index into the server's signal table (1 406 entries in a
    console's snapshot);
  - the state `u16`: the signal type's state, e.g. 1 stop, 7 CLEAR2 for НМ11;
  - **the mode `f32`**:
    - 0 off: the server holds the state the console gave ("close" gives 1,
      "set an aspect" the aspect, with `u8` 1);
    - 1 on: the signal chooses its state by its own logic. "Open" on НМ11
      stayed red. The ТЧД's "Сигналы" menu with "Включить" for type
      `t_stop_r` switched every signal of that type at once: 276 `0x4618`
      to the ДСП, 192 `0x460C` to the players, states 2 (43 or 60 km/h)
      or 1. Again with "Отображать как поездной" ticked, it sent only 212
      `0x4618` with the same fields and nothing to the players: that tick
      is not in the protocol (PROBABLE: how a console draws the signal);
    - 2 automatic block;
  - `u8` 1 when the dispatcher picks an aspect.
- The server takes a manual state at once (`0x40FB90` → `0x40C714`):
  - the heads that have that state get it, its drawing and a speed;
  - the other heads get drawing −1;
  - the players get `0x460C`, the consoles `0x4618` back.

  Under automatic block the server chooses the state (red here) and sends
  mode 2.
- **`0x460C`, corrected:**
  - index;
  - `u16` `+78`, 0 in every package of the session, UNKNOWN;
  - the state `+84`;
  - the drawing `+88`;
  - `+92`;
  - the mode `+96`;
  - the speed limit `+100`, m/s, −1 none (11.9 for M96 in state 2).

**A train the server drives** (the ChS4z train 5, set to TRAFFIC on the
ТЧД):
- **Making it TRAFFIC:** `0x461A` and `0x4605` with kind 2.
- **The console's commands:**

  | Button | Package |
  |---|---|
  | "Изменить направление" | `0x4506` (train flags `+79` bit 2) |
  | "Начать движение" | `0x4505` 1 (`+79` bit 1) |
  | "Задать скорость" (5 km/h, top 150, acceleration 1, deceleration 2) | `0x4508` 1.389, 41.667, 1, 2 |
  | "Удалить из мира" | `0x4602` with flag 0: the clients take the train off the track and keep it |
  | placing it again | `0x4603` and `0x4606`, no new `0x4607` |

  Flag 1 of `0x4602` deletes the train ("Deleting", below).
- **Assembling its route on the ТЧД:**
  - the server throws the junctions (`0x460A` to everyone);
  - it switches the route's signals on: mode 1, states 1 to 3, 25
    `0x4618` to the ДСП and `0x460C` to the players;
  - it sends `0x4619` with the length: 9 572.81 m.
- **The server moves the train itself** and sends its `0x4603` every
  **0.5 s**, not every 1.5 s:
  - it speeds up at the given 1 m/s²;
  - it held 11 m/s at a 43-km/h signal and then went on speeding up;
  - so the 5 km/h of "Задать скорость" is not a target. PROBABLE: the
    speed the train is given at once.
- **`0x4619` is the distance left to the route's end:** 9 572.6 → 9 475.6
  m as the train ran, every 0.5 s. Earlier it grew because the train was
  moving away from the end.
- Signals behind the train switch: `0x460C` to the players, `0x4618` to
  the ДСП.
- A train with chocks and a handbrake set ("Закрепить состав") did not get
  going: the server braked it at the deceleration (2) from the first
  `0x4603` (0.39 m/s, −2 m/s²).

**The console's routes** ("Сборка маршрута поезда"):
- **The window.** A train's route is a chain of segments: the station's
  routes and the hauls from the route database (`.rtsdb`). In the session:
  - Краснодар-2: Н → НМ1 [П], 860 m;
  - НМ1 → НМ1С, 412 m;
  - НМ1С → Н1 [П], 199 m.

  The controls:
  - "Задать" appends the chosen segment;
  - "Отменить" takes one off;
  - "Направление" changes the direction "from the last point of the route";
  - the number is the priority;
  - "Станция" is a toggle that keeps the console's map on that station
    (no package).
- **`0x450B`**, one per segment:
  - the id: `0xFF0000nn` from the ДСП, the server's id in its echo;
  - the train id;
  - the station route's id in the database (639, 648, 927, 1083…);
  - the station's name (`u16 n` + UTF-16: "Краснодар-1", "Краснодар-2");
  - two `u16`, around −4545 / 14592: a check, the tile of the station
    route's start point; a segment whose values differ is refused
    (`0x454194`);
  - the direction `u8`: which end of the station route it starts from
    ("Направление"); then two flags, PROBABLE "hold" (the train's next
    segment waits while this one is set) and "reversed" (the train runs it
    backwards: it goes into the train's direction on it); all three 0 in
    the session;
  - the length `f32`, m;
  - the priority `u32` (0, or 2 from the window's field);
  - the state `u8`, the junction list `u16` (index + 1) and the number of
    its junctions thrown so far `u16`, as in `0x450E`.
- **From the ДСП** the console sends `0x450B`. The server answers:
  - `0x450B` with its id;
  - `0x450C`: the request id, the new id, ok `u8` 1, no text;
  - `0x450E`: id, state, junction list, priority, length, junctions
    thrown.
- **How the server sets segments** (`0x46A348`, `0x41F520`; from the code):
  - all segments wait in one list ordered by priority, each train's own in
    their order. A segment is taken when the train's previous one is set
    (state 1 or 3) and does not hold;
  - it is checked first (`0x469804`): the train exists; on a stretch, no
    route for an oncoming train ("На перегон задан маршрут встречному
    поезду"); a path from the previous segment's end ("Не найден путь от
    предыдущей точки пути до нового маршрута"); an occupied station track
    asks the ТЧД's operator ("Путь … занят поездом. Все равно задать
    маршрут?"), while a console's request skips the question;
  - then the junctions are thrown one by one (state 2, the count in the
    last `u16`), the signals switched on (state 1), the train marked on it
    when it enters (3) and the segment dropped when it has left (4);
  - PROBABLE: the priority decides which waiting segment gets its
    junctions first; nothing else reads it.

  **From the ТЧД** the server sends `0x450B` and `0x450E` to the consoles.
- **"Задать" sets the segment at once,** from either console:
  - the junctions (`0x460A`);
  - the signals on it switched on (mode 1), `0x4618` to the consoles and
    `0x460C` to the players: the entry signal of Краснодар-2 opened;
  - `0x4619`: the distance left to the end of the whole route, which grows
    with each segment (5 943.6 → 7 343.6 m).

  A segment for the ChS4z that did not go on from where the train stood
  was accepted (`0x450C`) and set nothing (PROBABLE: it waits).
- **Cancelling:**
  - from the ДСП: `0x450D` with the id; the server echoes it and answers
    `0x450C` (id, 0, ok);
  - from the ТЧД, the train's whole route: `0x450D` for every segment to
    the consoles, `0x4619` −1 to everyone, the route's signals closed
    (mode 0, state 1). The junctions stay as they are.
- **Speed limits of open signal states**, seen in `0x460C` on Kuban:
  state 2 11.9 m/s (43 km/h), 3 17.5 (63), 6 23.1 (83), 7 none (−1).
- **"Выключить" for a signal type** (`t_stop_r`, the ТЧД's "Сигналы"):
  212 `0x4618` with mode 0 and the states kept, nothing to the players. The
  heads keep what they show.

**Messages and pause.**
- A message from the ТЧД goes to its addressee only: `0x4611`, `u32 n`,
  UTF-16 text ("Отправил....", `\r\n`, more text), a zero.
- Pause: `0x4616` 1 to everyone, the console too; 0 lifts it. For the
  342 s of pause **not one package was sent**: no positions, lights,
  sounds, time, substation loads, nor the helper link.

**Deleting.** `0x4602` with flag 1 deletes a train (trains 1, 2, 4, 5 were
gone). The ДСП sent every such `0x4602` back to the server 0.3 s later
(PROBABLE: the shared code repeating the command, harmless).

**A train's route from the console** (`0x4619`):
- it is the length of the route in metres: 128.11 to a red exit signal,
  26.34 for a short one. The route is laid from the head along the current
  junctions; junctions are set separately;
- the server recomputes it while the train moves and sends it to everyone,
  players too, every 0.5 s when it changes (41.6 → 46.2 m);
- "cancel" sends −1; the server sends nothing back.

**A derailment** (MashFirst's VL80, train 4, two sections, at 30 m/s):
- The owner sent `0x4636` four times only: at the derailment (13015.2 and
  13015.4 s, one section, then both) and with its `0x4603` while the train
  still moved (every 1.5 s). The train stopped at 13017.5 s and nothing more
  came, though the second section was still tipping over (0.92 of
  0.98 rad).
- Every client runs the falling of every derailed car itself, from the last
  placement it got. The two players' sections came to rest differently, and
  nothing evens them out: the protocol has no final placement.
- The consoles have no "put back on the rails". "Удалить из мира" and
  placing the train again do it: both players saw the VL80 on the rails.
  No `0x4637` and no new `0x4636` were sent.

**Removing and placing a player's own train crashed its owner** (the
derailed VL80; MashFirst's `rts1.exe` wrote a dump):
- `0x4602` with flag 0 clears the train's `+4` bit `0x10` and zeroes its
  head's track node (`+192`; client `0x4C3DE0`). The frame loop
  (`0x424488`) skips a train without `0x10`, so the client lived on.
- Placing sent `0x4603`, `0x4606`, `0x4603` (13217.98 s). The owner skips
  `0x4603` for its own train. `0x4606` sets `0x10` again and rebuilds the
  head from the train's position (`+232`, the one `0x4603` writes).
- Within 0.35 s the client was dead: its relay stopped between 13217.79 and
  13218.32 s, and the `0x4623` that came later found it gone.
- The dump: an access violation reading `0x24` at `rts1.exe+0x80124`. The
  look-ahead for signals (`0x508520`) walked the track from the head
  (`0x4800C4`) with node 0 and offset 41.26 m, exactly where the VL80 had
  stopped.
- Why the head still had no node after `0x4606` is not proven. Either the
  position had none either (the owner never takes `0x4603` for its own
  train), or the frame ran between `0x4606` setting `0x10` and its copy. A
  TRAFFIC train lives through the same buttons (the ChS4z above): its
  `0x4603` comes before `0x4606` and is taken.
- A server that keeps RTS clients alive places an owned train with
  `0x462F`, which reaches the owner's own train too, or does not remove it
  from the world.

**A player's reconnect** (MashFirst after the crash, "Restore player
position" and "Restore train state" ticked):
- **"Подключиться"** sends the login (`0x4600`). The server answers and
  closes the old connection of that nickname, which was still open (the
  crashed process). For the menu it sends `0x4601` with train 3.
- **"Зайти"**, minutes later, asks for the snapshot (`0x4608`); it comes in
  0.2 s:
  - the root, the time, the junctions (8.3 KB) and the signals (62 KB);
  - the electrification, to a player too: `0x4646` for each substation,
    `0x4648`, `0x4645`;
  - three trains (3, 1, 4), each `0x4607`, `0x4603`, `0x4606`;
  - `0x4601` for trains 3 and 4: both MashFirst's (numbers 101 and 266);
  - MashSecond's position (`0x462D`, in no train). No `0x462E`, the
    player's own included;
  - `0x4608`, then `0x461F` type 2: the helper link is back.
- **The player's place:** MashFirst came back where the crash had found
  him, though nothing in the snapshot named it: the client read it from
  its recovery file (above).
- The VL80 came without `0x4636`: placing it had already ended the
  derailment. No names (`0x461A`) and no route lengths (`0x4619`) came
  either.
- **The others got part of the snapshot too:** while it was built,
  MashSecond and the ДСП got the `0x4603` of all three trains and the
  ED9M's `0x4613`, the dispatcher's brake values from before. A reconnect
  set those pressures on everyone's ED9M again.
- **After entering** (53 s on) the client:
  - answered the helper link: `0x461F` type 4, with train 0;
  - sent all its lights at once: 1 877 `0x4617` in 0.2 s, 73 KB (1 846 of
    them for the 8-car ED9M, 31 for the VL80).
- **The helper link did not come back:** MashSecond did not see the cab.
  - The old link closed when the crashed process went (14061.95 s).
  - MashSecond's node then knocked at the engineer 18 times in 221 s with
    the old link's session: every 12 s at first (10 s and a 2-s failed
    connect), more than the 5 attempts read from the code.
  - When the engineer listened, it was joined and dropped at once.
    PROBABLE: the session no longer matched the new one in `0x461F`. No
    new attempt followed.
- **The VL80's state** fell by the server's timers while its owner was
  away: `0x4605` to everyone with "Включен" at 13503.89 s and "Холодный"
  at 14223.94 s, 480 and 1 200 s after it stopped. Nobody pressed
  anything.

**Seen from openRTS's observer (2026-11-06, `net.md`).**
- **A route given by hand and an assembled one open the exit signal
  differently.** "Задать маршрут" opened it white, the assembler
  ("Собрать маршрут..") green; the observer showed the same as RTS.
  PROBABLE: the hand route is only the length laid from the train along
  the junctions (`0x4619`), its signals get states 1 to 3 (state 1
  `STOP_AND_PROCEED`: the white invitation), while an assembled station
  route is set by the interlocking with a full aspect.
- Cancelling a route sent 5 `signal_head` (closed) and `route_left` −1, no
  junction: they stay as they were. Assembling one sent 9 `junction` and
  39 `signal_head`.
- `route_left` reaches the players too, every 0.5 s while it changes.
- A player's client sends `train_pos` about every 1.5 s, and one out of
  turn when the train stops or starts (0.06 s after the last).
- The ТЧД's players table: "Отклик" is the seconds since the server last
  heard the user (`0x41D114`: now − the user's `+104`, set on every
  package received); no code was found that drops a user for it. "Вход"
  is the user's `+144`, whose writer was not found; PROBABLE packages in.

## Still UNKNOWN

- The first optional block of `0x4638` (`+196`) and three values of a
  `0x463B` stream (`+20`, `+36`, `+40`).
- What `0x450B`'s second and third flags do (PROBABLE "hold" and
  "reversed") and whether the priority does more than order the queue.
- Why relayed coupler actions can miss on another client. The action is
  carried out directly on the cars named by train and index (`0x4C3DE0` →
  `0x463F84`), not through that client's menu, so the menu's state does not
  matter. PROBABLE: the cars' order or their end flags differed after the
  relayed coupling. Not proven.
- Why the reconnect's snapshot held a "Отдельные пути" setting (`0x4645`,
  node 1592) that nobody made in the session, as far as known.

**Answered by the static reverse after the capture (2026-11-05):**
- `0x4633` comes only from a mini-server host, `0x4612` from the
  receiver itself, `0x4637` from nobody (the rows above);
- `0x460C`'s `+78` is the semaphore arm's position;
- `0x461A`'s second string is the train's notes;
- `0x4645` is a piece of track's own supply, not a phase;
- `byte_54C807` marks the user list busy: the ТЧД operator's player menu,
  a player action or a message (`0x41E7F0`, `0x41EA08`, `0x452318`). While
  it is set, the main thread leaves helper answers and `0x4623` in the
  queue for a later pass; nothing is lost;
- a part a client cuts off stays its own train: in a network game the new
  part takes its parent's kind (`0x469858`), and the client keeps sending
  its lights and, when it moves, its position.
