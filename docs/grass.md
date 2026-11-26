# openRTS grass

openRTS grows its own grass near the eye. It is not RTS's grass: RTS reads a
route's `.rtst` but testers say its drawing there was never finished. openRTS
takes only *where* grass grows from the `.rtst` and draws the blades itself.

Only the experimental shading path draws it; the RTS-compatible path has none.

## Where it grows

- A route's `<route>/<route>.rtst`, `TerrainTextures`: a ground texture with
  `UseGrassTextures` (some `TextureSpread`) grows grass, `GrassHeight` sets how
  tall, the summed spread how dense. `NoGrassInSeason` turns it off in those
  seasons, `GrassDistanceFromTrack` keeps it off the tracks.
- Any other ground (no `.rtst`, or a texture it does not list): the vertex
  shader looks at the ground texture's colour under the blade. Green to olive
  ground grows grass; grey ballast and tarmac, brown soil, sand and snow do
  not. The blades get shorter as the ground turns less green, so there is no
  hard edge.
- Never under the objects standing on the ground around the camera
  (platforms, buildings, fences, yards: their boxes; not trees, bushes or a
  route's own grass and flowers by their file names).
- Never on tracks and roads (their axes, as the forests' clearance), lower at
  the ballast's edge; never in winter or while snowing.

## How it looks

Each tuft is three thin pointed blades fanning out of one root (45
vertices), swaying with the wind, coloured by the blurred ground texture
under it, a little greener, darker at the root. In spring and summer some
are flowers: a thin stem with a flat star of five white, yellow, blue or red petals, more in
the lush clumps. Smooth noise
makes clumps and bare spots. Blades are grown on the CPU in 4×4 m cells
around the eye, cached, and drawn instanced per ground patch.

## Settings

Options → Graphics: «Трава openRTS вблизи» (on/off), «Густота травы»
(0.25–2), «Дальность травы» (20–120 m, default 45). Saved as `grass`,
`grass_density`, `grass_reach`. `OPENRTS_GRASS_TRACE=1` prints the blade
count.
