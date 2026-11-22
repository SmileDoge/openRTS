# Placements

A placement is a set of trains, or parts of them, picked on the route and
kept to be put down again beside the trains there: two long freight trains
standing at a station, a few cars in a siding. openRTS's own (RTS has
situations of the whole route only). Since 2026-11-21 one file for the
single player, the server and the dispatcher's console alike (The file,
below): a player's placement sent to the organiser is put in the console's
folder or on the server as it is, and a placement can include others (a
station's holds the trains of its own and includes one train's).

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
`placements/<route>/<name>.placement` in the user data folder (the window
shows the folder; the dispatcher's console keeps its own there too): each
run of picked cars of a train is a train of its own (a train with a car
dropped in the middle gives two), its front where its first car's front
stands (found from the train's front back along the track by the cars'
offsets), with its kind (the driven train PLAYER, a test driver's TRAFFIC,
the others STATIC), its category for the speed posts, its state (cold, on,
running, as Track control put it) and its cars: their files, turned, the
chocks, the handbrake and the brake shoes. The number is left to the
server (0). The cars get new keys each time the placement is put down: it
can be put down more than once.

## The file

UTF-8 lines (`net/placement.hpp`):

```
openRTS placement 1
route <the route's folder name>
include <another placement's name>
train <number> <kind> <subtype> <state> <top speed m/s> <node> <offset> <forward 0|1>
name <text>
notes <text>
from <the included placement it came from>
car <flags> <handbrake 0..1 | -> <folder\name.eng|.wag>
shoes <front>,<rear>
end
```

- `route` (optional): a placement of another route is refused.
- `include` (any number): another placement of the same folder, put with
  it; theirs in turn, each once (a loop or a second way to the same one is
  not put twice; a missing one is told).
- `train`: the kind as RTS's (0 STATIC, 1 PLAYER, 2 TRAFFIC, 3 LOOSE),
  the subtype the console's category (0 other, 1 passenger, 2 freight),
  the state 0 "Холодный", 1 "Включен", 2 "Движется", the number 0 the
  server's lowest free; the place the front of its first car on a TDB
  vector node, facing along it when `forward` is 1.
- `name`, `notes`: "\n" a new line, "\\" a backslash.
- `from`: written only where a placement is shown with its includes'
  trains (the server's `placement get`); a copy kept has none.
- `car`: flags as the RTS car record's (1 turned, 2 cold, `0x100` a chock
  at its end towards the train's front, `0x200` at the other: RTS's
  PlaceDrag); the handbrake 0..1.
- `shoes` (optional): the car's brake shoes, front and rear bogie
  (`formats::brake_shoe_name`). The server's records do not carry them:
  a placement put by the server gives the cars their own.

Older files are read as well: the header `openRTS server placement 1`
(the server's first days) and the single player's `# openRTS placement`
files of before (their trains STATIC and running, the number the
server's).

## Includes

The placement chosen in "Placements": "Includes (n):" with each one's
button (a click takes it out) and "include another placement" (a list of
the others). The same in the server's window ("Open: includes, trains")
and the dispatcher's console (the placement shown), and on the server's
command line: `placement include|exclude <name> <other>`.

## Putting down

- "Show where they stand": the placement's trains are made and placed,
  drawn as boxes, not yet in the game: green where the track is free, red
  where a car of it comes within 2.5 m of a car there (the track taken).
- "Put them down": they join the trains there, standing (static, asleep,
  as Track control's standing trains); without trains on the route the
  first becomes the player's. Not while the track is taken.
- "Cancel" drops the preview; "Delete" the file.

The trains of its includes come with it ("n of them from the placements
it includes"). Each train starts as the file says: its category, cold
(no air) or on; the chocks and the handbrake on.

## Moving a train and its chocks

Track control's train menu (single player and the dispatcher, 2026-11-21):

- "Move it (on the map)": the train's cars as a preview (as "Put a
  consist here"), its front where the train's is; dragged along the
  track, a click on another track moves it there, "Turn it around", then
  "Put it here". In single player the train stands there (stopped
  first); in the network game the server puts it again there
  (docs/server.md, "Placements").
- "Chocks": none, under the head, under the tail, under both ends (the
  first car's end towards the train's front, the last car's other end),
  the one it has ticked.
- "Put a consist here" and the console's new train take the chocks too
  ("no chocks" by default).

Checked (`OPENRTS_PLACEMENT_CHECK`, 2026-11-18): an ЭД4М of six cars on
Kuban, its second car dropped: saved as two trains of one and four cars,
shown where it stands (the track taken, as it is by itself), each shown
car within 0.66 m of the car it came from (the preview's spacing from the
files' sizes; once put down the shapes are fitted as for any train).
Again 2026-11-21 with an ЭД9М of eight cars in the common format: the
file `openRTS placement 1`, the chocks under both ends saved and back in
the preview, 7 cars within 0.70 m, the train moved 40 m back (0 m off)
and back again.
