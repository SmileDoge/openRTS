# Rolling stock: how RTS reads consists, engines and wagons

Reverse of `rts1.exe` (IDA, on the copy), implemented in
`formats/src/rolling_stock.cpp` and `formats/src/rts_text.hpp`. The physics
these values feed is in `runtime-overview.md`, the brakes in `brakes.md`.

Confidence: CONFIRMED (code read), PROBABLE (strong evidence, an
alternative is possible), UNKNOWN.

## 1. Text reading (`0x4578C0`, "parser_maybe")

CONFIRMED.

- **Whole file, lower-cased:** the file is decoded (UTF-16 with BOM, else the
  ANSI code page) and lower-cased with `CharLowerBuffW` before any search.
  Keywords are therefore case-insensitive and nothing else is.
- **Comments:** `Comment`, `Skip` and `_Skip` blocks are blanked first
  (`0x459B38`).
- **Keyword search** within a range of the text, in document order:
  - a match must be preceded by whitespace or the range start, and followed
    by optional whitespace and `(`;
  - the first match wins;
  - a block's content excludes the brackets; quotes are respected when the
    closing bracket is found.

  This is a text search, not a tree parse. A keyword inside another block
  is found if it comes first; a missing block reads as 0.
- **Tokens:**
  - `0x458190` skips to the next token (a quoted string counts as one);
  - `0x458688` reads one token (quoted, or up to whitespace or a bracket).
- **Numbers:** `_wtof` / `_wtoi`: the leading number of the token, the rest
  ignored.

## 2. Value units

- **Quantity parser `0x45A8AC`** (CONFIRMED): the leading number, then a
  substring check on the same token, in this order:

  | Substring | Factor |
  |---|---|
  | `kn` | ×1000 |
  | `cm` | ×0.01 |
  | `kw` | ×1000 |
  | `mw` | ×10⁶ |
  | `mph` | ×1609/3600 |
  | `km` | ×1/3.6 (km/h) |
  | `gal` | ×3.79 |
  | a trailing `t` | ×1000 |

  No other unit is converted: `m`, `n`, `w`, `kg`, `in`, `ft`, `lb`, `s` give
  the bare number. Because the checks are substrings, `kn` also matches
  inside a longer token.
- **Product parser `0x45AA40`** (CONFIRMED): `a*b` reads two quantities and
  multiplies them. It is used for `DerailRailForce`, which defaults to
  240000 when ≤ 0.
- **Brake values** are psi and are converted to atm by 1/14.69 (`brakes.md`).

## 3. Consists (`0x4770E0`, `0x477928`)

CONFIRMED.

- **Search order** for a consist name:
  1. `ROUTES/<route>/RTS_ACT/<activity>/`;
  2. `TRAINS/CONSISTS_RTS/`;
  3. `TRAINS/CONSISTS/`.

  openRTS does 2 and 3 (no activities yet), and also accepts a virtual path
  for consists in subfolders.
- **`TrainCfg`** holds:
  - the name;
  - `MaxVelocity` (`_wtof`);
  - `Durability`.
- **`Engine` / `Wagon` blocks**, read in document order inside `TrainCfg`:
  - `UiD`;
  - `EngineData` / `WagonData` (name, then folder under `TRAINS/TRAINSET`);
  - `Flip` (present = flipped);
  - `Cold` (engines);
  - `FreightAnim`;
  - `PlaceDrag`;
  - `HandbrakePercent`.

## 4. Engine and wagon files (`0x566944`, flags 21)

CONFIRMED.

- **Files:** `name.eng` / `name.wag` plus a companion with "rts" inserted
  before the extension: `name.rtseng` / `name.rtswag`.
- **Merge (`0x566F68`):** for the sections `Wagon`, `Engine` and
  `EngExtended`, the companion block's content is inserted at the start of
  the same block of the main file, so its values are found first. A section
  missing in the main file is appended.
- **`RTSInclude`** is not processed for `.eng` / `.wag`.
- **Consequence:** after the merge, the first token of the `Wagon` block is
  the companion's first value, not the name. openRTS takes the name from the
  file stem (PROBABLE that RTS keeps it from the consist).

## 5. Wagon fields (`0x44247C`)

CONFIRMED unless marked.

- **Shapes and sounds:**
  - `RTSWagonShape`, else `WagonShape`; `FreightAnim`;
  - `RTSExteriorSound`, else `Sound`; `RTSCabinSound`.
- **`Type`:** `Freight`, `Carriage`, `Engine`, `Tender`.
- **`Mass`:** `_wtof(token) · 1000`, **always tonnes** whatever the unit
  written (`80kg` reads as 80 t).
- **`Size`:** three `_wtof` values (width, height, length). Each must be
  > 0.
- **`WheelRadius`, `NumWheels`:** plain numbers (`0.625m` reads 0.625).
- **`Coupling`:**
  - **Which block:** the first block is used for coupling [0]. The second is
    searched after the end of the first; when absent it copies the first.
    In the physics, [0] is at the joint behind a car and [1] ahead of it,
    swapped for a flipped car (`runtime-overview.md`).
  - **Values:** `Stiffness`, `Damping` and `Break` are read with `_wtof`
    (the first value for both tension and compression). `r0` uses the
    quantity parser. `CouplingHasRigidConnection` is present or absent.
    `Type`: automatic, bar or chain.
  - **Defaults when 0:** stiffness mass·100 (tension) / mass·120
    (compression); damping mass·200 / mass·250; break mass·100 / mass·200.
    A second block with zeros takes the first block's values.
  - **No `Coupling` block at all:** the values read as 0 and the defaults
    apply.
- **`Friction`:** c1 (quantity), e1 (quantity), transition (`_wtof`, **no
  unit conversion**), c2 (quantity), e2 (quantity). When e1 = 0 and the
  transition is 0, e1 becomes 1.
- **`Adheasion`** (MSTS spelling): four `_wtof` values. [1] is the wheelslip
  limit factor.
- **Derailment:** `DerailRailHeight` (quantity), `DerailRailForce`
  (product, default 240000), `DerailBufferForce`.
- **Brakes:** see `brakes.md` for the field list. `BrakeEquipmentType` and
  `BrakeSystemType` are substring checks on the block (PROBABLE: some exact
  strings were cut in the decompilation). `TripleValveRatio` ≤ 0 is an error.
  `RTSMaxReleaseRate` / `RTSMaxApplicationRate` override the MSTS values.
  `AddBrakeForce` and `ScaleBrakeForce` modify `MaxBrakeForce`.
  `TrainPipeLeakRate` is parsed but never used.
- **Engine block:**
  - `MaxPower`, `MaxForce`, `MaxContinuousForce`, `MaxVelocity`, `Sanding`,
    `DynamicBrakesMaximumForce` use the quantity parser;
  - `MaxCurrent` and `AirBrakesMainResVolume` are plain numbers;
  - the throttle `NumNotches` is in `EngineControllers / Throttle`;
  - `EngExtended`: `LibraryModule`, `Cabin`, `VariableStack`.

## 6. Paths (`.pat`)

From the file format (openRTS `formats/src/train_path.cpp`, not reversed
in RTS):
- **`TrackPDPs`:** points `tile_x tile_z x y z kind flags`. Kind 1 is a
  junction, 2 a plain point.
- **`TrackPath`:** `TrPathName`, `Name`, `TrPathStart`, `TrPathEnd` and
  `TrPathNodes`. Each node is `flags(hex) next next_siding point_index`, and
  0xFFFFFFFF means none.
- **Placement:** openRTS puts the player train's rear on the first point,
  facing the next one (MSTS explore mode). How RTS places it is UNKNOWN.

## 7. Content survey (openRTS `tools/train-check`)

On the installed content (2026-10):
- 2342 consists read, none failed. 2209 have all their stock.
- 4285 stock files load, none broken; 176 files are missing.
- 1828 engines, 1818 of them with a `LibraryModule` DLL.
- `train-check --run` (120 s full throttle, then emergency) passes for all
  2209 complete consists.

## Body on its springs and rail joints (`0x4F5B34`, parser `0x44247C`)

Wagon fields: `FrameSpringStiffness` (+280, double, default mass·30),
`FrameSpringDamping` (+288, double, default mass·1.25), `FrameOscCorrection`
(+296/+300/+304: lateral amplitude, lateral frequency, vertical amplitude;
default 1, negative → 0) — CONFIRMED.

Per car, three axes (offsets +116/+120/+124, rates +132/+136/+140, hunting
phase +128), integrated in 0.05 s sub-steps with spring and damper — CONFIRMED:
- lateral: Q1·m·v²/R from the radius under the bogies (|R| ≥ 1), plus hunting
  `sin(phase)·v·0.008·mass·Q1`, phase += `v·k·0.3·m·dt·Q2/damping`;
- vertical: on the first sub-step, `−0.001·n·(|v|/10.5)·m/dt²` near a node
  end, else `−0.0005·j·(|v|/10.5)·m/dt²` near a rail joint, times Q3; plus a
  term from the grade change under the bogies (not ported);
- longitudinal: `−a·m`.
- Stiffness is FrameSpringStiffness + 45 · car field +144 (UNKNOWN, perhaps the
  load).

Impacts per bogie: `n = 1 − (distance since the node end entered) / w`,
`j = 1 − (distance since the last rail joint) / w`, both clamped to 0 below,
`w = WheelRadius · 1.25 · NumWheels`. Rail joints lie every 50 m along a node,
or on nodes longer than 2000 m in groups at 0, 12.5, 25, 37.5 m every 600 m.
Crossing a joint triggers the wagon sound event 3, changing node event 2.

Use: only the driven engine's offsets, times player mass · 0.0055, move the
cab camera (`0x424488`); the car shapes are drawn without them. openRTS
applies them to the car bodies too (viewer slider, 1 = this size); in the cab
the player is in, the cab shape stays at rest and the camera takes the
offsets, so the cab sways around the driver as in RTS. (The 0.0055 factor was
not found again as a constant: UNKNOWN; openRTS moves the camera by the
offsets times the slider.)

## The exterior in the cab (`0x438D50`, parser `0x44247C`)

- **CONFIRMED** wagon type byte +32: bits 0-1 the `Cabin ( "x.s" n )`
  parameter, bits 4-5 `DoNotHideExtInCabin ( n ) & 3`, bits 6-7
  `DoNotHideExtInSalon ( n ) & 3`.
- On a camera change (`0x438D50`): entering the cab hides the car's exterior
  (its shape instance flag +88 = 20) unless bits 4-5 are set; putting the
  head out of the window (camera mode 4) shows it again (flag 0), back in
  the cab hides it. Bits 6-7 do the same for a salon.
