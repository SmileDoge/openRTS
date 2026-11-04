# Electrification

How openRTS gives electric engines their overhead line, as RTS does
(`rts1.exe` `0x503CE4`, `0x5B53A8`; the reverse notes are in
`docs/reverse/train-dll.md` §14).

## The route's files

Both UTF-16 text in the route folder, named after the `RouteID`:

| File | What it holds |
|---|---|
| `<RouteID>.electrotype` | `ElectricSupplyTypes` (voltage, frequency, voltage drop per km, colour), `CatenaryTypes` (wire position, wires, burn factor), `ElectricSubstations` (supply type, position, voltage, power, phases) |
| `<RouteID>.electrodb` | `ElectroSections` (power sources: substation, phase, `Active`, `FeederPos`, `UseSubstationPos`, `FeederQ`; `CurrentSource`), `SectionElectrification`: per TDB vector node, its `Electricity` pieces (electrosection from an offset on; 0 is dead) and `Catenary` pieces (where a wire is) |

Read by `formats::load_route_electrification` (`openrts/formats/electrification.hpp`).
The `.trk`'s `Electrified ( 1 )` is `RouteDefinition::electrified`.

## The line under an engine

`simulation::Electrification::at(node, offset, point, engine)`, in the order
RTS takes:

1. **An `.electrodb` with electrosections:** no wire over the point (no
   catenary piece covers it) gives nothing; a wire on a dead piece
   (electrosection 0) gives a dead line. Otherwise the electrosection's
   source: the only one, else the chosen one (`CurrentSource`, or what the
   Track control switch set), else every enabled source with the highest
   voltage winning. The source's voltage is its substation's `Voltage` ×
   `FeederQ` % − `VoltageDrop` per km of distance from the feeder (or the
   substation with `UseSubstationPos`); the frequency is the supply
   type's.
2. **Only an `.electrotype`:** its first supply type everywhere.
3. **Only `Electrified` in the `.trk`:** each engine the middle of its first
   `WorksOnLineVoltages` range (an engine without any gets 3 kV DC, an
   openRTS choice).
4. **Not electrified:** no voltage.

## The train's line

Before each step `Electrification::apply` looks under every electric engine
with a pantograph up (risen to the top of its animation, else what the DLL
wants, else while the train works) and not burnt, takes the highest voltage
found in each group of cars joined by the high-voltage bus (an engine
without one is a group of its own) and gives it with its frequency to every
electric engine of that group (`CarState::line_voltage` /
`line_frequency`): RTS's `LineVoltage` / `LineFreq`. No pantograph up in
the group: 0 V.

- **Engines with a DLL** see it in `Locomotive::LineVoltage` / `LineFreq` and
  decide themselves (the VL10k opens its breaker on AC and scales its force
  by voltage / 3000; the VL82m shows both voltmeters).
- **Engines without a DLL** (the keyboard driver, test trains) pull only on a
  kind of current one of their `WorksOnLineVoltages` ranges has (AC or DC by
  the frequency; any when the engine lists none). The voltage level is not
  checked: real lines run outside the listed ranges near substations
  (Chernyshevsk reaches 3.8 kV against the VL10k's 2.5..3.5 kV).
- Native plugins get it in `OrtsTrainInputs::line_voltage` / `line_frequency`.

The Train window's "each engine its own voltage" ignores the route's line
(as an electrified route without an `.electrotype`): a DC engine such as
the VL10k then works on Kuban's AC line too.

## Docking stations

A section with several sources (Pavlovsk's ИЦВЖТ: 3.3 kV DC and 27.5 kV AC)
is fed from its `CurrentSource`. Track control's *Electrification* layer
switches the source, as the dispatcher's "Switch power source" does in RTS.
The option "a section with several sources feeds what the engine works on"
is what RTS does in an activity: the first source whose substation voltage
and frequency an engine works on, in its `WorksOnLineVoltages` order.

## The current, the wire, the substations

As RTS (`0x503CE4`, `0x5B6030`; details in `docs/train-systems.md` and
`docs/reverse/train-dll.md` §17):
- the cars joined by the high-voltage bus share the line (an engine without
  one has its own);
- the line current is the group's power over the voltage, split by the
  raised pantographs, and goes to the DLLs (`LineCurrent`);
- a pantograph past `MaxLineCurrent` burns the wire at once, or with
  `MaxLineCurrentCatenaryBurnTime` when the heat (current / the wire's
  `BurnQ`, cooled by `MaxLineCurrentVelocityIncrease`) reaches the limit;
  it gives no line for 60 s;
- each substation phase's load over 2 s windows is in Track control's
  electrification layer. RTS's client only reports it (package `0x4649`);
  the voltage does not sag with it, and `PowerDecline` is not used.

## Not done

- Drawing the wires from the catenary types (RTS draws them itself).
- The activity's `SetTrainElectricity`, the dispatcher console's power
  controls (roadmap step 6).

## Installed routes

| Route | Line |
|---|---|
| Kuban | 27.5 kV 50 Hz |
| Chernyshevsk | 3 kV DC |
| Pavlovsk | 27.5 kV AC and 3.3 kV DC (a docking station) |
| Novokuznetsk | 3.5 kV DC and 27.5 kV AC |
| Kievskij hod, the test route | electrified without an `.electrodb` |
| Uspensk | not electrified |

Tests: `openrts.electrification` (a synthetic line with a dead stretch and a
docking section, the fallbacks, the keyboard driver) and
`openrts.content.electrification` (the installed routes).
