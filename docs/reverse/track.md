# Track network in RTS

Status: static analysis of `rts1.exe` plus measurements on the 7 installed
routes (`openrts-track-check`). Implemented in `formats` (`track_sections`,
`track_database`) and `simulation` (`track_geometry`, `track_network`,
`track_traveller`). Plan and acceptance: `docs/track.md`.

| Address | Role |
|---|---|
| `0x483694` | route loader: tsection.dat (sections, shapes), .tdb |
| `0x481AF8` | TDB track nodes |
| `0x482680` | TDB item table |
| `0x481440` | traveller: advance by a distance |
| `0x4882A8` | position inside one section |
| `0x47ECD0` | leave a node: pins, junctions, modes |
| `0x48C85C` / `0x4F9550` | junction release / clearance after a facing tail |
| `0x47E26C` / `0x47E364` | traveller from a world position |
| `0x4F5B34` | train movement: car, bogie, front and rear travellers |
| `0x47F214` / `0x48106C` | look-ahead for items / `block_state` |

## Geometry: tsection.dat

### CONFIRMED

- **Sections:**
  - `SectionSize (gauge length)` gives a straight;
  - `SectionCurve (radius angle)` gives an arc: `length = |angle| · π/180 ·
    radius`, the angle is stored in radians;
  - `SectionSkew (deg)` keeps the radius at 0 and gives the straight an
    angle;
  - route-local sections are indexed after the global ones.
- **Shapes:** `FileName`, `NumPaths`, `MainRoute`, `ClearanceDist`,
  `SectionIdx (n x y z deg s1..sn)`, flags.
  - A junction's default and initially selected branch is the shape's
    `MainRoute`.
  - `ClearanceDist` is the facing-tail clearance.
- **Position in a section (`0x4882A8`).** In MSTS axes, as openRTS uses
  them; RTS works with X negated:
  - the start of the section is (x, y, z) with pitch `ax` and heading `ay`;
    forward is `(sin ay, 0, cos ay)`;
  - arc: `turn = angle · f` (fraction f of the length), forward `R·sin|turn|`,
    lateral `R·(1 − cos turn)` to the right for a positive angle;
  - height: `y − sin(ax) · forward`;
  - the end heading is `ay + turn`;
  - skewed straight: RTS scales the skew angle by f as well.
- **Measured** (sections of one node, end of k to start of k+1): this
  convention gives a median of 0.5–0.7 mm and a p95 of 0.7–2.5 cm on all
  routes. The 7 other sign combinations are metres off.

## Topology: .tdb

### CONFIRMED

- **Vector nodes:** per section 16 values — section, shape, world tile x/z,
  world uid, two flags, a hex string, tile x/z, x y z, pitch, heading, roll.
  RTS negates x on load.
- **Pins of a vector node:** `TrPins (1 1)`: pin 0 is the node's start,
  pin 1 its end.
- **Junction node:** `TrJunctionNode (a shape b)`, `UiD (world tile x/z,
  world uid, flag, tile x/z, x y z, angles)` and `TrPins (1 2)` — trunk
  first, then the branches. All 555 Kuban junctions have exactly 2
  branches.
- **Pin direction** (all Kuban pins agree; matches the RTS check in
  `0x47ECD0`):
  - a pin to a vector node: 1 = that node's start (enter forward), 0 = its
    end (enter backward);
  - a pin to a junction: 1 = its trunk (a facing move), 0 = a branch side
    (a trailing move).
- **Links:** vector nodes link only through junctions and end nodes (Kuban),
  and junction links meet the vector ends within 0.1 mm (median).
- **Items:** `TrItemSData (offset along the node, flags)`, `TrItemRData
  (x y z tile)`, and type-specific blocks (`TrSignalType (flags dir angle
  type)`, `TrSignalDirs`, `SpeedpostTrItemData`, platform and siding names).

## Traveller and junctions

### CONFIRMED (`0x481440`, `0x47ECD0`)

- **State:** node, section index, offset in the section, offset from the
  node start, a direction flag, and the cached pose.
- **Leaving a node:** RTS follows the pin and continues through junctions
  until it reaches a vector node.
- **Facing move** (enter at the trunk): take the selected branch.
  - occupy (train head): mark the junction occupied;
  - release (train tail): register a clearance of `ClearanceDist` for this
    train (`0x4F9550`).
- **Trailing move** (enter from branch k): always exit through the trunk.
  - occupy: **the switch is thrown to k** — a run-through, no derailment.
    If another train holds the junction and the activity fails on forced
    junctions, the activity fails (`0x41B4F4`);
  - release: clear the occupied flag, drop this train from the reservation
    list, and set the switch to the next reservation's branch or to the
    default (`0x48C85C`);
  - probe (look-ahead): the move fails if k is not the selected branch;
    `block_state` reports this as `BLOCK_JN_OBSTRUCTED`.
- **End node:** the advance fails; a car then derails (`0x5059A8`) unless
  derailment is disabled.

### CONFIRMED (`0x47E364`) — traveller from a world position

For each vector node near the point (tile ±1), section by section:
- a section is tried only if its start is within its length + 0.1 m of the
  point;
- RTS samples the section every **5 m** and takes the **first** sample
  within tolerance + 2.6 m;
- the last section before an end node also tries its very end (−1 cm).

openRTS `TrackNetwork::locate` returns the nearest point instead (2 m
samples, refined).

### CONFIRMED (`0x47F214`, `0x48106C`)

Look-ahead stops only at a matching item or an end node, and never keeps
visited nodes; see `signals.md` for the closed-loop hang. openRTS bounds it
by distance and visited (node, direction) pairs.

### UNKNOWN

- **Switch blade frame in RTS:** how RTS maps junction state to the blade
  frame is not traced. Content and screenshots support MainRoute = frame 0,
  other branch = frame 1 (see `shape-animation.md`).
- **The two other numbers of `TrJunctionNode`.**
- **The hex string in vector sections.**

## Measurements on installed routes (`openrts-track-check`)

| Route | Vector / junction / end | Joints median / p95 / max | Components |
|---|---|---|---|
| Chernyshevsk_RTS | 1626 / 973 / 333 | 0.5 mm / 1.0 cm / 200 m | 11 |
| Kievskij_hod_RTS | 2960 / 1795 / 535 | 0.6 mm / 0.7 cm / 65 m | 45 |
| Kuban_RTS | 921 / 555 / 177 | 0.7 mm / 1.7 cm / 0.22 m | 5 |
| Novokuznetsk | 2248 / 1343 / 467 | 0.6 mm / 1.1 cm / 0.23 m | 6 |
| Pavlovsk_RTS | 1780 / 1061 / 377 | 0.5 mm / 1.3 cm / 0.32 m | 6 |
| TestRoute_RTS | 58 / 36 / 8 | 0.7 mm / 2.5 cm / 0.16 m | 2 |
| Uspensk_RTS | 1234 / 724 / 297 | 0.6 mm / 1.2 cm / 62 m | 62 |

- **Gaps of tens of metres** are single defects in the route data. They
  appear in the tool's "worst joints" list and do not stop a traveller:
  topology chooses the next section.
- **Uspensk_RTS** has one dangling pin (node 1016 → 0), treated as a track
  end.
- **Section ends against their `TrackObj` origin:** median ≤ 1 mm. A p95 of
  2.5 m comes from switch paths that start off the shape origin.
