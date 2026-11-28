# Cargo and loading

openRTS loads cars at pickups as RTS does (its rules in
`reverse/cargo.md`): bulk loaders pour into the cars standing under
them, racks fill tanks, a trestle empties hoppers and gondolas, and
cranes put cargo objects on cars from their window. The car's mass
grows with its load, so the train feels it.

## Playing

- **Num3** on a pickup (off the cab, the pickup nearest the aim within
  30 m; a coupling or a bogie nearer the aim gets the car menu instead)
  switches it: off, ready, active, off. Ctrl+Num3 asks a rack or a
  trestle to unload. In openRTS's keys: ` and Alt+`.
- **A bulk loader** (PickupType 1–4, 6, 10) pours while a car's loading
  area (or intake point) stands under its point, the car within the
  pickup's SpeedRange (0: standing): at PickupSpeed m³/s, as long as the
  car has room and is under its weight limit; then the next car. A
  stream of the cargo falls from over the car and the pile grows in its
  area. "Ready" becomes "active" by itself over a car with areas.
- **A rack** (5, 7, 16) fills a tank at a hundredth of its speed, as
  RTS; Ctrl+Num3 drains it.
- **Fuel and sand** (rts1.exe 0x4E59A0): a diesel rack (7) fills a
  diesel locomotive's tank at PickupSpeed l/s up to its `.eng`'s
  MaxDieselLevel (5000 l when none, as RTS's DLL host), a sand box (10)
  an engine's sand up to its MaxSandingTime; without a locomotive at it
  the rack fills a tank car and the sand box pours into a car as bulk.
  The fuel is the car's (`CarState::fuel_left`, kept in saves); the DLL
  reads it as `FuelAmount` and burns it at its `FuelConsuming`.
- **A trestle** (13) empties a loaded car standing over it at the car's
  DumpSpeed (else the pickup's), below its DumpMaxVelocity.
- **A crane or a platform** (12, 14, 15), active with a car standing in
  its area, opens the **"Load cargo"** window: the car (of those in its
  area), the loading area, how the load is placed (centre, front, back,
  manual: one item), centred across the car, the kind of cargo, the
  cargo (the pickup's CargoTable, else every cargo object and bulk kind
  of its loading types: one list for all, as RTS), its weight and how
  many, and what the car has; Load, Unload, Unload all. A load too heavy
  for the car or one that does not fit is refused with RTS's message.
  Closed, the crane is ready again.
- **Cargo information** in the car menu lists what the pickups loaded
  besides the consist's FreightAnim.
- **Sounds:** `pickup.sms` (the route's SOUND, else the global one) at
  the pickup within 300 m: "ready" and "off" when it is switched (diesel
  1 / 2, sand 11 / 12, bulk 21 / 22, liquid 31 / 32, gas 35 / 36), the
  loop while it works (fuel 3, sand 13, bulk falling 101, liquid 33, gas
  37, a trestle's dump 251; stopped by the next code), a crane's work done
  (221 boxes, 223 containers, 225 bulk, 227 stacks, 229 fridge; +1
  unloading); Variable1 the car's percent.
- Messages: the pickup ready / switched on / off, "too fast", "this car
  does not take this cargo", the car full, the pickup out of cargo.

## What it is made of

- The cars' loading: `TRAINS/TRAINSET/DEFAULT/RTSCargo.dat`, then
  `RTSCargoCustom.dat` (their `WagonType` blocks matched to the car's
  file by pattern or name, the first that matches).
- The cargo kinds: RTS's 24 built in (codes, densities, piles as
  `rts1.exe`), and openRTS's own from
  `TRAINS/TRAINSET/DEFAULT/openRTSCargo.dat` (its pack
  `PACKS/openrts-cargo.zip`; a mod may hold another): `Cargo ( ID ( ASH )
  Name ( "Ash" ) RussianName ( "Зола" ) Density ( 1000 ) Pile ( 0.3 )
  LoadingType ( PILE ) Texture ( oRTSCargo_ash ) )`. Twelve come with
  openRTS: ash, clay, gypsum, shale, glass shards, wood chips, salt,
  ore-bearing rock, coke, slag, cast iron, sinter (the testers'
  densities).
- A bulk cargo's texture: `GLOBAL/TEXTURES/<texture>1.ace`, else without
  the number, else `.png` / `.dds`; RTS's are `RTSCargo_coal1.ace` and
  the like. Without one a bulk cargo is drawn with RTS's gravel.
- Cargo objects: `TRAINS/OBJECTS/RTSObjects.dat`, then the route's (one
  with the same ID replaces the global one); their shapes in
  `TRAINS/OBJECTS/CARGO`, `TRAINS/OBJECTS`, `GLOBAL/SHAPES`.
- Pickups: a world `Pickup` by its own fields (PickupType, SpeedRange,
  PickupCapacity), any other object by its `.sd`'s `ESD_Pickup_Object`.
  A pickup's stock is its CargoTable's mass of its cargo, else unlimited.

## Drawing

- A pile is openRTS's own model (`renderer/cargo_shape.hpp`): a heap over
  the area's footprint, its slopes by the cargo's pile factor, the
  texture every 2 m; a little cargo heaps in the middle, more spreads to
  the walls and rises, a full car is heaped over the rim a little. A car
  without areas (a hopper, a tank) or an `Invisible` area shows none, as
  RTS.
- Cargo objects stand on the area's floor where they were placed, stacks
  one on another.
- The falling stream is a column of the cargo's texture from over the
  car down to the pile, turning a little each frame. A trestle's dump
  falls from the car's DumpPoints (else under its areas) to below the
  rails.
- **A crane** lowers each object it loads from 6 m over its place on a
  rope, one after another (stacks one by one), and lifts each it unloads
  away; a shape that names its `PickupCraneShapeNodes` moves them: the
  bridge along PickupCraneOriented's axis, the cabin across, the hook down
  from HookInitialHeight to the load (no route's shape has them yet).
- **A car's LoadingAnimation** (hatches, a dump car tipping) plays its
  nodes to their last frame (or AnimFrame) over AnimTime while the car
  is emptied at a trestle (LoadingUnloading 2) or filled (1), and back
  after (no RTSCargo.dat of the build has one yet).

## Kept

A saved game keeps each car's loads (`cargo=` lines of its train) and the
diesels' fuel. In a network game each car's loads go to the others as
`0x461C` (`net.md`, "Cargo").

## Not yet

- Sockets (11).
- Industries that use what is brought and make products
  (`reverse/cargo.md`, stage 7).
