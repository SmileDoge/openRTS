# The car menu (Num3)

Status: static analysis of `rts1.exe` (IDA, decompiled), CONFIRMED unless
marked otherwise:
- `0x4121BC`, `0x54E634` — input handlers;
- `0x415A34` — picker;
- `0x462FC8` — menu builder;
- `0x463F84` — action handler.

The menu texture is `menug.ace`; the menu object lives in `0x61D6C4`.

## Opening and closing

- **Opening.** `VK_NUMPAD3` calls `0x415A34(99)`. The result's type byte
  `+32` decides what opens:
  - 15 — a door / get-on action (`0x4210FC`; with Ctrl, mode 5);
  - 16 — a coupling: menu type 1;
  - 17 — a bogie: menu type 2.

  Both menus are built with `0x462FC8(type, car, end)`. `end` is the int16
  at `+34`: 0 for the end or bogie towards the train front, 1 for the rear
  one. From the keys the menu sits at the screen centre; with `end` > 1 it
  is placed at the cursor.
- **While open:**
  - Up / Down move the highlight (`0x465C94`);
  - Enter or a click picks the item (`0x463F84`);
  - Esc or Num3 closes the menu.

  A handled item closes the menu. "Couple" on a moving train instead greys
  itself out.
- **Self-close (`0x408574`):** a coupling menu of a train that is not the
  player's closes when the camera is farther than the car's length + 5 m.

## The picker (`0x415A34`, argument 99)

The picker runs only off the cab: there is no current locomotive, or the
mode `byte_61D394` is 6 (on foot). The cursor plays no part. It takes the
nearest point that projects into the viewport (`gluProject`), over every
car:

- **Coupling** — car-local `(0, 1, ±length/2)`:
  - within 5 m of the camera;
  - the car stands (|v| < 0.1) or belongs to the player's train or a train
    coupled to it;
  - the player's AI-type trains are skipped (in multiplayer, only when
    flagged).

  The end index is `(Flags & 1) != 0` for +z and the inverse for −z: the
  shape's front is the train front unless the car is turned.
- **Bogie** — car-local `(0, 0, z)`:
  - z is the first / last bogie of the car's bogie list (`+348` count,
    `+352` 72-byte records), else ±length/4;
  - within twice the type's `+36` (Size width) of the camera;
  - not the current locomotive;
  - |v| < 3 m/s or the player's train.

## Building (`0x462FC8`)

Ids are those `0x463F84` switches on. A coupling joint is the pair
`[22]` = the car ahead (its rear end) and `[23]` = the car behind (its front
end).

### Coupling, both cars present

| Id | Item | Shown when |
|---:|---|---|
| 1 | Uncouple | unless both couplers are type 1 (bar) |
| 8 / 9 | Close / Open end valves | Open if both cocks are closed (`+29` 0x80 / 0x40) |
| 2 / 3 | Disconnect / Connect train pipe | Connect if either hose is off (`+29` 0x02 / 0x01) |
| 4 / 5 | Disconnect / Connect charging pipe | both `BrakeSystemType & 4`; bits 0x08 / 0x04 |
| 6 / 7 | Disconnect / Connect EP cable | both `BrakeSystemType & 2`; bits 0x20 / 0x10 |
| 11 / 10 | Disconnect / Connect SME-cable | type `+28` bit 2 on the car ahead and bit 1 on the car behind (pairs swapped when flipped); connected: `+30` 0x02 / 0x01 |
| 13 / 12 | Disconnect / Connect high-voltage bus | type `+28` bits 8 / 4; `+30` 0x08 / 0x04 |

### Coupling, a free end

| Id | Item | Shown when |
|---:|---|---|
| 16 | Couple | see below |
| 9 / 8 | Open / Close end valve | that end's cock |
| 14 / 15 | Unlock coupling / Lock coupling opened | Unlock when the end is held open (`+14` 1 front / 2 rear) |
| 17 | Fix coupling | `+14` 0x10 / 0x20: broken |

"Couple" needs a neighbouring train at that end (train `+360` / `+364`,
gap `+212` / `+276`). Let `r` be the two ends' coupling `+28` (r0) summed.
- If gap ≥ `r`, nothing is shown.
- Both trains must stand.
- If either coupler is not automatic, the types must match.
- If both are automatic, one end must be held open and the gap must be
  ≤ 0.75 · `r`. Otherwise they couple by themselves on contact.

### Bogie (`end` 0 / 1 is the front / rear bogie)

| Id | Item | Shown when |
|---:|---|---|
| 9 / 10 | Trigger board / unboard event | activity only |
| 1 / 2, 3 / 4 | Remove / Place drag | front / rear bogie, `+31` 0x01 / 0x02 |
| 5 / 6 | Release / Apply handbrake | `HandbrakeValue` (`+112`) > 0.01; not on an engine with type `+388 & 0x20` |
| 12 / 11 | Shut / Open release valve | `+32` 0x80 set / `BrakeEquipmentType & 0xB` |
| 7 | Shut emergency valve | `+32` 0x10 |
| 8 | Cargo information | freight car with cargo |

The type-3 menu is the activity's own; type 4 is the "Sim" menu (server,
track control, shunting).

## Actions (`0x463F84`)

Car flags are the dword at `+28` (`ts.h` Wagon Flags), set with
`0x4FED6C(car, mask, on)`. Sounds are discrete triggers on the car's sound
sources:
- `0x4A8B54` on the external source `+376`, the cab source `+444+16` and
  the engine source `+372+204`;
- `0x46AF1C` on all of them;
- `0x4A9DE8(source, 80, -1)` sets variable trigger 80, the air.

**Coupling (type 1):**

| Id | Effect | Flags | Triggers |
|---:|---|---|---|
| 1 | `0x469858(train, index of [23], 0, 0)`: uncouple with the cocks as they are and the couplers not held open | | |
| 2 | train pipe hoses off | 0x200 / 0x100 | 82, and 80 where that end's cock is open |
| 3 | train pipe hoses on | | 83, 81 |
| 4 / 5 | charging pipe off / on | 0x800 / 0x400 | 82 / 83 |
| 6 / 7 | EP cable off / on | 0x2000 / 0x1000 | 84 / 85 |
| 8 | cocks shut | 0x8000 / 0x4000 | 86, 81 |
| 9 | cocks opened | | 87, and 80 where the hose is off or the end is free |
| 10 / 11 | SME cable on / off | 0x20000 / 0x10000 | 89 / 88; then `0x4F995C` rebuilds the multiple-unit groups (`+432` / `+396` of each engine) |
| 12 / 13 | HV bus on / off | 0x80000 / 0x40000 | 91 / 90; then `0x4F9FB4` sets each engine's bus ends (`+436` / `+440`) |
| 14 / 15 | `0x4FF4B0(car, end, 0 / 1)`: free / hold open | | 59 / 62 |
| 16 | couple (`0x469ED8`) after clearing the held-open bits | | |
| 17 | starts fixing the coupling | `0x61D6D0` = car, with a time | |

Multiplayer sends ids other than 1, 16 and 17 to the server (`0x4CCA34`).

**Bogie (type 2):**

| Id | Effect | Flags | Triggers |
|---:|---|---|---|
| 1 / 2 | front drag off / on | 0x1000000 | |
| 3 / 4 | rear drag off / on | 0x2000000 | |
| 5 / 6 | `HandbrakeValue` = 0 / 1 | | the brake sound `+384` |
| 7 | the emergency valve shut | `+32` &= ~0x10 | |
| 8 | the cargo dialog | | |
| 9 / 10 | activity events | | |
| 11 / 12 | release valve open / shut | `+32` 0x80 | 92 / 93 |

Removing a drag, releasing the handbrake or opening the release valve on a
train that is not the player's wakes it (`0x5067A4(train, 3)`).

What the flags do in the train update is in `brakes.md`:
- emergency: the pipe vents at 18 atm/s;
- release valve: the cylinder bleeds at 0.5 atm/s;
- each fastened bogie: 10 kN of brake force.
