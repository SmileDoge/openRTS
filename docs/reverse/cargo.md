# Cargo and loading (RTS)

What RTS has for cargo, pickups and loading, and what openRTS takes from
it. Sources: RTS's own file reference (`RTS\RTSFileReference.html`, shipped
with the game), the strings and tables of `rts1.exe`, the files of RTS
0.44.1 and the routes of the current build, and what testers told of the
game itself.

## Files

| File | Where | What |
|---|---|---|
| `RTSCargo.dat` | `TRAINS\TRAINSET\DEFAULT` (RTS 0.44.1 also `RTS\SD`) | `CargoTypes` and `WagonTypes` |
| `RTSCargoCustom.dat` | `TRAINS\TRAINSET\DEFAULT` | the same, the user's own (read after it) |
| `RTSObjects.dat` | `TRAINS\OBJECTS` and the route's folder | `CargoObjects` and `ServiceObjects` |
| `<shape>.sd` | beside a shape | `ESD_Pickup_Object`: the shape is a pickup |
| `.wag` / consist | the car | `WagonCargoDescription`, `FreightAnim` |
| `SOUND\pickup.sms` | | the pickups' sounds |
| `GLOBAL\TEXTURES\RTSCargo_<cargo><n>.ace` | | a bulk cargo's texture |

All are SIMISA text files (`JINX0D0t` for `RTSCargo.dat`, `JINX0C3t` for
`RTSObjects.dat`).

### RTSCargo.dat

```
CargoTypes (
    Cargo ( ID ( COAL ) Name ( "Coal" ) Density ( 5000.0 ) Texture ( "coal.ace" ) SMSTrigger ( 1 2 ) )
)
WagonTypes (
    WagonType (
        Name ( "Hopper" )
        WagonName ( Pattern ( "*Hopper*e.wag" ) Filename ( "RRT_TD_MailCar_0001.wag" ) )
        LoadingType ( PILE ) LoadingType ( DUMP )
        IntakePoints ( IntakePoint ( X Y Z Tolerance ) DumpPoint ( X Y Z Width ) )
        LoadingAreas ( LoadingArea ( BoundingBox ( X1 Y1 Z1 X2 Y2 Z2 ) DumpFromSide ( BOTTOM ) ) )
        Capacity ( 0.55 ) DumpSpeed ( 0.72 ) DumpMaxVelocity ( 0.0 ) MaximumWeight ( 370.0 )
    )
)
```

- `Cargo`: a bulk cargo's ID, name, density, texture and the sound
  triggers of its pouring. The build's file has COAL, GRAIN, FERTILIZER,
  SAND and COKE.
- `WagonType`: the loading of the cars whose file names match: by a
  pattern (`*` any) or a whole name. The same fields as a car's own
  `WagonCargoDescription`:
  - `LoadingType` (`CargoLoadingType`, below), as many as it takes;
  - `IntakePoints`: `IntakePoint ( X Y Z Tolerance )` where it is filled
    (in the car's space), `DumpPoint ( X Y Z Width )` where it is emptied;
  - `LoadingAreas ( n LoadingArea ( ... ) )`: `BoundingBox` where the load
    lies, `ParentNode` (a node of the shape it is loaded into),
    `AreaPivot`, `DumpFromSide` (`BOTTOM`, `TOP`, `LEFT`, `RIGHT`,
    `FRONT`, `BACK`), its own `LoadingType`, `Invisible ( b )` (a covered
    car filled without showing the load);
  - `LoadingAnimation`: `LoadingUnloading`, `LoadingSide`, `ForAreas`,
    `ForIntakePoints`, `Nodes`, `ForLoadingType`, `AnimFrame`, `AnimTime`:
    the hatches opening, a dump car tipping (the shape's animation, else
    one made from `AnimTime`);
  - `Capacity`, `MaximumWeight`, `DumpSpeed`, `DumpMaxVelocity`,
    `UseBoundingBoxAsArea`.

### CargoLoadingType

| Value | RTS's meaning |
|---|---|
| `BOX` | boxes, loaded by hand |
| `CONTAINER` | a container, by a crane only, needs an area |
| `DUMP` | dumped out |
| `ENERGY` | charged from a cable |
| `FRIDGE` | carried in refrigerator cars only |
| `GAS` | gas |
| `LIQUID` | liquid |
| `PILE` | bulk, needs an area or intake points |
| `STACK` | a stack loaded by hand or a crane |

### Cargo types built in

`rts1.exe` knows 19: `coal`, `grain`, `gravel`, `slate`, `iron_ore`,
`fertilizer`, `cement`, `sand`, `oil`, `mazut`, `chemical`, `gasoline`,
`alcohol`, `water`, `gas`, `mail`, `food`, `luggage`, `message`; the
reference also lists `DIESEL`, `ELECTRICITY` and `ORE`. A bulk cargo's
texture is `GLOBAL\TEXTURES\RTSCargo_<cargo><n>.ace` (`RTSCargo_` and
`%i.ace` in `rts1.exe`; `n` from 1: RTS 0.44.1 ships `RTSCargo_coal1`,
`_grain1`, `_gravel1`, `_sand1`).

### RTSObjects.dat

```
CargoObjects (
    CargoObject ( ID ( Container1_1 ) Name ( "..." ) Description ( "..." )
        LoadingType ( CONTAINER ) CargoType ( ... ) Shape ( t_container1_1.s )
        Size ( X1 Y1 Z1 X2 Y2 Z2 ) Volume ( f ) Stackable ( 3 ) Mass ( 50t ) MassMaximum ( 150t ) )
)
ServiceObjects (
    ServiceObject ( ID ( DRAG_NORMAL ) Type ( DRAG ) Shape ( drag.s ) WagonAttachment ( ... ) WorldAttachment ( ... ) )
)
```

A cargo object is a piece loaded onto cars: a container, a lorry, rails,
crates, a flour bag (`Size` or `Volume` is needed). The shapes lie in
`TRAINS\OBJECTS\CARGO`.

### ESD_Pickup_Object

A shape whose `.sd` has it is a pickup wherever it stands:

```
ESD_Pickup_Object (
    PickupType ( PickupTypeID f )
    SpeedRange ( MinSpeed MaxSpeed )
    PickupAnimData ( AnimType AnimTime )          MSTS's, not used
    PickupCapacity ( MaxCapacity PickupSpeed )     a socket: VA and volts
    CargoType ( CargoType )
    PickupLength ( f )                             a platform's length
    PlacementMargin ( f )                          tolerance on the track, m
    PickupPoints ( n { X Y Z } ... )               fastened to the track
    PickupArea ( X1 Y1 Z1 X2 Y2 Z2 )               a crane needs one
    CargoTable ( Item ( CargoLoadingType CargoType CargoObjectID NumItems Mass ) )   -1: no limit
    PickupCraneType ( 1 bridge | 2 jib | 3 platform )
    PickupCraneOriented ( 0 Z | 1 X | 2 -Z | 3 -X )
    PickupCraneShapeNodes ( Bridge ( n ) Cabin ( n ) Winch ( n ) Hook ( n ) HookInitialHeight ( f ) )
)
```

A platform loads a car within `PickupLength` of a pickup point, or in its
area.

| PickupType | What |
|---|---|
| 1 | cargo, grain, bulk |
| 2 | cargo, coal, bulk |
| 3 | cargo, gravel, bulk |
| 4 | cargo, sand, bulk |
| 5 | fuel, water: liquid loading rack |
| 6 | fuel, coal, bulk |
| 7 | fuel, diesel: liquid loading rack |
| 8 | platform unloading covered cars |
| 10 | sand box |
| 11 | socket / hose |
| 12 | loading crane |
| 13 | unloading trestle for bulk |
| 14 | a crane loading bulk |
| 15 | platform unloading covered cars |
| 16 | loading and unloading rack for liquids |

### Elsewhere

- Consist: `FreightAnim` with `FreightAnimCargoID`, `FreightAnimCargoType`,
  `CargoObjectID`, `ServiceObjectID`, `FreightAnimRemove` (openRTS reads
  it: `ConsistFreight`, the car's mass, the cargo dialog).
- Activity: the `PickupEvent` condition (`Location`, `Radius`,
  `WagonList`, `CargoMassLoad`, `CargoMassUnload`, `CargoType`,
  `PickupTask`, `TrainID`); `CargoMass` of a wagon group.
- Sounds: `pickup.sms`, variable 1 the load in percent; triggers 1/2 a
  diesel pump ready / off, 11/12 sand, 21/22 coal, 31/32 liquid, 35/36 gas,
  101 a pile starts falling (and the `Ted_cargo_*` sounds).
- Multiplayer: `0x461C` cargo on a car (`reverse/multiplayer-protocol.md`).
- `rts1.exe`'s texts: "Load cargo", "Unloading cargo", "Cargo
  manifest", "Pickup type: %s    Current cargo: %s", "Cargo: %s Amount:
  UNLIMITED", "Failed to place cargo in the selected area", "Cargo:
  %0.02ft, used %0.02f of %0.02f volume", "Show cargo loaders".

## How RTS loads (rts1.exe)

Static analysis (IDA), CONFIRMED unless marked:
- `0x407588` loads `RTSCargo.dat`, then `RTSCargoCustom.dat`, lowered,
  through `0x4D77E4`, which reads **only the `WagonType` blocks**
  (`0x4D8164`): `CargoTypes` is not read. The cargo kinds, their
  densities and textures are built in.
- `0x45B930` maps a cargo name to its code; `0x50CD6C` gives its density
  (kg/m³); `0x50D1F0` a pile's shape factor; `0x50D298` its texture
  `RTSCargo_<name><n>.ace`:

  | Code | Name | kg/m³ | Pile |
  |---:|---|---:|---:|
  | 1 | diesel | | |
  | 2 | gasoline | 700 | |
  | 3 | alcohol | 790 | |
  | 4 | gas | 1150 | |
  | 5 | electricity | | |
  | 8 | sand | 1500 | 0.7 |
  | 9 | water | 1000 | |
  | 10 | coal | 1500 | 0.5 |
  | 50 | gravel | 1750 | 0.3 |
  | 51 | grain | 770 | 0.4 |
  | 52 | slate | 1450 | 0.6 |
  | 53 | ore | 1200 | 0.2 |
  | 54 | iron_ore | 3400 | 0.2 |
  | 55 | fertilizer | 1300 | 0.25 |
  | 56 | cement | 1510 | 0.1 |
  | 57 | metal | 3400 | 1 |
  | 58 | oil | 850 | |
  | 59 | chemical | 1840 | |
  | 60 | mazut | 910 | |
  | 300 | mail | | |
  | 301 | food | | |
  | 302 | luggage | | |
  | 303 | message | | |
  | 304 | container | | |

- A car's room (`0x501D80`): with loading areas their summed volume,
  else `Capacity` × the car's `Size` box; the load's mass is capped at
  `MaximumWeight` × that box (`0x5020E0`: kg per m³ of the box; 370 for
  a hopper). A car's loads are records (cargo, area, volume, mass); the
  car's mass grows by volume × density.
- **Num3 on a pickup** (`0x4121BC` → `0x4210FC`): the state cycles off →
  ready → active → off; Ctrl+Num3 asks to unload (types 5, 7, 8, 13,
  16). "Pickup ready" / "Pickup activated" / "Unloading cargo" are shown.
- **Each frame, each pickup** (`0x4E59A0`, from the main loop), over the
  cars in its reach (`0x4E4A90`) moving within `SpeedRange`:
  - 7 (diesel) fills a diesel engine's fuel, 10 an engine's sand, at
    `PickupSpeed` per second up to the `.eng`'s;
  - 1–4, 6, 10 (bulk): a car whose intake point is under it
    (`0x4E974C`, within the point's tolerance) and not full is filled at
    `PickupSpeed` m³/s; the stock (`CargoTable`, else unlimited) falls by
    volume × density; a falling-cargo object hangs over the car (the
    disc the testers see) and the pile grows; it stops when the car is
    full (state 3) or the stock is out (4); sound 101 while it pours;
  - 5, 7, 16 (liquids): fills in percent, Ctrl+Num3 drains;
  - 13 (bulk trestle): empties the car at its `DumpSpeed` (else the
    pickup's), only below `DumpMaxVelocity`, with the car's unloading
    animation;
  - 12, 14, 15 (cranes, platform): when active and the car stands in the
    area, the **"Load cargo" dialog** (resource 113, `0x53641C`) opens.
    The loading types it offers: 12 container, stack, box; 14 pile,
    stack; 15 stack, box, fridge.
- **The dialog**: Wagon (the cars in reach), Area (all, "Internal area",
  "Bounding box", Area 1..n), Placement (four ways; the fourth is
  manual, one item only), Center, Cargo Type (all, pile, liquid,
  container, stack, box, fridge), the cargo list (from the pickup's
  `CargoTable`: "name [t]"), Weight, Amount, Loaded cargo ("name X n
  [t]"), Load, Unload, Unload all, Cancel. Load places the items one by
  one (`0x4E8890`); a car over its maximum weight gets "too heavy", an
  item that does not fit "Failed to place cargo in the selected area".

## On the routes

348 shapes with `ESD_Pickup_Object` in the build: Pavlovsk 140,
Chernyshevsk 94, Novokuznetsk 54, Stepy Priazovya 36, Kuban 22,
TestRoute 2. By type: 12 (crane) 90, 13 (bulk trestle) 50, 7 40, 2 32,
5 28, 1 20, 4 20, 8 20, 3 16, 11 12, 15 12, 10 4, 16 4. `CargoTable` in
4 (mail containers, unlimited); `PickupCraneShapeNodes` in none: no crane
is animated.

## In the game (testers)

- Bulk: a disc appears in the air over the car and the cargo pours from
  it.
- Cranes (since RTS 0.43): the train stands in the crane's area; Num3
  opens a menu: the cars in the area, how the load is placed, the car,
  where on it (right, middle, left) and the cargo. The list is one for
  all: a flat car may take a load meant for covered cars, but one that
  does not fit is refused.
- A pickup's stock has no limit.

## openRTS

Done 2026-11-25..26 (`cargo.md`): stages 1–6 (with fuel and sand,
sounds, the network, the cranes' lowering and nodes, the cars'
LoadingAnimation); left: stage 7.

Plan (request 2026-11-25-1: twelve bulk cargos of openRTS's own and the
loading itself, animated):

1. Formats: `RTSCargo.dat` + `RTSCargoCustom.dat` + openRTS's own (in a
   mod), `RTSObjects.dat` (global and the route's), `ESD_Pickup_Object`,
   `WagonCargoDescription` and `WagonTypes` matched to a car.
2. Simulation (`simulation/`, so the server runs it): a car's load (kind,
   mass, volume, where it lies), the pickups in the world and the car in
   their reach, loading and unloading at `PickupSpeed`, the car's mass,
   `0x461C`.
3. The view: a pile in the car's area (its height from the volume, the
   cargo's texture), cargo objects placed and stacked, hatches and
   tipping (`LoadingAnimation`).
4. The menu (Num3) as RTS's, in Russian first.
5. openRTS's own: animated loaders (a stream from the hopper, a crane's
   bridge, cabin, winch and hook moving when the shape names them),
   sounds.
6. openRTS's cargos: ASH, CLAY, GYPSUM, SHALE, GLASSSHARDS, WOODCHIPS,
   SALT, PORODA, COKE (RTS has one), SLAG, CASTIRON, SINTER; textures
   `oRTSCargo_<cargo><n>.ace`, found as RTS finds `RTSCargo_`. The
   testers' densities, kg/m³: ash 1000, clay 1500, gypsum 2300, shale
   2700, glass shards 1500, wood chips 400, salt 2100, ore-bearing rock
   3000, coke 450, slag 750, cast iron 7200, sinter 2300.
7. Industries that process (openRTS's own, as in Trainz; RTS's pickups
   stay unlimited): an industry has a stock of each cargo it takes and
   gives, consumes what is brought and makes its products in set
   proportions at a set period (oil 1000 l in, every so often: 450 l
   petrol, 400 l diesel, 150 l kerosene out); a pickup loads from the
   stock, so it can run out. Kept by the server, like the trains.
