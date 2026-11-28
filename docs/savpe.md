# openRTS's САВПЭ

The САВПЭ (the electric trains' automatic announcer and driving aid) of the
installed content is MaDaDev's `Usavp.dll` with its sound script
`Usavp.sms`. openRTS is making its own in its place, as it did the КЛУБ and
САУТ in place of `Brakes.dll` (docs/brakes-dll-replacement.md). The real
device keeps a base of stops and the distances between them, counts the way
by its path sensor, and the "Пуск" key before leaving clears what it has
gathered; it does not know the train's coordinate.

## Stages

1. **The data** (done 2026-11-28): the stops on the train's route and their
   voices (below).
2. The device, by the train's place on its route all along (testers
   2026-11-28: no odometer, no "Пуск" clearing it, in a simulator): "Платформа
   А, следующая Б" some 200 m before the platform's end, "Следующая Б, двери
   закрываются" some 15 s after stopping, the welcome, "со всеми
   остановками", "конечная". No keyed route nor hidden key presses: the
   stops come from the train's path, the number from its timetable. A
   setting as the КЛУБ's: openRTS's, or the installed one.
3. Its screen (`USAVP_DISP`), lamp and keys on the ЭД4М and ЭД2Т as the
   installed САВПЭ shows them, nothing added.
4. The timetable maker at the spawn: the running times by the limits with
   speeding up and slowing down ("strict" Vmax − 10 km/h, "easy" Vmax − 20),
   stops by ticks, standing times in steps of 30 s, the start time, the
   train's number (four digits from 6 or 7, even or odd by the direction).
   Made in the game, run at once (no restart); kept by openRTS, single
   player for now. One speeding up and slowing down for every train.
5. Tests and documentation; checked on the lines with voices.

## Stage 1: the stops and their voices

`simulation/savpe.hpp`:

- `savpe_stops`: the platforms the route passes, in order, each once (a
  station's platforms in a row on one track count once): the station's
  name, station or halt (a name with "пл.", "о.п.", "км"), the far end's
  distance and the platform's length, its milepost
  (`simulation/milepost.hpp`); a stop with no post within 5 km (a path's
  start) takes its neighbour's less the metres between them.
- `savpe_voice_lines`: the installed script's timetables
  (`MDD_INFO_R<line>_<train>_S<n>_SOUND / _POS`, `_WEL_SOUND`): each line's
  stops at their km (POS in hundreds of metres) with what is said leaving
  the stop before (S odd, POS 0) and arriving (S even). Two kinds:
  - made of parts (line 20, Moscow): leaving "следующая" + the name +
    "двери закрываются", arriving the name + "следующая" + the next one's,
    or "конечная"; the name's file alone and the common phrases are kept,
    so any timetable can be voiced. Different trains of the line have
    different voices (`sled.wav` / `next.wav`): stage 2 takes the phrases of
    the name's voice;
  - whole phrases a stop (line 33, Kuzbass: `слЗеньково.wav`,
    `Зеньково.wav`; line 37 numbered): the phrases as they are.
- `match_savpe_voices`: the line whose points are near most of the stops
  (within 0.3 km; km repeat between lines), each stop its voice.

The installed script has six lines: 2 (Pavlovsk), 14, 20 (Moscow), 29, 33
(Western Siberia), 37. Elsewhere the stops have no voice until name files
are given.

`openrts-path-check <root> <route> x [path...] --savpe` prints the lines
and each path's stops with their voices. Novokuznetsk "Прокопьевск п.3
(пасс)": 14 stops, all voiced from line 33 (`о.п. 343 км` →
`сл343км.wav` / `343км.wav`, `Зеньково`, `Калачёво` → `Калачево.wav`, ...).

Tests: `openrts.savpe` (a script of both kinds, the matching),
`openrts.content.savpe_novokuznetsk`.
