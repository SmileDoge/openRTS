# Brakes in RTS

Status: static analysis of `rts1.exe`:
- `0x424488`, the train update: pipes, triple valve, brake force;
- `0x5031D8`, the DLL `Run` wrapper: independent brake, main reservoir;
- `0x44247C`, the wagon parser: brake parameters.

Field names come from `ts.h` (`Wagon`, `Engine`), whose offsets match the
car and engine objects used in that code (`+68 Velocity`, `+84 Force`, …).
Everything below is CONFIRMED from the decompiled code unless marked.

## Who does what

- **Locomotive DLL:** it sets demands on its `Engine` object:
  - `TrainPipeRate`;
  - `IndependentBrakeValue`;
  - `MainResRate`, `ChargingRate`;
  - `EPTvalue`;
  - `HandbrakePercent`;
  - `BrakeSystemsEngaged` (bit 1: brake pipe, 2: electro-pneumatic, 4: charging pipe);
  - `EngineFlags` (bit 1: release the train brake on this loco, 2: release
    the independent brake).

  There is no brake valve logic in the engine itself.
- **The engine (`0x424488`) does the air:**
  - pipes along the train;
  - the triple valve and auxiliary reservoir in every car;
  - cylinder to force.

## Units

- **Pressures and rates** are in atmospheres (and per second). The parser
  multiplies the psi values of `.wag` by 0.068073519 = 1/14.69:
  - `MaxReleaseRate`, `MaxApplicationRate`, `MaxAuxilaryChargingRate`;
  - `BrakeCylinderPressureForMaxBrakeBrakeForce`, `TrainPipeLeakRate`.
- **RTS overrides:** `RTSMaxReleaseRate` / `RTSMaxApplicationRate` replace
  the MSTS values when given (the release one only if > 0).
- **`TripleValveRatio`** is taken as is. A value ≤ 0 is an error ("Triple
  valve ratio non-positive") and the wagon fails to load.

## Wagon type fields (`0x44247C`)

| Offset | Key | Notes |
|---:|---|---|
| `+308` | `BrakeEquipmentType` | flags: 1 triple_valve, 2 auxilary_reservoir, 4 ep_brake / electro-pneumatic, 8 handbrake, 0x10 vacuum_brake |
| `+310` | `BrakeSystemType` | flags: 1 air_single_pipe, 2 ep / electro-pneumatic, 4 air_twin_pipe / air_charge_pipe, 8 vacuum_single_pipe |
| `+312` | `MaxHandbrakeForce` | N; only with handbrake equipment |
| `+316` | `MaxBrakeForce` | N |
| `+320` | `TripleValveRatio` | |
| `+324` | `MaxReleaseRate` (or `RTSMaxReleaseRate`) | atm/s |
| `+328` | `MaxApplicationRate` (or `RTSMaxApplicationRate`) | atm/s |
| `+332` | `MaxAuxilaryChargingRate` | atm/s |
| `+336` | `BrakeCylinderPressureForMaxBrakeBrakeForce` | atm |
| `+340` | `TrainPipeLeakRate` | atm/s; parsed but not used: no code reads `+0x154` of the wagon type as a float (all `0x154` operands checked in IDA; the other hits are pointers of other structures) — PROBABLE |
| `+508` | main reservoir volume (engines) | relative units |

## Car fields (`ts.h` Wagon)

| Offset | Field | Role |
|---:|---|---|
| `+44` | `BrakeForce` | result, N |
| `+52` | `IndependentBrakePressure` | locomotive cylinder from the independent brake |
| `+56` | `AuxiliaryPressure` | auxiliary reservoir |
| `+60` | `EPTvalue` | electro-pneumatic demand (−1 = none) |
| `+64` | `PTValue` | control chamber (see below) |
| `+96` | `BrakeCylinderPressure` | train brake cylinder |
| `+100` | `TrainPipePressure` | brake pipe |
| `+104` | `TrainPipeRate` | pipe change during the frame (Δp) |
| `+108` | `ChargingPipePressure` | second (charging) pipe |
| `+112` | `HandbrakeValue` | 0..1 |
| `+404` | main reservoir pressure (engines) | |

### Flags

- **Flags byte 2** (`+29`), as `ts.h` documents:
  - `0x01` / `0x02`: brake pipe hose front / rear disconnected;
  - `0x04` / `0x08`: second pipe front / rear disconnected;
  - `0x10` / `0x20`: EP line front / rear disconnected;
  - `0x40` / `0x80`: end cock front / rear closed.
- **Flags1** (`+32`):
  - `0x10`: emergency venting;
  - `0x80`: the release (bleed) valve is pulled.
- **Flags byte 4** (`+31`), bits 1 and 2: front / rear "bogie fastened"
  (brake skids, chocks).

## Each frame, in order

### 1. Locomotive demands

For cars with a running DLL:
- **Brake pipe:** `TrainPipeRate` is taken when `BrakeSystemsEngaged & 1`.
- **EPT:** `EPTvalue` is taken when `& 2`. It is spread to the neighbouring
  cars whose `BrakeSystemType` has EP and whose EP lines are connected;
  every other car gets −1 (no EP demand).
- **Charging pipe:** `ChargingRate` is taken when `& 4`.
- **Handbrake:** `HandbrakeValue = HandbrakePercent / 100` on the
  locomotive.

### 2. Venting

Per car:
- **Emergency** (flag `0x10`): the pipe vents at 18 atm/s.
- **Open pipe end:** an open end cock with nothing coupled (first car, or
  the hose disconnected) vents at 10 atm/s. Its hiss goes to sound variable
  80, as `p/3`.
- **Bleed valve** (flag `0x80`): the cylinder bleeds at 0.5 atm/s.

### 3. Pipe from the locomotive

The pipe is split into **connected segments**, bounded by disconnected
hoses and closed end cocks.
- **Rate < 0:** the pipe at the locomotive vents by `rate · dt`.
- **Rate > 0:** the pipe is connected to the main reservoir. Pipe sections
  move towards the volume-weighted average of the reservoir and the
  section:
  - pipe volume = car length × 0.0024 (a pipe of about 55 mm bore);
  - the reservoir uses its `+508` volume;
  - the step factor is `min(1, 30 / L) · dt · rate`, where L is the summed
    length from the locomotive.

  So air reaches far cars more slowly. The reservoir pressure drops by what
  it gives.
- **Charging pipe:** the same for the second pipe on twin-pipe cars.

### 4. Pipe equalisation between cars

For neighbouring cars in a segment:
- the pressures move towards their volume-weighted average with the factor
  `min(1, 30 / distance between the car centres) · dt`;
- the same for the charging pipe where both cars have it.

Then `Δp = p_now − p_at_frame_start` (`+104`).

### 5. Electro-pneumatic brake

For cars with EP and `EPTvalue ≥ 0`, the cylinder moves towards
`EPTvalue`:
- **Up:** limited by `MaxApplicationRate · dt` and by the air in the
  auxiliary reservoir — `A` drops by `ΔC / ratio · 0.625`, and `C` cannot
  pass the equalisation point `(A − C)/(ratio + 0.625)`.
- **Down:** by `MaxReleaseRate · dt`.
- **Pneumatic part:** it is ignored for that car (`Δp = 0`).

### 6. Triple valve

For cars with `BrakeSystemType` air single or twin pipe.

Notation: `p` pipe, `A` auxiliary reservoir, `C` cylinder, `K` control
chamber (`PTValue`), `R` = `TripleValveRatio`.

- **Control chamber `K`:**
  - **single pipe:** `K = A`;
  - **twin pipe:**
    - `A` charges from the charging pipe:
      `ΔA = (CP − A) · 0.05`, at most `MaxAuxilaryChargingRate · dt`,
      and `CP −= 10 · ΔA`;
    - while `C = 0` and the pipe is not falling, `K` follows `p − 0.2` at
      `0.04 · p` per second.
- **If the pipe is not falling (`Δp ≥ 0`):**
  - **charge:** when `A + 0.2 < p`, `ΔA = (p − A − 0.2) · 0.05`, at most
    `MaxAuxilaryChargingRate · dt`; `A += ΔA`, `p −= 10 · ΔA` (none while
    a twin-pipe car's charging pipe is above the pipe);
  - **release:** when the pipe rose, `C −= Δp · R`, down to 0. Release is
    *graduated*: proportional to the pipe rise.
- **Else, if `K > p` (application):**
  - `C += −Δp · R` and `A += Δp · 0.625`, so the cylinder fills in
    proportion to the pipe drop;
  - **limit:** `C` cannot exceed the equalisation with `A`.
    `ΔA = −(A − C)/(R + 0.625)`, `C += −ΔA · R`, and `C ≤ A`;
  - **`MaxApplicationRate` does not limit the pneumatic application** —
    only EP and the independent brake.
- **Sound triggers:** 14 when the cylinder starts filling, 54 (`0x36`) when
  it empties.

### 7. Locomotive brake cylinder (`0x5031D8`)

- **Independent brake:** `IndependentBrakePressure` moves towards
  `Engine::IndependentBrakeValue`:
  - up at `MaxApplicationRate`, down at `MaxReleaseRate`;
  - a negative demand leaves it as it is;
  - `EngineFlags & 2` bleeds it to 0 at `MaxReleaseRate`, then clears the
    flag.
- **Releasing the train brake on the locomotive:** `EngineFlags & 1` bleeds
  the loco's train brake cylinder at `MaxReleaseRate` and then clears the
  flag (with sound 54 when the independent brake is also empty).
- **Main reservoir:** `+= Engine::MainResRate · dt`, not below 0. The DLL
  models the compressor and consumption.

### 8. Brake force

```text
BrakeForce = max(BrakeCylinderPressure, IndependentBrakePressure)
             / BrakeCylinderPressureForMaxBrakeBrakeForce × MaxBrakeForce
           + HandbrakeValue × MaxHandbrakeForce
           + Engine::BrakeForce            (locomotives: dynamic brake etc., from the DLL)
           + 10 000 N per fastened bogie   (flags +31 bits 1, 2)
           + track retarders               (track item effect, by distance)
```

- The ratio is not capped at 1.
- This force joins the dissipative force of the coupler solver
  (`runtime-overview.md`). It reduces speed only, as static friction.

## Not traced

- **Initial pressures** when a train is created: charged or empty, and per
  `Init` state.
- **Where flags1 `0x10`** (emergency) is set: train separation, the
  passenger alarm, the DLL?
- **`TrainPipeLeakRate`:** its use.
- **Vacuum brakes** (`0x10` / `8` flags): no code path seen in `0x424488`.
- **Wheel slide under braking:** the adhesion limit is seen for traction
  only.
