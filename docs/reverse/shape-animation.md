# Shape (.s) animations

Status: structure checked on content (`A1tPnt3dBLft.s` and 900 sampled shapes
from GLOBAL, TRAINSET and Kuban); RTS loading read from `0x43A944`, wagon
bindings from `0x4F0BC8`. The RTS function that evaluates a frame is not
traced. Implemented in openRTS: `formats/shape_binary` (parsing, binding by
name), `formats/shape_animation` (evaluation), per-instance frames in the
renderer, switch blades driven by junctions in the viewer.

## Model

### CONFIRMED

MSTS animation is **rigid node animation, not skinning**. Every matrix of
the shape hierarchy (a "node": door, wheel set, pantograph arm, switch blade)
can have keyed rotation and translation. A frame gives each animated matrix a
new local transform. Children follow through the hierarchy (`hierarchy`
parents), and vertices stay bound to one matrix through their vertex state.

## Binary layout

### CONFIRMED (content)

| Token | Block | Contents |
|---:|---|---|
| 29 | `animations` | u32 count, then `animation` blocks |
| 28 | `animation` | u32 frame count, u32 frame rate, then `anim_nodes` |
| 27 | `anim_nodes` | u32 count, then `anim_node` blocks |
| 26 | `anim_node` | label = node (matrix) name; one `controllers` block |
| 25 | `controllers` | u32 count, then controllers (possibly 0) |
| 24 | `tcb_rot` | u32 count, then rotation keys |
| 21 | `linear_pos` | u32 count, then `linear_key` |
| 20 | `tcb_key` | u32 frame, quaternion x y z w, tension, continuity, bias, in, out (40 bytes) |
| 23 | `slerp_rot` | u32 frame, quaternion x y z w (20 bytes); rare, inside `tcb_rot` |
| 19 | `linear_key` | u32 frame, x y z (16 bytes) |

- **Keys:** they are at integer frames from 0 to the frame count, so there
  can be `frames + 1` keys (the switch has frame count 2 with keys at 0, 1,
  2).
- **Frame rate:** almost always 30 (192 of 202 animated shapes).
- **Frame counts:** typical values are 2–3 (switches, doors) and 250
  (pantographs, wheels).
- **TCB parameters are unused:** all 21 652 sampled `tcb_key`s have zero
  tension, continuity, bias, in and out. Spherical interpolation between
  keys gives the same result as a TCB spline would.
- **Animated nodes in content** (sample): `DOOR_*`, `ROD`,
  `PANTOGRAPH{BOTTOM,MIDDLE,TOP}*`, `WHEELS*`, `MIRROR*`, `MOVINGTRACK`,
  switch `POINT_LEFT` / `POINT_RIGHT` and the throw rod.

### Example: `GLOBAL/SHAPES/A1tPnt3dBLft.s`

- One animation: 2 frames at 30 fps.
- 8 anim nodes, one per matrix: `rels`, `sneg`, `taga`, `nasip`, `perkl`,
  `POINT_LEFT`, `r2`, `POINT_RIGHT`.
- `POINT_LEFT` / `POINT_RIGHT`: rotation keys at frames 0, 1, 2 with
  quaternions (0,0,0,1), (0,0.0078,0,1), (0,−0,0,1). The blade turns by
  about 0.9° about Y at frame 1 and back at frame 2. The position stays at
  the blade pivot.
- `taga` (throw rod): x 0.519 → 0.643 → 0.519.
- Other nodes have empty controllers.

## RTS loading (`0x43A944`)

### CONFIRMED

- **Animation record:** frame count, frame rate, node count and the nodes
  (16 bytes).
- **Node record** (40 bytes): position and rotation key counts and arrays,
  and the bound matrix.
- **Binding:** a node is bound to the matrix with the **same name,
  compared case-insensitively**, not by position. The matrix gets the node
  index too.
- **Axis conversion:** RTS mirrors X on load:
  - position keys: x is negated;
  - rotation keys: quaternion y and z are negated.

  This is the same reflection RTS applies to geometry. openRTS keeps MSTS
  axes and mirrors in the camera.
- **Errors:** missing keys or a frame out of range are reported
  (`log_and_show_message`: "anim node #%i doesn't have all the keys",
  "Animation frame out of range in s-file").

### CONFIRMED (geometry and screenshots)

- **Rotation keys in MSTS axes:** they act like world `QDirection`. The
  stored row-vector matrix rows are the rows of the standard rotation
  matrix of the key quaternion.
  - On `A1tPnt3dBLft.s` at frame 1 the left blade tip (8.6 m from its
    pivot) moves 0.13 m towards the track centre. With the other sign it
    would move through its stock rail.
  - On TestRoute the blades of `A1tPnt3dBRgt.s` swap stock rails when the
    junction is thrown, and the closed blade matches the branch (MainRoute
    straight: right blade closed; right branch: left blade closed).

### UNKNOWN

- **Interpolation:** whether RTS uses slerp or nlerp, and how it clamps or
  wraps frames between keys (the evaluation function is not found yet).
  openRTS uses slerp and clamps.

## What drives the frame

### CONFIRMED (wagons, `0x4F0BC8`)

Nodes are classified by name prefix:
- `bogie*` → type 4 (bogie rotation, sorted by position along the car);
- `wheels*` → type 2 (rolling).
- `door_a`…`door_c` and `door_d`…`door_f` are the two door sides; the side
  also follows the sign of the node's X position.
- Pantograph nodes, `AnimTime`, `AnimNode` and the RTS extended animations
  (`ESD_Extended_Animation`, `AnimationNode`, `Shortcut`, `AnimTimeScale`)
  are parsed in the same area. They are for the train work.

### Switches

- **CONFIRMED (content):** every switch shape the installed routes use has a
  2-frame animation, and almost all return to the first key at frame 2.
  - Frame 0 is the MainRoute position; frame 1 is the other branch.
  - The "last key" is the rest position again, so it is not the thrown
    one.
- **PROBABLE:** RTS selects the frame from the TDB junction's selected
  branch (`0x47ECD0`).
- **UNKNOWN:** whether RTS animates the movement over time, and its speed.
  openRTS moves at the shape frame rate (30 fps), i.e. almost at once.

## For openRTS

1. **Parser:** read block 29 into `ShapeModel` — animations with frame
   count and rate; nodes bound to matrices by name; position and rotation
   key tracks.
2. **Evaluation:** a pure function (frame → local matrices) that replaces a
   matrix's rotation and translation with the interpolated key values. Use
   slerp between rotation keys and linear between position keys.
3. **Renderer:** a per-instance frame. Model draw ranges already carry the
   matrix chain; animated instances need their own world matrices per draw.
4. **Drivers:** junction state for switches; distance travelled and wheel
   radius for wheels; door, pantograph and mirror states later with the
   train.
