# Документация openRTS на русском

Здесь лежат русские копии документов из `docs/` с той же структурой папок:
`docs/ru/X.md` — перевод `docs/X.md`, `docs/ru/reverse/X.md` — перевод
`docs/reverse/X.md`.

**Основной текст — английский.** При расхождении верен он. Изменение
английского документа вносится и в его русскую копию в той же правке.
Адреса, имена функций, полей и ключевых слов, пути и команды оставлены как
есть; метки уверенности CONFIRMED / PROBABLE / UNKNOWN тоже.

## Подсистемы

| Документ | О чём |
|---|---|
| [roadmap.md](roadmap.md) | план и текущее состояние |
| [train.md](train.md) | поезд: физика, тормоза, клавиатурный машинист, сцепка, кабина |
| [train-systems.md](train-systems.md) | огни, пантографы, песок, двери без DLL; табло и свет кабины; кабели и ремонт сцепки; АЛСН; смена кабины; нативные плагины; ток и пережог провода |
| [train-dll.md](train-dll.md) | что такое DLL локомотива; мост к старым DLL и API нативных плагинов |
| [track.md](track.md) | путевая сеть: геометрия, топология, стрелки |
| [signals.md](signals.md) | сигналы и их скрипты |
| [paths.md](paths.md) | маршруты поездов, бронирование стрелок, трек-контроль |
| [sound.md](sound.md) | звук: скрипты `.sms`, звуки мира, поездов и кабины |
| [electrification.md](electrification.md) | электрификация: секции, подстанции, напряжение |
| [net.md](net.md) | сетевой протокол RTS в openRTS: формат, снимок мира, `openrts-net-dump`, `--mp-snapshot` |
| [world-objects.md](world-objects.md) | леса, переезды, автомобили, вода, осадки и дым |
| [experimental-effects.md](experimental-effects.md) | новый рендер: что делает и сколько стоит |
| [vfs.md](vfs.md) | виртуальная файловая система, паки и моды |

Пошаговое руководство по пакам и модам изначально написано по-русски:
[`docs/vfs-tutorial-ru.md`](../vfs-tutorial-ru.md).

## Разбор RTS (reverse)

| Документ | О чём |
|---|---|
| [runtime-overview.md](reverse/runtime-overview.md) | обзор работы `rts1.exe`: главный цикл, физика поезда, решатель сцепок, сход |
| [train-dll.md](reverse/train-dll.md) | слой RTS ↔ DLL локомотива: вызовы, ведущие и ведомые, кабина, огни, ток в линии, АЛСН |
| [legacy-dll-abi.md](reverse/legacy-dll-abi.md) | ABI старых DLL: загрузка, `InitRTSLibrary`, `DLLVersion`, адаптеры |
| [rolling-stock.md](reverse/rolling-stock.md) | как RTS читает составы, локомотивы и вагоны |
| [brakes.md](reverse/brakes.md) | тормоза в RTS |
| [air-brakes-real.md](reverse/air-brakes-real.md) | настоящие пневматические тормоза 1520 мм (не RTS) |
| [car-menu.md](reverse/car-menu.md) | меню вагона (Num3) |
| [cab-screens.md](reverse/cab-screens.md) | экраны кабины (дисплеи SCREEN) и другие дисплеи |
| [signals.md](reverse/signals.md) | `sigcfg.dat`, `sigscr.dat` и машина сигналов |
| [track.md](reverse/track.md) | путевая сеть в RTS |
| [shape-animation.md](reverse/shape-animation.md) | анимации моделей |
| [shape-rendering.md](reverse/shape-rendering.md) | отрисовка моделей, проходы, освещение |
| [sky.md](reverse/sky.md) | небо и время суток |
| [terrain.md](reverse/terrain.md) | рельеф и выбор окружения |
| [dispatcher.md](reverse/dispatcher.md) | пульт диспетчера, трек-контроль, модуль маршрутов |
| [multiplayer.md](reverse/multiplayer.md) | сетевая игра: клиент, мини-сервер, сервер диспетчера |
| [multiplayer-protocol.md](reverse/multiplayer-protocol.md) | каталог сетевых пакетов: содержимое, направления, обработка |
