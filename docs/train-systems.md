# Train systems around the driver

What a train has besides traction and brakes: lights, pantographs, horn,
bell, sand and doors when no DLL drives them; the cab's lamps and
displays; the cables and the coupler between cars; taking another cab;
locomotive logic in a native plugin; the line current and the wire. The
reverse notes behind it are in `docs/reverse/train-dll.md` (§15–§18) and
`docs/reverse/car-menu.md`. CONFIRMED unless it says PROBABLE.

## Exterior lights without a DLL

RTS lights a car's `Lights` by their `Conditions` whenever no DLL switches
them (`0x4F07D4` for a train, `0x4FFDF8` for one light); for the player's
train it skips only the engines a DLL drives. openRTS:
`simulation/train_lights` (`car_lights`, `light_condition`).

| Condition | Values | When on |
|---|---|---|
| `Headlight` | 1 day, 2 dusk, 3 night, 4 dusk or night | by the light level (below 0.5 is dusk); a cold engine counts as day |
| `Control` | 1 not the player's, 2 the player's | the train's type (player and remote players vs the rest) |
| `Service` | 1 out of service, 2 working | out of service: a STATIC or LOOSE train, or a cold engine |
| `Unit` | 1 middle, 2 first, 3 last, 4 first reversed, 5 last reversed | the car's place; reversed when the train runs with its rear leading; a single car is first and last |
| `TimeOfDay` | 1 day, 2 night | a day light goes out only when it is night and dim, a night light only when it is day and bright |
| `Weather` | 1 clear, 2 rain, 3 snow | the weather |

- A STATIC or LOOSE train (standing, nobody at its handles) shows none.
- In a network game another player's train skips this: it shows its
  owner's lights (`0x4617`, `reverse/multiplayer.md`).
- Each light fades over `FadeIn` / `FadeOut`; with several `States` it
  cycles them by their `Duration`s, or spreads `Cycle` evenly over them
  (`0x4FF9B8`).
- RTS re-evaluates them on events (night falling, coupling, the train's
  type or direction changing); openRTS every frame, which gives the same.
- Trains farther than 3 km are not worked out; the nearest nine beams
  light the scene, glows show for every train.

## Pantographs without a DLL

RTS's AI trains (`0x424488`): the main pantograph is 1, or 2 when the car
is turned or the train runs reversed (RTS compares bit 0 with bit 0x80, so
both together give 2 too); the second goes up below
`RaiseSecondPantoWhenSpeedBelow`; `LowerPantoOnStop` lowers its mask at a
stop (PROBABLE: openRTS takes the test driver's waiting point);
`AlwaysUsePanto` raises its mask; `ExcludePantograph` (repeated, 1..8)
never. The driven train without a DLL raises the rear one by the
direction of travel (an openRTS choice) and applies the same masks; `P`
lowers and raises them. A cold engine keeps them down.

- `PantographRaiseTime` (the wagon's, default 3 s) is how long one takes.
- Sounds as the MSTS scripts expect: 45 / 46 the first up / down, 66 / 67
  the second.

## Horn, bell, sand, doors without a DLL

The driven train without a DLL (and without a native plugin):

| Key | What | SMS triggers |
|---|---|---|
| Space (held) | horn | 8 / 9 |
| `B` | bell (whistle) on / off | 10 / 11 |
| `X` (held) | sander on every engine | 4 / 5 |
| `P` | pantographs down / up | 45 / 46, 66 / 67 |
| `Y` / `U` | left / right doors (GlobalID 2 / 1), only standing | 48 / 49 |

Sand (RTS `0x424488`): adhesion times the `.eng`'s `Adheasion` third value
while sanding; `MaxSandingTime` seconds of sand (−1 unlimited); the sanded
rail lasts the engine's length of travel after it stops. A DLL sands with
`Engine::Sanding`, a native plugin with `OrtsTrainOutputs::sanding`.

## The cab

- **Displays** (`0x525DB0`): a `DISPLAY` shows its `StateNodes`: node j for
  bit j of the State, or with `DISPLAY_SEQUENCE` for the State j + 1.
  `DisplayFade ( fade blink )`: with a blink a lamp or display is shown
  `blink` s and hidden `blink` s. Not done: the fade itself (RTS fades the
  node in and out; openRTS switches it).
- **Lamps** (`LightSources`) without a DLL follow their `.sd` Flags as RTS
  runs a shape's lamps (`0x4B0284`): `RUNNING` burns, `NIGHT_AUTO` while
  the world is at most `AutoWhenWorldLit` lit (0: at night), `NIGHT_ONLY`
  never by day. A DLL switches them itself.
- **Taking another cab** (`0x435654` → `0x436734` → `0x468C74`): walking
  through a door into another engine's cab with a desk makes it the
  player's at once. With DLLs the host enters it: `Init` if needed,
  `ChangeLoco` (the previous master as `Prev`), the multiple-unit groups
  again. Without a DLL the keyboard driver takes that engine as master
  with its handles at 0 (throttle, reverser) and the brake handles as
  they were. The reverser keys are the cab's: forward in a cab facing the
  train rear moves the train back.

## Between cars

The car menu (Num3, `docs/reverse/car-menu.md`) now has everything RTS
offers:
- **Cables**: the control cable (`SMECable`) and the high-voltage bus
  (`HVBus`) where both touching ends have one (`.eng`, front / rear). They
  start connected; uncoupling parts them (`0x469858`), coupling does not
  join them (`0x469ED8`): the menu connects them (SMS 88 / 89, 90 / 91).
  A parted control cable regroups the DLLs (`0x4F995C`): a slave left out
  of its master's group gets `LostMaster`. The HV bus decides which
  engines share the line (below).
- **A broken coupler**: a collision whose share of the impact (the other
  train's mass over both) passes the end's `Break` force breaks that end
  (`0x58A07C`, car `+14` 0x10 / 0x20). A broken end does not couple; "Fix
  coupling" mends it in 120 s, one at a time (`0x463F84` id 17).
- **Cargo information** (`0x502680`) for a freight car with cargo: its
  `FreightAnim` entries of the consist (cargo type, mass, volume) and the
  car's mass, shown for 30 s. The consist's `FreightAnim` shapes are drawn
  on the car at `FreightAnimPosition` (found in the car's folder,
  `TRAINS/OBJECTS` or `GLOBAL/SHAPES`) and their mass adds to the car's.

## The line, the current and the wire

`simulation::Electrification::apply` as `0x503CE4`:
- **By HV bus groups**: the cars joined by the high-voltage bus share the
  highest voltage under their raised, unburnt pantographs; an engine
  without a bus is a group of its own (the EMUs' head cars get their line
  from the motor car).
- **Current**: the group's power (a DLL's `PowerConsuming`, a native
  plugin's or the keyboard driver's |F v|) over the voltage, split by
  raised pantographs (`Locomotive::LineCurrent`, given to the DLLs).
- **Burning**: without `MaxLineCurrentCatenaryBurnTime` a pantograph over
  `MaxLineCurrent` burns the wire at once; with it the wire's heat grows by
  the current / the wire's `BurnQ` and cools by `MLC_B + MLC_A x speed`
  (`MaxLineCurrentVelocityIncrease`) until it reaches `MaxLineCurrent x
  BurnTime`. A burnt pantograph (`LocoFlags` 0x200) gives no line for
  60 s. The Train window can switch burning off.
- **Substations** (`0x5B6030`): each substation phase adds up the power it
  gave over 2 s windows; Track control's electrification layer shows it
  against the substation's `Power`. RTS's client only reports it to the
  server (package `0x4649`, relayed to the dispatchers); nothing in RTS
  lowers the voltage by it.

## Native plugins

An engine with `EngExtended ( ORTSTrainPlugin ( "x.dll" ) )` (relative to
its folder) runs that 64-bit plugin instead of an RTS DLL when it is the
driven one: the plugin drives it, works its cab (`.sd` controls by name,
the cab's `AssignKey` keys, the mouse, RTS's step rules) and its gauges and
lamps, posts sounds and messages, raises its pantographs and sands. The
3-D cab shows the plugin's state as it shows a DLL's.

`sdk/samples/vl10k` is the ВЛ10к's logic ported from its RTS DLL as an
example: the main circuit (pantographs, breaker, DC 2..4.5 kV, compressor),
the controller's 45 positions in four connections with field weakening,
the DLL's tractive effort curves, the reversing keys, the 395 and 254
valves, the gauges and lamps. Not ported: multiple units, regenerative
braking, КЛУБ / САУТ, the auxiliary machines. To try it: a copy of the
ВЛ10к whose `.eng` names the plugin instead of `LibraryModule` (the
installation here has `TRAINS/TRAINSET/openRTS_VL10k-native` and the
consist `openRTS_VL10k-native`, made for this test and not shipped).

## ALSN

The DLLs' `ALSN` now gets what RTS gives (`0x508520`): A — the signal
objects ahead of the train's front not yet reached, B — those behind its
rear, C — those its ends pass this step (within speed x step x 1.5), each
as `SignalsInfo` (distance; the highest aspect of its NORMAL, DISTANCE,
SHUNTING and INFO heads in `Aspect[0..3]` with flags 1 / 2 / 4 / 8; the
speed the train obeys there); then the lowest limit, the next speed post's
limit and its distance (floats in the two integer arguments), and whether
the engine faces against the train. `RTSSignalData*` is the signal's
route database entry (tile, position, name; `reverse/train-dll.md` §19).
`RTSGetTrackItems`, `RTSGetNamedInteger` and `RTSIsSMSTriggerOn` answer as
RTS does too (same section).
