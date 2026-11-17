# Locomotive DLLs against real stock

openRTS runs the content's locomotive DLLs (docs/train-dll.md) as RTS runs
them: their cab logic, protections and air are theirs, and openRTS does not
patch them per DLL. Where a DLL does what real stock would not, it stays so
until openRTS's own systems take that stock over
(docs/car-architecture.md); this page collects those cases, as testers
find them, as the list of what openRTS's own cabs have to do right.

Each entry: the stock and its DLL, what the DLL does, what real stock does,
when testers found it, and what was checked here. A case belongs here only
when the DLL does it the same way under RTS (`openrts-train-check --dll`
runs a DLL without the game; with the air "as RTS" openRTS adds nothing of
its own). What openRTS itself does wrong goes to its own document instead.

## Cases

| Stock (DLL) | The DLL | Real stock | Found | Checked |
|---|---|---|---|---|
| ЭД4М (`mdd_ED4m-Alias/ED9M.dll`) | On a cold start the ППТ (cab switch 28, `ZK_3`) stays «Хвостовой» in every cab (its cab file's `States ( 3 2 )`; on the starts "on" and "running" the DLL's Init puts the player's cab at «Головной»). With it at «Хвостовой», putting the reverser key in (at 0) vents the pipe through the ЭПК: ТМ 0, ТЦ 3.8. With «Головной» the pipe charges again. | The ППТ makes up the ЭПТ circuit (lamp К lit). Without К, charging the brakes trips the СК valve, which lets the air above the ЭПК's breakaway valve out (the КОН too), whatever the reverser. | testers, 2026-11-17 (screenshots: reverser out / in at 0 / ППТ «Головной») | ППТ after Init: 0 on the start "on", 2 on a cold start (`openrts-train-check --dll consist "ED4M-0050 10vag"`, air RTS and the DLL's valve alike) |
| ЭП1 (`mdd_EP1-Alias/ep1.dll`, `Brakes.dll`) | Changing cabs with the ЭПК left on: nothing trips, but traction does not make up and nothing says why. | The ЭПК trips when the cab is changed with it on. | testers, 2026-11-17 | not yet |

## What to tell a player

- ЭД4М, cold start: in the head cab put the ППТ at «Головной», in the tail
  cab leave «Хвостовой»; the К lamp lights, and the reverser trips nothing.
- ЭП1, changing cabs: switch the ЭПК off in the cab left.
