# Sound

Status: foundation. World sounds of routes play in the viewer. Train, cab and
DLL-driven sounds come with the train (roadmap steps 2 and 5).

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
- **openRTS:** stream sources are not used yet (cab work); every stream plays
  at the object, which is what RTS does for the bad references.

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

- When a group activates:
  - every trigger is enabled again;
  - streams run, stream volumes reset to 1;
  - `Initial_Trigger`s fire in order.
- When it deactivates, all voices stop.
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
- **RTS `DiscreteTriggerPairs`:** an "on" code passes once until an "off"
  code of the pair arrives.
- **`Event_Trigger`:** `WagonOnJoint` / `WagonOnJunction` /
  `WagonOnAnyJoint` events from the train.
- **Indices:** `EnableTrigger` / `DisableTrigger` count from 1, as
  `ingame.sms` numbers them.

### Actions

- **`PlayOneShot`:** plays a file once.
- **`StartLoop`:** loops the whole file and replaces the stream's loop.
- **`StartLoopRelease`:** plays into the loop region and loops it. The
  region comes from the first `smpl` loop, else the first two cue points.
- **`ReleaseLoopRelease` (`…WithJump`):** leaves the loop and plays to the
  end.
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
    is unknown to RTS).
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

- **Route ambient sounds:**
  - `ingame.sms` (what its `Variable1` is, is UNKNOWN);
  - weather sounds from `.env` (`world_environment_sound_ex/in`, RTS);
  - default signal, crossing and tower sounds from the `.trk`.
- **With the train:**
  - wagon and engine scripts with speed, distance travelled, joint and
    junction events;
  - group 7 for the player's train.
- **With the cab:** `SoundSources` / `Sources` / `Attenuation` inside cabs,
  `AllowStreamOnWindowCamera`, VariableN and trigger codes from DLLs, named
  triggers.
- **Loading:** clips decode on the render thread when first played; moving
  that to a worker comes with train sounds.
