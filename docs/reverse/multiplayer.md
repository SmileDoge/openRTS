# RTS multiplayer: the client, the player mini-server, the dispatcher server

Analysed read-only on copies, 2026-11-03:
- `rts1.exe` (`build/ida/rts1.i64`);
- `rts_server.exe`, the user's console build (`build/srv/rts_server.i64`);
- `rts_server_DSP.exe` (`build/dsp/dsp.i64`, see `dispatcher.md`).

The network logs of the user's server were used for counts only. They hold real players' addresses and nicknames and are not copied here.

Every package id with its payload: `multiplayer-protocol.md` (2026-11-05).

## Three programs, three roles

| Program | Role |
|---|---|
| `rts1.exe` | Has three roles: <ul><li>the player client;</li><li>the **player mini-server** ("Player-mini-server"), started from the in-game menu in single player;</li><li>a **peer node** for the helper (assistant driver) link.</li></ul> |
| `rts_server.exe` | The dedicated server with the dispatcher console built in (`TfPult`, "RTS Dispatcher"), default port 800. It links RTS's own simulation code (train physics, path module, signals, DLL exports such as `_SpeedLimit`). It is the authority for: <ul><li>junctions, signals and time;</li><li>train kinds and assignments;</li><li>trains it drives itself: TRAFFIC trains, with positions every 0.5 s (CONFIRMED by the live capture of 2026-11-05).</li></ul> |
| `rts_server_DSP.exe` | The same source built as a dispatcher client of `rts_server.exe` (`dispatcher.md`). |

## Transport and framing — CONFIRMED

**Transport.** Plain TCP over WinSock 2, driven by `WSAAsyncSelect` on hidden windows:
- "RTS Simulator Listener";
- "RTS Server Listener";
- "Player listener window";
- "RTS DSP Listener";
- "RTS Network Node \<name\>".

There is no UDP.

**A package:**
- `u16 id`;
- `u32 length` (the whole package, the 6-byte header included);
- the payload.

Payloads nest packages with the same framing, e.g.:
- `0x4603` holds `0x4604` per car;
- `0x4620` → `0x4621` → `0x4628` → `0x4629` / `0x462A`.

Data is little endian. A string is `u16` characters, then UTF-16 with its terminating zero.

**Limits:**
- the server reads a login of at most 64 bytes;
- after login a user's package is at most `0x3C00` (15 360) bytes;
- otherwise the server disconnects the user (`0x47FF`);
- the client takes up to `0x1388000` bytes from the server (the startup snapshot);
- a NetworkNode takes at most `0x1000` bytes from a link that has not identified itself.

The login reply and the error replies have no length field (below).

## Login — CONFIRMED

Client → server (`rts1.exe` `0x4C1818`, `rts_server.exe` dispatcher side `0x44D904`):

| Field | Value |
|---|---|
| `u16` | `0x4600` player (`rts1.exe`), `0x4500` dispatcher (`rts_server_DSP.exe`) |
| `u32` | `12 + 2n` |
| `u16` | protocol version, 101 |
| `u16` | `n`, nickname length, 1..15 |
| UTF-16 | nickname, zero-terminated |

The address is "host:port":
- `rts1.exe` resolves it to IPv4 or IPv6;
- the dispatcher resolves IPv4 only, port 800 when none is given.

Server → client on success (`rts_server.exe` `0x4436C8`, the mini-server `0x4D0D8C`; read by `0x4C1B64`), with no length field:

| Field | Value |
|---|---|
| `u16` | `0x4600` |
| `u16` | 101 |
| string | a description: route title, "Сценарий: …", "Сезон: …" |
| `u8`, `u8` | season, weather |
| string | route id |
| `4 × u32` | route counts: junctions, signals and two more. PROBABLE: electrosections and substations (`rts1.exe`: "Route count of electrosections doesn't match server's") |

The client checks the route and the counts (`0x47A1A0`). On a mismatch it shows "you don't have the required version of the route".

On an error the server sends only a `u16` code and closes the connection:

| Code | Meaning |
|---|---|
| `0x4701` | nickname busy |
| `0x4702` | <ul><li>the server takes no new players, or no new dispatchers: a separate switch for each kind;</li><li>on the mini-server: the name is not in its player list.</li></ul> |
| `0x4703` | startup data refused: already sent (a second `0x4608`), or the player has no train assigned yet |
| `0x4704` | the first package is neither `0x4500` nor `0x4600` ("Probably wrong engine version") |
| `0x4705` | protocol version is not 101 |
| `0x4706` | packet flood (the server's packet limits), then disconnect |
| `0x47FD` / `0x47FE` / `0x47FF` | disconnect: receive error / body cut short / bad length or a refused package |

**Reconnect.** On the dedicated server a login with a nickname it already holds is not "busy": the new socket is attached to that user ("User N reconnected"). The main thread checks the name within 12 s (`0x442A94`, the ban list too). The mini-server answers `0x4701` while that name is connected.

**Startup data.** After the reply the client sends `0x4608`. The server takes it only from a player it has assigned a train (`0x44ECAC` sets the user's `+41`) or one coming back to a game it was in (`+92` bit 1); otherwise it answers `0x4703` (`0x4436C8`). The client's menu waits for the assignment before "Зайти". The server sends a snapshot of its world:
- `0x4600` (root);
- `0x460D` time;
- `0x4609`, then `0x460A` per junction;
- `0x460B`, then `0x460C` per signal;
- electrification `0x4646`..`0x4648`;
- pickups and global objects `0x464B`..`0x464E`;
- trains: `0x4607`, `0x4603` (`0x4606`) and cargo `0x461C` each;
- players `0x462D`;
- `0x4608` to end it.

The mini-server sends the same (`0x4D14F0`). `rts1.exe` writes it to `multdata` and loads the game from it (`0x4CAEAC`).

In the menu the client then waits ("Waiting for assignment…"):
- `0x4601` assigns it a train ("Received assignment!!! Train number");
- `0x461F` type 1 makes it a helper;
- `0x461E` means "Waiting for synchronization with engineer".

## The client in play (`rts1.exe` with `rts_server.exe`) — CONFIRMED

**Connection.**
- The socket of the menu login stays open in play: window "RTS Simulator Listener", log `logmult.txt`.
- `0x460A`, `0x460C`, `0x460D` and `0x4616` are applied in the network thread as they arrive; the rest is queued for the main loop.
- **When the socket closes** (`FD_CLOSE`), the client queues a `0x4612` of its own (`0x4CB390`). Its handler:
  - shows "Server down, simulator will now quit";
  - writes the multiplayer recovery file (`0x47B09C`);
  - quits.

  There is no reconnect in play: any dropped connection ends the game. The next login can restore the position and the own trains' state from the client's `multRecoveryData.dat` (`multiplayer-protocol.md`, "The player's recovery file"). Seen on 2026-11-03 when the test proxy was restarted.

**The player's own train (kind PLAYER):**
- simulated locally: physics, DLLs, cab;
- while it moves, `0x4603` goes to the server at most every 1.5 s (`0x5DC` ms, `0x424488`), and once more when it stops;
- the payload:
  - train id, speed (+92), acceleration (+96), position, +272, +292;
  - per car a `0x4604`: an `f64` position and two floats.

**Every other train:**
- taken from the `0x4603` the server relays;
- between updates its position is extrapolated with that speed and acceleration;
- after 5 s without an update the speed and acceleration drop to 0.

The client never takes the server's position for its own train, except `0x462F` (a forced position).

Client → server events:

| Id | What |
|---|---|
| `0x460A` | junction: index, branch, lock (handled by the server at once, `0x40C52C`) |
| `0x460E` / `0x460F` | uncouple / couple |
| `0x461B` | a car menu action: the dedicated server relays it to the others, every client carries it out (the mini-server's host carries it out itself) |
| `0x4617` | a car light's state ("Other players' trains", below) |
| `0x4639`, `0x463A`, `0x463D` | a car's sound: a discrete trigger, the whole state, the variables (below) |
| `0x463C` | a car's pantographs and a diesel's engine (below) |
| `0x4623` | mode 2: asks a train's owner for its state; the server forwards it to the owner (below) |
| `0x462D` | the player's position, the person icon of Ctrl+F7 (relayed with the user id and name added) |
| `0x4636` | a flag per car (`+32 & 0x20`, PROBABLE: derailed), then a `0x4638` block (a placement) for each car with it; sent with `0x4603` while the train flag `+5 & 0x10` is set (`0x4CDF70`, `0x57FF64`). The block's fields are UNKNOWN in detail |
| `0x4649` | CONFIRMED (2026-11-04, client `0x4CF464`): a substation's load — substation, phase (int16), the energy summed over the window, its length in s (`0x5B6030` sends it every 2 s while the line is loaded); forwarded to dispatchers only |
| `0x4640`, `0x4641`, `0x461D` | UNKNOWN |

Server → client:

| Id | What |
|---|---|
| `0x460C` | a signal's aspect (index, aspect, …) |
| `0x460A` | a junction's branch and lock |
| `0x460D` | time of day |
| `0x4605` | a train's kind, subtype, number (car numbers, bits 18+), start state — PROBABLE |
| `0x4613` | brake pressures of a train other than the player's (four values per car). PROBABLE: what the dispatcher's "Тормоза" form sets |
| `0x4610` | a train's id changed (after coupling; see "Coupling in play") |
| `0x4611` | a dispatcher's text message ("Dispatcher") |
| `0x4616` | server suspended (pause) |
| `0x4601` | train assignment |

## The dedicated server (`rts_server.exe`) — CONFIRMED unless marked

**Threads and connections:**
- a listener thread (`0x444658`: bind and listen; `0x442E8C`: accept);
- one thread per connection ("Started thread %i for connection from address %s");
- a send queue and thread per user ("PlayerOut semaphore %i");
- an address that keeps failing is blocked; there is `banlist.txt`.

**Login.** The first package fixes the kind: `0x4500` is a dispatcher (kind 1), `0x4600` a player (kind 0).

**Rights:**
- players' `0x4508`, `0x4509`, `0x4602`, `0x4605`..`0x4607`, `0x4613` and `0x461A` are dropped;
- a dispatcher needs a permission bit besides its kind;
- `0x4645`..`0x4647`, `0x464A` and `0x464B` (electrification, pickups) come from dispatchers only;
- `0x4648` is dropped from anyone.

**Rate limits.** Two counters per user: all packages, and `0x4639` / `0x463A` / `0x463D`. They are checked against the packet limits of the server settings. Over the limit the server answers `0x4706` and disconnects.

**Routing** (`0x4436C8`, sent by `0x442E8C`). A package goes to the main thread (the server's world), or is relayed, or both. A relayed package is sent to one of five audiences:

| Audience | Who gets it |
|---|---|
| 1 | one user |
| 2 | everyone in play except the sender |
| 3 | users of one kind (dispatchers) |
| 4 | everyone except the sender whose player position (`0x462D`) is within 3 000 m of the train the package names (`0x442E10`). A package naming no known train is not sent |
| default | everyone in play |

Which ids go where:

| Ids | Relayed to | Server reads them |
|---|---|---|
| `0x461B`, `0x461D`, `0x463F` | audience 2 | no |
| `0x4617`, `0x4639`, `0x463A`, `0x463C`, `0x463D` | audience 4 | no |
| `0x461C`, `0x4636`, `0x4640`, `0x4641` | audience 2 | yes |
| `0x4649` | audience 3 (dispatchers) | yes |
| other ids | not relayed | main thread only |

Special cases:
- `0x4649` goes to dispatchers only;
- `0x4608` asks for the snapshot.

The main loop is `TfPult::MainTimerTimer`, "main dispatcher function".

**Dispatchers** get the world packages too, and their own `0x4504`..`0x450E`. PROBABLE: player and train lists, `0x450A` the trains for the map.

**Network log.** It is written with the "lognetpackages" / data trace setting (the menu item "Data trace", `TfPult_mnDataTraceClick` → `0x417064`) and hides frequent packages unless the trace level is raised (`0x46BB98`). The level (`word_53D27C`) is 0 in the exe and nothing writes it (CONFIRMED, 2026-11-05), so the lists below are never logged: a full capture needs a proxy (`multiplayer-protocol.md`, "Live capture"):
- level 1 or more: `0x4603`, `0x4606`, `0x4607`, `0x4617`, `0x4620`, `0x4623`, `0x4624`, `0x4630`, `0x463C`;
- level 2 or more: `0x4609`, `0x460B`, `0x460D`, `0x4618`, `0x4619`, `0x4622`, `0x462D`, `0x462E`, `0x4632`, `0x4639`, `0x463A`, `0x463D`, `0x463F`.

That is why the user's logs show no train positions. One 8-hour session of about 50 users:
- clients sent a few hundred `0x460A` each and little else;
- the server sent 220 000 `0x460C`, 83 000 `0x460A`, 15 000 `0x4613` and 8 000 `0x4605`.

## Coupling in play, two players' trains too — CONFIRMED unless marked (2026-11-04)

Read from `rts_server.exe` and `rts1.exe`. A and B below are two players: A's
train runs into B's.

**Train kinds.**
- On the server (`+296`): 0 STATIC, 1 PLAYER, 2 TRAFFIC, 3 LOOSE. PROBABLE
  numbering: it follows the console's list and the server's use below.
- On a client, another player's PLAYER train becomes kind 2 with flag
  `0x10000` (the `0x4607` handler in `0x4C3DE0`). Its position comes from
  `0x4603`.

**1. The client of the train that runs in notices it** (its physics calls
`0x469ED8`):
- its own train (kind 1) always takes the other in: if the other is its own,
  the two swap places;
- first it waits, pumping the network, while either train has flag `0x800`:
  at most 500 × 50 ms (`0x4CDF30`). `0x4610` clears that flag. PROBABLE:
  `0x800` marks a train waiting for an id from the server;
- it couples locally and sends `0x460F`: its train's id, the other's id, and
  one byte for each train's end.

**2. The server** (`0x40FB90` case `0xD` → `0x4501E0`):
- the other train's cars join the sender's train at that end, turned where
  needed, positions recomputed; every car now belongs to the sender's train;
  the other train is deleted (`0x457F44`) and leaves the console's lists;
- **two players:** when both trains are PLAYER with different owners and B is
  connected, the server unassigns B: `0x4601` with a 0 byte to B
  (`0x44ECAC`);
- `0x460F` (the merged train's id, B's old id, the two end bytes) to everyone
  but the sender (audience 2);
- two players only: `0x4610` (B's old id, the merged train's id) to B
  (audience 1);
- `0x4603` / `0x4604` of the merged train to everyone but the sender
  (`0x44F2F0`).

**3. Every other client** repeats the coupling from `0x460F` (`0x469ED8`
with `0x631E76` set: no swap).

**4. Player B:**
- B's train goes into A's, which on B's client is another player's train
  (kind 2): its position now comes from A's `0x4603`;
- the client's player train (`0x61D680`, PROBABLE) moves to the merged
  train, which gets flag `0x20`. B stays in their cab; the train is moved by
  A's simulation. PROBABLE: nothing carries B's controls into it (the cab's
  switches go to a helper only, `0x4622`; the others get lights,
  pantographs and sounds, "Other players' trains");
- the in-game handler reads `0x4601` too (`0x4C3DE0`; it reaches the case
  by subtraction, so a search for the constant misses it). Earlier notes
  said it had none. CONFIRMED by the live capture of 2026-11-05: the
  server sends B this `0x4601` ("not yours") **before** the `0x460F`. B's
  client makes its own train another player's (kind 2), so its DLL stops
  (controller, doors, КЛУБ), and the train then moves by extrapolation
  (`multiplayer-protocol.md`, "Live capture");
- `0x4610` names B's old train, which is already gone: the client logs
  "not found" (event 5) and nothing changes.

**Not done by RTS:**
- no speed is computed for two players' trains: the cars are joined as they
  are;
- B does not become a helper: only the dispatcher assigns helpers;
- the uncoupling later is `0x460E`, and the server picks the cut part's id
  (the client reads it into `0x631E78` before splitting, `0x469858`). Who
  owns the cut part is not traced.

**The server's own coupling** (`0x40DCDC`; the server moves LOOSE trains
itself, a static train pushed without coupling becomes LOOSE):
- a moving train reaching a STATIC or LOOSE one takes it in at the speed
  `(v1·m1 + v2·m2) / (m1 + m2)` (PROBABLE: `0x458E80` is the mass);
- reaching a PLAYER or TRAFFIC train, it is taken in by that train, and the
  `0x460F` goes to everyone. `version.txt`: "a PLAYER or TRAFFIC train
  absorbs the LOOSE one";
- PROBABLE: a LOOSE train standing for 30 s becomes STATIC.

**Both clients at once (PROBABLE).** When both trains move towards each
other, both clients can notice the coupling:
- each couples with its own train in front and sends `0x460F`;
- the server carries out the first. The second names a train that no longer
  exists, and `0x458EC0` finds nothing;
- on B, the server's `0x460F` names A's train, which B has already taken in,
  and is dropped too (event 5). `0x4610` then renames B's merged train to
  A's id: this looks like what `0x4610` is for;
- that merged train is still kind 1 on B: both clients would simulate it and
  send its `0x4603`. Whether the server drops positions from a player who
  no longer owns the train is not traced.

## Other players' trains: lights, pantographs, sounds — CONFIRMED unless marked (2026-11-05)

Read from `rts1.exe` and `rts_server.exe`.

**A remote train has nothing of its own.** On a client another player's
train is kind 2 with flag `0x10000` (above). `Run` skips kinds 2 and 4
(`0x424488`), and the regroup needs flag 4. So the train has no DLL and no
library sounds: its owner sends what it looks and sounds like.

**Who sends.** The owner's client sends in the simulation step
(`0x424488`), for each of its trains with flag 4 that is not of kind 2 or 4:
its own train and the parts cut off from it. Per car:

| Id | When | Payload | On the receiving client |
|---|---|---|---|
| `0x4617` (`0x4CC864`) | a light changed: its lit bit, colour or radius (`0x566314`) | train id, car index, light index (`u32` each); lit (`u8`); radius (`+88`), colour (`+8`), position (`+12`, three floats) | sets the light's "on" bit (`+0` bit 3) and copies the rest; the light fades in or out by itself (`0x4FF9B8`) |
| `0x463C` (`0x4CEE44`) | at most every 250 ms, when the car changed (`0x566520`) | train id, car index, then `u8` car kind (`+12`), `u32` `Flags` (`+28`), `u32` `+32`, then `u8` each: `+14`, the raised-pantograph mask (`+13`), "running" | the pantograph mask, then `0x504584` when `Flags` bit 1 is set; for a diesel (kind 2) "running" clears the cold bit (`LocoFlags` bit 3), else sets it. `Flags`, `+32` and `+14` are skipped |
| `0x463D` (`0x4CE5C8`) | at most every 200 ms, when they changed (`0x566384`) | train id, car index (`u32`), sound slot (`u16`), Var1..8, the distance travelled (`+284`), the volume (`+308`) (`f32` each) | copied into that sound object (`0x4CED34`) |
| `0x4639` (`0x4CE870`) | every discrete trigger in the car's sound queue (96 codes) while the object is active (PROBABLE: object flag `+4` bit 0) | train id, car index (`u32`), slot (`u16`), `1` (`u16`), the code (`u32`), `0` (`u32`) | the code goes into that sound object's queue (`0x4A9F48`); the last `u32` is not read |

- "Running" is the byte `+208` of the DLL engine (`car+484`) when the car
  has a DLL, else "not cold".
- The slot is always 3 here: the exterior `.sms` (`car+376`).
- The codes in the group's `NonDistributableTriggers` are not sent: the name
  means "not distributed over the network". They also stay out of the flood
  count (`sound.md`).
- The remote car's sound takes its speed and acceleration from `0x4604`.
  The step sets the object's `+204` / `+292` from the car.

**Slots on the receiving side** (`0x4C3DE0`):
- 1, the cab sound (`car+444+16`): for `0x463A` only, and only on a kind-4
  train. PROBABLE: kind 4 is the engineer's train on its helper's client.
  `0x4639` / `0x463D` for a kind-4 train are dropped;
- 2, the object at `car+372+204`: accepted; no sender found;
- 3, the exterior `.sms`.

**The whole state, asked once.**
- When a client loads another player's train (the train flag `|= 2`;
  `0x4232A8`, `0x424190`), it sends `0x4623` (`0x4D0228`): mode 2, the
  train id, two zeros.
- The server (`0x40FB90`) takes mode 2 only. It finds the train and
  forwards the package to the train's owner alone. While `byte_54C807` is
  set (the ТЧД operator's player menu or message is using the user list) it
  leaves them in the queue for a later pass.
- The owner (`0x4C9120` → `0x4D0CA4`) answers for each car:
  - every light (`0x4617`);
  - the exterior sound's whole state (`0x463A`, `0x4CE290`): the
    `0x463D` values, the number of streams, then per stream a nested
    `0x463B` (`0x4CE290` / `0x4CEA38`): its flags, the current trigger + 1
    (0 none), the value it started with (PROBABLE the loop position), the
    stream flags, three values (UNKNOWN, PROBABLE volumes), the trigger
    count and a `u32` per trigger. A client whose sound is active restarts
    the stream on that trigger (`0x4A8C00`); an inactive one keeps them
    for when it wakes;
  - the car state (`0x463C`).
- Mode 1 comes from a helper that loads the engineer's train (kind 4). The
  engineer answers with its whole cab (`0x4D03E4`: `0x4624`…, the cab sound
  as `0x463A` slot 1).

**The server relays** `0x4617`, `0x4639`, `0x463A`, `0x463C`, `0x463D` to
every player within 3 km of the train, without reading them (audience 4,
`0x4436C8`). The 500-a-second limit counts `0x4639`, `0x463A` and `0x463D`.

**Lights.** In a network game the conditions pass (`0x4F07D4`) skips a
kind-2 train with flag `0x10000`. Its lights are only what `0x4617` set.
So the others see the owner's lights as its DLL or its conditions made
them, cycling states included.

**Not sent:**
- the library sounds (USAVP, КЛУБ);
- the cab sound and the cab's switches: they go to a helper only (`0x4622`,
  "Helpers").

**`dword_61D31C+36`** is set while the client plays through its network
thread, connected to a server or to a player's mini-server. PROBABLE: every
sender above queues to that thread when it is set. A mini-server's host
sends its own train straight to its peers.

**For openRTS:**
- **Legacy on:** send and take these as RTS does. A remote car needs:
  - its exterior `.sms` with the owner's variables and triggers;
  - its lights from `0x4617`;
  - its pantographs from `0x463C`.

  openRTS's own trains take the same things from the DLL host; a remote
  train takes them from the packages instead.
- **Legacy off:** the same data in openRTS's own packages, and the library
  sounds can be added.

## The player mini-server (`rts1.exe`) — CONFIRMED

**Starting it:**
- The in-game menu (Ctrl+Alt+F7) offers "Configure server" only while not connected to a server, that is in single player (`0x462FC8`).
- The dialog (`0x534710`, `0x4C3990`) has:
  - start;
  - server name;
  - "only these players", with a list of names.

**The node.** It is a NetworkNode named "Player-mini-server" (type 2, flags `0x1054`). It listens on the first free port of `useportsfrom`..`useportsto` from `config.ini` (by default 801..16000).

**Its handler** (`0x4CA588`) knows one login, `0x4600`:
- version 101, name 1..15 characters;
- a name connected now or equal to the server's own: `0x4701`;
- with a list, a name not in it: `0x4702`;
- a wrong version: `0x4705`;
- a name of 0 or over 15 characters: the connection is dropped.

`0x4608` gets the snapshot of the host's live world.

**The host acts as the server:**
- junction requests `0x4642`, when the junction is not locked or reserved;
- coupling and uncoupling `0x460E` / `0x460F`;
- car menu actions `0x461B`, carried out on the host;
- player positions `0x462D`, relayed;
- train physics and cab packages `0x4620`..`0x4624` and `0x4632` (`0x4C9120`).

A sender thread (`0x5CEA84`) sends the host's world:
- signals `0x460C`;
- junctions `0x460A`;
- time `0x460D`;
- trains `0x4603` / `0x4620`;
- cab state `0x4624`….

Track control's "Assign to player…" sends `0x4601`.

**No dispatcher.** There is no `0x4500` branch, and no `0x45xx` package anywhere in `rts1.exe`: the `0x45xx` constants there are save-game chunk ids.

## Helpers (assistant driver) — CONFIRMED (flow), PROBABLE (details)

`disablehelpers` in `config.ini` turns them off.

1. The dispatcher assigns a helper. The server sends the engineer `0x461F` type 2:
   - add or remove;
   - the helper's id and name;
   - a session.
2. The engineer's client starts its own NetworkNode (type 1, flags `0x1034`). The node:
   - expects that peer;
   - opens a listening port in 801..16000;
   - answers `0x461F` (`0x4CD878`).
3. The server sends the helper `0x461F` type 1:
   - the engineer's name;
   - IPv4 address and port;
   - session;
   - the train.
4. The helper's node connects **straight to the engineer**. It makes up to 5 attempts and reconnects on loss (the code; the live capture of 2026-11-05 saw 18 attempts after an engineer's crash).
5. It introduces itself with `0x1700`:
   - node id: a hash of its name;
   - the session.

   The engineer matches it by these two.
6. Over that link:
   - the engineer streams the train: `0x4620`, with cars `0x4621`, engine `0x4628`, electric / diesel `0x4629` / `0x462A`;
   - the engineer streams the cab: `0x4624`..`0x462C`, `0x4631`;
   - either side sends `0x4622` when it moves a cab control (`0x4CFC74`;
     the other side applies it through `CanSwitch`, `0x4C9120`): train id,
     car index, the control (index `u32`, kind `u16`, owner `u16`: 1 the
     cab, 2 the object at `car+372`; `0x528030`), its position (`+48`), its
     value (`+44`), its animation (two `u32`);
   - the helper sends `0x4623` mode 1 when it loads the engineer's train: a
     sync request, answered with the whole cab ("Other players' trains").

**Who streams — CONFIRMED** (`0x424488`). A client streams when two things hold:
- its train is of kind PLAYER;
- a node exists.

The direction of the link does not matter. The client streams:
- `0x4620` every 500 ms;
- the full cab state (`0x4624`…) every 60 s.

A sender thread does the sending (`0x5CEA84`).

**Which side listens — CONFIRMED.** The engineer always listens (`0x52D184` → `0x52CF80`); the helper always connects out (`0x52D074`). So the engineer must accept incoming TCP on that port: NAT and firewalls get in the way.

## NetworkNode (`rts1.exe` `0x52B6E4`) — CONFIRMED

**The thread:**
- a thread with a message window "RTS Network Node \<name\>";
- semaphores `NNNodeSemaphore`, `NNInSemaphore`, `NNOutSemaphore`.

**Links** have three states:
- 1: accepted, not yet identified;
- 2: identified;
- 4: outgoing.

**Flags** (+92):
- `0x10`: deliver packages of state-2 links;
- `0x20`: deliver packages of state-4 links;
- `0x40`: deliver packages of unidentified links;
- `0x800`: read the normal framing from unidentified links;
- `0x1000`: deliver node events.

**Hello** (`0x52DAD8`): `u16 0x1700`, `u32 14`, `u32` node id, `u32` session. An unknown peer is admitted only when the node accepts anyone.

**Node events** reach the handler as `0x1701`:
- 1: connected;
- 2: connection lost;
- 3: closed;
- 6: reconnected;
- 7: a new link.

**Outgoing links** retry every 10 s up to the attempt limit ("Max connection attempts reached"). Packages can be forwarded from one link list to another (`0x52CB8C`).

## Why the dispatcher cannot join `rts1.exe` — CONFIRMED

1. **Port.** The dispatcher's default is 800, the dedicated server. The mini-server sits on the first free port of 801..16000.
2. **No answer.** At the right port, `0x4500` reaches `0x4CA588` as a package from an unidentified link. There is no branch for it, so nothing comes back: no reply, no error code, the connection stays open.
   - The dispatcher's connect timer polls for a reply that never comes. PROBABLE: it has no timeout.
3. **Nothing to talk to.** Even admitted it would find:
   - none of the dispatcher packages: no `0x4504`..`0x450E`, no handling of the `0x4505` / `0x4506` / `0x4508` / `0x4509` commands;
   - no dispatcher rights;
   - none of the console's server-side logic (routes, interlocking, train forms).

## A data channel between openRTS clients over an RTS server — CONFIRMED (code), not yet tested

Two openRTS clients on an original `rts_server.exe` can exchange data of their own:
- Use a package the server relays unread to audience 2: `0x461D` or `0x463F`, world object state. Up to 15 360 bytes of the server's limit, over TCP.
- Start the payload with 20 bytes that name no world object (u32 id, 12 bytes of position, u32), then put openRTS's own data after them.

How everyone else treats it:
- **Original clients** look the object up by those 20 bytes within 50 m (`0x4E0E4C`). They find nothing, read nothing more and log nothing.
- **The dispatcher** has no case for these ids and ignores them.
- **The server** relays them to everyone in play except the sender. It does not read them.

**Limits:**
- There is no addressing: everyone gets every package, so the target and a marker go inside.
- The server's packet limits apply. Each user is counted in 5 s windows (`0x44563C`); over a limit the server answers `0x4706` and kicks the user. The defaults, which the server settings can change:
  - 1 000 packages a second overall;
  - 500 a second for `0x4639` / `0x463A` / `0x463D`.
- What really costs is the server's upload: every tunnel package is copied to every player in the game.
- The server's full data trace records the bytes.
- `0x461B` would also do (car ids below zero and action 1 do nothing), but it is a real action and best left alone.
- Audience 4 ids need a real train and reach only players near it.

## For openRTS (step 6)

**Legacy mode** means speaking this protocol:
- TCP and the framing above;
- logins `0x4600` / `0x4500` at version 101, the reply and the error codes;
- the startup snapshot;
- train positions every 1.5 s, extrapolated on the receiving side;
- junctions, signals and time decided by the server.

**One server, both roles.** The openRTS server can take players and dispatchers in one program, as `rts_server.exe` does. A player-hosted game is the same server running inside the game; then the dispatcher can join it too.

**Helpers without an open port:**
- **openRTS clients (Legacy off).** There is no peer link at all. The helper's and the engineer's packages go over their normal server connections, and the server forwards them by train. The cost is one more hop of delay and the server's traffic (`0x4620` twice a second).
- **An RTS helper.** It always connects out. The openRTS server can give it its own address and a relay port in `0x461F` type 1 and forward the bytes. A helper never needed an open port.
- **An RTS engineer.** It always listens, but it can be told to connect out instead: the server sends it a `0x461F` type 1 in place of type 2, pointing at a relay. The relay pairs the two outgoing connections by the session of their `0x1700` hellos and pipes them together. Streaming and the helper's packages do not depend on the link direction. CONFIRMED by a test, below.

**Test, 2026-11-03.**
- Setup:
  - one PC: `rts_server.exe` on port 800, two `rts1.exe` from two folders, a VL80s on Kuban;
  - a proxy in front of the server (`tools/mp-capture/mp_proxy.py`).
- Results, by the proxy's mode:
  - **observe:** the helper links straight to the engineer on port 801 and sees the cab;
  - **helper only through the relay:** works;
  - **both sides connecting out, no open port anywhere:** works, cab switches follow both ways.
- Found on the way, CONFIRMED unless marked:
  - The server sends the engineer a `0x461F` type 2 again, with a **new session**, each time the helper logs in (it is also sent at the engineer's own login when the helper was assigned beforehand).

    A second type 1 to the engineer makes it drop its outgoing links (`0x52D4F0`), and the new link was not opened either. PROBABLE: the drop is handled after the new link is made.

    So the relay tells the engineer once and maps later sessions of the same helper to the first one. It answers the server for the engineer every time:
    - `0x461F` subtype 4: helper id, port, session, train;
    - the layout matches the real engineer's answer byte for byte.
  - `0x461F` type 1 from the real server is 190 bytes. `rts1.exe` reads the first 54: name, address, port, session, train, number, 2 bytes. The rest is UNKNOWN.
  - Over the link:
    - VL80s: `0x4620` of 223 bytes about every 0.5 s;
    - on joining, the helper sends `0x4623` (22 bytes), a sync request;
    - the engineer answers with the whole cab, `0x4624` (5 758 bytes).
  - The engineer streams as soon as its connection is up, before a helper exists. A relay should drop that, not queue it.

**Coupling of two players' trains.**
- Legacy on: as RTS does it (above), so RTS clients see what they expect.
- Legacy off:
  - the server decides a coupling in one step, so two clients cannot both
    carry it out;
  - the player whose train was taken in chooses: stay in the merged train as
    a helper, or uncouple and take their engine back.

**What the live capture of 2026-11-05 asks of openRTS** (Legacy off; with
Legacy on, RTS clients keep what they do):
- **Remote trains** are drawn about one package period in the past and
  interpolated between known points, not extrapolated. With RTS senders
  that is 1.5–2 s. When a client must predict, the acceleration fades out
  instead of being carried on. openRTS senders send on every control
  change and 10–20 times a second.
- **Doors and the joints** (hoses, cocks, cables, chocks, handbrakes) are
  part of the car state the server holds and sends to everyone. A car menu
  action names the cars by key, not by index.
- **A train that changes owner** keeps one state: nothing freezes on one
  client and nothing restarts from `Init` on another. Either the engines'
  DLLs keep running where they ran (a client's DLL host already runs
  several trains), or their state goes with the train. The choice is open.
- **Dispatchers** get the owners and the assignments. A split sends the new
  part's name and number to everyone.
- **Derailed cars:** while a car falls its owner sends its placement
  often, and once it lies still, the final one; every client shows that.
- **Removing a train from the world** and placing it back never leaves a
  train without its place on the track, a player's own train included
  (RTS crashed its owner, `multiplayer-protocol.md`).
- **The route direction** is a dispatcher's button (`roadmap.md`, the
  console).

**Next reverse steps:**
- the payloads of the `0x45xx` dispatcher packages;
- the `0x4620` family;
- the remaining `0x4603` fields;
- a capture with the server's full data trace to check the counts and the period (done 2026-11-05 through the proxy; a pickup was left out: only Uspensk has any);
- after a coupling: the server takes `0x4603` for a PLAYER train from any
  sender, with no owner check (`0x40FB90`). A part cut off later keeps the
  kind and the owner of its train (`0x44FE34` → `0x456958`). Both are in
  `multiplayer-protocol.md`.
