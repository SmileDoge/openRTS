# AGENTS.md

## Проект

`openRTS` — независимая реализация/реимплементация RTrainSim (RTS) с упором на совместимость с существующим контентом RTS/MSTS и постепенную замену старого движка современными подсистемами.

Основная цель проекта на текущем этапе: не "переписать всё сразу", а поэтапно восстановить форматы данных, ABI, поведение подсистем и построить новый движок вокруг подтверждённых знаний.

Предполагаемое направление нового рендера: современный DirectX 12 renderer. Не привязывай архитектуру остальных подсистем к DX12 сильнее, чем это необходимо.

---

## Рабочие каталоги

Проект запускается с двумя исходными папками:

- `openRTS/` — **единственная основная папка разработки**. Новый код, документация, инструменты, тесты и build-файлы создаются здесь.
- `RTS/` — установленная оригинальная игра RTrainSim и материалы для анализа.

### Критическое правило: оригинальная RTS не является рабочей директорией

Нельзя без явного запроса пользователя:

- изменять или перезаписывать `rts1.exe`;
- изменять DLL игры;
- переименовывать или переносить файлы игры;
- удалять игровые ресурсы;
- "чистить" директорию RTS;
- запускать build-скрипты с output в директорию RTS;
- автоматически конвертировать оригинальные ресурсы in-place.

Допустимо читать файлы RTS для совместимости и анализа.

IDA analysis files (`.i64`, `.id0`, `.id1`, `.id2`, `.nam`, `.til`) являются отдельными артефактами анализа. Их разрешается изменять **только средствами IDA/IDA MCP и только в рамках задачи по реверсу**. Код проекта и build-система не должны писать в них напрямую.

Если для теста нужно изменить/преобразовать игровой файл, сначала скопируй его в `openRTS/testdata/`, `openRTS/scratch/` или другой каталог внутри `openRTS`.

---

## Документация RTS

Официальные/проектные материалы RTS будут находиться в:

```text
docs/RTS/
```

Ожидаемые файлы:

```text
docs/RTS/
    RTSFileFormat.dat
    RTSFileReference.html
    export.pdf
    ext.pdf
```

Также в установленной RTS может находиться `ts.h`, описывающий интерфейс DLL локомотивов. Документация указывает типичный путь:

```text
TRAINS/TRAINSET/tsrLoco_VL80k-125/CAB/ts.h
```

Не копируй `ts.h` и не создавай свою версию "по памяти", если можно прочитать оригинальный файл из установки RTS. Если проект позднее решит vendoring/снимок ABI делать намеренно, это должно быть отдельным осознанным изменением.

---

## Приоритет источников

При исследовании поведения RTS придерживайся следующего порядка.

### 1. Документация RTS

Сначала проверь `docs/RTS/`.

В частности:

- `RTSFileFormat.dat` — декларативное описание форматов/узлов/параметров;
- `RTSFileReference.html` — справочник структур файлов RTS;
- `export.pdf` — руководство по моделям кабин, анимациям и DLL локомотивов;
- `ext.pdf` — расширенные параметры дополнений и API RTS.

Если поведение или формат уже документированы, **не реверси их заново без причины**.

### 2. Форматы и поведение MSTS

RTS построен поверх экосистемы Microsoft Train Simulator и использует значительный объём MSTS-контента и форматов.

Open Rails может использоваться как технический справочник по MSTS:

- форматы `.s`, `.sd`, `.eng`, `.wag`, `.con`, `.act` и другие;
- загрузка маршрутов;
- модели путей и world-данные;
- MSTS-совместимые структуры и семантика.

**Важно:** Open Rails распространяется под GPL. Не копируй нетривиальный код Open Rails в `openRTS`, если лицензирование проекта явно не позволяет это и пользователь отдельно не попросил. Используй Open Rails прежде всего как reference implementation для понимания форматов и поведения, затем пиши независимую реализацию.

### 3. IDA / IDA MCP

Используй IDA для:

- недокументированных расширений RTS;
- конкретного runtime behavior;
- внутренних структур движка;
- renderer internals;
- связи parser -> runtime structures;
- вызовов DLL;
- edge cases, отсутствующих в документации;
- проверки предположений.

IDA должна дополнять документацию, а не заменять её.

### 4. Runtime verification

Если документация, Open Rails и IDA расходятся, пометь расхождение и проверь поведение на реальной RTS минимальным воспроизводимым тестом.

Не "разрешай" противоречие догадкой.

---

## Уровни уверенности

При реверсе не выдавай гипотезы за факты.

Используй три уровня:

```text
CONFIRMED   — прямо следует из документации, символов, ts.h,
              нескольких независимых xref/use sites или runtime test.

PROBABLE    — хорошо поддерживается структурой данных/вызовами,
              но остаётся хотя бы одно разумное альтернативное объяснение.

UNKNOWN     — назначение неизвестно либо основано на слабой догадке.
```

Для восстановленных структур желательно документировать:

```text
offset
size
type
candidate name
confidence
evidence
```

Пример:

```text
+0x18  u32  vertex_count   CONFIRMED
       Evidence:
       - multiplied by 48 before glBufferData
       - same field bounds vertex iteration elsewhere
```

Не переименовывай сотни функций/полей в IDA на основании одной догадки.

Хороший порядок:

```text
unknown
  -> hypothesis
  -> callers/callees
  -> xrefs
  -> data-flow / field uses
  -> documentation comparison
  -> runtime check when needed
  -> confirmed rename/type/comment
```

---

## Что уже известно о RTS

Ниже перечислены исходные рабочие знания. Они полезны как точки старта, но всё, что помечено как inference, необходимо подтверждать перед тем, как делать его жёстким ABI/format contract.

### Связь с MSTS

RTS сохраняет совместимость с существенной частью контента MSTS.

Документация прямо указывает, что внешняя модель локомотива может использоваться из MSTS без переделки. RTS добавляет собственные кабины/салоны, расширенные SD/ENG/WAG-параметры, DLL логику локомотивов, дополнительные эффекты, события и прочие расширения.

### Форматы, описанные в RTSFileReference

В справочнике присутствуют как минимум:

- Activity;
- Consist;
- Engine;
- Wagon;
- Shape description;
- Shape repaint/modifications;
- RTS Cargo Description;
- RTS Objects / Cargo / Special;
- RTS Terrain effects.

Есть большой набор condition/outcome узлов сценариев и многочисленные расширения RTS.

### Расширения моделей и окружения

Документация описывает среди прочего:

- интерактивные двери и переходы;
- кабины и салоны;
- анимированные подобъекты;
- приборы;
- свет;
- расширенную анимацию;
- альтернативные текстуры;
- bounding volumes;
- скрытие внутренней/внешней геометрии;
- траву;
- снег;
- лужи;
- укрытие объектов от дождя;
- дополнительные terrain effects.

Поэтому `.sd` и RTS-расширения моделей являются важной частью совместимости, а не декоративным дополнением.

---

## RTSFileFormat.dat

`RTSFileFormat.dat` следует рассматривать как потенциальный машиночитаемый источник схемы форматов RTS.

Перед тем как писать отдельные ad-hoc parsers, исследуй возможность построить:

```text
RTSFileFormat.dat
        |
        v
schema loader
        |
        +--> generic AST / parse tree
        |
        +--> validator
        |
        +--> typed wrappers
        |
        +--> generated enums / metadata (если это действительно удобно)
```

Не генерируй огромные типизированные классы автоматически только потому, что можешь. Сначала проверь, насколько схема стабильна и насколько удобно сохранить неизвестные/новые узлы.

Предпочтительно уметь round-trip/сохранять неизвестные узлы, если это не усложняет первую реализацию чрезмерно.

---

## DLL API локомотивов

RTS использует Win32 DLL-модули для логики локомотивов.

Документация указывает:

- DLL создаётся Win32-компилятором;
- в качестве подходящего инструмента упоминается C++ Builder;
- требуется 4-byte / Double Word alignment;
- интерфейс описывается в `ts.h`;
- управление идёт через объекты `Locomotive`, `Engine`, `Cabin` и производные классы;
- библиотека не должна самостоятельно владеть/освобождать переданными ей объектами движка.

Основные функции включают:

```text
Init
ChangeLoco
LostMaster
CanWorkWith
CanSwitch
Switched
Run
ALSN
SpeedLimit
```

`Init`, `ChangeLoco`, `CanWorkWith` и `Run` описаны как обязательные для основного управляющего модуля.

RTS ищет функции DLL по экспортируемым именам; старый Win32 ABI и decoration имён имеют значение.

### Семантика

DLL отвечает в том числе за:

- инициализацию локомотива;
- обработку переключателей;
- внутреннюю логику систем;
- состояние оборудования;
- тяговую силу;
- динамическое/электрическое торможение;
- тормозные магистрали;
- песок;
- приборы;
- звуковые триггеры;
- взаимодействие секций/локомотивов по СМЕ;
- устройства безопасности и локомотивную сигнализацию.

В `Engine::var` существует стек/массив пользовательских переменных, размер которого задаётся `VariableStack` в ENG.

Есть также auxiliary DLL/plugins, которые могут реализовывать отдельные системы: устройства безопасности, автоинформаторы, особенности конкретных машин и т.п.

### Важное архитектурное ограничение

Существующие RTS DLL являются **Win32/32-bit**.

Если `openRTS` будет 64-bit, он не сможет просто загрузить старые 32-bit DLL в тот же процесс.

Не принимай решение молча. Возможные варианты:

1. собирать совместимый 32-bit `openRTS`;
2. сделать отдельный 32-bit plugin host и IPC к 64-bit движку;
3. не поддерживать бинарные DLL напрямую и реализовать другой compatibility layer.

До явного решения пользователя архитектура должна оставлять этот вопрос открытым.

---

## Ключевые runtime-классы из документации

Документация описывает, среди прочего:

```text
Locomotive
ElectricLocomotive
DieselLocomotive

Engine
ElectricEngine
DieselEngine

Cabin
SignalsInfo
SpeedLimitDescr
TrackItemsItem
AuxLibrary
```

Из `Locomotive` доступны такие концепции, как:

```text
PantoRaised
Flags
Velocity
Acceleration
IndependentBrakePressure
AuxiliaryPressure
BrakeCylinderPressure
TrainPipePressure
ChargingPipePressure
LocoFlags
MainResPressure
SandLeft
Slaves
NumSlaves
NumEngines
```

Из `Engine`:

```text
Cabin* cab
float* var

Power
Force
BrakeForce

EngineVoltage
EngineCurrent
EngineForce

IndependentBrakeValue
TrainPipeRate
AuxiliaryRate
MainResRate
ChargingRate
HandbrakePercent
EPTValue
DynamicBrakePercent

EngineFlags
Reverse
Wheelslip
ThrottlePosition
UR
BatteryCharge
CurrentMilepost
```

Используй оригинальный `ts.h` как авторитетный источник layout/prototypes, когда он доступен.

---

## Renderer: текущие находки из IDA

В `rts1.exe` был исследован renderer function, ранее условно названный `draw_elements_1`.

Текущие наблюдения:

### OpenGL

Оригинальный RTS использует OpenGL.

Присутствуют два пути:

1. shader path через `shape_prog`, `glUseProgram`, uniforms и vertex attributes;
2. legacy fixed-function path через `glVertexPointer`, `glNormalPointer`, `glTexCoordPointer`, `glColorPointer`.

Также присутствует опциональный VBO path.

### Vertex layout

Сильная рабочая гипотеза/частично подтверждённый layout:

```cpp
struct Vertex
{
    float position[3];    // 0x00
    float normal[3];      // 0x0C
    float uv0[2];         // 0x18

    // 0x20..0x27: UNKNOWN, определить отдельно

    uint8_t attrib3[4];   // 0x28
    uint8_t attrib4[4];   // 0x2C
}; // stride 0x30 / 48 bytes
```

Основание:

```text
attribute 0: 3 x float @ +0
attribute 1: 3 x float @ +12
attribute 2: 2 x float @ +24
attribute 3: 4 x u8    @ +40
attribute 4: 4 x u8    @ +44
stride: 48 bytes
```

Legacy path подтверждает position/normal/UV и один из color-like attributes.

**Не придумывай назначение байтов 0x20..0x27 без дополнительного evidence.**

### Indices

Наблюдаемое поведение:

```text
primitive topology: triangles
index type: uint32
index_count = triangle_count * 3
```

### GPU upload

Vertex/index buffers создаются лениво при первом использовании:

```text
CPU data
   -> glGenBuffers
   -> glBindBuffer
   -> glBufferData
```

Это важно при сравнении lifetime и ownership старого и нового renderer.

### Shader data

В shader path используются uniform blocks / UBO (`GL_UNIFORM_BUFFER`) и несколько bindings.

Не переносить старую state-machine архитектуру OpenGL буквально в DX12. Сначала восстановить семантику данных, затем спроектировать нормальный DX12 resource/PSO model.

---

## Правила реверса renderer

Для каждой крупной функции renderer старайся ответить на вопросы:

```text
Who owns this object?
Who allocates it?
Who initializes each field?
Who destroys it?
Is the CPU representation persistent?
When is the GPU resource created?
Can it be invalidated/rebuilt?
Which render pass uses it?
Which file format fields populate it?
```

Особенно полезные цели:

```text
Model
Shape
Mesh
MeshChunk
Submesh
Material
Texture
SceneObject
Transform
Animation state
LOD
Render pass
Shader/program state
```

Связывай IDA-структуры с полями `.s/.sd/.eng/.wag` там, где это возможно.

---

## Рекомендуемая стратегия реализации

### Этап 1: исследовательский фундамент

Сделать:

```text
docs/reverse/
    renderer.md
    formats.md
    dll-abi.md
    structures.md
    functions.md
```

Вести там подтверждённые findings и ссылки на источники/IDA addresses.

### Этап 2: парсер и schema tooling

Первым полезным standalone tool может быть:

```text
tools/rts-schema
```

Задачи:

- загрузить `RTSFileFormat.dat`;
- построить внутреннее представление схемы;
- уметь показать FileTypes/NodeTypes/parameters;
- позднее использовать схему для validation/parser metadata.

### Этап 3: asset tools раньше полной игры

Предпочтительно создавать маленькие инструменты:

```text
tools/shape-viewer
tools/asset-dump
tools/config-dump
tools/route-inspect
```

Вместо отладки каждого формата внутри полноценного игрового цикла.

Хороший milestone:

```text
оригинальный RTS/MSTS shape
    -> наш parser
    -> наш CPU mesh
    -> DX12 upload
    -> отображение оригинальной текстуры
```

### Этап 4: world/map

После стабильного asset layer:

- terrain;
- route/world tiles;
- track;
- scenery;
- placement/transforms;
- LOD;
- animation.

### Этап 5: simulation compatibility

Затем:

- ENG/WAG semantics;
- train consist;
- brakes;
- electric/diesel-specific state;
- cab interactions;
- signalling;
- scenario/activity behavior;
- DLL compatibility.

---

## Проектирование нового кода

Не копируй архитектурные ограничения старого OpenGL renderer, если они нужны были только из-за старого API.

Сохранять нужно:

- формат данных;
- наблюдаемое поведение;
- timing/physics semantics там, где они важны;
- совместимость контента;
- ABI, если мы явно решили его поддерживать.

Необязательно сохранять:

- OpenGL state machine;
- внутренние адреса/offsets оригинального exe;
- lazy GPU upload ровно в той же точке;
- старую систему глобальных переменных;
- старый threading model;
- старые ограничения API, не являющиеся частью observable behavior.

Разделяй:

```text
compatibility layer
simulation/domain
asset formats
renderer
platform
tools
```

---

## Не делай giant rewrite одним заходом

Не создавать "готовый движок" на десятки тысяч строк в одном изменении.

Предпочтительный цикл:

```text
исследование
-> минимальная спецификация
-> небольшой implementation
-> тест на реальном ресурсе RTS
-> сравнение с оригиналом
-> commit
-> следующий слой
```

Каждый крупный шаг должен иметь наблюдаемый результат.

---

## Тестовые данные

По возможности тестируй на реальных файлах RTS/MSTS, но:

- оригиналы не изменять;
- минимальные samples копировать в локальный testdata только если это допустимо;
- не коммитить большие/чужие игровые assets без необходимости и разрешения;
- где можно, создавать маленькие synthetic fixtures.

Парсеры должны иметь unit tests на:

- корректный input;
- неизвестный узел;
- optional node;
- repeated node;
- malformed nesting;
- invalid parameter;
- Unicode;
- legacy text encoding, если формат этого требует.

---

## Работа с IDA MCP

IDA MCP используется как исследовательский инструмент.

Хорошие запросы:

```text
Find all callers/callees of function X.
Trace where object Y is allocated.
Find every access to offsets in structure Z.
Identify code that parses token "LibraryModule".
Trace GetProcAddress calls for locomotive DLL exports.
Find the parser that populates a specific runtime field.
Compare fixed-function and shader rendering paths.
```

Плохие запросы:

```text
Reverse the whole game.
Rename everything.
Convert the entire executable to C++.
```

Перед массовыми изменениями IDB:

- сначала собери evidence;
- переименовывай только устойчиво установленные сущности;
- записывай ключевые findings в `docs/reverse/`, чтобы знания не жили только внутри IDA database.

---

## Полезные точки входа для IDA

Для DLL subsystem:

```text
LoadLibrary
GetProcAddress

"_Init"
"_ChangeLoco"
"_CanWorkWith"
"_CanSwitch"
"_Switched"
"_Run"
"_ALSN"
"_SpeedLimit"
```

Для renderer:

```text
glDrawElements
glVertexPointer
glVertexAttribPointer
glBufferData
glBindBuffer
glUseProgram
glUniform*
glUniformBlockBinding
glBindBufferBase
```

Для файловых форматов:

- искать имена RTS/MSTS nodes;
- расширения файлов;
- строки ошибок;
- magic/header strings (`SIMISA...`);
- функции token parser / lexer.

---

## Как фиксировать reverse-engineering findings

Пример записи:

```markdown
## Function 0x0045ABCD — LoadSomething

Confidence: PROBABLE

Evidence:
- caller A passes parsed Shape node;
- caller B stores return value into SceneObject+0x68;
- function opens `.s` files;
- resulting object later reaches renderer.

Open questions:
- ownership of texture list;
- meaning of flags at +0x24.

Do not rename:
- field +0x24 until another use confirms it.
```

Для structs:

```markdown
## Candidate: MeshChunk

| Offset | Type | Name | Confidence | Evidence |
|---:|---|---|---|---|
| 0x0C | Vertex* | vertices | CONFIRMED | passed to GL_ARRAY_BUFFER upload |
| 0x18 | u32 | vertexCount | CONFIRMED | size = 48 * field |
| 0x20 | u32 | submeshCount | PROBABLE | loop bound over draw records |
| 0x28 | GLuint | vertexBuffer | CONFIRMED | glGenBuffers target |
```

---

## Git и изменения

Перед крупными автоматическими правками:

```text
git status
git diff
```

После логически завершённого шага желательно оставить маленький понятный commit.

Не выполнять автоматически:

```text
git reset --hard
git clean -fdx
```

если пользователь явно этого не попросил.

Не удалять чужие/непонятные изменения только потому, что они мешают текущей задаче.

---

## Имена и стиль

Для восстановленных оригинальных сущностей:

- использовать понятные нейтральные имена;
- не притворяться, что guessed name является оригинальным именем автора;
- если имя не подтверждено, допускается `Candidate*`, `Unknown*`, либо комментарий с hypothesis.

В новом коде избегать переносить IDA-имена (`sub_`, `v72`, `dword_...`) за пределы слоя reverse notes.

Новый implementation должен иметь нормальные domain names.

---

## Что считать успехом

Первый этап проекта успешен не тогда, когда появилось "много кода", а когда мы можем доказуемо выполнить цепочку:

```text
RTS/MSTS asset
    -> parse
    -> typed/validated representation
    -> render/simulate
    -> сравнить результат с оригинальным RTS
```

Главный принцип проекта:

> **Документировать то, что известно; измерять то, что можно проверить; реверсить только то, чего действительно не хватает; не превращать догадки агента в спецификацию.**

## Прочая информация

### RTrainSimPlugins

RTrainSimPlugins-premake/ is a previous reverse-engineering and plugin
development project.

Treat it as secondary evidence and a source of previously discovered
RTS internals.

Code in this repository may contain:
- experimental assumptions;
- hardcoded offsets;
- version-specific behaviour;
- incomplete reverse engineering;
- workarounds for the original RTS.

Before carrying assumptions into openRTS, verify them against:
1. official RTS documentation / ts.h;
2. IDA analysis of the target RTS version;
3. runtime behaviour where necessary.

Do not modify this repository unless explicitly requested.

### Некоторые дополнительные файлы

По пути `D:\Games\MDDRTS\TRAINS\TRAINSET\tsrLoco_VL80k-125\CAB` хранится ts.h и .cpp/.h файлы для поезда