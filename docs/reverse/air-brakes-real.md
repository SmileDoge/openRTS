# Real air brakes (1520 mm): research for the openRTS pneumatics

This is not a reverse of RTS. It collects how the real devices work, as a
base for an openRTS brake model next to the RTS one
(`docs/reverse/brakes.md`), which stays as it is for Legacy.

Confidence marks:
- **CONFIRMED (n):** stated by a technical source (n = source number, list
  at the end). Two sources that agree are noted.
- **PROBABLE:** follows from confirmed facts, or a single weak source.
- **UNKNOWN:** not found, or the sources contradict each other.

Units: 1 кгс/см² ≈ 1 atm ≈ 0.1 MPa. Values are kept as the sources give
them.

## 1. Parts common to all air brakes

- **ТМ** (тормозная магистраль, brake pipe): charged to the charging pressure
  from the locomotive. A pressure drop applies the brakes, a rise releases
  them (automatic brake).
- **ЗР** (запасный резервуар, auxiliary reservoir): the air that fills the
  cylinder. 78 l on passenger cars with ВР 292 — CONFIRMED (4). Freight cars
  with ВР 483: 78 l — CONFIRMED (3). Other volumes (55 l, 135 l) appear only in
  an unverified summary — UNKNOWN.
- **ТЦ** (тормозной цилиндр, brake cylinder).
- **РК** (рабочая камера, control chamber): a reference pressure the
  distributor compares the pipe with. **ВР 483 has one; ВР 292 does not** —
  CONFIRMED (3, 4). In ВР 292 the reference is the ЗР side of the main piston
  (ЗК connected to ЗР).
- **КДР / ДР** (камера / канал дополнительной разрядки, quick-service): at the
  start of an application the distributor takes some pipe air itself. Each
  next car then sees a faster local drop. This keeps the brake wave from
  fading along the train — CONFIRMED (4, 7).
- **Мягкость** (stability): a slow pipe drop does not apply the brakes. The
  reference chamber bleeds into the pipe through a calibrated path as fast as
  the pipe falls. This lets the driver's valve remove an overcharge, and
  small leaks do not brake the train — CONFIRMED (3, 4).
- **Чувствительность** (sensitivity): a service-rate drop of a small amount
  must apply the brakes — CONFIRMED in principle (3, 4); the numbers are in
  the device sections below.

## 2. ВР 483 (freight)

Mechanism (3, 6): a main part with the main piston (ГП) between ЗК (pipe side
via МК) and РК. A main-part diaphragm drives the quick-service (ДР) valve. A
mode switch selects empty / medium / loaded and flat / mountain.

| Property | Value | Confidence |
|---|---|---|
| Stability threshold | "до 0,03 МПа/мин" (≈0.3 atm/min). The air goes from ЗК and РК to МК through the stability valve (КМ, Ø0.9 mm) without moving the parts | CONFIRMED (3). Source 6 says "до 0,4 МПа за 1 минуту" (≈4 atm/min), which contradicts it; 7 gives 0.2–0.7 atm/min through a 0.65 mm hole. Taken as **0.3 atm/min, PROBABLE** |
| Transition to quick-service | "Повышение темпа разрядки вплоть до 0,1 МПа/мин" opens the ДР valve | CONFIRMED (3) |
| Service rate | 0.1–0.4 atm/s | CONFIRMED (7, 8) |
| Main piston starts at | a ЗК drop of 0.015 MPa (≈0.15 atm). ДР closes at 0.05 MPa | CONFIRMED (3). Source 6: 0.04–0.05 MPa — UNKNOWN which |
| ТЦ by load mode | empty 0.14–0.18, medium 0.28–0.33, loaded 0.39–0.45 MPa | CONFIRMED (3) |
| ТЦ fill, full service | to 90 % in "15–22 с" (OCR) | CONFIRMED (3). Source 6: to 0.35 MPa in 16–22 s, head of the train, loaded |
| Release, flat mode | "легкий бесступенчатый отпуск" (full release, not graduated). The pipe must come back to about 0.02–0.03 MPa below РК | CONFIRMED (3, 6) |
| Release, mountain mode | graduated: the ТЦ keeps a pressure matching the pipe's shortfall | CONFIRMED (3, 6) |
| Release times | train of medium length: head 35–40 s, tail 20–25 s; the head starts earlier | CONFIRMED (3) |
| РК charge | flat 3 min, mountain 4 min (to 0.46 MPa) | CONFIRMED (3) |
| ЗР charge | 78 l to 0.48 MPa in about 4.5 min | CONFIRMED (3) |
| Brake wave | service 100–160 m/s, up to 280 m/s with quick-service. Emergency 200–300 m/s. Tail of a 100-car train about 5.5 s after the head | CONFIRMED (6, 7), the service figures contradict each other — UNKNOWN which |
| Random pipe rise | "При случайном повышении давления в ТМ отпуск тормоза не происходит" (on the lap, rises up to 0.03 MPa) | CONFIRMED (6, 8) |
| Overcharged РК blocking a release | not found | UNKNOWN |
| Pipe drop to cylinder law | no formula found; the ТЦ depends on the depth of the drop and the mode spring | UNKNOWN (qualitative only, 6) |

## 3. ВР 292 (passenger)

Mechanism (4): a slide-valve triple valve with КДР, a mode switch
(К short / Д long / УВ accelerator off) and an emergency accelerator. It is
"непрямодействующим и истощимым" (not self-lapping, exhaustible) —
CONFIRMED (4).

| Property | Value | Confidence |
|---|---|---|
| Stability | a drop "темпом до 0,5 кг/см2 за 1 мин" does not brake. This is what lets the driver's valve remove an overcharge | CONFIRMED (4) |
| Application rate | 0.06–0.6 atm/s | CONFIRMED (4), the lower bound is odd — PROBABLE |
| КДР | 1 l | CONFIRMED (4) |
| ТЦ per pipe drop | "примерно в три раза" the drop (the ЗР : ТЦ volume ratio) | CONFIRMED (4) |
| Full service | at a drop of 1.3–1.5 atm the ЗР equalises with the ТЦ | CONFIRMED (4) |
| Release | a pipe rise of 0.1–0.15 atm. **Full release, not graduated** | CONFIRMED (4) |
| Release time | К 9–12 s, Д/УВ 19–24 s | CONFIRMED (4, 8) |
| Emergency fill | to 3.5 atm: К 6–9 s, Д/УВ 12–16 s | CONFIRMED (4, 8) |
| ЗР charge | 130–180 s on a test stand, to 5.0–5.2 | CONFIRMED (4) |
| Brake wave | service 50–120 m/s (with КДР), emergency 190–220 m/s | CONFIRMED (4, 8) |

## 4. ЭПТ and ЭВР 305 (passenger trains, EMUs)

| Property | Value | Confidence |
|---|---|---|
| Scheme | two-wire electric control (working wire plus rails/body), 50 V DC. Polarity selects application or hold | CONFIRMED (5) |
| РК | 1.5 l. It drives a pneumatic relay, and the relay fills the ТЦ from the ЗР | CONFIRMED (5) |
| Fill | 3 atm in 2.5–3.5 s (≈1 atm/s), the same time in every car | CONFIRMED (5, 8) |
| Steps | minimum application step 0.2 atm. Release is full or graduated (minimum release step 0.2–0.3 atm) | CONFIRMED (5, 8) |
| Release | 3.5 → 0.4 atm in 8–10 s (1.3 mm hole) | CONFIRMED (5) |
| Inexhaustible | the pipe stays charged while braking, and the ЗР is refilled from it | CONFIRMED (5) |
| Fallback | if the ЭПТ is lost, the ВР 292 brakes on the pipe | CONFIRMED (5) |
| EMUs (ЭР2 and later) | ЭВР 305-001 plus ВР 292 | CONFIRMED (5). ЭД4М specifics — UNKNOWN |

**"Twin pipe" in MSTS/RTS content** (`Air_twin_pipe`) is not a Russian
distributor type. In RTS it is the second, charging pipe that feeds the
reservoirs (`docs/reverse/brakes.md`). On 1520 mm stock that pipe is the main
reservoir pipe between locomotive sections. It is set on locomotives such as
the ВЛ10к, not on wagons — PROBABLE.

## 5. Driver's valve 394/395

The handle sets the pressure in the **УР** (уравнительный резервуар,
equalising reservoir). A relay (equalising piston) makes the pipe follow the
УР: it feeds from the main reservoir or vents — CONFIRMED (1, 2).

| Position | Action | Confidence |
|---|---|---|
| I (зарядка и отпуск) | pipe from the main reservoir through wide channels. УР through a 1.6 mm hole, narrowed so the handle can be held longer. The УР pressure selects what the pipe will hold after II | CONFIRMED (1, 2) |
| II (поездное) | the reducer holds the charging pressure in the УР. The stabiliser removes an overcharge from the УР at a constant rate (0.45 mm hole) | CONFIRMED (1, 2) |
| Overcharge removal rate | 6.0 → 5.8 atm in 80–120 s (≈0.1–0.15 atm/min) | CONFIRMED (9, two teaching sources agree) |
| III | lap without feed | CONFIRMED (2) |
| IV | lap with feed: leaks are made up to the УР pressure | CONFIRMED (2) |
| V | service: the УР vents through a 2.3 mm hole, 0.2–0.25 atm/s | CONFIRMED (1, 2) |
| VA | slow service for long trains: 0.5 atm in 15–20 s | CONFIRMED (1, 2) |
| VI | emergency: pipe to atmosphere through a wide channel | CONFIRMED (1, 2), the time is UNKNOWN |
| Charging pressure | freight 4.8–5.0 (empty) and 5.3–5.5 (loaded) under the 1994 instruction. EMUs 4.5–4.8 | CONFIRMED (10, 11), current rules may differ |
| First service step | freight: УР −0.6–0.7. Passenger: 0.3–0.5 (1994), 0.3–0.6 (current) | CONFIRMED (10, 12) |
| Overcharge level in practice | test: УР to 6.5–6.8, then II. In driving, depends on how long I is held | CONFIRMED for the test (9); for driving UNKNOWN |

**Inference (PROBABLE):** the stabiliser rate (0.1–0.15 atm/min) is below the
stability of both distributors (483: 0.3, 292: 0.5 atm/min). This is why an
overcharge is removed without applying the brakes. It is a design property of
the valve and the distributors together, not of the distributor alone.

## 6. The train pipe

- An air pressure wave travels at about 330 m/s. The brake wave is slower,
  and quick-service in each distributor sustains it — CONFIRMED (7).
- Release wave: 50–70 m/s. It depends on the main reservoir, leaks, pipe
  resistance and reservoir recharge — CONFIRMED (7).
- Leaks slow the drop towards the tail. The pressure gradient along a train
  depends on them — CONFIRMED qualitatively (7). **No numeric gradient or
  equation found — UNKNOWN.**
- Temperature: −1 m/s of brake wave per °C below 0. Charging pressure:
  −8 m/s per 1 atm less — CONFIRMED (7).
- Passenger pipe leak limit: 0.2 atm/min or 0.5 atm in 2.5 min — PROBABLE
  (unverified summary, 8).

## 7. The openRTS stopgap against this (removed)

The `distributor_stability` setting measured here was removed; openRTS keeps
the RTS triple valve and drives it with the ВЛ10к DLL's 395 valve
(`docs/train.md`).

Measured: a single-step application at the head, then lap for 60 s, cylinder
of car 1 / last car. ВЛ10к + 86 tank cars (89 cars):

| Pipe step | RTS | stability on |
|---|---|---|
| 0.2 | 0.00 / 0.00 | 0.00 / 0.00 |
| 0.3 | 0.06 / 0.06 | 0.14 / 0.09 |
| 0.5 | 0.56 / 0.56 | 0.65 / 0.60 |
| 0.7 | 1.06 / 1.06 | 1.14 / 1.09 |
| 1.5 | 3.06 / 3.06 | 3.14 / 3.09 |

- **RTS dead band:** in RTS the reference is the ЗР (`K = A`), which charges
  only to `p − 0.2`. So the first 0.2 atm of every step brakes nothing.
  Stability (`K = p`, threshold 0.15) does not make small steps worse: it is
  0.05 more sensitive. But both are far below the real response: ВР 292 gives
  about 3× the drop from the first step plus quick-service. The real minimum
  steps are 0.3–0.7.
- **0.5 atm/min** (stability): the ВР 292 value. ВР 483 has 0.3. One value
  for all types is not justified.
- **0.15 atm threshold:** close to the confirmed main piston start of ВР 483
  (0.015 MPa). But the current model then drops these 0.15 atm from the
  cylinder. A real device does the opposite: quick-service adds a local drop
  and the cylinder jumps. The value is plausible; the way it is used is not.
- **Overcharge +0.8 atm, latched:** the real overcharge is not a constant. It
  is how long the driver holds I (the УР fills through 1.6 mm). The 5.8 it
  gives is within the practice range, but as a fixed step it is a stand-in.
- **Removal 0.25 atm/min** (keyboard valve, `distributor_stability_rate / 2`):
  faster than the real stabiliser (0.1–0.15 atm/min). It is under 483's 0.3
  only by a small margin. It should be 0.1–0.15 atm/min on the УР, and the
  pipe follows the УР.
- **Graduated release everywhere:** wrong for ВР 292 (full release on
  +0.1–0.15) and for ВР 483 in flat mode. It is right only for 483 mountain
  mode and for ЭПТ.
- **No quick-service, no load or length modes, no emergency accelerator.**
  The pipe is the RTS one (feed attenuated by `min(1, 30/L)`), not a flow
  model. Its tail delays and wave speeds are not checked.
- **No self-braking in tests does not prove the model.** A model with no
  application at all would pass it too. The service-step table above is the
  check that must pass as well.

## 8. What RTS itself does

**rts1.exe:** only the generic triple valve of `docs/reverse/brakes.md`
(`K = A`, a 0.2 atm dead band, graduated release, no stability, no
quick-service) — CONFIRMED (IDA). Its strings have no driver's valve, no
equalising reservoir logic and no distributor types. The only hook is
`Engine::UR`, a field the DLL writes.

**The driver's valve lives in the locomotive DLLs.** They drive the pipe
through `Engine::TrainPipeRate` and keep the УР in `Engine::UR`. There are two
kinds in the installation.

**DLLs with sources** (CONFIRMED, read):
- **`tsrLoco_VL10k-1461/cab/vl10k_main.cpp`, crane 395** (cab switch 0; no VA
  position). In every position, the pipe follows the УР.

  | Position | What the code does |
  |---|---|
  | I (held) | УР +0.25 atm/s, up to the main reservoir. Pipe rate `(УР − p)/1.8`, charging only |
  | II | УР up to 5.0 at ≤ 0.25 atm/s. Above the pipe, **УР −0.003 atm/s (0.18 atm/min, the stabiliser)**. While the loco's brake cylinder is not empty and `УР − p < 0.1`, **УР +0.15 atm/s**: an automatic overcharge that pushes the release through the RTS triple valve. Pipe charges at `(УР − p)/1.8` and vents down to the УР at ≤ 0.5 atm/s |
  | III | vents down to the УР. Otherwise a constant **−0.005 atm/s** (leaks without feed) |
  | IV | vents down to the УР. Feeds +0.05 atm/s when the УР is more than 0.1 above the pipe |
  | V | **УР −0.2 atm/s** (matches the real 0.2–0.25), pipe −0.25 atm/s |
  | VI | УР −1.44 atm/s, pipe −1.2 atm/s. The ЭПК (vigilance) gives the same |

- **`tedStepanki_emu/CAB/er9t_main.cpp`, EMU ЭР9Т:**
  - I: УР +0.25 atm/s up to the main reservoir, pipe `(УР − p)·0.4`;
  - II: УР up to 4.5 only, no overcharge removal;
  - ЭПТ: a cylinder target in `var[5]`, applied at 0.5 atm/s and released
    at 0.5 or 0.8 atm/s, through `Engine::EPTvalue`;
  - the charging (second) pipe is held at 5.0.
- ЧС4з, ТЭП70, ВЛ80к have their own variants (not read in detail).

**`mdd_Libraries/Brakes/Brakes.dll`** (no sources, 245 KB, x86) is a
third-party add-on library by MaDaDev, not part of RTS. It is an RTS
AuxLibrary on the same `ts.h` interface: it writes `Engine::UR`,
`TrainPipeRate` and `EPTvalue` like any locomotive DLL. About **2000 `.eng` files** in the installation load it.
Parameters come from `AuxLibraryParameters`:
- `BR_395ID1`: the 395 crane cab control;
- `BR_254ID1`: the independent brake valve 254;
- `BR_367ID1`: блокировка 367;
- `BR_EPT`, `BR_EPTAPL1`, `BR_EPTREL1`: the ЭПТ;
- `BR_ZARPRESS`: charging pressure, e.g. 4.6 on the ЭД2Т;
- `BR_MODE`, `BR_RAZOBSHK`;
- manometers, lamps, ЭПК, САУТ, КЛУБ.

It tells nothing about RTS itself; it is one more DLL valve, tuned against
the RTS triple valve like the others. Reversing it is optional (only to
match MaDaDev stock closely); its numbers are UNKNOWN.

**Consequence for openRTS (PROBABLE, important):** these DLLs are tuned
against the RTS triple valve. The ВЛ10к, for one, overcharges by itself in II
to get a release through the RTS valve. When the legacy DLL host runs
(`docs/train-dll.md`), the wagons must keep the RTS valve for such a
locomotive. Otherwise the DLL's crane and a realistic distributor fight each
other. A realistic distributor model only makes sense together with an
openRTS driver's valve (keyboard controller or openRTS plugin), or with a
DLL known to be written for it.

## Sources

1. Краны машиниста 394 и 395 — Энциклопедия нашего транспорта (after Крылов
   В. И. и др., «Приборы управления тормозами», 1982):
   https://wiki.nashtransport.ru/wiki/Краны_машиниста_394_и_395
2. Поездной кран машиниста усл. № 395:
   https://oltep.narod.ru/hepl_torm/4.2/4.2.htm
3. Асадченко В. Р., «Автоматические тормоза подвижного состава», ВР 483М:
   https://poezdvl.com/books/asadchenko/asadchenko_50.html
4. Воздухораспределитель усл. № 292-00 — ЧИПС:
   https://tpschips.ru/readarticle.php?article_id=46
5. Электровоздухораспределитель усл. № 305 — ЧИПС:
   https://tpschips.ru/articles/elektrovozduxoraspredelitel-usl-305
6. «Правда о железнодорожных тормозах: часть 6 — приборы торможения грузового
   типа», Хабр: https://habr.com/ru/articles/812223/
7. Тормозные процессы. Тормозная и отпускная волна:
   https://poznayka.org/s98229t1.html
8. Учебные материалы по тормозам (ВР 292, 483, ЭПТ):
   https://studfile.net/preview/14227258/page:23/ ,
   https://studfile.net/preview/14227258/page:25/ ,
   https://studfile.net/preview/14227258/page:27/
9. Проверка темпа ликвидации сверхзарядного давления:
   https://lektsii.com/2-15611.html ,
   https://studfile.net/preview/11240662/page:26/
10. Инструкция по эксплуатации тормозов ЦТ-ЦВ-ЦЛ-ВНИИЖТ/277 (1994):
    https://meganorm.ru/Data2/1/4293816/4293816648.htm
11. Обслуживание тормозного оборудования:
    https://lokomo.ru/podvizhnoy-sostav/obsluzhivanie-tormoznogo-oborudovaniya.html
12. Новые правила по тормозам (протокол № 83):
    https://prolokomotiv.ru/pravila-po-tormozam-protokol-83.html

Unreachable during the research: scbist.com (Асадченко ch. 21–23, ЭР2),
rcit.su. Most sources are teaching material, not the manufacturer's
specifications or the current rules. The numbers should be checked against
the current «Правила технического обслуживания тормозного оборудования» and
the device specifications when they are available.
