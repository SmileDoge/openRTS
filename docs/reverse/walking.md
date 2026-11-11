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

## Walking on a car's outside (mode 6, `sub_4322F0`, `0x434444`)

**CONFIRMED** each step tests the place stepped to (`0x434444`, the car's
frame):
- on a plane: inside its corners along X and Z and not more than 0.05 m
  under it; his height the plane's, sloping between its corners along Z;
- else on a ledge: within 0.3 m of one of its segments, put onto it;
- else in an open door's `InteriorPlane` (flag 2): through it into the cab
  or salon when the place is in their geometry (the door's passage,
  `0x4310B0`);
- else, from a plane: with flag 2 / 4 over to the next car (`0x4313FC`);
  without `EXIT` he stays where he was; with it he gets off to the ground
  (`0x4369F4`) when the train is slower than `max_speed_to_get_off`
  (config.ini), else he stays.

## Walking on the ground: objects' `ESD_Complex` boxes (`sub_4322F0`, mode 1)

**CONFIRMED** `ESD_Complex ( n ESD_Complex_Box ( rx ry rz tx ty tz x1 y1 z1
x2 y2 z2 ) ... )` (`0x4BB7BC`, 48 bytes a box): a box between the two
corners, turned by rx, ry, rz degrees (Z · Y · X, `sub_45EC40`; into the
box by the inverse, `sub_45EE28`) and moved by tx, ty, tz in the shape's
frame. E.g. Kuban's `R_FootBridge5mStairwayBoth`: its deck (y 8.7..9.4)
and two flights, 26 m boxes turned ±23 degrees about X.

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
  out across the nearer side.
- On no box: he stands on the track's bed near a track (`0x47E26C`, within
  3 m, rising over 2.5 m), else on the ground; never under the ground.

## In openRTS

`docs/train-systems.md` / `docs/reverse/train-dll.md` (the engineer).
Since 2026-11-11: climbing in at a door whose doorway's floor is the lowest
step finds the floor above it (the ER9PK); the rest of the above is being
done.
