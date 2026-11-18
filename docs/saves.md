# Saved games

A saved game keeps the game where it stands, in a free run or in a scenario,
and starts it again from there, as RTS's save and Load did (RTS saves on F2,
`0x4121BC`).

## In the game

- **Saving:** the openRTS panel (Esc), "Saved games": a name and "Save";
  F2 saves at once as "быстрое сохранение" (the quick save). Not in a
  network game yet.
- **Loading in the game:** the same section, a save and "Load": the save
  is put back over the game running, which must be the same route and,
  for a scenario's save, the same scenario.
- **Starting from a save:** the launcher's "Saved game" (the route's
  saves, the newest first) starts the game with `--load <file>`: the
  route, the scenario, the time, season and weather come from the save's
  header; the world loads as for a new game and the save goes over it.
- The files: `saves/<route>/<name>.save` in the user data folder
  (`%LOCALAPPDATA%\openRTS`).

## What is kept

- **The header:** the route, the scenario (its name and the file
  `--activity` takes), the time of day, season, weather and thunderstorm.
- **The junctions:** every junction's branch.
- **The trains** (`TrainView::save_game`): each one as a situation keeps it
  (its name, role, front, the cars with their keys and brake shoes), and:
  - its running state (`Train::save_state`, `state_text.hpp`): where its
    ends stand, each car's offset, speed, travel and wheels, its air (pipe,
    reservoirs, cylinders, ЭПТ), handbrake, hoses, cocks, couplers,
    cables, valves and chocks, the distributor's switches, flats, the
    shoes' wetness, heat and wear, sand; the realistic air's distributors
    and the air flowing between the cars; the controllers' handles, valves
    and compressor governors;
  - its path and place on it (the leg and the next mark), a scenario
    train's service, its remove and waiting times;
  - the DLLs' state by car key (`LegacyConsist::save_state`, RTS's
    saved locomotive, `docs/reverse/train-dll.md` §20);
  - whether the engineer was in the cab.
- **The scenario** (`ActivityView::save`): its clock, every event's state
  (enabled, level, timers, the wagon states last seen), the scripts
  waiting, the services in play, the cars tracked, the journal, the
  messages open; the traffic started or given up, the services held, the
  block points (TrainView's `[scenario]` part).

## How it is put back

1. The junctions are thrown, the trains put on the track as a situation
   (the cars keep their keys; new cars take keys above them).
2. Once the shapes of the trains near enough to load are fitted (the
   fitting sets the car spacing; half a minute at most), each train's
   state, path and scenario fields go back.
3. Once the DLL host has the cars, their saved state is written over their
   `Init`, as RTS does.
4. The scenario's state goes over the scenario made anew: its start events
   do not run again.

Checked (2026-11-18): an ЭД4М on Kuban saved in the cab and started again
from the save: its ends where they were to the millimetre, the air going
on, its six DLLs back, the engineer in the cab; the Pavlovsk exam saved and
started again: its ten trains, seventy events, scripts and services the
same. `train_test` saves a train running and braking and puts it back into
a new one: both run on alike to the last digit, with RTS's air and the
realistic air.

## Not yet

- A network game.
- The bodies' sway, a derailment, the wire's heat, the sounds: they start
  at rest.
- Track control's own paths drawn on the map (a route's path is kept).
