# The engineer on foot: trajectories, cars' outsides, objects' boxes

Static analysis of a copy of `rts1.exe` (`build/ida/rts1.i64`, 2026-11-11),
with RTS's `ext.pdf` ("Кабины, салоны, внешняя модель", the 2026-11
build's `RTS` folder). RTS keeps X negated in its own coordinates (every
`.sd` position, rotation about Y and Z, translation X); below, values are
as the files write them. The keys in a chair, doors and the cab's walking:
`train-dll.md` section 13.

## What the content uses

In the 2026-11 build: `Trajectory` in 3337 `.sd` files of TRAINSET (13717
of them: doors, ladders), `ExteriorLadders` in 1318, `ExteriorPlanes` in
858, `ExteriorLedges` in 825; `ESD_Complex` boxes in 251 `.sd` files of
the routes (1508 boxes: footbridges, platforms, buildings).

## A car's outside (`ESD_Exterior_Details`, read by `0x4F0BC8`)

**CONFIRMED** layouts:
- `ExteriorDoor` (120 bytes): `Name`, `Connects` (+60), `GlobalID` (+56;
  1 / 2 swapped on a turned car), `ParentObject` (+34), `Position` (+16),
  `Radius` (+28), `Trajectory` (+68 count, +72 points, flag 1 at +52),
  `InteriorPlane` (+76, two corners, flag 2), `AnimTime`, `Opened` (+44),
  `CanBeOpened` (flag 4; without the key: openable), `SMSTriggers`.
- `ExteriorLadder` (72 bytes): `Connects` (+52), `Position` (+16),
  `Radius` (+28), `Trajectory` (+64 count, +68 points).
- `ExteriorPlane` (40 bytes): two corners (+12..+32, the box between them),
  flags (+36): `EXIT` 1 (he may step off it, to the ground), the word for
  the front car 2 and for the back car 4 (swapped on a turned car).
- `ExteriorLedge` (24 bytes): a `Trajectory` (`0x529D70`): segments he
  walks along.
- A trajectory point: X Y Z and the kind of move to it (16 bytes).
- `Connects` (`0x4D47A4`): `EXTERIOR` −1, `EXT_WAG` (the car's outside)
  −2, `INTERIOR` (the cab) −3, `WAGON_FRONT` / `WAGON_BACK` −4 / −5
  (swapped on a turned car), a salon −7, an area's name its index.

## Moving along a trajectory (Ctrl+Enter)

**CONFIRMED** `0x4310B0` (a door), `sub_4352B0` (the path), `0x435654`
(each frame, from the main update `0x424488`):
- The path is the trajectory's points in the car's frame (outside: turned
  into the world's by the car's rotation and position). It starts at the
  point **nearest the engineer**: a trajectory serves both ways, up and
  down. He first goes from where he stands to that point.
- Each frame he moves towards the next point by speed × dt (at most to
  it); within 0.02 m of it the next point is taken. The speed by the kind
  of the point moved to, d the distance left, L the most it has been on
  this stretch, s `player_speed` (config.ini `engineer.velocity`):

  | Kind | Speed |
  |---|---|
  | 0 (and any other) | s |
  | 1 | s; in the last 0.3 L: 0.7 s · d / 0.3 L + 0.01 |
  | 2 | in the first 0.4 L: 0.7 s · (L − d) / 0.4 L + 0.1; s; in the last 0.3 L: 0.7 s · d / 0.3 L + 0.1 |
  | 3 | in the first 0.4 L as 2, then s |
  | 4 | ("a jerk") 0.4 s, from 0.9 L down to 0.75 L 0.4 s · (2.01 − (d − 0.75 L) / 0.075 L), down to 0.5 L 0.4 s · ((d − 0.5 L) · 1.9 / 0.25 L + 0.1), then 0.4 s |
  | 5 | 100000 m/s: at once |
  | 6 | by the horizontal distance: half the stretch a second, at least 2 s (not in ext.pdf) |

- At the end his height is the last point's; where he is then is the
  passage's other side (`Connects`): −3 the cab (loaded, the engine his;
  "Failed to load cabin" turns him back), −7 the salon, −2 the car's
  outside (mode 6), −1 the ground (mode 1), a chair (5) or a window (6).
- A door without a trajectory: the path is where he stands, the passage
  ends at once.
- **CONFIRMED** which way and to what (the end's word is the `Connects`
  word on the side it goes to): a ladder (`0x431F88`, Ctrl+Enter within
  its Radius): from the ground, when its second word leads somewhere but
  the ground, forwards; else backwards when its last point is nearer him
  than its first. A ladder without `Connects` (the ChME3's roof ladders)
  ends with him where it ends, on the car's outside. An outer door
  (`0x4310B0`): forwards, to its second word. A cab's door to the car's
  outside (Ctrl+Enter, `0x431270`): the car's `ExteriorDoor` of the same
  name (with a trajectory) backwards, to the cab door's other side.
- **CONFIRMED** falling (`0x4121BC`): where the place stepped to is lower
  he falls at 9.8 m/s² (`flt_61D418`) until he is there; higher, he is put
  there at once.

## Walking in a cab (mode 2, `sub_4322F0`, `0x434C58`)

**CONFIRMED** The cab loader (`0x51CD88`) keeps an `InteriorPlane ( [name]
x1 y1 z1 x2 y2 z2 )` as written, X negated (24 bytes at the cab's +164,
count +72; a door's at its +76, flag 2), sorting nothing. Each step tests
the place stepped to (`0x434C58`, the cab's frame):
- a plane holds it when it lies from the first corner's X to the second's
  and from the first's Z down to the second's (file coordinates); a plane
  written the other way round holds nobody (in the 2026-11 build: three in
  the 2TE116's cabs, and 26 doors' planes, all outer doors and passages).
  Its height goes from y1 at z1 to y2 at z2 (along Z only; 10 sloped
  planes, e.g. the ER9P's door steps from 1.0 to 0.0);
- of the planes holding it, the highest under his feet + 0.4 `Height`
  (any way down); then an open door's plane (+44) above that one; then a
  ledge (`+104` count, `+200`, 24 bytes) within 0.3 m (four cabs have
  them; not in openRTS).

Then (`sub_4322F0`, the cab's branch): held, he goes there at its height.
Not held, from a plane he is brought into that plane's rectangle (`0x4350E0`:
he slides along its edge); from an open door's plane, outside every
`InteriorArea` (`sub_51434C`): `EXTERIOR` gets him off (slower than
`max_speed_to_get_off`), `WAGON_FRONT` / `WAGON_BACK` to the next car
(`0x4313FC`), `EXT_WAG` onto the car's outside (`0x431E00`); else he stays
where he was. Nothing else stops him in a cab: no walls, chairs or desks,
only where the planes are not. The VL10k's `CABIN` (X -0.251..0.336) is the
aisle between its chairs (0.717, -0.798): one cannot step onto them.

The chairs' points: 539 `Pos3` of 582 lie on a plane, 43 up to 0.3 m off
(the VL10k's 0.034 m; RTS keeps him there until a step lands on a plane).
Seated his feet are `Height − Height1` under `Pos1.y`, risen at `Pos2.y`,
standing up at `Pos3` (`0x436CA8`, `0x43778C`); the eye `Height` above.

## Walking on a car's outside (mode 6, `sub_4322F0`, `0x434444`)

**CONFIRMED** The loader (`0x4F0BC8`) sorts an `ExteriorPlane`'s and an
outer door's `InteriorPlane` corners along X and Z (`0x45B750`; Y stays as
written); the words `EXIT` (flag 1), `WAGON_FRONT` (2) and `WAGON_BACK` (4,
the two swapped on a turned car). Each step tests the place stepped to
(`0x434444`, the car's frame), in this order:
- on a plane: strictly inside it along X and Z and not more than 0.05 m
  under either corner's height: the first such plane; his height `y2 +
  (y2 − y1) · (z − z_low) / (z_high − z_low)`, y1 and y2 as written (RTS's
  own formula: from y2 at the low Z the wrong way; every exterior plane in
  the 2026-11 build is flat);
- else on a ledge: within 0.3 m (3D) of one of its segments, beside it
  (the last such), put onto it;
- else in an open door's `InteriorPlane` (+44 open, flag 2): there, his
  height as it was;
- (else on the car's own boxes, kind 9: not in openRTS.)

Not held (`sub_4322F0`), by what he stood on last (`dword_61D6B0`):
- a plane with flag 2 / 4: over to the car at that end (`0x4313FC`);
  without `EXIT` he stays where he was;
- an open door's plane whose second word is `INTERIOR`: when the cab's
  planes hold the place (`0x434C58`), through the door along its way
  (`0x4310B0`), into the cab; `WAGON_FRONT` / `WAGON_BACK`: to the next car;
- else (a plane with `EXIT`, a door's plane, nothing): at the side
  (|x| ≥ 0.4 of the car's width) down to the ground (`0x4369F4`, when the
  train is slower than `max_speed_to_get_off`), at an end over to the next
  car, else down to the ground.

From the cab, off an open door's plane whose word is `EXT_WAG` (walking
in a cab, above), he is on the car's outside where it holds him
(`0x431E00`), mode 6. The ChME3 (`mdd_CHME3-1277.sd`): six steps and
ladders (four steps from the ground, `EXTERIOR EXT_WAG`, kinds 0 4 4 4 1;
two up to the roof at 4.0 without `Connects`), six walkway planes at 1.4
without `EXIT` (one cannot step off them: down by the steps only), two
ledges on the roof, its cab doors `DOOR_D` / `DOOR_A` (`EXT_WAG INTERIOR`,
ways of two points) and the passages over the couplers. From the roof
ladder's top a step that lands over the walkway's plane takes him down
onto it (the plane is under him), one inwards onto the roof's ledge.

## Walking on the ground: objects' `ESD_Complex` boxes (`sub_4322F0`, mode 1)

**CONFIRMED** `ESD_Complex ( n ESD_Complex_Box ( rx ry rz tx ty tz x1 y1 z1
x2 y2 z2 ) ... )` (`0x4BB7BC`, 48 bytes a box): a box between the two
corners, turned by rx, ry, rz degrees (Z · Y · X, `sub_45EC40`; into the
box by the inverse, `sub_45EE28`) and moved by tx, ty, tz in the shape's
frame. E.g. Kuban's `R_FootBridge5mStairwayBoth`: its deck (y 8.7..9.4)
and two flights, 26 m boxes turned ±23 degrees about X. The loader
negates x1, x2, ry, rz and tx (RTS's frame) and sorts the corners: in the
file's frame the box is `Rx(rx) · Ry(ry) · Rz(rz)` of the box between the
sorted corners, plus (tx, ty, tz). A shape without `ESD_Complex` but with
`ESD_Bounding_Box ( x1 y1 z1 x2 y2 z2 )` gets that one box (2037 route
`.sd` in the 2026-11 build have one, 251 have `ESD_Complex`). A box is the
author's ladder too: the lighting mast `Osvstozar4.sd` (Kuban,
Novokuznetsk, Pavlovsk, ...) is 30 boxes of 1 m one on another (y 0..30)
and a platform 4 x 2 m on top (30..30.3); walking at it one climbs it box
by box (each top 1 m, 0.55 Height, above his feet).

For every loaded object with boxes near him (2.5 × its radius), the place
stepped to, the one stepped from and his head are taken into each box
(radius r = 0.3 m, `0x5E04E4`):
- his head inside a box (within r): pushed out of it across the nearer
  side (a wall, a roof); a box of a car's object (kind 17) on the car's
  outside puts him there (`0x436B58`);
- his feet over the box (within r): on its top when within r of it (he
  stands on it); when its top is less than 0.4 × his height above his
  feet, onto it (steps and flights); from 0.4 to 0.9 × his height, he
  **climbs** it: a path of three points (half way up with kind 2, up,
  over to the place, kind 0) run as a passage; higher, a wall: pushed
  out across the side nearer his feet.
- The boxes go in their order and the climb starts at once: on a stack
  (the mast) the box over the one he stands on is climbed before the one
  his head would be in pushes him away.
- The step's place is turned back into the world after an object's boxes
  moved it; a rise steeper than 0.8 of the way counts as on a box.
- On no box: he stands on the track's bed near a track (`0x47E26C`, within
  3 m, rising over 2.5 m), else on the ground; never under the ground.

## In openRTS

`docs/train-systems.md` / `docs/reverse/train-dll.md` (the engineer); the
rules: `viewer/src/walk_rules.cpp` (`Passage`, `outside_footing`,
`step_among_boxes`), `Engineer` (`walk_planes`, `walk_on_car`,
`update_on_car`), `TrainView::carry_out`.

Since 2026-11-11, as above:
- in a cab on its planes (`Engineer::walk_planes`), with two additions of
  openRTS's own: from a place off every plane (a `Pos3`) a step takes him
  onto the nearest plane within 0.3 m, and climbing in by an outer door
  puts him on the plane 0.45 m inside it (RTS runs the door's
  `Trajectory`); off a door's plane to `EXT_WAG` it looks 0.1 m on for the
  area's edge (the ChME3's door plane ends 0.036 m short of it, less than
  a step at a walk). A cab without planes walks on its shape's surfaces;
- on a car's outside (`Engineer::Mode::on_car`): the steps and ladders
  (Ctrl+Enter from the ground or the car), the walkways, ledges, outer
  doors (Enter opens a cab's door through the cab's door of its name),
  through a door into its cab and out again, over to the next car's
  outside, down to the ground; trajectories with RTS's speeds; falling;
- on the ground among the world's objects' boxes (`ESD_Complex`,
  `ESD_Bounding_Box`): an object with boxes is walked by them only (its
  surfaces not read), with the setting «collide with objects»;
- through a passage door into the next car straight on (openRTS's own,
  `TrainView::pass_to_next_car`): as far beyond that car's door's point
  and as far aside as he was beyond his own (the ED9M: 0.6 m past z 10.4
  at 11.0), on its floor there or up to 1 m further in; else at its door's
  point as before (RTS runs that door's way).

Not done: the speeds to get on and off (`MaxSpeedToGetOn` /
`MaxSpeedToGetOff`), the track's bed, a cab's ledges and a car's own boxes
(kind 9, 17), the salon's side (−7), and over to the next car's cab from
its outside. The eye is `Height` above his feet everywhere but in a cab
without planes (0.85 `Height`); before 2026-11-11 also outside (0.27 m
lower than RTS's).
