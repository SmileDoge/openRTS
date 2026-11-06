# Sound

Status: the routes' world sounds, the trains' sounds, the cab sound and
what the DLLs post play in the viewer (the sections below).

## Layout

| Part | Where |
|---|---|
| `.sms` scripts, `.ws` world sources | `formats/sound` |
| decoded clips, file lookup | `audio/sound_bank` |
| script runner (activation, triggers, curves) | `audio/sound_instance` |
| output: miniaudio engine and node graph | `audio/audio_engine` |
| route world sounds around the listener | `audio/world_sounds` |
| checks over an installation | `openrts-sound-check <root>` |

- **Runner and output are separate.** The runner drives voices through a
  `VoiceSink`. Tests use a recording sink and the offline engine, which
  renders the miniaudio graph into a buffer with no device.
- **Later spatial audio** (Steam Audio) attaches as nodes in the miniaudio
  graph, without touching the runner.
- **Viewer:**
  - flags `--no-sound` and `--volume <0..1>` (default 0.5);
    `--sound-offline` runs the sounds without a device (nothing is heard,
    voices do not advance): with `OPENRTS_SOUND_TRACE` it shows which groups
    go on;
  - a volume slider and a voice count in the panel;
  - world-sound counters in the route section.
  - Automated runs use `--no-sound`.
- **Check tool:** `openrts-sound-check` prints what RTS ignores in the
  installed content.

## Content (installed)

- **Scripts:** 1056 `.sms`, all read; one of them is a RAR archive named
  `.sms` and is silent.
  - Streams: 13 269; triggers: 60 263.
- **Ignored by RTS** (and by openRTS):
  - 817 triggers, mostly `Dist_Travelled_Trigger`, which RTS does not know;
  - 101 actions with a misspelt keyword:
    - `ReleaseLoopReleasep` in 79 scripts;
    - `Enable_Trigger` in 15;
    - `ReleasLoopRelease` in 5.
- **World sounds:** 131 `.ws` files with 428 sources.
- **Sound files:** 5969 are reached and all decode; 1283 have a loop region.
- **Missing:** 487 file names the scripts use are absent from the
  installation, so those sounds are silent, as in RTS.
- **WAV encodings:** PCM 8/16/24/32-bit and float, 8–48 kHz, mono and stereo.
- **Loop regions:** `smpl` loops are in 1780 files, cue points in 459.

## How rts1.exe reads .sms (CONFIRMED, static analysis)

| Address | Role |
|---|---|
| `0x4DD0EC` | loads a script: groups, streams, triggers, curves |
| `0x4DF894` | reads one action |
| `0x4578C0` | keyword search |
| `0x459B38` | removes comments |
| `0x45B668` | camera flags |
| `0x439DE8` | resource cache, with a sound object "kind" |

### Reading the text

- **Lower case:** the file is lower-cased first (`CharLowerBuffW`).
- **Comments:** `Comment ( … )`, `Skip ( … )` and `_Skip ( … )` blocks are
  blanked out first.
- **Keyword search:**
  - a keyword is searched in the text of the enclosing block, from its start
    and in document order;
  - it matches only with whitespace or the block start before it and `(`
    after it (whitespace allowed);
  - so misspellings never match. `ReleaseLoopReleasep (` is not
    `ReleaseLoopRelease`; `Enable_Trigger` is not `EnableTrigger`.
  - Only the letter case is free.
- **Quirk — fields are found anywhere in the block's text.** A group without
  its own `Volume` takes the first `Volume ( )` of one of its streams.
- **`Tr_SMS` is not required.** Groups are found anywhere in the file.
- **Counts:** `Streams ( n`, `Triggers ( n`, `PlayOneShot ( n`,
  `TriggerActions ( n`, `CurvePoints ( n`, `SoundSources ( n` limit the
  entries read. RTS reads at most n and stops early when fewer are present.

### Triggers

- **Type by exact block name:** `initial_trigger`, `discrete_trigger`,
  `variable_trigger`, `random_trigger`, `event_trigger`.
- **Other blocks** inside `Triggers` (`Dist_Travelled_Trigger`, …) keep
  their place in the list and do nothing. `EnableTrigger ( n )` /
  `DisableTrigger ( n )` count them; `Skip` blocks are not counted, being
  removed.
- **Variable names**, exact: `speed`, `variable1`…`variable8`, `distance`,
  `distance_travelled`, `acceleration`, `radius`, plus `_dec_past` /
  `_inc_past`. Curve controllers: `speedcontrolled`,
  `variable1controlled`…, `distancetravelledcontrolled`,
  `accelerationcontrolled`, `radiuscontrolled`.

### Actions

The action of a trigger is the first keyword found in this order:
1. `TriggerActions` — its entries are read the same way;
2. `SetStreamVolume`, `DisableTrigger`, `EnableTrigger`;
3. `DisableStream`, `SuspendStream`, `EnableStream`;
4. `HaltPlayback`, `PostTrigger`;
5. `IncreaseVariableValue`, `SetVariableValue`;
6. otherwise `ReleaseLoopRelease`, `ReleaseLoopReleaseWithJump`;
7. otherwise `PlayOneShot`, `StartLoop`, `StartLoopRelease`.

- **`Fade ( s )`** is read anywhere in the action and applies to any of
  them.
- **Selection:** `SelectionMethod ( RandomSelection )` selects randomly; any
  other value, or none, selects sequentially.

### How RTS plays a sound (CONFIRMED 2026-11-04)

- **The wave** (`0x4DF378`): only `fmt `, `data` and `smpl` are read; `cue `
  is not. Each `smpl` loop is kept as byte offsets (start, end × the block
  size). A file without `smpl` gets one loop: the whole data.
- **The buffer** (`0x4AA178`): a DirectSound buffer is made for each start
  with just the bytes it plays, flags `0xAA` (volume, frequency, software,
  static). No pan and no 3-D: RTS does not place sounds, it only sets
  their volume.
- **What a start puts in it** (`0x4A8C00`):
  - `PlayOneShot` and `StartLoop`: the whole file;
  - `StartLoopRelease`: from the file's start to the loop's end, looped as
    a whole, so the part before the loop plays on every pass. Only when the
    group comes on again (`0x4A9BA8`) or a stream's triggers run again
    (`0x4A9A18`, object flag 4) is it the loop alone;
  - `ReleaseLoopRelease` stops the looping: the buffer plays to its end,
    which is the loop's end;
  - `…WithJump`: from (data length − the place reached), or from the loop
    start when that is past it; with the loop alone, from the place reached;
    on to the end of the file.
- **Each update** (`0x4A78A0`): volume = `Volume` × the volume curve ×
  `SetStreamVolume` × the object's volume × `Attenuation` by distance
  (unless the curve is distance-controlled), × the fades, to the buffer as
  `log10(volume) × 3000` hundredths of a dB (floor −10000). Frequency: the
  curve's value in Hz (100 000 at most); below 100 Hz the sound is silent.
- **Content (ChS4t):** `ezda`, `al4545`, `transform`, the fans and the
  compressors have `smpl` loops, and most of them begin quiet: by the code
  above RTS's loop falls to near silence at each pass (`chs4t_comp`,
  `vent_vu`, `al4545`). openRTS loops the `smpl` region instead (level even
  across the seam); `vent_vu`'s seam steps 3.3 times the wave's usual step.

### How a stream runs its actions (`0x4A8C00`, CONFIRMED 2026-11-04)

- **A stream plays one sound at a time.** It has one sound buffer and is
  busy while that plays.
  - `PlayOneShot` on a busy stream is ignored.
  - `StartLoop` / `StartLoopRelease`: ignored while the stream's loop is
    looping. If the stream is busy otherwise (a one-shot, or a released
    loop playing out), that sound is stopped and the loop starts anew.
  - `ReleaseLoopRelease` and `…WithJump` do nothing on a stream that is not
    busy.
- **`ReleaseLoopRelease`** without `Fade`: the buffer stops looping and plays
  its current pass to the end. With `Fade`: it fades out.
- **`ReleaseLoopReleaseWithJump`** stops the buffer and goes on from another
  place, once:
  - a loop region from cue points: from the current place to the end of
    the file;
  - no cue points: from (length − the place it had reached), so it plays as
    much of the file's end as it had played of its start.

  A loop started and released with a jump in the same frame therefore
  plays almost nothing.
- **Codes per frame** (`0x4A8284`): before the streams, the queue goes
  through the trigger pairs (`0x4A874C`). A sound object that takes 1700
  codes within 2000 updates stops taking codes ("Sound object generates too
  many discrete triggers"). The codes in the group's
  `NonDistributableTriggers` do not count. They are also the codes a
  network game does not send to the other players (`0x4639`,
  `reverse/multiplayer.md`).
- **Content (ChS4t, `chs4t.dll` `Run` `0x4064F4`):**
  - with the auxiliary compressor on (switch 22 at "[ВК]"; the DLL's `Init`
    puts it there for a running start), the DLL posts 67 every frame while
    its pressure `var[19]` > 0 is still rising, and 68 while it is above
    7.0;
  - the pressure rises towards 7.0 + 0.7 per compressor, so above 7.0 both
    codes come every frame, for good;
  - `ev_CHS4t-cab.sms` starts `kompr_pant2.wav` (0.56 s, no cue points)
    with 67 and releases it with a jump with 68, and lists 67 and 68 as
    `NonDistributableTriggers`. In RTS the compressor falls silent at 7.0.

### Stream `Sources` (PROBABLE, static: 0x4DD0EC and its exception table)

- **`Sources ( n i … )`** points a stream at the group's `SoundSources`
  entries (indices from 0).
- **An index at or beyond the group's `SoundSources` count** throws inside
  a try block of the loader. Its handler (0x4DDE32):
  - logs message 0x39, "Error when loading sound sources in %s";
  - leaves the stream without sources;
  - goes on loading. The stream still plays, at the object.
- **Installed content:** 232 such references in TRAINSET (45 scripts), e.g.
  the ED4M, 2TE10, VL10K and TEM2 exterior scripts, which have no
  `SoundSources` at all.
- **A script that fails as a whole** (any other exception) is caught in
  0x4A72D4: message 0x23, "Failed to load sms-file %s"; that object has no
  sound.
- **openRTS:** a stream with a bad reference has no sources and plays at
  its own volume. The cab script's sources set its streams' volumes
  ("Rooms and sound proofing", below).

### Unknown to RTS

RTS ignores these, and so does openRTS:
- `Dist_Travelled_Trigger`;
- `Mono`, `Ignore3d`, `LoadAllWaves`;
- `TrackType` (an Open Rails extension);
- `SequentialSelection` as a keyword (sequential is simply the default).

### Groups by owner

The sound object's kind decides which `ScalabiltyGroup` is used. It is the
first match in file order, not by number:

| Owner | RTS kind | Group |
|---|---|---|
| `.ws` sources, crossings, pickups | 6 | the first group |
| weather and environment | 8 | the first group; activation distances 0 (none) |
| the player's train | 5 | group 7, else the first group |
| other trains | 7 | the first group numbered 5 or 0 |
| a car's own script, as its `.eng` / `.wag` loads it | 0 | group 0 only |

**Another player's train in a network game** (`0x4F0BC8`, the car load:
a train of kind 2 with flag `0x10000` while playing through the network;
static, PROBABLE: a branch for an unknown mode, `dword_61D31C+4`, is left
out): an engine neither cold nor AI (`LocoFlags & 0x88` = 0) gets kind 5,
like the player's own; any other engine that is not cold kind 7; a cold
engine and a wagon keep their script as loaded, kind 0. So another
player's ED9M sounds its horn and whistle, which only its head cars'
group 7 has. openRTS does the same for remote trains, deciding when the
car's sound is made.

### Activation

- **Defaults:** activation within 500 m. Deactivation beyond 550 m; when
  `Deactivation` exists without `Distance`, the activation distance + 50.
- **Cameras:** a camera listed in both `Activation` and `Deactivation` stays
  active (RTS clears its deactivation flag).
- **CONFIRMED (observed in RTS) — a group without `Activation` cameras plays
  for any camera.**
  - This covers all 28 such groups installed. All of them have no
    `Activation` block at all.
  - Among them is `SOUND/CROSSING.SMS`: in RTS the crossing is heard from
    the cab and from outside alike.
  - Not traced: the RTS runtime code that tests the camera flags.

## openRTS runtime (PROBABLE where not stated)

### Activation

- **CONFIRMED (`0x44AB7C`, the sound objects' update; flags `0x45B668`):**
  the camera flags of a group (`+32`): `Activation` CabCam 2, PassengerCam
  4, ExternalCam 8; `Deactivation` 0x10 / 0x20 / 0x40 for the same cameras.
  - Outside (the free and the external camera, on foot, the head out of a
    window): ExternalCam groups.
  - In the train (walking, seated): CabCam groups of the player's train's
    objects while a cab is loaded, PassengerCam ones while a salon is, and
    ExternalCam groups of anything not in the player's train (other trains,
    the world, the weather).
  - So `rain_in.sms` / `snow_in.sms` (CabCam, PassengerCam) never start:
    the weather belongs to no train. In the cab `rain_ex.sms` plays through
    the room's share ("Rooms and sound proofing", below).
  - openRTS (`TrainView::sound_camera`): the engineer in the driven car:
    cab; in another car of the train: passenger (the ED9M's scripts list
    PassengerCam); on foot outside or the head out of a window: external;
    other trains, the world and the weather: external.
- **CONFIRMED going off (`0x4A9CF4` → `0x4A9A84`):** every voice stops; a
  stream that was looping keeps its current trigger (flag bit 0) and the
  buffer position of a `StartLoop` (state `+64`); others forget theirs.
- **CONFIRMED going on again (`0x4A9BA8`):** every trigger marked current
  fires again (the loops resume at their position), random triggers wait
  their minimum delay; trigger and stream states are not reset.
- **CONFIRMED while off:** the object's 96-entry queue of discrete codes is
  not run (`0x4A8284` runs only for an active object); the codes run at the
  next active update.
- openRTS (`audio::SoundInstance`):
  - the first activation enables every trigger, runs the streams at volume
    1 and fires the `Initial_Trigger`s;
  - later ones resume the remembered loops (the position from the loop's
    age), then run the codes queued meanwhile (up to 96);
  - before (until 2026-11-01) every activation fired the initial triggers
    again and the loops a DLL had started were lost after a trip into the
    cab.
- The free camera of the viewer counts as `ExternalCam`.
- **Deactivation distance below the activation distance** (four installed
  groups: the ED4M, ED9M and EP2D `engMotor` scripts, 1000 / 300 m):
  deactivation wins. Between the two distances the group stays silent
  instead of starting and stopping on alternate updates. RTS's runtime test
  is not traced.

### Rolling stock in the viewer

- **Group by role:** the player's train (the driven one) plays group 7,
  other trains (test driver, standing) the first group numbered 5 or 0, as
  RTS chooses for kinds 5 and 7. A car whose script has no such group is
  silent, e.g. the ED4M head cars on a train that is not the player's. A
  train that changes role gets its sounds rebuilt.
- **Clips are loaded before anything plays:** every car's script, group and
  clips first, then the updates. Before, the first car of a kind decoded its
  files while the cars before it already played; the same loops then ran a
  fraction of a second apart and sounded like an echo.
- **Random loop phase (openRTS choice, on by default, "Car loops start at
  random points" in the train panel):** `StartLoop` voices begin at a random
  frame. The same loop on twelve cars otherwise adds up in phase: one loud
  point with comb filtering instead of a train-long sound.
  - RTS (PROBABLE, 0x4A8C00): a voice starts at 0, or at a position kept in
    the stream state (+64) when there is one; no random offset seen.
  - `StartLoopRelease` voices and one-shots start at 0: their beginning
    matters (a start-up sound, a knock).

### Triggers

- **`Variable_Trigger ( X_Inc_Past v )` / `_Dec_Past`:** fires when X
  crosses v between two updates.
- **First update after activation:**
  - `Distance` is evaluated as if the listener came from outside, so a
    station loop starts when the camera appears inside its range;
  - other variables start from their current value.
- **`Random_Trigger`:**
  - fires after a delay drawn from `Delay_Min_Max`, again and again;
  - `Volume_Min_Max` scales that sound;
  - re-enabling draws a new delay.
- **`Discrete_Trigger ( code )`:** codes come from the simulation, a DLL or
  `PostTrigger`.
- **RTS `DiscreteTriggerPairs`** (`0x4A87C8`):
  - an On code is dropped while its pair is known to be on, an Off code
    while it is known to be off;
  - before a pair's first code both pass;
  - a Halt code sets the pair off.

  The DLLs read the states (`RTSIsSMSTriggerOn`,
  `reverse/train-dll.md` §19).
- **`Event_Trigger`:** `WagonOnJoint` / `WagonOnJunction` /
  `WagonOnAnyJoint` events from the train.
- **Indices:** `EnableTrigger` / `DisableTrigger` count from 1, as
  `ingame.sms` numbers them.

### Actions

- **One sound per stream**, as RTS (2026-11-04, "How a stream runs its
  actions" above).
- **`PlayOneShot`:** plays a file once, unless the stream plays something.
- **`StartLoop`:** loops the whole file.
- **`StartLoopRelease`:** plays into the loop region and loops it. The
  region comes from the first `smpl` loop, else the first two cue points.
- **Both starts** leave a looping loop alone; anything else the stream plays
  stops and the loop starts anew.
- **`ReleaseLoopRelease`:** leaves the loop; the current pass plays to the
  end.
- **`…WithJump`:** in a loop region it plays on to the end of the file;
  otherwise from (length − the place reached), found by the voice's cursor.
  Started and released in one update, the sound stops at once (the ChS4t's
  auxiliary compressor above 7 atm: silent, as in RTS).
- **`Fade`:** a started loop fades in, a released loop fades out and stops.
- **`EnableStream` / `DisableStream` / `SuspendStream` (index from 1):** a
  disabled stream stops its voices; a suspended one keeps them but its
  triggers pause.
- **`PostTrigger ( code )`:** delivers the code in the next update.
- **`SetVariableValue` / `IncreaseVariableValue ( variable value )`:** they
  replace that input from then on.

### Volume, pitch and position

- **Volume:** group volume × stream `Volume` × `SetStreamVolume` × volume
  curve × random volume × fade.
- **Curves:** `VolumeCurve` / `FrequencyCurve` are linear between points and
  clamped at the ends. `Granularity` is not used.
- **Frequency:** the frequency curve gives Hz; pitch = Hz / the file's sample
  rate.
- **Positional voices:**
  - they are panned by direction;
  - distance attenuation is off, because scripts carry their own distance
    curves;
  - weather and environment sounds are not positioned (`Ignore3d` itself
    is unknown to RTS);
  - nor are the sounds heard at the listener: the cab's own script inside
    and the library sounds in the car (2026-11-04). Placed at the head, a
    frame's travel away from the listener once the train moves (1 mm at
    1–2 km/h), they were panned by turns and lost up to half their power:
    miniaudio pans a source farther than 1 mm, and one straight ahead gets
    0.5 per channel. RTS places no sound at all (above).
- **CONFIRMED, not ported (2026-11-04):**
  - `0x4A78A0` sets DirectSound's volume to 3000 × log10(v) hundredths of
    a dB: 30 dB a decade, an amplitude of v^1.5 (openRTS: v);
  - objects with flag 0x20 (`.ws` sources, `0x41B6D0`;
    `AuxLibrarySoundExternal`, `0x56DC3C`) fade with distance by the
    `Attenuation` rule (`0x4A7764`, below) unless their volume curve is
    distance controlled. Rolling stock has no such flag.
- **UNKNOWN:**
  - whether RTS adds DirectSound 3D rolloff on top of the curves;
  - how RTS applies `Granularity`, `Priority` and the voice limits;
  - where RTS takes the resume position (+64 of a stream's state) from.
- **CONFIRMED (test):** scene coordinates are MSTS and are mirrored in X for
  miniaudio. A source on the camera's right (+X while looking along +Z) is
  louder in the right channel.

### Files and openRTS variants

- **Lookup order:** a `File ( "name" )` is looked up next to the script,
  then in the global `SOUND` folder.
- **Compressed variants:** openRTS prefers a file with the same name and a
  compressed extension (`.ogg`, `.flac`, `.mp3`) next to the WAV. Content can
  ship WAV for MSTS/RTS and a smaller file for openRTS with one script.
- **Other formats:** a script may also name such a file directly; MSTS/RTS
  then cannot play it.
- **Decoding:** miniaudio decodes WAV, MP3 and FLAC; Ogg Vorbis goes through
  `stb_vorbis`.
- **Not supported yet:**
  - Opus — it needs libopus and opusfile; miniaudio has a decoder for them;
  - loop tags (`LOOPSTART` / `LOOPEND`) in compressed files.
- **Script-level variant, possible now:** the group selection above shows
  that RTS picks by number only 7, 5 and 0, otherwise the first group in the
  file. A group with another number placed after the RTS groups is never
  chosen by RTS. openRTS can use it for its own variant — the number and
  the rule are still to be agreed.

## Not yet

- **Route ambient sounds:** `ingame.sms` (what its `Variable1` is, is
  UNKNOWN); default signal and tower sounds from the `.trk`. (Weather and
  level crossing sounds came with `v0.2.0`, `world-objects.md`; the trains'
  sounds with roadmap step 2, above.)
- **With the cab:** a salon's own sound (`Interior ( SoundInt )`, one
  installed wagon). Named triggers are done (2026-11-04,
  `reverse/train-dll.md` §19).
- **Loudness:** RTS's volume law and distance fade (the CONFIRMED items
  under "Volume, pitch and position").

## The cab sound (2026-11-01)

- The engine the player drives plays its cab script as a second sound
  object (owner: the player's train), next to its exterior sound.
- **CONFIRMED** (`0x44247C`): the cab script is the Engine block's
  `RTSCabinSound`, else its `Sound` (MSTS kept it there; the MaDaDev ED9M
  does so); the exterior one the Wagon block's `RTSExteriorSound`, else its
  `Sound`. Each is looked up in the block, not the whole file.
  Inside the 3-D cab it is heard at the driver's seat, outside at the car.
- Sound objects get the camera kind (`cab` inside the 3-D cab, `external`
  otherwise) for their streams' `CabCam` / `ExternalCam` activation.
- A legacy DLL's triggers (`SMSObject::Queue`, `PostTriggerCab` /
  `PostTriggerEng`) go to the cab and the exterior object; its
  `SMSObject` Var1..Var8 replace the engine variables 1..8 the keyboard
  driver would set.
- **Loading:** clips decode on the render thread when first played; moving
  that to a worker comes with train sounds.

## Aux library sounds (2026-11-02)

- An `AuxLibrary` with `AuxLibrarySound ( file.sms )` (path relative to the
  .eng) has an SMS object of its own: what the library posts to it
  (`RTSGetObject(lib, AUXLIB, Sound)`) plays there, not in the cab script
  (Brakes.sms: КЛУБ / САУТ codes 120..3223, Usavp.sms: voice messages
  2000+). The host reports them per library (`CarOutput::aux_sound`).
- From the reference: `AuxLibrarySoundType` AMBIENT (no position), WAGON
  (at the car, with its variables, the default) or ROUTEFORWAGON (the
  route's SOUND first); `AuxLibrarySoundPosition` in the car;
  `AuxLibrarySoundExternal` fades with distance (else heard as at the
  listener); `AuxLibrarySoundOwnStack` keeps its own Var1..8 (else the
  engine's); `AuxLibrarySoundSingleWagon` plays only while the player is in
  that car.

- `openrts-train-check --triggers` prints every trigger a DLL posts, per
  car and SMS (ext, cab, aux n).

### Library sounds (CONFIRMED, 2026-11-05)

- **Made once,** when the engine's DLL is bound (`0x5026FC` → `0x56DC3C`):
  - a sound object of the kind `AuxLibrarySoundType` gives (`0x4D4B08`,
    `0x4AA540`): WAGON 5 (also when absent: group 7, else the first),
    AMBIENT 8, ROUTEFORWAGON 9, anything else 6;
  - put on the list of all sound objects (`0x44A9DC`) with its engine, its
    `AuxLibrarySoundPosition` and flags: 0x40 placed in the car (WAGON,
    ROUTEFORWAGON), 0x20 `External`, 0x100 `OwnStack`, 0x200
    `SingleWagon`.
- **Updated every frame whatever its train** (`0x44AB7C`):
  - its point is the car's place × the position; the distance from it to
    the camera opens and closes the group and drives the curves;
  - Var1..8 are the engine's DLL ones unless `OwnStack`;
  - the camera rules see the train (`car's train == 0x61D680`): in the cab
    or the salon, CabCam and PassengerCam groups need the car in the
    player's train, ExternalCam ones a car out of it; outside cameras
    ExternalCam groups only;
  - `SingleWagon`: only the player's current engine, any other stops;
  - the room: the current engine's libraries pass from their room to the
    listener's; others play at what passes to the open air.
- So a part cut off from the player's train keeps its libraries' sounds:
  `Usavp.sms` (`Activation ( ExternalCam () PassengerCam () CabCam () )`,
  200 m) speaks, `Brakes.sms` (`CabCam` only, `SingleWagon`) is silent.
- openRTS (`TrainView::update_aux_sounds`): the same for every train of the
  DLL host; the other trains take the external camera, as their exterior
  sounds. `SingleWagon` waits for the engineer in the cab car (RTS: the
  current engine, then the camera rules).

## Rooms and sound proofing (2026-11-04)

Static analysis of `rts1.exe`, CONFIRMED unless marked: `0x44AB7C`,
`0x44B6B4`, `0x51434C`, `0x5143DC`, `0x513E8C`, `0x4D47A4`, `0x51CD88`,
`0x4A78A0`, `0x4A7764`. openRTS: `audio::InteriorAcoustics`,
`CabView::sound_transmission`, `TrainView::update_listener`.

### The data

The cab loader `0x51CD88` reads them; a salon's loader (`0x515434`) the
same way.
- **Rooms:** `InteriorAreas ( n InteriorArea ( name X1 Z1 X2 Z2 ... ) )`,
  68 bytes each.
- **Doors** (`InteriorDoors`, 120 bytes):
  - `Connects ( a b )` (`0x4D47A4`): EXTERIOR −1, EXT_WAG −2, INTERIOR −3,
    WAGON_BACK / WAGON_FRONT −4 / −5 (swapped for a turned car),
    INTERIORPASS −7, else a room's name (−6 and its index; an unknown name
    leaves 0);
  - `SoundProof ( closed open )` at `+104` / `+108`, default 1 / 1;
  - `Opened` (its presence) opens it from the start.
- **Windows** (`InteriorWindows`, 100 bytes): `SoundProof` at `+80` /
  `+84`, default 0 / 0. A window's room is the one its `Position` is in
  (`+92`, `0x51434C` at load); without a Position it has none.
- **Content:** 237 installed cab `.sd` files have `SoundProof`, 240 have
  `InteriorAreas`. The ChS4t has CABIN1, MACHINE and CABIN2; side doors
  0.5 / 0.9, the cab doors 0.6 / 0.9, the windows 0.5 / 0.8.

### The room of a point (`0x51434C`)

The last room whose rectangle holds the point, strictly: X1 < x < X2 and
Z2 < z < Z1 (RTS keeps X negated). A point outside every rectangle is in
no room.

### What passes from one room to another (`0x5143DC`)

- **The same room:** everything (1).
- **Through a door:** every door with a side in the room the sound is in
  (`0x513E8C`). Its share is closed + (open − closed) × how far its node
  has opened, or by its State without a node. That share is multiplied by
  what passes on from the door's other side to the listener's room.
- **The open air:** a door's other side is the open air when it is
  EXTERIOR, EXT_WAG or a car end. INTERIOR, INTERIORPASS and unknown names
  lead to room 0.
- **No loops:** a way enters no room it came through, nor the open air
  twice.
- **Windows:** towards the open air, the windows of the room the sound is in
  count too.
- **The result** is the best way.
- **ChS4t** (`openrts-train-check D:\Games\MDDRTS --cab ev_CHS4t-563
  ev_chs4t-563`): CABIN1 hears 0.50 of the outside shut, 0.80 with its
  windows open, 0.81 with everything open. MACHINE to CABIN1: 0.60.

### Where it is used (`0x44AB7C`, every frame)

- **The listener's room:** the player's position in the car
  (`pos_in_train`), in the cab or salon he is in.
- **Outside sounds:** every sound object plays at its group volume × what
  passes from the listener's room to the open air. That covers trains,
  `.ws` sources, crossings, the weather and library sounds.
- **A library sound placed in the locomotive** (`AuxLibrarySoundPosition`,
  flag 0x40 from `0x56DC3C`) inside a room: what passes from its room to
  the listener's instead.
- **The cab script** (`0x44B6B4`) is not one of these objects. It plays at
  its group volume while the player is in that locomotive.
  - Its `SoundSources ( n x y z volume … )` are points in the car; a turned
    cab turns them.
  - Each point counts with its volume × what passes from the listener's room
    to the point's room, and with its distance from the listener.
  - A stream with `Sources` plays at the loudest of its points, each through
    the stream's `Attenuation` over its distance.
- **The head out of a window** (camera mode 4): the cab script keeps
  playing only if its group has an `AllowStreamOnWindowCamera` stream, and
  then only those streams (`0x4A78A0`: object flag 0x200).
- **`Attenuation ( D1 D2 type )`** (`0x4A7764`):
  - type 1: 1 − 0.5 / D1 × d / D2 up to D2, then 0.5 / D1 × D2 / d;
  - type 2: D1 − d / D2, not below 0;
  - any other type: 0.5 × (2 − d / k) up to k, then 0.5 × k / d. Here k is
    50 m, or a fifth of the deactivation distance when that is below 250 m.

### openRTS

- **The listener:** `TrainView::update_listener` takes the camera's
  position in the engineer's cab or salon (openRTS choice; RTS takes the
  player's position, PROBABLY the same while he sits or walks). Doors and
  windows count as far as their nodes have opened (`CabView`).
- **The share** goes to every car's sound, the library sounds, the world
  sounds, crossings and the weather. The cab script gets its sources and
  the window rule.
- **Checked live:** the capture report's `trains:` line gives `listener
  room <name> outside <share>`. In the ChS4t's cab it is CABIN1, 0.50.
- **Not traced:** where RTS's `pos_in_train` is while the head is out of the
  window. openRTS uses the eye, which is mostly outside the rooms.
