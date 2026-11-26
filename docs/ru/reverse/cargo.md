# Грузы и погрузка (RTS)

Что есть в RTS для грузов, пунктов погрузки и самой погрузки и что из
этого берёт openRTS. Источники: справка RTS по файлам
(`RTS\RTSFileReference.html`, идёт с игрой), строки и таблицы `rts1.exe`,
файлы RTS 0.44.1 и маршруты нынешней сборки, а также рассказы тестеров о
самой игре.

## Файлы

| Файл | Где | Что |
|---|---|---|
| `RTSCargo.dat` | `TRAINS\TRAINSET\DEFAULT` (в RTS 0.44.1 ещё `RTS\SD`) | `CargoTypes` и `WagonTypes` |
| `RTSCargoCustom.dat` | `TRAINS\TRAINSET\DEFAULT` | то же, свой файл пользователя (читается после) |
| `RTSObjects.dat` | `TRAINS\OBJECTS` и папка маршрута | `CargoObjects` и `ServiceObjects` |
| `<модель>.sd` | рядом с моделью | `ESD_Pickup_Object`: модель — пункт погрузки |
| `.wag` / состав | вагон | `WagonCargoDescription`, `FreightAnim` |
| `SOUND\pickup.sms` | | звуки пунктов погрузки |
| `GLOBAL\TEXTURES\RTSCargo_<груз><n>.ace` | | текстура насыпного груза |

Все — текстовые файлы SIMISA (`JINX0D0t` у `RTSCargo.dat`, `JINX0C3t` у
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

- `Cargo`: ID насыпного груза, название, плотность, текстура и триггеры
  звука его ссыпания. В файле сборки есть COAL, GRAIN, FERTILIZER, SAND и
  COKE.
- `WagonType`: погрузка вагонов, чьи имена файлов подходят: по маске
  (`*` — что угодно) или по полному имени. Поля те же, что у
  `WagonCargoDescription` самого вагона:
  - `LoadingType` (`CargoLoadingType`, ниже), сколько нужно;
  - `IntakePoints`: `IntakePoint ( X Y Z Tolerance )` — где вагон
    наполняют (в координатах вагона), `DumpPoint ( X Y Z Width )` — где
    опорожняют;
  - `LoadingAreas ( n LoadingArea ( ... ) )`: `BoundingBox` — где лежит
    груз, `ParentNode` (подобъект модели, в который грузят), `AreaPivot`,
    `DumpFromSide` (`BOTTOM`, `TOP`, `LEFT`, `RIGHT`, `FRONT`, `BACK`),
    свой `LoadingType`, `Invisible ( b )` (крытый вагон наполняется, груз
    не виден);
  - `LoadingAnimation`: `LoadingUnloading`, `LoadingSide`, `ForAreas`,
    `ForIntakePoints`, `Nodes`, `ForLoadingType`, `AnimFrame`, `AnimTime`:
    открытие люков, опрокидывание думпкара (анимация модели, иначе
    сделанная по `AnimTime`);
  - `Capacity`, `MaximumWeight`, `DumpSpeed`, `DumpMaxVelocity`,
    `UseBoundingBoxAsArea`.

### CargoLoadingType

| Значение | Что значит в RTS |
|---|---|
| `BOX` | коробки, грузятся вручную |
| `CONTAINER` | контейнер, только краном, нужна область |
| `DUMP` | отвал |
| `ENERGY` | зарядка от кабеля |
| `FRIDGE` | возится только в рефрижераторах |
| `GAS` | газ |
| `LIQUID` | жидкость |
| `PILE` | насыпной, нужна область или точки погрузки |
| `STACK` | куча, грузится вручную или краном |

### Встроенные виды грузов

`rts1.exe` знает 19: `coal`, `grain`, `gravel`, `slate`, `iron_ore`,
`fertilizer`, `cement`, `sand`, `oil`, `mazut`, `chemical`, `gasoline`,
`alcohol`, `water`, `gas`, `mail`, `food`, `luggage`, `message`; в справке
ещё `DIESEL`, `ELECTRICITY` и `ORE`. Текстура насыпного груза —
`GLOBAL\TEXTURES\RTSCargo_<груз><n>.ace` (`RTSCargo_` и `%i.ace` в
`rts1.exe`; `n` с 1: в RTS 0.44.1 лежат `RTSCargo_coal1`, `_grain1`,
`_gravel1`, `_sand1`).

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

Грузовой объект — то, что грузят на вагон штучно: контейнер, грузовик,
рельсы, ящики, мешок муки (нужен `Size` или `Volume`). Модели лежат в
`TRAINS\OBJECTS\CARGO`.

### ESD_Pickup_Object

Модель, у которой в `.sd` он есть, — пункт погрузки, где бы она ни стояла:

```
ESD_Pickup_Object (
    PickupType ( PickupTypeID f )
    SpeedRange ( MinSpeed MaxSpeed )
    PickupAnimData ( AnimType AnimTime )          из MSTS, не используется
    PickupCapacity ( MaxCapacity PickupSpeed )     у розетки: ВА и вольты
    CargoType ( CargoType )
    PickupLength ( f )                             длина платформы
    PlacementMargin ( f )                          допуск на пути, м
    PickupPoints ( n { X Y Z } ... )               привязка к пути
    PickupArea ( X1 Y1 Z1 X2 Y2 Z2 )               крану обязательна
    CargoTable ( Item ( CargoLoadingType CargoType CargoObjectID NumItems Mass ) )   -1: без ограничения
    PickupCraneType ( 1 мостовой | 2 стреловой | 3 платформа )
    PickupCraneOriented ( 0 Z | 1 X | 2 -Z | 3 -X )
    PickupCraneShapeNodes ( Bridge ( n ) Cabin ( n ) Winch ( n ) Hook ( n ) HookInitialHeight ( f ) )
)
```

Платформа грузит вагон в пределах `PickupLength` от точки погрузки или в
своей области.

| PickupType | Что |
|---|---|
| 1 | груз, зерно, насыпная погрузка |
| 2 | груз, уголь, насыпная погрузка |
| 3 | груз, гравий, насыпная погрузка |
| 4 | груз, песок, насыпная погрузка |
| 5 | топливо, вода: сливно-наливная эстакада |
| 6 | топливо, уголь, насыпная погрузка |
| 7 | топливо, дизель: сливно-наливная эстакада |
| 8 | платформа для разгрузки крытых вагонов |
| 10 | песочница |
| 11 | розетка / шланг |
| 12 | погрузочный кран |
| 13 | разгрузочная эстакада для сыпучих |
| 14 | кран, грузящий сыпучие |
| 15 | платформа для разгрузки крытых вагонов |
| 16 | сливно-наливная эстакада |

### В других местах

- Состав: `FreightAnim` с `FreightAnimCargoID`, `FreightAnimCargoType`,
  `CargoObjectID`, `ServiceObjectID`, `FreightAnimRemove` (openRTS его
  читает: `ConsistFreight`, масса вагона, окно груза).
- Сценарий: условие `PickupEvent` (`Location`, `Radius`, `WagonList`,
  `CargoMassLoad`, `CargoMassUnload`, `CargoType`, `PickupTask`,
  `TrainID`); `CargoMass` группы вагонов.
- Звуки: `pickup.sms`, переменная 1 — загрузка в процентах; триггеры 1/2 —
  дизельный насос готов / выключен, 11/12 — песок, 21/22 — уголь, 31/32 —
  жидкость, 35/36 — газ, 101 — куча начинает сыпаться (и звуки
  `Ted_cargo_*`).
- Сетевая игра: `0x461C` — груз на вагоне (`reverse/multiplayer-protocol.md`).
- Тексты `rts1.exe`: "Load cargo", "Unloading cargo", "Cargo manifest",
  "Pickup type: %s    Current cargo: %s", "Cargo: %s Amount: UNLIMITED",
  "Failed to place cargo in the selected area", "Cargo: %0.02ft, used
  %0.02f of %0.02f volume", "Show cargo loaders".

## Как грузит RTS (rts1.exe)

Статический разбор (IDA), подтверждено, если не сказано иное:
- `0x407588` грузит `RTSCargo.dat`, затем `RTSCargoCustom.dat` (в нижнем
  регистре) через `0x4D77E4`, который читает **только блоки `WagonType`**
  (`0x4D8164`): `CargoTypes` не читается. Виды грузов, плотности и
  текстуры зашиты в игру.
- `0x45B930` переводит имя груза в код; `0x50CD6C` даёт плотность
  (кг/м³); `0x50D1F0` — форму кучи; `0x50D298` — текстуру
  `RTSCargo_<имя><n>.ace`:

  | Код | Имя | кг/м³ | Куча |
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

- Место в вагоне (`0x501D80`): с областями погрузки — их суммарный
  объём, иначе `Capacity` × ящик `Size` вагона; масса груза ограничена
  `MaximumWeight` × этот ящик (`0x5020E0`: кг на м³ ящика; у хоппера
  370). Грузы вагона — записи (груз, область, объём, масса); масса вагона
  растёт на объём × плотность.
- **Num3 на пункте погрузки** (`0x4121BC` → `0x4210FC`): состояние идёт по
  кругу выкл → готов → грузит → выкл; Ctrl+Num3 — разгрузка (типы 5, 7,
  8, 13, 16). Пишется "Pickup ready" / "Pickup activated" / "Unloading
  cargo".
- **Каждый кадр, каждый пункт** (`0x4E59A0`, из главного цикла) по
  вагонам в досягаемости (`0x4E4A90`), едущим в пределах `SpeedRange`:
  - 7 (дизель) заправляет тепловоз топливом, 10 — песком, со скоростью
    `PickupSpeed` в секунду до значений `.eng`;
  - 1–4, 6, 10 (насыпь): вагон, чья точка загрузки под пунктом
    (`0x4E974C`, в пределах допуска точки) и не полон, наполняется со
    скоростью `PickupSpeed` м³/с; запас (`CargoTable`, иначе бесконечный)
    убывает на объём × плотность; над вагоном висит объект падающего
    груза (тот самый круг) и растёт куча; погрузка кончается, когда вагон
    полон (состояние 3) или кончился запас (4); пока сыплется — звук 101;
  - 5, 7, 16 (жидкости): наполняет в процентах, Ctrl+Num3 — слив;
  - 13 (эстакада для сыпучих): опорожняет вагон со скоростью его
    `DumpSpeed` (иначе пункта), только ниже `DumpMaxVelocity`, с
    анимацией разгрузки вагона;
  - 12, 14, 15 (краны, платформа): когда пункт активен и вагон стоит в
    области, открывается **окно «Load cargo»** (ресурс 113, `0x53641C`).
    Какие виды погрузки оно даёт: 12 — контейнер, куча, коробки; 14 —
    насыпь, куча; 15 — куча, коробки, рефрижераторный.
- **Окно**: Wagon (вагоны в досягаемости), Area (все, «Internal area»,
  «Bounding box», Area 1..n), Placement (четыре способа; четвёртый —
  вручную, только один предмет), Center, Cargo Type (все, насыпь,
  жидкость, контейнер, куча, коробки, рефрижераторный), список грузов (из
  `CargoTable` пункта: «имя [т]»), Weight, Amount, Loaded cargo («имя X n
  [т]»), Load, Unload, Unload all, Cancel. Load ставит предметы по одному
  (`0x4E8890`); если вагон тяжелее предела — «слишком тяжело», если
  предмет не влезает — «Failed to place cargo in the selected area».

## На маршрутах

В сборке 348 моделей с `ESD_Pickup_Object`: Павловск 140, Чернышевск 94,
Новокузнецк 54, Степи Приазовья 36, Кубань 22, TestRoute 2. По типам:
12 (кран) 90, 13 (эстакада для сыпучих) 50, 7 — 40, 2 — 32, 5 — 28,
1 — 20, 4 — 20, 8 — 20, 3 — 16, 11 — 12, 15 — 12, 10 — 4, 16 — 4.
`CargoTable` в 4 (почтовые контейнеры, без ограничения);
`PickupCraneShapeNodes` — ни в одной: ни один кран не анимирован.

## В игре (по словам тестеров)

- Насыпь: над вагоном в воздухе появляется круг, из него сыплется груз.
- Краны (с RTS 0.43): поезд стоит в области крана; Num3 открывает меню:
  вагоны в области, способ расстановки груза, вагон, где на нём (справа,
  посередине, слева) и груз. Список один на всё: на платформу можно
  погрузить груз для крытых вагонов, но не влезающий груз не грузится.
- Запас у пункта погрузки бесконечный.

## openRTS

Сделано 2026-11-25..26 (`cargo.md`): этапы 1–6 (с топливом и песком,
звуками, сетью, опусканием и узлами кранов, LoadingAnimation вагонов);
осталось: этап 7.

План (запрос 2026-11-25-1: двенадцать своих насыпных грузов openRTS и сама
погрузка, с анимацией):

1. Форматы: `RTSCargo.dat` + `RTSCargoCustom.dat` + свой файл openRTS (в
   моде), `RTSObjects.dat` (общий и маршрута), `ESD_Pickup_Object`,
   `WagonCargoDescription` и `WagonTypes`, подобранные к вагону.
2. Симуляция (`simulation/`, чтобы её вёл сервер): груз вагона (вид,
   масса, объём, где лежит), пункты погрузки в мире и вагон в их
   досягаемости, погрузка и разгрузка со скоростью `PickupSpeed`, масса
   вагона, `0x461C`.
3. Вид: куча в области вагона (высота по объёму, текстура груза),
   грузовые объекты по местам и в штабелях, люки и опрокидывание
   (`LoadingAnimation`).
4. Меню (Num3) как в RTS, сначала по-русски.
5. Своё openRTS: анимированные погрузчики (струя из бункера, мост,
   тележка, лебёдка и крюк крана, если модель их называет), звуки.
6. Свои грузы openRTS: ASH, CLAY, GYPSUM, SHALE, GLASSSHARDS, WOODCHIPS,
   SALT, PORODA, COKE (у RTS есть свой), SLAG, CASTIRON, SINTER; текстуры
   `oRTSCargo_<груз><n>.ace`, ищутся так же, как RTS ищет `RTSCargo_`.
   Плотности от тестеров, кг/м³: зола 1000, глина 1500, гипс 2300,
   сланец 2700, стеклобой 1500, щепа 400, соль 2100, рудная порода 3000,
   кокс 450, шлак 750, чугун 7200, агломерат 2300.
7. Индустрии с переработкой (своё openRTS, как в Trainz; пункты погрузки
   RTS остаются бесконечными): у индустрии запас каждого груза, который
   она принимает и выдаёт; привезённое она потребляет и выпускает
   продукты в заданных пропорциях с заданной периодичностью (нефть
   1000 л на входе, раз в заданное время: 450 л бензина, 400 л ДТ, 150 л
   керосина на выходе); пункт погрузки грузит из запаса, так что он
   может кончиться. Ведёт сервер, как и поезда.
