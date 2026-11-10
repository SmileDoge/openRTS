# RTS scenarios (activities)

RTS's scenarios are `.act` files in a route's `RTS_ACT` folder, each with a
folder of its own files beside it (`RTS_ACT/<name>/`: consists, paths,
sounds). The format is RTS's own, not MSTS's activity format. Sources:
RTS's `activity.pdf` and `activity_WagonStateVariables.pdf` (in the `RTS`
folder of the 2026-11 build; the semantics below quote them),
`docs/RTSFileReference.html` ("Activity"). openRTS reads it in
`formats/src/activity.cpp` (`formats::Activity`), puts the trains on the
track in `TrainView::start_activity` and runs the events in
`viewer/src/activity_view.cpp` (`ActivityView`, which sees the world
through `ActivityWorld`: the route scene's trains and signals, a test's
fake). The roadmap's step 7.

## What is read

All of the file: the header (name, description, start and end places,
start time, duration, season, weather, where the player starts), the
static objects (`StaticObjects`: a `SOUND` object is a `.sms` on a car),
the menus (`ActivityMenus`), the scenario's parameters
(`TrainsDistanceOfNoReturn`, `TrafficLimitsTable`, `GlobalBlockPoints`),
the services (type, path, `ServiceStarts`, `ServiceFlags`, block points,
trains with their consist, placement, start and remove times, speeds,
rates, efficiency, offset, flags, state) and every event
(`GlobalEvents`, a service's `ServiceEvents`: id, activation level,
enabled, `Permanent`, conditions and outcomes, kept as written).

## Starting it

- **Choosing it.** The start menu lists the chosen route's scenarios
  (Single player, "Scenario") with where it starts and ends, the start time,
  duration, season, weather and description; a scenario chosen sets the
  trains, the start, the time, season and weather. The game takes
  `--activity <name>`; its time, season and weather unless `--time`,
  `--season`, `--weather` are given.
- **The scenario's own files.** A consist or a path is looked for in the
  scenario's folder first, then as usual.
- **The trains.** The player's service's train (the header's, else the
  first `PLAYER` service with a start time), driven, at its path's start
  (the last car's rear there; RTS UNKNOWN) or its `TrainPlacement`; every
  `STATIC` service's trains, the last car's rear at `PointStart`, the train
  towards `PointHeading` (activity.pdf: "a point setting the train's
  direction"). Each train and each car keeps its scenario ID (the service
  and train it started in; a car's ID adds its consist UiD), as RTS's
  14-character wagon IDs `ssss-tttt-nnnn` name them. A train whose stock is
  missing is left out (the log names the car).
- **The player's path.** The player's train gets its service's path: the
  dispatcher reserves it and sets its junctions, the service's block points
  hold it (RTS); `AssignService` gives it the new service's path.
- **Traffic.** The `TRAFFIC` services' trains start at their times and run
  as RTS's traffic: one body along their paths, RTS's controller, block
  points, removed at their ends (`docs/traffic.md`).
- **The player.** In the train the header names (its cab for `CABIN`),
  else on foot at `PlayerStartPosition` facing the cab car, else beside the
  cab car (RTS's default, `0x42F4A4`).
- **Loading.** The loading screen waits for the trains within 2 km of the
  player's; car shapes load once each for every train, at most 6 at once,
  the nearest first (Kuban's scenario 42 s to 6.4 s, Novokuznetsk's 88 s to
  15 s, about 12 s of it the route's own world).
- **Far trains.** Car shapes are loaded only for the trains within 4 km of
  the camera or of the player's train, and let go once no train within
  6 km uses them (the log: `trains: N shapes ... let go`); the player's
  train always keeps them. Novokuznetsk's scenario with its traffic had
  every train along the line loaded: 13.9 GB of textures (12.8 GB the
  trains') on a 16 GB card, the rest pushed into system memory (the
  process at 10 GB), a GPU frame of 70-150 ms; now 5.6 GB, 1.4 GB, 8 ms.
  The overlay shows the textures on the GPU and the trains' share.

## The events

An event is armed while it is enabled, its activation level is above 0
and its service has a train in play (the player's start service; one an
`AssignService` gives the train; a traffic service once its train started;
global events always). Once the trains
stand and the loading screen is gone, the `ActivityStarts` events run;
then every frame each armed event whose condition holds fires. A fired
event runs its outcomes in order; `Suspend ( t )` pauses it for t s of game
time (t ≤ 0: till `ActivateEvent`), a modal message holds it and the game
till the message is closed. It fires once, a `Permanent` one again each
time its condition comes true. The log names every event that fires
(`activity: event <service>/<id> at <time>`).

Conditions run:

| Condition | How |
|---|---|
| `ActivityStarts` | at the start |
| `OnStart` | when its service starts (the start's, one an `AssignService` gives, a traffic train starting) |
| `OnMenuSelect ( MenuID MenuItemID )` | the item picked in the menu (F8) |
| `OnCloseMessageBox ( MsgID ButtonID )` | the message closed by that button |
| `PlayerBoardsTrain ( Area )` | the player in the train's cab (`CABIN`, `INTERIOR`, `SALON`; `EXTERIOR` not yet) |
| `ReachLocation ( Location Radius Reach SpeedNotAbove SpeedNotBelow )` | `WAGON_ANY` (default), `WAGON_ALL`, `WAGON_FRONT`, `WAGON_BACK`, `WAGON_PLAYER` / `PLAYER` (the player's car); speeds in m/s |
| `CoupleCars ( ConjTrainID )` | the event's train and that one coupled |
| `WagonState` | see below |
| `ActivateOnTime`, `ActivateByTimer` | the time of day; s since the event was enabled |

`WagonState` (activity.pdf 27): relations `Equal`, `NotEqual`, `Above`,
`AboveOrEqual`, `Below`, `BelowOrEqual` (static) and `IncPast`, `DecPast`,
`SetTo`, `SetNotTo` (dynamic: when the new value meets it and the last did
not), each `( Area Variable Idx SubIdx Value )`, all of them (`All`, the
default) or any (`Any`). Looked at only while the train's state is tracked
(`StartWagonStateTracking`) or once (`CheckWagonState`, the static ones).
The car: `WagonNum` (from the head from 0, negative from the tail) or
`WagonID`. Variables read (activity_WagonStateVariables): `BrakeCylinder`,
`Auxiliary`, `PipePressure` (SubIdx 0 the brake pipe, 1 the feed pipe),
`MainReservoir`, `IndependentBrake` (kgf/cm2), `Handbrake` (%),
`Velocity`, `Acceleration` (Idx 0 the magnitude, 1 towards the head, 2
towards the tail; m/s), `SMSStream` (the car's `CABIN` or `WAGON` sound:
SubIdx the stream, its value the trigger that last started a sound in it
+ 1 while the sound plays, 0 silent; PROBABLE: RTS's stream state, and
Kuban's scenario waits on `CABIN SMSStream 0 24 1` for the radio's call
to the station, stream 24 "Радиостанция", trigger 157). The others are
not read yet (the log names them; the condition waits).

Outcomes run: `ShowMessage` (a window, `ShowModal` holding the game,
`Buttons` MB_OK / MB_OKCANCEL / MB_YESNO / MB_YESNOCANCEL, `Expires`),
`PostMessage` (the F11 window), `ShowHint`, `Suspend`, `SetMainMenu`
(the F8 menu; −1 none), `ShowActivityMenu`, `EnableEvent`,
`DisableEvent`, `ActivateEvent`, `IncreaseActivationLevel`,
`SetActivationLevel` (service −1 the global events, −2 the event's),
`SignalOverride` (the signal object standing at `Position`, all its heads:
`SignalMode`, `SetAspect`, `SetDrawState`; `Cancel` gives it back to its
script), `AssignService` (the train is the new service's from then: its
`OnStart`), `StartWagonStateTracking`, `CheckWagonState`,
`SendSoundTrigger ( ObjectNum TriggerID )` (the code to the scenario's
`SOUND` object on its car, heard as the cab's while the player is in that
car; object −1: the car's own sounds), `ModifyWagons`' `OpenEndValves` /
`CloseEndValves` (the cocks at the group's ends: the first car's front, the
last's rear; the whole train by default), `ReleaseBlockPoint`,
`EnableBlockPoint`, `ModifyVelocity`, `WaitHere` (`docs/traffic.md`),
`SuccessfullyCompleteActivity`, `ActivityFailed`. An outcome's `TrainID`
names its train (default the event's).

The interface: messages in the middle of the screen; F8 the report menu
(the main menu `SetMainMenu` set; "Report: F8" at the top right while
there is one); F11 the scenario's window with everything said; hints at the
top. `--capture-ok` closes the messages and `--capture-report` picks each
menu's first item in an automated run (with `--capture-board`).

## Not yet

- Traffic's rest (`docs/traffic.md`, "Not yet"): `SERIAL` starts,
  `SpawnTrain`, `RemoveTrain`, `AlternatePath`, the conditions
  `ReachBlockPoint`, `ApproachBlockPoint`, `ReleasedBlockPoint`, the
  traffic's orders in `WaitAtLocation`; the server running it.
- The timetable: `StopAtStation`, `DepartFromStation`,
  `TimetableCompleted`, `ReachReversePoint`, `ReachEndPoint`; a
  `NewPathLoad` path of `AssignService`.
- `UncoupleCars` (the condition and the outcome), `SetDefaultUncouple`,
  `DeliverCars`, `PickupEvent`, `WaitAtLocation`, `ReachVelocity`,
  `RandomEvent`, `OnActivationLevel`, `PlayerBoardsTrain ( EXTERIOR )`.
- `ModifyWagons` beyond the end valves; the other WagonState variables
  (switches, doors, lamps, couplings, pipes' flags...).
- Player tasks, saves, weather changes, `FailActivityOn...`, static
  objects other than sounds.
- With the sound off (`--no-sound`) no sound plays, so `SMSStream` stays 0
  and Kuban's scenario waits at its first call.

## Novokuznetsk's "Скорый поезд 77" (2026-11-10)

15 services (3 `PLAYER`, 1 `STATIC` with 54 trains, 11 `TRAFFIC`), 27
events, 8 menus: every report is a menu item (F8). Played through by
`tests/activity_run_test.cpp` on a made-up world (reports, the coupling,
the places): 22 messages, 13 signals held and 14 let go, the train
shunting → the express → Prokopyevsk's shunting, completed. In the game
(`--capture-board 0 --capture-ok --capture-report`): boarding gives the
first menu, the two first reports open the shunting signals found by their
positions. On the 2026-11 build all 55 trains to stand are made. Its 11
traffic trains start; started at 09:49 (`--time 09:49`), ЭП1 - 308 stands
at its block point till 09:50 (`ReleaseOnTime`), then runs on.

## Kuban's "Кубанский выльник" (2026-11-10)

23 services (2 `PLAYER`, 1 `STATIC` with 32 trains, 20 `TRAFFIC`), 42
events; the reports are the radio's call (`SMSStream`), the brake test the
last car's cylinder (`WagonState`), the inspector's end valves
(`ModifyWagons`), the station's voice lines `SendSoundTrigger` to the
scenario's `sounds-cab.sms` on the engine's two sections. Played through by
the test on a made-up world: 20 voice lines, 6 signals, 4 end valve moves,
completed. In the game: its sound objects hang on their cars, boarding arms
the radio's call. On the 2026-11 build all 33 trains to stand are made.
Its 20 traffic trains start at 03:00, each with its block point found on
its path; the ones without (ЭП1М Албаши, Сочи, ТКС...) run their paths at
up to 38 m/s, the others stand 10 m short of their points till the
player's events release them (23 `ReleaseBlockPoint`s in the scenario).
The whole update stays about 3 ms a frame.
