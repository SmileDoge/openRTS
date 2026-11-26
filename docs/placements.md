# Placements

A placement is a set of trains, or parts of them, picked on the route and
kept to be put down again beside the trains there: two long freight trains
standing at a station, a few cars in a siding. openRTS's own (RTS has
situations of the whole route only). Single player for now; the network
game will take them through the server.

## Picking

The Train window, "Placements", "Pick trains with the mouse (free
flight)": with the free camera (not on foot or in a cab),

- a click picks the train under the cursor (and drops the others);
- Ctrl+click adds another train, or drops one picked;
- Shift+click drops one car of a picked train, or takes it back (only a
  car coupled in a picked train);
- a click on nothing drops them all.

The picked cars show in yellow boxes. A car under the cursor is the one
whose length on the screen (1.5 m above its rails) passes nearest it,
within 3 % of the view; of two alike, the nearer.

## Saving

"Save the picked trains" with a name writes
`placements/<route>/<name>.placement` in the user data folder: each run
of picked cars of a train is a train of its own (a train with a car
dropped in the middle gives two), standing, its front where its first
car's front stands (found from the train's front back along the track by
the cars' offsets), its cars as a situation writes them (folder, file,
engine, turned, UiD, brake shoes). The cars get new keys each time the
placement is put down: it can be put down more than once.

## Putting down

- "Show where they stand": the placement's trains are made and placed,
  drawn as boxes, not yet in the game: green where the track is free, red
  where a car of it comes within 2.5 m of a car there (the track taken).
- "Put them down": they join the trains there, standing (static, asleep,
  as Track control's standing trains); without trains on the route the
  first becomes the player's. Not while the track is taken.
- "Cancel" drops the preview; "Delete" the file.

Checked (`OPENRTS_PLACEMENT_CHECK`, 2026-11-18): an ЭД4М of six cars on
Kuban, its second car dropped: saved as two trains of one and four cars,
shown where it stands (the track taken, as it is by itself), each shown
car within 0.66 m of the car it came from (the preview's spacing from the
files' sizes; once put down the shapes are fitted as for any train).
