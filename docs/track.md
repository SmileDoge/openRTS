# Track network

Status: steps 1–5 implemented. Roadmap step 1: openRTS knows the track
network the way RTS does, so trains can later move on it and signals can
look along it. Nothing here moves trains yet.

## Using it

```text
openrts_viewer --window --route D:\Games\MDDRTS\ROUTES\Kuban_RTS [--probe]
    T            track overlay on/off (yellow track, magenta junctions, red ends,
                 signals red, speed posts blue, platforms cyan; green = selected branch)
    P            put the probe on the nearest track (300 m); --probe: at start, camera following
    Up / Down    drive the probe (Shift: x5)
    J            throw the junction nearest to the probe (or camera)
    V            camera follows the probe
    F6 / button  save a debug view -> debug_views/last.view (+ timestamped copy)
openrts_viewer --window --view last        reopen the saved view
openrts-track-check D:\Games\MDDRTS [route...]   checks and measurements
```

Code:
- `formats/track_sections` (geometry);
- `formats/track_database` (topology);
- `simulation/track_geometry`, `track_network`, `track_traveller`;
- `formats/shape_animation` (switch blades);
- `viewer/track_view`.

Findings: `docs/reverse/track.md`. Tests: `tests/track_test.cpp`.

Based on the analysis of `rts1.exe` (`0x483694` loader, `0x481AF8` TDB nodes,
`0x481440`/`0x4882A8` traveller, `0x47ECD0` node transitions and junctions,
`0x4F5B34` train movement) and on the installed content. All 7 installed
routes have no `Dyntrack` and no route-local track sections, so those come
last.

## 1. Reverse notes

Write `docs/reverse/track.md` with CONFIRMED / PROBABLE / UNKNOWN labels:
- **Files:** `tsection.dat` and `.tdb` layout.
- **Traveller:** 64-byte structure, advance algorithm, arc and slope maths.
- **Junctions:**
  - facing move takes the selected branch;
  - trailing move throws the switch to the arriving branch (no derailment);
  - a junction stays occupied until the tail is `ClearanceDist` past it;
  - release resets it to the next reservation or `MainRoute`;
  - "forced junction" failure;
  - probe mode, which gives `BLOCK_JN_OBSTRUCTED`.
- **Consumers:** how train movement uses travellers (cars, bogies, front and
  rear, occupancy).
- **Still to trace** (small targeted IDA passes):
  - placing a traveller from a world position (`0x47E26C` / `0x47E364`);
  - what `SectionSkew` and route-local sections do;
  - how junction state drives the switch blade animation.

## Geometry and topology

The network has two independent layers, and openRTS keeps them apart as RTS
and Open Rails do:

- **Topology (`.tdb`)** — what connects to what.
  - `TrVectorNode` is a chain of sections without branches;
  - `TrJunctionNode` is a switch (one trunk pin, several branch pins);
  - `TrEndNode` is a buffer end;
  - `TrPins` link the nodes, with the end of the linked node;
  - `TrItemTable` / `TrItemRefs` put items (signals, speed posts,
    platforms, ...) on vector nodes.

  Movement, junction choice, occupancy and look-ahead use only this graph and
  distances along it.
- **Geometry (`tsection.dat`)** — what a section looks like.
  - `TrackSection`: straight (gauge, length) or curve (radius, angle),
    `SectionSkew`;
  - `TrackShape`: the paths through a track shape (`SectionIdx`: start
    offset, start angle, section list), `MainRoute`, `ClearanceDist`,
    `XoverPt`.

  Each `TrVectorSection` in the `.tdb` only references a section by index
  and gives its placement (tile, position, angles).
- **Joining them:** a point at distance `s` in a vector node is found by
  topology (which section, offset inside it). Its world position comes from
  geometry: the section's shape evaluated at that offset from that section's
  own placement. Section lengths come from geometry and define the distances
  topology works with.

Consequence: gaps or kinks between consecutive section placements in the
data never break movement. The next section is chosen by topology and
starts from its own placement. A gap only shows up as a small jump in the
position.

## 2. Formats

Two parsers in `formats`, both with byte and VFS entry points:

- **Geometry — `tsection.dat`** (GLOBAL, then the route file appended after
  the global sections, as RTS indexes them):
  - `TrackSections`: `SectionSize`, `SectionCurve`, `SectionSkew`;
  - `TrackShapes`: `FileName`, `NumPaths`, `MainRoute`, `ClearanceDist`,
    `SectionIdx` paths, `XoverPt`, tunnel / road / crossover / manual flags.
- **Topology — `.tdb`:**
  - `TrackNodes`:
    - vector node: per section the section index, shape index, tile,
      position and angles — placement only, no shape data;
    - junction node: track shape index and the UiD of its world object;
    - end node;
    - `TrPins` with the linked end;
  - `TrItemTable`: signal (with `TrSignalDirs`), speed post, platform,
    siding, level crossing, crossover, pickup, car spawner and empty items,
    plus `TrItemRefs` on vector nodes.
- **Coordinates** stay in the MSTS frame used by the renderer. RTS negates X
  on load; openRTS mirrors in the camera instead.
- **Tests:** synthetic files plus all 7 installed routes:
  - every route parses;
  - node and item counts match the headers;
  - every pin points back;
  - every section index and junction shape index exists in `tsection.dat`.

## 3. Simulation: geometry, topology, traveller

In `openrts_simulation` (core + formats only, no renderer), as separate
types:

- **Geometry:**
  - **`TrackSectionGeometry`:** the shape of one section (straight / arc);
    point, heading and pitch at an offset from a given placement. Pure
    functions, unit-tested on straights and left and right arcs.
  - **`TrackShapeGeometry`:** paths of a track shape for switch and
    crossing geometry (used by placement and by drawing junctions).
- **Topology:**
  - **`TrackGraph`:** vector, junction and end nodes, pins, per-section
    lengths (taken from geometry) and item offsets along vector nodes.
  - **`JunctionState`:** selected branch, default `MainRoute`, occupied flag,
    reservation list, clearance countdown. Manual switching and reservations
    share it.
- **Movement:**
  - **`TrackTraveller`** (RTS `0x481440` semantics):
    - topological position: node, section, offset in section, offset from
      node start, direction;
    - advances by a signed distance through the graph;
    - an advance that hits an end node fails, as RTS reports it before
      derailing a car;
    - world pose (tile, local position, heading, pitch, curve flag) comes
      from geometry at the current position.
  - **Junction transitions** (`0x47ECD0`): facing / trailing logic with the
    modes occupy (head), release (tail), probe and plain.
  - **Placement:** traveller from a world position (nearest vector section in
    the tile and its neighbours), once step 1 has traced RTS's search.
- **Tests:**
  - **topology (strict):**
    - a single pass through a vector node, from one boundary pin to the
      other, visits each of its sections exactly once (in either direction),
      and its length equals the sum of those section lengths;
    - leaving a node through a pin enters the linked node at the right end
      and direction;
    - movement over the whole graph is not a tree walk: nodes are revisited
      and cycles exist (loops, wyes, reversing through junctions). Graph
      tests and tools (reachability, look-ahead) track visited nodes and
      must terminate on cyclic networks; a traveller may pass the same node
      any number of times;
    - look-ahead (signals, items, block state) is bounded by distance,
      checked at every node, and by a visited set of (node, direction), so a
      closed loop A–B–C–A without a matching signal ends after one lap and
      reports "nothing found". RTS does not do this: `0x47F214` and
      `0x48106C` walk until they find an item or an end node, with the
      distance limit checked only when an item is found. On a loop without a
      matching signal this is an endless loop (PROBABLE cause of the
      known MSTS/RTS loop hang). openRTS does not reproduce the hang; tests
      cover a ring without signals, a ring with a signal only in the
      opposite direction, and a wye;
  - **junctions (strict):** facing and trailing moves, run-through throw,
    clearance release, probe result;
  - **geometry joins (measured, not required):** for every joint between
    consecutive sections and between linked nodes, the distance and heading
    difference between "end of one" and "start of the next". Reported per
    route as a distribution (median, 95th percentile, maximum, the worst
    joints with node and section). Real routes are hand-built and contain
    gaps, so the test fails only on gross errors that point to an openRTS
    bug — e.g. most joints in a route off by metres, or a systematic
    sign/axis error. Good data should give near-zero medians. The
    thresholds are set after the first measurement on the 7 routes;
  - **agreement with the world:** section placements against `TrackObj`
    placements in the world files, as the same kind of measured statistic.

## 4. Viewer: see and drive the network

- **Overlay:** a debug line renderer draws vector sections as polylines
  (curves sampled), junctions and end nodes as markers, track items as small
  glyphs (signal, speed post, platform, level crossing). Each part can be
  toggled in the panel.
- **Probe:** a key drops a traveller on the track nearest to the camera,
  and keys move it forward and back at speed. The panel shows node, section,
  offset, heading and pitch. The camera can follow the probe — the first
  "ride" along the route.
- **Switching:** a key throws the nearest junction. The probe then takes the
  chosen branch, and a trailing move throws the switch itself, as in RTS.
- **Check:**
  - the lines lie on the rails over a whole route (Kuban, TestRoute);
  - the probe runs from end to end following topology;
  - the panel shows the worst geometry joints, so data gaps can be
    inspected on screen, as they are, not hidden.

## 5. Switch visuals

The switch shapes carry animated blades (`A1tPnt3dBLft.s`: `POINT_LEFT`,
`POINT_RIGHT`, throw rod `taga`), and RTS animates them from the junction
state. This needs (see `docs/reverse/shape-animation.md`):
- shape animations (rigid node tracks, not skinning) in the binary shape
  parser;
- per-instance animation frames in the renderer;
- the junction-to-frame mapping from step 1.

Blades then follow `JunctionState`.

## 6. Later in this area

- **`Dyntrack`:** parse it and build its geometry and TDB sections. No
  installed route uses it.
- **RTS additions:** `.electrodb` / `.electrotype` electrification sections
  (needed for electric traction), track retarders, `.rdb` roads (car
  spawners).

## Order and size (done)

| Step | Depends on | Size |
|---|---|---|
| 1 reverse notes | — | small |
| 2 parsers (geometry, topology) | 1 | medium |
| 3 geometry, graph, traveller, junctions | 2 | large |
| 4 viewer overlay and probe | 3 | medium |
| 5 switch blade animation | 3, shape animations | medium |
| 6 dyntrack, electrification | later | — |

Steps 1–4 are the milestone "the network is loaded, drawn and can be driven
by a probe". Step 3 is what the train (roadmap step 2) builds on.
