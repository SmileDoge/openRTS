# Paths, reservations, speed limits, Track control

Roadmap step 4 (`roadmap.md`). Status: implemented, 2026-11-01. A train can
follow a path: the dispatcher reserves and sets the junctions ahead of it,
signals clear along the reserved way, it can run as traffic (AI), and the
Track control window shows and edits all of this. Reverse notes:
`reverse/dispatcher.md` (Track control, the path module and junction
queues), `reverse/track.md` (junction release).

## Using it

```text
openrts_viewer --window --route D:\Games\MDDRTS\ROUTES\Kuban_RTS --consist <name> [--path <name>] [--track-control]
    Ctrl+Alt+F7   the Track control window (also a checkbox in the debug panel)
openrts-path-check <installation root> <route> <consist> [path... | #N...] [--seconds N] [--together] [--following M]
openrts-path-check <installation root> <route> x --replan        (each path planned from its ends, compared with the .pat)
openrts-path-check <installation root> <route> x --junction N    (a junction, its tracks and their signals)
openrts-track-check <installation root> [route...]    (now also: every .pat resolved, speed post directions)
```

**Information keys** (as RTS's, `reverse/dispatcher.md`; for the followed
train, else the driven one):
- **F5** — a page of the train's cars: the overall line (speed,
  acceleration, limit, next limit and its distance, kilometre) and per car
  its mass, pipe / auxiliary / cylinder pressures, velocity, acceleration,
  forces, handbrake, derailment; F5 again turns the page, past the last one
  it hides; **Shift+F5** the train as a whole (mass, length, passenger /
  freight cars, engines);
- **Ctrl+F5** — the kilometre and picket (from the nearest kilometre post
  behind, counting the way the posts ahead go), the limit now, the limits
  ahead within 10 km with their distances, the next signal;
- **F6** — the stations ahead with their distances (RTS shows the
  activity's schedule there; openRTS has no activities yet): along the path,
  or along the junctions as set within 40 km; a platform named after its
  station shows only the rest ("о.п. ЗИП, п.I");
- **the panels can be dragged** with the left mouse button; each keeps the
  edge it is anchored to (F5 the left one, Ctrl+F5 and F6 the right one) and
  its place is remembered in `viewer_state.ini`;
- **Ctrl+F7** — signal names from the route database (`<route>/<RouteID>.rtsdb`)
  within 150 m, 3 m above the signal, fading from 30 m on; Track control
  shows them next to the signals when zoomed in. On the installed routes
  75–97 % of the named units find their TDB signal item as RTS matches it
  (x/y/z within 0.2 m); every other one stands 19–29 m from its signal (a
  systematic shift, median about 20 m on every route; why is UNKNOWN), where
  RTS draws its name. openRTS gives each such unit the nearest signal item
  no other unit has, within 30 m, and puts the name over that signal's place
  on the track;
- **F7** — platform and siding names as RTS shows them: a thick line along
  the track between the two ends of each (blue platforms, yellow sidings) and
  the name on a post above its middle ("station (platform)"); and every car's
  number ("AAAA-BBBB-CCCC" as RTS writes it: the train, 0, the car's UiD in
  its consist) on the side facing the camera, 2.7 m from the end of the body,
  within 60 m;
- with Ctrl+F7 and with the service labels (L), every signal gets a line
  down to the place on the track its head stands for, with a short mark
  along the track pointing the way the head protects — which track a signal
  between two tracks governs;
- **Ctrl+Alt+F7** (as RTS) — Track control (also a checkbox in the panel and
  `--track-control`); **Ctrl+F6** saves a debug view (F6 alone is the
  stations panel).

**Track control** (RTS's in-game "RTS track control"):
- the route's real track from above on a light green background, as RTS
  draws it; wheel zooms about the cursor, right drag moves, a right click
  opens the menu for what is under the cursor, a left click selects;
- trains as their cars: magenta driven, blue traffic (AI), olive standing,
  red derailed; the selected train outlined, its path drawn: the reserved
  way green, the rest of the leg blue, later legs pale; W / R / E mark
  waiting, reverse and end points;
- junctions: orange reserved, red occupied, the set branch green when zoomed
  in; signals as squares in the colour of their aspect, each with a line to
  the place on the track its heads stand for and a mark there pointing the
  way the head protects (which track a signal between two tracks governs),
  and its name from the route database when zoomed in; platforms as a thick
  blue line along the track between their two ends, sidings yellow, their
  names on a leader;
- the player (the camera) as a small figure with a cone the way it looks;
  "Teleport here" in the menu of a track point (eye height over the track,
  looking along it), a junction or an empty spot (over the ground, keeping
  the view direction) moves the camera there and stops following a train;
- station names as RTS writes them: black, larger text at the middle of each
  station's platforms; zooming out, only the stations with more platforms
  stay named (RTS's rule: items × 300 above its zoom, none past zoom 3500);
  "Stations" turns them off;
- menus:
  - **train:** jump to it (the camera follows it), drive it with the
    keyboard / run it as traffic (AI) / let it stand, create or edit its
    path, cancel the path, load a route path, save the path, uncouple, remove;
    "Signals for this train": the mode its signals get (as they are, SHUNT,
    NORMAL — RTS's SetSignalMode) and whether it obeys shunting, distance and
    info signals;
  - **junction:** who reserves it; throw it (refused while occupied or
    held by a path); on the edited path, switch it at that pass;
  - **signal:** its heads (aspect, draw state, block_state, route_set, mode,
    whether overridden); per head an override (RTS SignalOverride): a mode
    (NORMAL, SHUNT, FAILED — dark, EXCL — left out of chains) and / or an
    aspect from its type, or clear it;
  - **track:** while editing, place the start, a point to pass, a waiting
    point (seconds), a reverse point, the end point; otherwise "path for the
    selected train to here"; put a consist here — from the content's list or
    with "Browse..." from any .con on disk (Windows' Open dialog; it starts
    in the folder the last one came from, kept in `viewer_state.ini` in the
    working directory; the cars still come from the content's TRAINSET);
  - **placing a consist** (after choosing it) shows a preview first:
    - every car at its true length, the front car orange with an arrow the
      way the train will face, engines pale orange; the front of car 0 at
      the clicked point, the cars behind it along the junctions as set (the
      way that fits is chosen at first);
    - drag a car with the left button to slide the consist along the track
      (it follows the set junctions, as the train would roll); a click on
      another track moves its front there, facing the same way on the screen
      as far as the track allows; arrows nudge it 1 m (Shift: 10 m);
    - Space turns it around on the same stretch (the front car goes to the
      other end); Enter or "Put it here" places it (as traffic or
      standing), Escape drops it; a right click on the preview offers the same;
    - a red outline: not enough track (it cannot be placed), or it runs into
      another train (within 1.6 m of one of its cars; it can still be
      placed);
  - **path point:** change the start direction, change a waiting time,
    remove the point;
- saved paths go to `user_paths/<route>/<name>.pat` in the working directory
  (never into the route) and appear in the path lists as `user/<name>`.

## Paths

`simulation/train_route` (`TrainRoute`, `PathPlan`):
- **Plan:** the start (node, offset, direction) and the points after it
  (pass, wait with seconds, reverse, end). The way to each point is the
  cheapest one, unless the point lists the junctions to pass (`through`,
  from a .pat) or the branch to take at facing junctions (`branches`, Track
  control's "switch junction").
- **Cheapest:** the length, plus 100 m for every facing junction taken off
  its MainRoute. With the bare shortest way a planned path out of
  Krasnodar-1 left by the diverging branch of junction 1416 onto the other
  track of the double line (marginally shorter); RTS's re-routing shows the
  same kind of mistake. Measured with `openrts-path-check --replan` (every
  installed .pat planned again from its ends only, compared junction by
  junction with the author's): 2 457 of 4 781 passes the same without the
  cost, 3 401 with 25 m, 3 409 with 100 m, 3 427 with 300 m; the rest are
  mostly station tracks the authors chose. Making track with signals only
  for the other direction dearer helped on Kuban and hurt on other routes,
  so it is not used.
- **Route:** legs of track; a reverse point ends a leg and the next one runs
  back. Each leg is a list of spans (a stretch of one vector node in the
  travel direction), the junctions passed (facing / trailing, the branch) and
  the marks, all by distance along the leg.
- **From a .pat** (as RTS loads it, `0x4886EC`, CONFIRMED): TrackPDPs of
  kind 2 are junctions (the junction node at that position, within 3 m,
  8 m as a fallback), others are points on the track; the first point is the
  start, the last the end; the start direction is the one that reaches the
  second point. Where a junction's branches both reach the next point (a
  passing loop with no point on it), RTS takes the last matching branch (its
  loop does not stop at the first match); openRTS does the same.
- **Node flags** (wait / reverse, the wait in the high word) follow the MSTS
  convention as Open Rails reads it — PROBABLE: no installed path uses them.
- **To a .pat:** the start, every junction passed, every plan point; where
  RTS would read a junction's branch differently, a point on the branch
  taken is added, so RTS and openRTS read the file the same way.
- **Checked:** all 109 installed paths on the 7 routes resolve
  (`openrts-track-check`), about 4 800 junction passes.

## Reservations

`simulation/dispatcher` (RTS `0x48A0F8`, queues `0x48C7B8` / `0x48C924`,
release `0x48C85C`; `reverse/dispatcher.md`). Every step, after the trains
moved:
1. the train is found on its leg (both ends; the one further along leads);
   points to pass are marked as reached, waiting points once the driver has
   waited (or the train ran past), a reverse point ends the leg when the
   train stands within 30 m of it, the end point finishes the path;
2. the junctions ahead are walked up to 3000 m (RTS walks the whole path;
   its priority test looks 5000 m / speed ahead):
   - a free junction is reserved and set for the train's branch (also
     trailing: set for the branch it arrives from);
   - one the train holds first is set for it (unless occupied);
   - one held by another train queues it; the reservation ends there,
     unless this train gets there more than 30 s earlier and the holder is
     beyond its stopping distance: then it takes the junction over (RTS
     `0x48C924`), and the holder's way ends there instead;
   - a junction occupied by another train and set the other way ends it;
3. a single line: a train does not reserve into a stretch between two
   junctions that another train holds or has reserved the other way, gives
   up the junction in front of such a stretch (another train may need it to
   clear the stretch), and stops one junction earlier when the train that
   blocks it will come through the stretch before the blocking junction;
4. reservations ahead that the way no longer reaches are given up; the
   train's tail releases the junctions it passes (the train does this, as
   before: `release_junction`, `ClearanceDist`).

The reserved way is the train's **movement authority**: signals beyond it
run with `enabled` 0 and stay at danger (RTS: heads beyond the train's path
or reservation end are not enabled), so a signal clears once the way is set
for the train. A junction reserved for a path cannot be thrown by hand
(`TrackNetwork::throw_junction`); trains without a path work as before (no
reservation, junctions as they are set).

## Speed limits

`simulation/speed_limits`; `formats::speed_post` reads SpeedpostTrItemData
as RTS does (`0x482680`, CONFIRMED): flag 2 a limit (with flag 1 the
restriction ends: the route's SpeedLimit from the .trk), the speed in km/h
or mph (flag 0x100), passenger / freight / both (0x20 / 0x40 / 0x80).
- **Direction** — PROBABLE, measured on the installed routes: the sign
  faces the heading 3/2 pi − angle and is for trains travelling against
  that facing. Warning boards come before the limit of their speed 11 times
  of 11, restriction ends after a limit for the same direction 193 times of
  209; the three other readings of the angle fail one test or both.
- **Which posts a train obeys** (CONFIRMED, static, 2026-11-06, `0x47F7B4`
  called by `0x47F7F0` from the look-ahead `0x508520` and others): by the
  train's category, train byte `+1`, the console's "Прочий" 0 /
  "Пассажирский" 1 / "Грузовой" 2. Category 0 obeys every post; a post for
  passenger trains (0x20) only category 1, one for freight (0x40) only
  category 2; a post for both, both. In a network game the client sets
  the category from the server only (`0x4607`, `0x4605`, `0x4C3DE0`); a
  player's `rts1.exe` sends `0x4605` only as a mini-server host (role 2,
  `0x4D249C`, after a split and a type change), so on a dedicated server
  the dispatcher sets it in "Редактирование поезда", traffic and players
  alike. Where single player takes it from is not traced yet. openRTS
  does the same with the server's category in a network game
  (`simulation::TrainCategory`, `SpeedLimits::applies`); in single player
  a train with no passenger cars is freight and any other passenger.
- **In force:** the nearest applicable post behind the train's front; a
  higher limit only once the rear has passed its post too.
- **Ahead:** the next applicable post within 3 km.

## AI trains

Since 2026-11-10 an AI train runs as RTS's traffic (`docs/traffic.md`):
one body along its path, RTS's controller (`simulation/traffic`), no
per-car physics. What follows is the earlier test driver,
`simulation/auto_driver` (`AutoDriver`, `path_orders`), kept in the code
and its tests but no longer used by the game: a driver working the
player's handles on the full physics.
- It works the player's handles: throttle notch by notch, the reverser
  while standing, the 395 valve (V until the equalising reservoir is down
  by 0.4–1.6 atm for the braking it needs, then III; II to release), the
  independent brake while standing.
- It stands at the end of its reserved way, at a waiting point (for its
  time), at a signal at danger (8 m short of each); it plans stops and the
  next lower limit with 0.35 m/s².
- Limits it keeps to: the speed post limit, the next signal's speed as a
  limit at that signal, and curves: 0.85 of the speed at which a car's
  m·v²/R reaches its DerailRailForce (the derailment rule), for the curves
  under the train and within braking distance ahead.
- **Checked** with `openrts-path-check`:
  - TestRoute_RTS, all 3 paths: every train arrives (58 km, 58 km, 47 km);
  - Kuban_RTS, all 12 paths alone (62–64 km each in 40 minutes);
  - four Kuban trains together, two opposing trains on one line, and a
    train following another 1.5 km behind (it stops at the signal before the
    occupied platform);
  - no derailment, no signal passed at danger, at most 6 km/h over a
    limit (at the post where a lower limit starts).

## Code

| Part | Where |
|---|---|
| .pat point kinds, node flags, writer; speed posts; .trk SpeedLimit | `formats/train_path`, `formats/track_database`, `formats/route_world` |
| plans, routes, .pat resolution and writing | `simulation/train_route` |
| reservations, authority | `simulation/dispatcher`, `TrackNetwork::cancel_reservation` |
| signals: authority, pass aspect | `simulation/signals` |
| speed limits | `simulation/speed_limits` |
| traffic: its controller and its orders | `simulation/traffic` (`docs/traffic.md`) |
| the earlier test driver | `simulation/auto_driver` |
| trains in the viewer: roles, spawning, paths | `viewer/train_view` |
| Track control | `viewer/track_control` |
| checks | `tests/path_test.cpp`, `tools/path-check`, `tools/track-check` |

## Not yet

- Station routes and interlocking as the dispatcher console sets them
  (route points, routes between them, route generation): step 6, with the
  console (`roadmap.md`).
- Signal modes and overrides (`SetSignalMode`, `SignalOverride`): set from
  Track control's train and signal menus; activities and the dispatcher
  console do not set them yet.
- Track control: the cargo layer (no data model yet; the electrification
  layer exists, `electrification.md`), the editor's three buttons, `Release
  block point` (the scenarios' block points run, `docs/traffic.md`).
- RTS's traffic priorities in the reservations (`0x48A0F8` time estimates)
  are only partly followed (the takeover rule).
