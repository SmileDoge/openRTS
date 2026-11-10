# Traffic (AI) trains

openRTS runs AI trains as RTS runs its `TRAFFIC` trains (the analysis:
`docs/reverse/traffic.md`): **the train is one rigid body following its
path**, and RTS's controller picks its acceleration. There is no per-car
physics for it: no traction, brakes, brake pipe, couplers, resistance or
derailment, no locomotive DLLs. So dozens of long trains cost little (the
Kuban scenario's 20 traffic trains, 32 standing ones and the player's: about
3 ms a frame for the whole update). Full physics stays for the player's
train and for loose cars.

The code is `simulation/traffic` (`TrafficDriver`, `traffic_orders`) and
`Train::set_kinematic`; it has no viewer types, so the multiplayer server
can run traffic the same way (planned, `docs/roadmap.md` step 7).

## How a traffic train runs

- **One body** (RTS `0x424488`): each 50 ms step its speed changes by the
  acceleration its controller chose, stopping at 0 rather than turning
  back; every car moves with it (positions, rail joints, junctions, the body
  springs and the joint knocks are as for any train).
- **The speed it may run at**: the least of the route's `SpeedLimit` (the
  `.trk`), its `TrainMaxSpeed`, and, times its `TrainEfficiency`, the speed
  post limit in force and the curve limit under it.
- **Where it must slow down or stand** (targets ahead): the end of its
  reserved way (10 m short), a signal at danger (10 m short), a waiting,
  reverse or end point of its path (1 m short; the path's end at
  `FinishSpeed`), the next speed posts and signal speeds (times its
  efficiency), the curves within 3 km (`TrafficLimitsTable`, below), the
  end of another train ahead on its way (10 m short). Of these it plans for
  the one that needs the strongest braking, (v − v_t) / d.
- **The acceleration**: below the speed it may run at, its `MaxAcc`;
  above it, its `MaxDec`. Approaching the target, with the plain braking
  distance d0 = (v² − v_t²) / 2·MaxDec: beyond 4·d0 it runs on; between
  4·d0 and d0 it brakes from a quarter of `MaxDec` up to all of it; nearer
  than d0, twice `MaxDec` (for a train ahead the decelerations double).
- **Curves**: the scenario's `TrafficLimitsTable` (up to 10 `TrafficLimit
  ( Radius Limit )`: curves tighter than Radius at Limit m/s; the last
  entry that applies wins), else RTS's own table (`0x407588`): below
  6000 m 75 m/s, 5000 55, 3500 45, 2500 38, 2000 32, 1500 28, 1000 25,
  600 17, 300 11, 200 7.
- **Waiting points**: it stands there for the point's time, then goes on.
- **What it shows**: from the acceleration a seeming throttle, 0.1 (idle)
  to 1, less at speed (RTS's formula): the engines' sound variables
  (`Variable1`, `Variable2` while it accelerates) and the diesel exhaust;
  electric engines raise their pantographs as RTS's AI trains do.

Traffic needs a positive `TrainMaxSpeed` and `MaxAcc`; a missing `MaxDec`
is the `MaxAcc` (RTS refuses such a train with message 0x88; openRTS logs
why and leaves it out).

## A scenario's traffic

- **Starting**: `ServiceStarts ( SINGLE )`: each train at its
  `TrainStartTime` (none: when the scenario starts); `SERIAL` is started as
  `SINGLE` for now; `NONE` is not started. The consist (the scenario's
  folder first) at its path's start, the last car's rear there,
  `TrainStartOffset` m further on; `StartupSpeed` given. Its place taken by
  another train: with `CHECK_IF_BUSY` it waits for the place to clear (till
  its `TrainRemoveTime`), otherwise its start fails (activity.pdf). The log:
  `activity: traffic <name> (<consist>, <n> cars) on its path`.
- **Its events**: a started train brings its service's events into play
  (its `OnStart` fires); its `ReachLocation` and the like are checked on it.
- **Removing**: at its path's end (not with `NO_REMOVE`; always with a
  `FinishSpeed`), or at its `TrainRemoveTime`. Without `FORCE_REMOVAL`
  only when it is 1 km or more from the player's train (activity.pdf: "not
  near the player"; RTS's distance UNKNOWN).
- **Flags read**: `IGNORE_SIGNALS`, `IGNORE_SPEED_LIMITS`; service flags
  `FORCE_REMOVAL`, `CHECK_IF_BUSY`, `NO_REMOVE`.
- **`ModifyVelocity ( MaxVel MaxAcc MaxDec )`** changes a traffic train's
  maximum speed and rates (not positive: unchanged); **`WaitHere ( t )`**
  makes it stand t s once it has stopped (t ≤ 0: for good).
- **The log** has a line for every traffic train once a minute of game
  time: speed, where on its path, how far its way is reserved and what
  holds it (`held by block point <service> <point>`, a junction taken).

## Block points

A block point (activity.pdf ch. 3; RTS `0x475B1C` reads it) is a place on
the paths: while it is enabled, no train it holds gets its way reserved past
it; the signals before it stay at danger, a traffic train stands at its way's
end before it.

- **Which trains**: a service's point holds that service's trains
  (`PointAffects` lists which; none: all of them); a global point
  (`GlobalBlockPoints`) the services listed, else every service on whose
  path it lies. The player's train is held as well: it gets its service's
  path at the start (the dispatcher reserves it and sets its junctions, as
  RTS does), and the new service's path on `AssignService` (PROBABLE: an
  empty `NewPathLoad` means the new service's own path).
- **Where**: on the `NumPassed`-th place (from 0) where the train's path
  comes within 10 m of its `Location`. What the RTS editor writes after the
  five numbers of `Location` (`NumPasses: 1`) is not read, by RTS either.
- **Released**: by `ReleaseBlockPoint ( ServiceID PointID )` (−1 the global
  points, −2 the event's service; point −1 all of them), at
  `ReleaseOnTime`, when a train named by `ReleaseOnReach ( ServiceID
  TrainID )` reaches it, when one named by `ReleaseOnApproach` gets its way
  reserved up to it.
- **Enabled again**: `EnableBlockPoint ( ServiceID PointID ( Distance ) )`.
  A train whose way already runs past it goes on when it is nearer than
  `Distance` or its distance of no return (`TrainsDistanceOfNoReturn`,
  default 5000 m; a train's `TrainDistanceOfNoReturn`); a farther one has
  its way beyond cancelled.

## Track control's AI trains

A train given "As traffic (AI)" in Track control runs the same way, with
openRTS's values: its engines' lowest `MaxVelocity` (else the route's limit,
else 33 m/s), 0.4 m/s² up and 0.5 down (RTS asks the dispatcher for none).
It needs an engine. Taken over by the player it gets its physics back.

## Not yet

- The server running the traffic (it will step the same `simulation`
  code; clients only show it).
- `SERIAL` starts (`ServiceTrainStartupTime`, intervals), `SpawnTrain`,
  `RemoveTrain`, `AlternatePath`, `CHECK_PLAYER_APPROACH`.
- Timetables: stopping at platforms (`TrainSchedule`, `StopAtStation`,
  `DepartFromStation`), the passengers' times.
- Conditions on block points (`ReachBlockPoint`, `ApproachBlockPoint`,
  `ReleasedBlockPoint`), `ReleaseManual` (Tab), `WaitAtLocation` as an
  order for traffic, the signal types a train obeys (`OBEY_SHUNTING`...),
  `TrainState` 0 (a cold traffic train), `MUTE`, `NO_LIGHTS`.
- Traffic does not couple; its contact with another train is the usual one.
