# The inspector

Status: a first version (2026-11-08). The base for editing content inside
the game: later a train and a cab editor.

F12 opens two windows:

- **The map's objects:** a tree of what is loaded around the camera.
  - *World:* the world tiles (3 x 3 round the camera), each with its
    objects (static objects, track, catenary masts, signals, speed posts,
    ground textures, forests, level crossings, pickups, road traffic), by
    kind and shape name.
  - *Trains:* every train, its cars in order.
  - A filter by name; "Choose with a click in the world": a click in the
    scene then chooses the object under the cursor (its shape's box, the
    nearest one; a box the eye is in only when nothing else is in the way)
    instead of working a control. Double-click in the tree: go to it.
- **Properties** of the chosen object, its box drawn in the scene:
  - a world object: kind, UiD, world tile, position, collide and static
    flags; a car: its train and place, `.eng` / `.wag`, mass, size, DLL;
  - its shape, where the shape comes from (the VFS source: a content
    folder, a pack or a mod) and where its override comes from, the shape's
    box and distance levels;
  - "Go to": the free camera beside it, looking at it;
  - **Materials:** the shape's primitive states, those with the same
    texture, shader and light material one row with their count and first
    and last number (a hundred animated pieces of four textures read as 301
    rows before); above them the textures and the parts counted.

## FullBright made ordinary, per shape

MSTS's FullBright surfaces (light material -8) take neither the sun nor the
lamps. The panel's "Light FullBright as ordinary surfaces" lights all of
them; a platform drawn FullBright is better lit as an ordinary surface, a
sign or a lamp's glass is not. In the inspector a FullBright material has a
"Lit as ordinary" tick: the change shows at once (the shape is reloaded),
"Save to the mod" keeps it.

- It is saved as `MODS_OPENRTS/<mod>/<shape path>.openrts`, e.g.
  `MODS_OPENRTS/openrts_edits/ROUTES/Kuban_RTS/SHAPES/Platform.s.openrts`
  (the mod's name in the window, remembered; `openrts_edits` at first). The
  shape and the content folders are not touched; RTS does not see it.
- A mod not yet in `openrts.ini` is added to its `[Mods]` (after the
  others): the VFS mounts it from the next start; for this run the change
  is held in memory.
- "Undo the changes" drops what is not saved (the file's override, if any,
  applies again). With every tick taken off, saving removes the file.

### The file

```text
OpenRTSShapeOverride ( 1
	PrimState ( 12 Image ( "platform.ace" ) LightMaterial ( -5 ) )
)
```

- `PrimState ( index ...)`: the primitive state's index in the shape.
- `Image`: the file name of its texture's image, lower case: a check; a
  shape replaced since, whose state no longer draws that image, is left as
  it is.
- `LightMaterial`: MSTS's light material (-5 normal, -6 / -7 shiny, -8
  FullBright, -9 circumference, -11 half bright, -12 dark shade).
- The model loader (`renderer::load_model_asset`) applies it after parsing
  the shape: the state gets a vertex state of its own, a copy of the one it
  shared with that light material, so the others keep theirs
  (`formats::apply_shape_override`). The override file is one of the
  model's dependencies: changed in a mounted mod, the model reloads.

## Code

`viewer/src/inspector.*` (the windows, choosing, the box, saving),
`formats/shape_override.*` (the file), `renderer::find_shape_override` /
`hold_shape_override` (the file or the one held for the run),
`RenderModel::primitives()` / `ModelBounds::min` / `max` (what the
inspector shows and picks by), `TrainView::inspected_cars`. The capture
option `--capture-inspect` opens it and clicks the picture's middle; the
capture prints the choice.
