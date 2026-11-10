# Traffic (AI) trains in RTS

Static, read-only analysis of a copy of `rts1.exe` (`build/ida/rts1.i64`,
headless IDA 9.0 + Hex-Rays, 2026-11-10), with RTS's `activity.pdf` (the
2026-11 build's `RTS` folder). Addresses refer to the analysed `rts1.exe`.

## The train's fields a scenario sets

**CONFIRMED** (the activity reader `0x471B9C` and the activity writer
`0x552360`, which saves a dispatcher's situation as an `.act`). The
scenario train record (`ActivityTrain`, candidate name):

| Offset | Field | Notes |
|---|---|---|
| +4 | `TrainEfficiency` | default 1.0, clamped to [0.03, 2.0] |
| +8, +12 | `TrainAcceleration` max acceleration, max deceleration | m/s², a negative value taken as its magnitude |
| +16 | `TrainMaxSpeed` | m/s |
| +20, +24 | `StartupSpeed`, `FinishSpeed` | m/s |
| +28 | `TrainDistanceOfNoReturn` | at least 250 m; default the scenario's |
| +32 | `TrainStartOffset` | m along the path |
| +36 | `TrainTimetableTollerance` | ≥ 0 |
| +60 | `TrainServiceType` | 1 passenger, 2 freight |
| +72 | `TrainState` | default 2 for a traffic service |
| +96 | `TrainFlags`; bit 0 start time given, bit 1 remove time given | both cleared when start > remove |
| +100, +104 | `TrainStartTime`, `TrainRemoveTime` | |

The train object (the same for every train; list `dword_61D794`, next at
`+0x18C`): `+0` its type (0 STATIC, 1 PLAYER, 2 TRAFFIC, 3 LOOSE,
4 MASTER_PLAYER, 5 MASTER_TRAFFIC; names `0x473CA8`), `+1` freight /
passenger, `+4` flags, `+16` its service, `+20` its scenario record,
`+28` / `+32` cars, `+36` its path, `+80` max speed, `+84` / `+88` max
acceleration / deceleration, `+92` its speed (m/s, signed), `+96` its
acceleration this step, `+168` / `+232` the placement points.

## Putting a train on its path (`0x4EF740`)

**CONFIRMED**: the cars laid back from the path's start (plus
`TrainStartOffset`) one after another (`0x481440`); the way reserved at
once (`0x48A0F8`); the fields taken from the scenario record (+80 ← +16,
+84 ← +8, +88 ← +12; flags; the timetable `0x4FA374`). **A traffic train
needs a positive max speed and acceleration** (a missing deceleration is
the acceleration): without them message 0x88 and the train is not placed —
activity.pdf's "TrainMaxSpeed: a positive value is required". The train is
not placed when the track ahead is taken (`0x47FE14`).

## How a traffic train moves (`0x424488`, the train update)

**CONFIRMED: kinematically.** For a TRAFFIC train (and the player's train
while the game drives it: `*(dword_61D31C + 68)`, PROBABLE the scenario's
`AIControlled`) the update picks an acceleration (`+96`), then
`speed += acceleration × dt` (stopping at 0 rather than reversing) and gives
the speed to the first and the last car. The per-car physics (resistance,
grade, couplers, the brake pipe, the DLLs' handles) runs only for LOOSE
trains and the player's own driving; STATIC ones stand. So a traffic train
is one rigid body following its path.

The controller (`0x424488`, the block from the `== 2` test):
- **The speed it may run at** — the least of: a global cap
  (`dword_61D7D0`), `TrainMaxSpeed`; the speed limits in force and ahead
  (fields +104, +108, +116..+128, +144: the track's, the next one's, a
  signal's, `ModifyVelocity`'s; which is which UNKNOWN), each times
  `TrainEfficiency`; the curves within 3 km ahead (`0x480204`): the
  scenario's `TrafficLimitsTable`, up to 10 radius thresholds
  (`dword_61D7DC`..) with their limits (`dword_61D804`..), 300 m/s (none)
  above them, times the efficiency.
- **Where it must slow down**: the next stop point of its way (`+60`, at
  `+72` m: the end of its reservation, a waiting point, a reverse point,
  a station stop; by the point's kind +40: kind 2 stops 40 m short, kind 5
  half a car short), the path's end at `FinishSpeed`, the speed posts
  along the way (kind 8), a train ahead in the same direction (coupled
  trains' couplers `0x505C08`, 10 m short). For each target speed v_t at
  d m it takes the one needing the strongest braking, (v − v_t) / d.
- **The acceleration**: below the speed it may run at, `+84` (no more than
  reaches it this step); above, −`+88`. Approaching the target of the
  strongest braking (v_t at d m), with Δv = v − v_t and the plain braking
  distance d₀ = (Δv / 2 + v_t) · Δv / dec = (v² − v_t²) / 2 dec:
  farther than 4 d₀ it goes on (an acceleration cut so as not to pass the
  4 d₀ mark); between 4 d₀ and d₀ it brakes with
  (0.25 + 0.75 · (1 − (d − d₀) / (4 d₀ − d₀))) · dec, 0.25 dec at 4 d₀ up
  to the full dec at d₀; nearer than d₀, 2 · dec. Behind a train ahead
  the decelerations double (and d₀ is halved when it is already late).
- **Stops**: at a station stop (kind 6) it lines up with the platform
  (centred or at its end, the stop's +108: 1 / 2), then waits its time
  (`+132`: the stop's time, scaled by the passengers, `+116` +60 / +64),
  not leaving before the departure time; arrival and departure go to the
  network / scenario as events 22 and 23 (`0x5C8C50`). Elsewhere a
  standing train waits `+136` s.
- **What it shows**: from the acceleration a seeming throttle (0.1..1,
  less at speed) drives each engine's sound and diesel (`+512` per engine
  `+32`), an electric's pantographs up below its `+632` speed.

## Reserving its way

`0x48A0F8` (`docs/reverse/dispatcher.md`): the junctions ahead in turn,
queues, the takeover by time to the junction, opposing moves; it stops at
a locked junction, a waiting point not yet due, and a block point whose
conditions (`0x48E36C`: the point's list of services / trains, `+100` /
`+104`, by its flag `+132` bit 3) say it holds this train.

## The defaults and the scenario's parameters

**CONFIRMED** (`0x407588`, run when a route loads; `0x470A3C`, the
scenario's top level):
- The curve table `dword_61D7DC` (radii) / `dword_61D804` (limits), when a
  scenario has no `TrafficLimitsTable`: 6000 m 75 m/s, 5000 55, 3500 45,
  2500 38, 2000 32, 1500 28, 1000 25, 600 17, 300 11, 200 7. A scenario's
  table replaces all ten (cleared first); an entry with a limit not above
  0 is dropped.
- The cap over all, `dword_61D7D0`: the route's `.trk` `SpeedLimit`.
  `dword_61D7D4`: its `TempRestrictedSpeed` (5 m/s when none or not
  positive), the speed the controller takes for a limit given as −1.
- `TrainsDistanceOfNoReturn`: default 5000 m, at least 250.

## Block points (`0x475B1C`)

**CONFIRMED** fields: `id` (+4), `enabled`, `location` (+32..+44: tile X,
tile Z, −x, y, z — RTS keeps x negated), `numpassed` (+108),
`releaseontime` (+112), `releaseonreach` (+116 / +120 service, train),
`releaseonapproach` (+124 / +128), `releasemanual`, `pointaffects` (+104
count, +100 the ids), `suppresserrors`. `Location` is read as its five
numbers; what the RTS editor writes after them (`NumPasses: 1`, seen in
every block point of the MaDaDev scenarios) is not read: `NumPassed` is
its own key.

## Names

**CONFIRMED** (the strings beside them in `rts1.exe`): the train flags
`OBEY_DISTANCE`, `OBEY_INFO`, `MUTE`, `NO_LIGHTS`, `PHYSX`,
`IGNORE_SIGNALS`, `OBEY_SHUNTING`, `DOORS_LOCKED`, `OWNS_PATH`,
`IGNORE_SPEED_LIMITS`, `CRITICAL_TRAIN`; the service flags
`FORCE_REMOVAL`, `CHECK_IF_BUSY`, `NO_REMOVE`, `CHECK_PLAYER_APPROACH`;
`ServiceStarts` `SINGLE`, `SERIAL` (and `NONE`). `PHYSX` PROBABLE: a
traffic train with its physics (not traced).

## In openRTS

`docs/traffic.md`: the kinematic train (`Train::set_kinematic`), the
controller (`simulation/traffic`), the block points in the dispatcher.

## UNKNOWN

- Which of +104 / +108 / +116..+128 / +144 is which limit.
- The scheduler of starts and removals (`TrainStartTime`,
  `TrainRemoveTime`, `ServiceTrainStartupTime`, the flags): not found
  yet; openRTS follows activity.pdf.
- How near the player a train is not removed without `FORCE_REMOVAL`.
- How the activity tick (`0x530D4C`, timed objects) and the event engine
  (`0x4027F0`, "Executing activity event") give traffic its orders
  (`WaitHere`, `StopAtStation` for traffic).
