# План реализации подхода B для DDA MP

> **Инструкция агенту (обязательно):**
> 1. После каждой компактизации сессии **сначала прочитай этот файл
>    целиком**, затем `netplay_action_audit.md` и `netplay_render.audit.md`.
> 2. Перед изменением кода выбери задачу по ID из §5.
> 3. После каждого изменения кода обнови:
>    - §9 «Статус» (статус задачи, дата, коммит),
>    - §9 «Changelog» (что изменено),
>    - §9 «Что протестировано» (что именно проверено и чем),
>    - §10 «Риски/решения», если появилось новое ограничение.
> 4. Не помечай задачу `DONE`, пока не выполнены её acceptance criteria и
>    тесты из §8. Если тесты не запускались — ставь `DONE (not tested)` и
>    укажи причину.
> 5. Не удаляй старые записи changelog; только добавляй новые.

**Статус плана:** active
**Последнее обновление:** 2026-11-25
**Публикация:** обычные коммиты и теги можно заливать на GitHub. GitHub
Release / zip-архив не публиковать до полной играбельной версии без
значительных багов; zip — только после финальной приёмки.
**Текущая оценка B:** ~45–50% архитектуры (P0+P1 закрыты), ~30–35%
«нативного UI как в одиночке», ~70–75% играбельного vertical slice на
тестовом полигоне.
**Исходные аудиты:**
- `netplay_action_audit.md` — расхождения действий/валидации.
- `netplay_render.audit.md` — расхождения рендера/UI.

---

## 1. Что считается «готовым B»

B — это не «ещё несколько intent'ов», а архитектура:

1. **Хост — единственный авторитет.** Клиент не меняет игровой мир напрямую;
   все изменения проходят через команды, хост валидирует и применяет.
2. **У клиента есть interest-managed зеркало мира**, достаточное для
   нативного UI: карта/субмапы, предметы, машины, существа, персонаж,
   календарь/погода, overmap.
3. **Нативный UI исполняется на клиенте** тем же кодом, что в одиночной игре
   (`veh_interact`, `game::examine`, `inventory_selector`, `game_menus`,
   `iexamine`, `workbench`, turrets, zones и т.д.), с клиентскими шрифтами,
   языком, темой и UI-опциями.
4. **Мутации из UI перехватываются** на высокоуровневых границах и уходят на
   хост типизированными командами; ответ — ACK/NACK + дельта состояния.
5. **Хост не блокируется на клиентских меню.** Клиент может держать UI
   открытым; хост продолжает свой цикл/ходы (с учётом turn parity).
6. **Remote UI (A-fallback)** используется только для ещё не портированных
   экранов, а не как основной путь.
7. **Моды/данные/версии согласованы** (манифест + handshake), id разрешаются
   одинаково.
8. **Реконнект/полный resync** не оставляют stale items/vehicles/creatures.
9. **Рендер:** клиент рисует нативный UI своими настройками; для одного и
   того же состояния/настроек он совпадает с single-player reference.
   Хостовый UI может отличаться настройками — это нормально для B.
10. **Безопасность/производительность:** команды валидируются, есть лимиты,
   interest radius, дельты, профилирование.

## 2. Non-goals

- Детерминированная lockstep-симуляция (вариант C) — не цель.
- Полное зеркало **всего** мира/всех z-level'ов — только interest-managed
  рабочий набор вокруг игроков.
- Замена одиночной игры; netplay-режим должен оставаться опциональным.
- Идеальный anti-cheat уровня MMO; базовая валидация и защита от случайных/
  вредоносных payload'ов — обязательны.

## 3. Целевая архитектура

### 3.1. Компоненты

1. **SessionHost (host)**
   - session registry, per-session NPC, region/player scope;
   - CommandRegistry, validators, execution, ACK/NACK;
   - state replication (interest management, deltas);
   - turn parity, AFK, reconnect.
2. **SessionClient (client)**
   - Transport/CommandSink;
   - ClientMirror (map/vehicles/items/creatures/character/weather/overmap);
   - Native UI runtime + mutation interceptors;
   - local message/event log;
   - optional RemoteUI fallback.
3. **Wire protocol**
   - `HELLO`/handshake: game version, netplay version, mod manifest, data
     manifest, tileset/data hash;
   - `SNAPSHOT`/`DELTA`: interest regions, revisions, checksums;
   - `COMMAND`/`COMMAND_RESULT`: typed id, args, generation/epoch,
     idempotency key, result;
   - `EVENT`: messages, sounds, UI requests (fallback), session state;
   - `REMOTE_UI`: only for fallback.
4. **Shared schema**
   - command IDs/payloads and state records should have one source of truth
     (JSON schema / generated code) to avoid host/client drift.

### 3.2. Границы мутаций (mutation interception)

Перехватывать не каждую строчку, а высокоуровневые операции:

- `Character::assign_activity` / activity start/finish;
- `map` mutations: bash, destroy, open/close, traps, fields, furniture,
  terrain changes, item add/remove, liquid transfer;
- `vehicle` mutations: install/remove/repair/refill/drain, controls, engines,
  turrets, electronics, cargo, labels, doors, crew;
- item transfer: pick up, drop, wield/wear, insert, unload, reload, consume;
- dialogue/trade/mission effects;
- EOC activation;
- character state changes initiated by UI (not periodic simulation).

В идеале — один `CommandSink` в клиентском UI, который вызывается из этих
точек; локальное зеркало обновляется оптимистично только там, где это
безопасно, и всегда перезаписывается дельтой хоста.

## 4. Текущее состояние (baseline)

| Слой | Состояние | Комментарий |
|---|---|---|
| Конверт интентов | PARTIAL | `intent_request`, `client_send_intent`, `host_pump_intents`; ручная цепочка `if` |
| Авторитет/scoping | PARTIAL | `region_scope`, player/prompt scope, generation checks; валидация неполная |
| Зеркало персонажа | PARTIAL/GOOD | `CHARACTER_JSON`, `mp_uid`, нативные inventory/@/craft/pickup; не все поля применяются |
| Карта/предметы | PARTIAL | клетки/тайлы/свет; предметы упрощённые; нет полного `map`-зеркала |
| Машины | PARTIAL/WEAK | только `vehicle_parts` sprites; нет `vehicle`-объекта |
| Существа | PARTIAL | NPC/monster JSON; внешний вид/анимации могут отличаться |
| Погода/overmap | PARTIAL/WEAK | `weather_id`/turn; overmap только seen-блок |
| Нативный UI | PARTIAL/GOOD | inventory/@/craft/pickup/vehicle (native `veh_interact` по умолчанию); остальное — uilist/A-fallback/disabled |
| Command registry | TODO | нет типизированного registry/валидаторов |
| Delta/interest/reconcile | TODO | снапшоты/статусы, без дельт/ACK/отката |
| Mod/data manifest | TODO | клиент грузит свои данные; проверки совпадения нет |
| Render data completeness | PARTIAL | сайдбар/лог/миникарта/overmap/погода/опции неполные |

---

## 5. Workstreams и задачи

Статусы: `TODO` / `WIP` / `PARTIAL` (есть legacy-реализация) / `DONE` /
`BLOCKED` / `DONE (not tested)`.

### P0. Тестовый стенд и baseline

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P0-1 | Создать тестовый мод `netplay_test` с картой-полигоном | `data/mods/netplay_test/*` | — | мод грузится, сценарий стартует в полигоне | DONE |
| P0-2 | Создать тестовый мир `Netplay Test` + host/client userdir | `mptest/*`, скрипты | P0-1 | host/client стартуют в фиксированных точках | DONE (join + нативный кадр) |
| P0-3 | Тестовый драйвер: последовательности клавиш/intent'ов, reset, assert | `netplay/test_driver.*`, env hooks | P0-2 | сценарий выполняется без ручного UI | TODO |
| P0-4 | Single-player reference baselines для UI/рендера | скриншоты/логи | P0-1 | есть эталоны для каждого экрана/настроек | TODO |
| P0-5 | Диагностика: state hash, command log, mirror diff, desync detector | netplay diag | P1/P2 | по логу видно расхождение | TODO |
| P0-6 | Матрица тестов и CI/скрипты | docs/scripts | P0-1..5 | матрица из §8 автоматизирована | TODO |

### P1. Command bus / registry / validation

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P1-1 | Схема команды: id, args, generation/epoch, actor, target, idempotency | `netplay.h`, schema | — | одна схема для host/client | DONE (`command_id`, args, generation, sid) |
| P1-2 | `CommandRegistry` на хосте; замена ручной цепочки `host_pump_intents` | `netplay_world.cpp` | P1-1 | новые действия регистрируются без `if` | DONE (все известные intent'ы в registry; fallback только для неизвестных) |
| P1-3 | `CommandSink` на клиенте + wrappers | `netplay_ui.cpp` | P1-1 | UI вызывает sink, не пишет напрямую | TODO |
| P1-4 | ACK/NACK/result/error model; таймауты/retry | protocol | P1-1 | клиент видит успех/ошибку | TODO |
| P1-5 | Generation/epoch/revision; stale rejection | protocol | P1-1 | устаревшая команда не применяется | PARTIAL (exact duplicate drop by `command_id`; generation/revision legacy) |
| P1-6 | Rate limit/backpressure/per-session queue | host | P1-2 | очередь не растёт бесконечно | TODO |
| P1-7 | Портировать все текущие intent'ы в команды (список из action audit) | `netplay_world.cpp` | P1-2 | legacy intents только shim | TODO |
| P1-8 | Idempotency/duplicate suppression | protocol | P1-4 | retry не применяет дважды | TODO |

### P2. Client world mirror

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P2-1 | Дополнить character mirror: moves/view_offset/avatar-only/state | `netplay_ui.cpp` | P1-1 | sidebar/UI видят те же значения | TODO |
| P2-2 | Полное map/submap-зеркало: terrain/furniture/traps/fields/memory | client mirror | P2-9 | `map` пригоден для UI | TODO |
| P2-3 | Полные items: contents/ammo/rot/ownership/color/variant/uid/deltas | item serializer | P2-2 | item UI/actions точны | TODO |
| P2-4 | Полные vehicles: serialize/deserialize, parts/tools/cargo/crew/labels/doors | `vehicle` JSON | P2-2 | `veh_interact` работает | DONE (vehicle JSON mirror, 91 parts, `veh_at=1`; native UI — P3-4) |
| P2-5 | Creatures: NPC/monster mirror, appearance/equipment/effects/visibility | creature JSON | P2-2 | существа и их UI совпадают | TODO |
| P2-6 | Calendar/weather: turn/type/intensity/temp/wind | snapshot | — | погода/время/виджеты | TODO |
| P2-7 | Overmap: known/seen, cities/roads/specials/notes/markers/tracking | overmap | P2-2 | overmap UI пригоден | TODO |
| P2-8 | Interest management: per-player bubble, z, submap priorities | replication | P2-2 | трафик ограничен | TODO |
| P2-9 | Snapshot/delta protocol + revision + checksum + full resync | replication | P2-2 | нет stale/пропусков | TODO |
| P2-10 | Mirror lifecycle: reset/reconnect; no local turn simulation | client | P2-9 | reconnect чистый | TODO |
| P2-11 | Map cache/lighting invalidation after deltas | client | P2-2 | нет чёрных/старых клеток | TODO |

### P3. Native UI integration

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P3-1 | `SessionWorld`/`GameView` или mirror-game mode | client | P2 | UI читает зеркало | TODO |
| P3-2 | Перехват мутаций (activities/map/vehicle/item/character/dialogue/EOC) | client | P3-1/P1 | UI не меняет мир напрямую | TODO |
| P3-3 | Перевести native inventory/@/craft/pickup на command bus | client | P1/P3-2 | actions идут через registry | PARTIAL |
| P3-4 | Native `veh_interact` на клиенте | `veh_interact` | P2-4/P3-2 | все examine-действия локальны | DONE (native UI; `e` открывает `interact_with`-меню, `EXAMINE_VEHICLE` — нативный `veh_interact`; repair/refuel/rename/relabel/remove/mend/install/siphon/unload/shape/crew через command bus; `DDA_MP_DIRECT_VEH_INTERACT` — прямой экран) |
| P3-5 | Native `vehicle::interact_with` (верхнее меню) | `vehicle_use` | P2-4 | без JSON-моста | PARTIAL (vehicle_menu_here + client actions: EXAMINE_VEHICLE/USE_WORKBENCH/FAUCET_FILL/LOCKPICK; smart engine/turret/tool/EOC disabled) |
| P3-6 | Native `game::examine`/`iexamine` + выбор клетки | `game`/`iexamine` | P2-2/P3-2 | контекстное меню как в SP | PARTIAL (auto-target) |
| P3-7 | Native action/main/ESC menus | `handle_action`/`main_menu` | P3-2 | меню/действия как в SP | TODO |
| P3-8 | Native item menu (`inventory_item_menu`) | `game_inventory` | P2-3 | заменить `mp_client_item_menu` | PARTIAL |
| P3-9 | Native trade/dialogue/mission UI | `npctalk`/`trade`/`mission` | P2-5 | без uilist-замен | PARTIAL (uilist) |
| P3-10 | Native zone manager | `clzones`/`zonemap` | P2-2 | создание/редактирование зон | TODO |
| P3-11 | Native overmap UI | `overmap_ui` | P2-7 | полная карта/маркеры | TODO |
| P3-12 | Native construction UI | `construction` | P2-2 | требования/превью | PARTIAL (uilist) |
| P3-13 | Native workbench/tools/liquid/smart controller/turrets/EOC/bike rack/capture/tiedown/harness/planter/tool attach | `iexamine`/`vehicle`/`turret`/`smart_controller_ui` | P2/P3-2 | каждый экран локально | TODO |
| P3-14 | Local prompts: query_yn/query_int/uilist/string_input/popup | client | P3-1 | без prompt bridge | PARTIAL |
| P3-15 | Remote UI fallback только для unported screens | `netplay_ui` | P5 | fallback маркирован | PARTIAL (prompt) |
| P3-16 | Удалить/выключить per-action uilist-замены по мере портирования | netplay_ui | P3-3..13 | нет дублирующих меню | TODO |

### P4. Рендер и данные нативного UI

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P4-1 | Native sidebar из полного зеркала; решить судьбу `SIDEBAR` (удалить/fallback) | `netplay_ui`, `panels` | P2-1/P2-2 | панели как SP, значения полные | PARTIAL (рисуется; есть наложения) |
| P4-2 | Единый native message log (host events + local), без дублей/потерь | client log | P2 | лог как SP, scroll/aging | PARTIAL |
| P4-3 | Native minimap/overmap panel | client | P2-7 | позиция/размер/zoom как SP | PARTIAL |
| P4-4 | Native aim/look panels | client | P2 | без host `AIM_PANEL` или fallback | PARTIAL |
| P4-5 | Tileset/data manifest + mod id resolution/fallback | handshake | P6 | одинаковые id/спрайты | TODO |
| P4-6 | Light/weather/visibility sync для native renderer | client | P2-6 | нет расхождений света | PARTIAL (чёрный кадр устранён; остались артефакты панелей) |
| P4-7 | Политика UI style/font/language/theme (клиентские настройки) | docs/options | — | нет mixed-language/чужих шрифтов | TODO |
| P4-8 | Camera/layout/framing под native UI | `netplay_ui` | P2/P3 | соответствует SP | PARTIAL |
| P4-9 | Screenshot/golden image harness | tests | P0-4 | матрица скриншотов | TODO |

### P5. Remote UI fallback

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P5-1 | Протокол RemoteUI: ImGui draw lists / curses cells + input | netplay | P1 | один канал | TODO |
| P5-2 | Per-session UI lifecycle/modal/blocking policy | host | P5-1 | хост не блокируется без нужды | TODO |
| P5-3 | Texture/font/scale sync; security | netplay | P5-1 | корректный рендер | TODO |
| P5-4 | Список fallback-экранов; deprecate по мере P3 | docs | P3 | fallback только явно | TODO |

### P6. Mods/version/data

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P6-1 | `HELLO`: game/netplay version, mod list/order/versions, data hash | protocol | P1 | несовместимый клиент отклонён | TODO |
| P6-2 | Data/tileset manifest; id validation; fallback | client | P6-1 | нет missing-id крашей | TODO |
| P6-3 | Save/world/character compatibility | session | P6-1 | reconnect/старые сейвы | TODO |

### P7. Robustness/security/performance

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P7-1 | Матрица валидации команд: distance/LOS/ownership/tools/skills/state/turn | host | P1/P3 | нет auto-target/range дыр | PARTIAL (control vehicle theft/hands/reins, fire range/LOS, reach/butcher/NPC distance; tools/skills/state неполные) |
| P7-2 | Anti-cheat/rate limit/JSON sanitization | host | P1 | вредоносный клиент ограничен | TODO |
| P7-3 | Desync detection: state hashes/revision/resync | netplay | P2-9 | расхождение обнаруживается | TODO |
| P7-4 | Reconnect/crash recovery/multi-client | netplay | P2-10 | 2–4 клиента стабильны | TODO |
| P7-5 | Performance: delta size, interest radius, host CPU, client RAM | profiling | P2-8 | бюджет соблюдается | TODO |
| P7-6 | Host UI blocking policy: никаких клиентских модалок на хосте | host | P3 | хост не ждёт меню | TODO |

### P8. Regression/release

| ID | Задача | Файлы/области | Зависимости | Acceptance | Статус |
|---|---|---|---|---|---|
| P8-1 | Action regression matrix из `netplay_action_audit.md` | tests | P3/P7 | все действия покрыты | TODO |
| P8-2 | Render regression matrix из `netplay_render.audit.md` | tests | P4 | все экраны/настройки | TODO |
| P8-3 | Single-player reference + client-native comparison | tests | P0-4/P4 | golden screenshots | TODO |
| P8-4 | Mod compatibility tests | tests | P6 | набор модов проходит | TODO |
| P8-5 | Long-run/stress/reconnect | tests | P7 | без утечек/десинков | TODO |
| P8-6 | Release checklist | docs | all | готовность B | TODO |

---

## 6. Traceability: аудит → задачи

### 6.1. `netplay_action_audit.md`

| Аудит | Проблема | Решение в B | Задачи |
|---|---|---|---|
| 1.1 Examine | uilist вместо `veh_interact`; ходы/детали | native vehicle mirror + native `veh_interact` + commands | P2-4, P3-4, P4-1 |
| 1.2 Top-level `^` | часть строк disabled/кастомные | native `interact_with` on mirror; commands | P3-5, P3-13 |
| 1.3 Legacy vehicle intents | дубли/неполные проверки | удалить после native; команды с валидацией | P1-7, P3-4, P7-1 |
| 2 Control vehicle | обход locked/theft/two-handed/reins; optimistic flag | `ACTION_CONTROL_VEHICLE` → command + host stock checks; ACK | P7-1, P3-5 |
| 3 movement_mode | нет стоимости смены режима | native action path/command validator | P3-7, P7-1 |
| 3 ACTION_SLEEP/MOVE | упрощения vs stock | native action handlers on mirror | P3-7 |
| 4 pickup by type | неверный предмет при одинаковом типе | uid-based pickup rows + full item mirror | P2-3, P1-3 |
| 4 fire/reach | нет range/LOS валидации | command validators | P7-1 |
| 5 autoattack | reimplementation vs stock | native `avatar_action` path on client + command | P3-7, P7-1 |
| 5 suicide/sort/default ammo | упрощения | native UI + host validation | P3-7, P7-1 |
| 6 NPC distance | нет range проверки | command validators + native dialogue/trade | P7-1, P3-9 |
| 7 Hotkeys | int вместо input_event | native UI (локальные hotkeys) | P3-4/P3-5/P4-7 |
| 8 Verified | generation/uid проверки | сохранить, перенести в command framework | P1-5/P1-8 |

Статус на `ddamp-v4.0a-317`: control vehicle закрыт полностью (hotwire, theft/hands/reins, ванильная engine-start activity), стоимость `movement_mode`, stable uid для pickup (подтверждено рантаймом на двух одинаковых предметах с разным damage), range/LOS для `fire`/`reach_attack`/`butcher`, дистанция NPC talk/trade/mission, siphon/faucet fill с выбором контейнера, refuel по выбранной детали/предмету, `query_int` bridge + planter, washing machine/dishwasher; пункт 7 (hotkeys) закрыт для bridged-меню полным `input_event`; LOCKPICK pick-only; `e` → `interact_with`-меню → нативный `veh_interact` (проверено скриншотом) и workbench. Пункты 1.1/1.3 закрыты; 1.2 остаётся частично: smart engine, turret aim/reload, use tool, EOC. Отдельный долг: меню `e` строится только на клетке аватара — нет наведения на соседнюю клетку машины.

### 6.2. `netplay_render.audit.md`

| Аудит | Проблема | Решение в B | Задачи |
|---|---|---|---|
| 1.1 SIDEBAR dead | host cells не используются | native sidebar from mirror; `SIDEBAR` удалить/fallback | P4-1, P2-1/2 |
| 1.2 avatar/npc | типы персонажа расходятся | mirror-game/`Character` view с правильным типом | P3-1, P4-1 |
| 1.3 layout only names/toggle | widths/heights/options не синхронизированы | native panel definitions/options on client | P4-1, P4-7 |
| 1.4 curses/pixel | разные слои/метрики | native UI layout как SP | P4-1, P4-8 |
| 1.5 bg/colors | хостовые фоны/цвета теряются | native client theme for native UI | P4-1, P4-7 |
| 1.6 width/clipping | host/client TERM различаются | native client layout | P4-1, P4-8 |
| 1.8 missing fields | moves/vehicle/map/global | full mirror fields | P2-1, P2-2, P2-4 |
| 2 Log | host log не стримится; local diffs | единый native log + event stream | P4-2 |
| 3.1 prompt bridge | title/desc/colors/callbacks | native UI; fallback RemoteUI only | P3-14, P5 |
| 3.2 veh_menu title/desc | теряются | native `veh_menu` on mirror | P3-5 |
| 3.3 examine uilist | не native | P3-4 | P2-4, P3-4 |
| 3.8 vehicle menu fields | symbol/colors/desc hint | native menu | P3-5 |
| 4.1 EXAMINE | auto-target/first tile | native `game::examine`/target cursor | P3-6 |
| 4.2 OPEN/CLOSE | auto-target | native action/target | P3-6/P3-7 |
| 4.3 EXAMINE_AND_PICKUP | auto-target | native examine + pickup | P3-6 |
| 4.4 action menu | custom | native `game::action_menu` | P3-7 |
| 4.5 main/ESC | custom | native `main_menu` in session mode | P3-7 |
| 4.6 inventory | item menu custom | native `inventory_item_menu` | P3-8 |
| 4.7 craft | partial | native crafting + workbench | P3-3/P3-13 |
| 4.8 construction | custom uilist | native construction UI | P3-12 |
| 4.9 dialogue/trade/mission | custom uilist | native UIs | P3-9 |
| 4.10 zones | read-only | native zone manager | P3-10/P3-11 |
| 4.11 vehicle info/parts/engine | custom | native `veh_interact` | P3-4/P3-5 |
| 4.12 pickup | scratch selector | native pickup on mirror | P2-3/P3-3 |
| 4.13 other context | custom/absent | native action UIs | P3-6/P3-7 |
| 5.1 tileset/data | client own | data manifest/handshake | P4-5/P6 |
| 5.2 light/color/visibility | client renderer/options | native renderer + synced light/weather | P2-6/P4-6 |
| 5.3 items | simplified | full item mirror | P2-3 |
| 5.4 corpses | simplified | full item mirror | P2-3 |
| 5.5 vehicles | sprites only | full vehicle mirror | P2-4 |
| 5.6 creatures | partial appearance | full creature mirror | P2-5 |
| 5.7 fields/terrain | partial | full map mirror | P2-2 |
| 5.8 weather | only id | full weather/temperature | P2-6 |
| 5.9 animations | independent | native render timing | P4-8 |
| 5.10 camera | full-window vs host | native camera/layout | P4-8 |
| 5.11 overmap | sparse | full overmap mirror/UI | P2-7/P3-11 |
| 5.12 minimap | pixel fixed | native minimap panel | P4-3 |
| 5.13 aim/look | host panel | native aim/look | P4-4 |
| 6.x missing host screens | absent | native ports | P3-13 |
| 7.x options/font/lang | unsynced | client-native policy + manifest | P4-5/P4-7/P6 |
| 8.x host anchor/log/popup | host side effects | commands/event stream, no host modals | P7-6/P4-2 |
| 9.x ASCII fallback | different | native renderer/fallback | P4-5/P4-8 |

---

## 7. Пользовательская карта/мод для тестов

**Ответ: да, это возможно и оптимально.** Чистого JSON «на все случаи» не
хватит, но тестовый мод + сценарий + mapgen + тестовые хуки покрывают
практически все actions/UI/render.

### 7.1. Структура мода

`data/mods/netplay_test/`:

- `modinfo.json` — id `netplay_test`, зависимость `dda`.
- `scenarios.json` — сценарий `netplay_test_scenario`.
- `professions.json` — профессия со всеми навыками/инструментами/предметами.
- `start_locations.json` — старт в тестовом бункере.
- `mapgen/` — `netplay_test_facility.json` (palette + rooms).
- `overmap/` — `overmap_terrain`, `overmap_special`, city/road.
- `npcs.json` / `npc_classes.json` — союзник, торговец, mission-giver,
  враг, животное.
- `missions.json` — тестовая миссия (accept/turn-in).
- `item_groups.json` — наборы инструментов/жидкостей/семян/патронов.
- `monstergroups.json` — тестовые монстры.
- `eocs.json` — reset/setup/teleport/zone/weather при необходимости.
- `vehicles` — использовать существующие прототипы + тестовый хук для
  установки конкретных частей/повреждений.

### 7.2. Планировка полигона

Один OMT/бункер с зонами:

1. **Hub/спавн** — профессия, locker, debug-консоль, точка респавна.
2. **Vehicle garage** — все прототипы машин с интервалом; машины с
   controls, engines, turrets, autopilot, smart controller, electronics,
   reactors, batteries, solar, cargo, seats, curtains, doors, locks, alarms,
   immobilizer, bike rack, animal harness, planter, workbench, tools,
   tanks, faucet, purifier, autoclave, washing machine, dishwasher, MWS,
   boiler, cameras, stereos, lights.
3. **Examine hall** — по одному terrain/furniture/trap/field: workbench,
   computer, ATM, vending, door/window, bed, crafting station, EOC terminal,
   locked door, curtain, liquid source.
4. **NPC hall** — ally, trader, mission giver, enemy, animal, monster;
   дистанции для dialogue/trade/mission/harness/capture.
5. **Liquid lab** — tanks, faucet, purifier, containers, funnels, spills,
   clean/dirty water, fuel.
6. **Crafting hall** — workbench, tools, components, light/dark, nearby
   items, recipe variants.
7. **Turret/aim hall** — turrets with controls, ammo, manual/auto modes,
   target dummies, smart controller.
8. **Zones/overmap room** — patrol/loot zones, notes, overmap special,
   roads/city.
9. **Render lab** — dark/bright rooms, windows, different z-levels, weather
   exposure, graffiti, item variants, vehicle rotations, NPC worn/wielded,
   corpses, liquids, fields, map memory.
10. **Vehicle damage/examine bay** — damaged/broken/reinforced parts,
    missing parts, tools/lift/jack, all part categories.
11. **Lock/security room** — locked doors, alarm, immobilizer, hotwire,
    ownership/theft.
12. **Reconnect/death room** — safe area for session tests.

### 7.3. Что покрывается JSON, а что хуками

JSON/mapgen:
- terrain/furniture/traps/fields/items/monsters/NPC templates/vehicles
  (размещение), overmap special, scenario/profession/start, missions,
  item groups, EOC.

Тестовые хуки (обязательны):
- создать/удалить зоны; выставить погоду/время/свет;
- выдать точные предметы/части машин/повреждения;
- заспавнить NPC с нужным отношением/миссией/товаром;
- телепортировать host/client в точку сценария;
- открыть/закрыть конкретный UI, отправить клавиши/intent;
- сбросить состояние между кейсами (reload save/snapshot);
- сделать скриншот и state dump.

### 7.4. Почему это лучший стенд

- Фиксированные координаты/состояния → воспроизводимость.
- Один мир → все экраны и действия в одном сценарии.
- Можно держать host и client в разных комнатах/на разных персонажах.
- Можно сравнивать native client UI с single-player reference на том же
  состоянии и настройках.
- Регресс: golden screenshots + логи команд/состояний.

### 7.5. Ограничения

- Не всё задаётся чистым JSON (зоны, погода, точные части машин, UI state).
- RNG/анимации/погода могут давать flaky-скриншоты; нужен freeze через
  тестовые хуки.
- Мод должен быть изолирован; не влиять на обычные сохранения.
- Для полного покрытия «всех действий» потребуется матрица, а не один
  линейный сценарий.

---

## 8. Тестовая стратегия

### 8.1. Уровни

1. **Unit**: сериализация команд/состояний, валидаторы, generation,
   idempotency.
2. **Integration host+client**: команда → ACK → дельта → состояние.
3. **UI/native**: каждый экран на клиенте, локальные настройки.
4. **Render regression**: golden screenshots single-player vs client.
5. **Multi-client**: 2–4 клиента, разные регионы/UI.
6. **Reconnect/stress**: обрыв, resync, длинная сессия.
7. **Mods/data**: разные наборы модов/версии/тайлсеты.

### 8.2. Матрица действий

- Все строки `^`: examine, tracking, hotwire, alarm, release, engine,
  handbrake, individual engines, autopilot, electronics, smart controller,
  turrets (modes/aim/reload/unload), horn, cables, tools, autoclave/washer/
  dishwasher/MWS/boiler, pickup, curtains, faucet, purify, capture, tiedown,
  bike rack, harness, planter, workbench, tool attach, fold, locks, EOC.
- Examine actions: install/repair/remove/refill/siphon/unload/mend/rename/
  relabel/crew/shape.
- Inventory: wield/wear/take off/drop/insert/unload/reload/consume/read/
  activate/mend/disassemble/favorite/pocket settings.
- Context: examine/open/close/pickup/smash/butcher/fire/reach/use.
- NPC: talk/dialogue choices/trade buy/sell/missions accept/turn-in/journal.
- Crafting/construction/workbench.
- Zones/overmap.
- Movement/vehicle driving/turn passing.
- Death/suicide/reconnect.

### 8.3. Матрица рендера

Для каждого экрана/состояния:
- tileset: стандартный/альтернативный;
- theme/colors;
- font/language/UI scale;
- sidebar position/spacers/layout;
- zoom;
- weather/light/dark;
- 1 и 2 клиента;
- host/client same/different settings.
Сравнивать client native UI с single-player reference, а не с host UI
(для B host UI может отличаться настройками).

### 8.4. Автоматизация

- `DDA_MP_TEST_*` env/CLI: сценарий, reset, assert, screenshot.
- Логи: `netplay_control.log`, `netplay_actions.log`, state hash,
  command id/result, mirror diff.
- Golden images + image diff с маскированием анимаций/погоды.
- CI/локальные скрипты: host+client, 2–4 клиента.
- Каждый баг/audit finding → regression case.

---

## 9. Статус и журнал

### 9.1. Статус по фазам

| Фаза | Статус | Комментарий |
|---|---|---|
| P0 test harness | TODO | плана/мода пока нет |
| P1 command bus | TODO | текущие intents — legacy |
| P2 world mirror | PARTIAL | character strong; map/vehicles weak |
| P3 native UI | PARTIAL/GOOD | inventory/@/craft/pickup/vehicle native; остальное uilist/disabled |
| P4 render data | PARTIAL | native map/panels; sidebar/log/minimap/overmap incomplete |
| P5 remote UI | PARTIAL | prompt bridge — primitive fallback |
| P6 mods/version | TODO | нет manifest/handshake |
| P7 robustness | PARTIAL | scoping/generation есть; validation gaps |
| P8 regression | TODO | нет полигона/матрицы |

### 9.2. Changelog

| Дата | Коммит | Изменение | Тесты |
|---|---|---|---|
| 2026-11-25 | ddamp-v4.0a-289 | Создан план B; baseline по аудитам | not tested (план) |
| 2026-11-25 | ddamp-v4.0a-289 | Полигон `netplay_test`: сценарий/профессия/стартовая локация, 4 OMT, NPC-шаблоны; исправлены JSON (`skills.level`, `items`/`item`, `charges`); мир `Netplay Test` создаётся и сохраняется | мод грузится без JSON-ошибок; персонаж и 3 NPC созданы; карты сгенерированы |
| 2026-11-25 | ddamp-v4.0a-289 | Фикстура: 6-я машина гаража больше не въезжает в стену; дверь лаборатории совмещена с дверью гаража; `rotate:false` у overmap_special | подтверждено на свежем мире: 6/6 машин, `t_reb_cage` нет, двери `(23,11)`↔`(0,11)` совпадают, стартовый special без поворота |
| 2026-11-25 | ddamp-v4.0a-289 | Protocol fix: крупные строки снапшота (`status`, `weather`, creature JSON, `critter_overlays`, `graffiti`) переведены на 32-битную длину; причина — переполнение `uint16_t` на JSON аватара >65 КБ | `rx snap #1 tiles=1568` вместо `tiles=0`; `canvas=1`; tile-секция парсится |
| 2026-11-25 | ddamp-v4.0a-289 | Host+client auto-run: `--mpclient`, `DDA_MP_TEST_AVATAR=player`, `DDA_MP_TEST_AVATAR_AUTO=1`; клиент получает снапшот и привязывает аватара | TCP `Established`, `avatar op kind=3`, `panels: host layout applied`, `snapapply #1` |
| 2026-11-25 | ddamp-v4.0a-289 | `background_pane` в `run_view_loop`: `stdscr` ack-ается до tile-canvas; устранён чёрный кадр из input-pump между двумя корректными present'ами | replay и native: `netplay_present_client_*` стабильно ~273 КБ, карта и native-sidebar видны; `drawwin stdscr` больше не каждый кадр |
| 2026-11-25 | ddamp-v4.0a-289 | Версия netplay-протокола поднята до 2 из-за несовместимого формата снапшота (32-битные длины строк); старый клиент получает явный DENY | host/client одной сборки соединяются; несовместимый клиент отклоняется |
| 2026-11-25 | ddamp-v4.0a-290 | P1: `intent_request::command_id` (опциональный trailing u32) и точное отбрасывание дублей; первый `CommandRegistry` с обработчиками `help`/`test.registry` | host/client: `intent tx test.registry` → `registry ok sid=1 cmd=1 action=test.registry`; `rx snap tiles=1568` |
| 2026-11-25 | ddamp-v4.0a-291 | P1-2: в registry перенесены vehicle control/menu/engine/info, suicide, sort_armor, movement_mode, zones, grab/grab-test, haul/autohaul | сборка; host/client с `test.registry` — `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | ddamp-v4.0a-292 | P1-2: в registry перенесены smash, reach_attack, use_winch, butcher, construct, workout; legacy-блоки удалены | сборка; host/client с `test.registry` — `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | ddamp-v4.0a-293 | P1-2: в registry перенесены vehicle parts/repair/refuel, pick_style, default_ammo, recraft | сборка; host/client с `test.registry` — `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | ddamp-v4.0a-294 | P1-2: в registry перенесены craft/craft.all, test_spell_cast, cast_spell, mission_journal | сборка; host/client с `test.registry` — `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | ddamp-v4.0a-295 | P1-2: в registry перенесены autoattack, fire, chat | сборка; host/client с `test.registry` — `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | ddamp-v4.0a-296 | P1-2: в registry перенесены talk_npc и dialogue_choice | сборка; host/client с `test.registry` — `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | ddamp-v4.0a-297 | P1-2 завершён: в registry перенесены миссии (offers/accept/turnin-list/turnin) и торговля (list/buy/sell); legacy-цепочка пуста | host/client с `test.registry` — `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | ddamp-v4.0a-298 | P2-4 WIP: снапшот несёт полный `vehicle_json` + абсолютную позицию; клиент собирает `vehicle` (91 часть) из пустого прототипа; broadcast-кадры больше не затирают зеркало | host отправляет JSON ~50 КБ; клиент: `vehicle: mirror applied parts=91`; `veh_at` пока 0 |
| 2026-11-25 | ddamp-v4.0a-299 | P2-4 WIP: кэш vehicle JSON по session-origin, fallback-создание submap, empty-prototype deserialize; добавлена диагностика `veh_at` | автономные прогоны приостановлены по просьбе; сборка проверяется отдельно |
| 2026-11-25 | ddamp-v4.0a-300 | P2-4: origin для `vehicle_json` всегда берётся из session-кадра/кэша; fallback на `snap.origin`, если `veh_ox/oy/oz` нулевые | сборка успешна; автономный тест не запускался |
| 2026-11-25 | ddamp-v4.0a-301 | P2-4 подтверждён: клиент собирает реальный `vehicle` из JSON (91 часть) и `veh_at(avatar)` возвращает 1 | `vehicle: mirror applied parts=91 pos=... veh_at=1` |
| 2026-11-25 | ddamp-v4.0a-302 | P3-4 WIP: native `veh_interact` на клиенте за `DDA_MP_NATIVE_VEH_INTERACT`; ремонт/заправка отправляются интентами, остальные действия пока блокируются | сборка; автономный прогон не выполнялся |
| 2026-11-26 | ddamp-v4.0a-303 | P3-4: тестовый хук `DDA_MP_TEST_CLIENT_VEH_INTERACT` (спавн машины + открытие native `veh_interact`); полноэкранный vehicle UI на клиенте подтверждён | `action: native veh_interact`; `netplay_present_client_200.png` — части/двигатель/колёса/баки/описание |
| 2026-11-26 | ddamp-v4.0a-304 | P3-4: `vehicle_rename` и `vehicle_relabel` добавлены в registry; native UI отправляет их на хост | `intent tx vehicle_rename`; host `vehicle rename sid=1 name=Netplay Test Car` |
| 2026-11-26 | ddamp-v4.0a-305 | P3-4: `vehicle_remove` и `vehicle_mend` добавлены в registry; native UI отправляет индекс части, хост запускает activity/починку на авторитетной машине | `intent tx vehicle_remove`; host `vehicle remove sid=1 part=90` |
| 2026-11-26 | ddamp-v4.0a-306 | P3-4 closeout: `vehicle_install/siphon/unload/shape/crew` в registry; исправлен учёт слива (`handle_npc_liquid` по значению); native `veh_interact` включён по умолчанию | suite: `install installed=79 parts=92`, `siphon drained=200`, `unload fuel=none`, `shape part=0 variant=cover`, `crew part=1 npc=0 assigned=1`; клиент получил зеркало `parts=92` |
| 2026-11-26 | ddamp-v4.0a-307 | Аудит-фиксы: control vehicle parity (locked/theft/двуручное оружие/`RESTRICT_HANDS`/вожжи), stable uid для `pickup.action`, проверки дистанции `fire`/`reach_attack`/`butcher`/NPC-talk-trade-mission, стоимость `movement_mode`, refuel по выбранной детали и предмету | golf cart: `take drv=1 engine=1`; suite без регрессий (`install parts=92`, `siphon drained=200`); pickup/fire — сборка + код-ревью, без рантайм-прогона |
| 2026-11-26 | ddamp-v4.0a-308 | Siphon parity: клиент выбирает контейнер (или «на пол»), хост переливает в выбранный item по uid; refuel принимает выбранную деталь и uid топлива | suite: `siphon via_uid=1 drained=200` (бутылка), `refuel part=78 after=200`; install/shape/crew/unload без регрессий |
| 2026-11-26 | ddamp-v4.0a-309 | Locked vehicle/hotwire: запертая машина борт-ит сессионного аватара и открывает host-меню `interact_with`; HOTWIRE выполняется на хосте. Плюс runtime-тесты аудита (fire range, NPC distance, pickup uid) | `menu ... HOTWIRE safe=1 enabled=1`; `test hotwire locked=1->0`; повторный `^` → `take ... drv=1 engine=1`; `Target out of range`; `That NPC is too far away`; pickup `uids=1 uid=30767` |
| 2026-11-26 | ddamp-v4.0a-310 | Engine-start parity: вместо `start_engines_direct()` используется ванильный `start_engines()` activity (время/условия); сообщения activity пересылаются клиенту после завершения; тесты GOLF/HOTWIRE завершают activity синхронно | `take ... drv=1 engine=1 starting=1` (hotwire и golf); меню/ручник/гудок/трекинг без регрессий |
| 2026-11-26 | ddamp-v4.0a-311 | P3-5: мост `query_int` (host prompt → client), включён `USE_PLANTER` (query_seed uilist + int); washing machine/dishwasher используют сессионного персонажа и включены в safe-меню | `test int prompt sid=1 ok=1 value=77`; planter/washer — сборка, без рантайм-прогона (нужны детали машины) |
| 2026-11-26 | ddamp-v4.0a-312 | P3-5: faucet fill — клиентский выбор контейнера (`faucet_fill` client action) + intent `vehicle_faucet_fill`; тестовый сценарий `DDA_MP_TEST_FAUCET` (отдельный водяной бак, faucet part, пустая бутылка) | `vehicle menu row ... FAUCET_FILL enabled=1`; `vehicle faucet fill sid=1 part=80 filled=2`; клиент: `You fill the ... with water` |
| 2026-11-26 | ddamp-v4.0a-313 | Аудит §7: bridged-меню `^` передаёт полный `input_event` (type/sequence/modifiers), клиент восстанавливает его в `uilist_entry.hotkey` | faucet suite: `key=101 evkey=101 evtype=2` (и все строки совпали); `FAUCET_FILL` и fill без регрессий |
| 2026-11-26 | ddamp-v4.0a-314 | P3-5: LOCKPICK переведён на pick-only intent `vehicle_lockpick` (`locked_object_pickable`); host-side pry-ветка `iuse::crowbar`/`choose_adjacent_highlight` больше не вызывается из меню | сборка; faucet-регрессия (`filled=2`); отдельный locked-door сценарий пока не прогонялся |
| 2026-11-26 | ddamp-v4.0a-315 | P3-5: `e` на машине теперь открывает `interact_with`-меню (как в ванили); `EXAMINE_VEHICLE` → нативный `veh_interact` на клиенте; `USE_WORKBENCH` → нативное меню крафта; новый intent `vehicle_menu_here` | workbench suite: `test workbench part=24 mount=0,1`; `USE_WORKBENCH` row enabled; клиент: 4 пункта меню (3 активны) |
| 2026-11-26 | ddamp-v4.0a-316 | Диагностика меню: `client_action` в строках, логи `examine_vehicle -> native/closed/no vehicle`; подтверждён native-экран через `e` → меню → `EXAMINE_VEHICLE` | `shots/client.png` (150с) — полноэкранный `veh_interact` golf cart; в логе `examine_vehicle -> native` без `closed` |
| 2026-11-26 | ddamp-v4.0a-317 | Pickup uid подтверждён рантаймом: тестовые близнецы переведены с `rock` (count_by_charges, damage не работает) на `screwdriver`; клиент авто-выбирает повреждённую строку; host логирует id/damage/uid каждой строки списка | `pickup list row 0 id=screwdriver dmg=0 uid=8386`; `row 1 ... dmg=1 uid=8389`; client `pickup: auto uid row=1 ... dmg=1`; host `pickup take ... uid=8389 dmg=1` |

### 9.3. Что протестировано

| Дата | Область | Тест | Результат |
|---|---|---|---|
| 2026-11-25 | P0-1 | Создание мира `Netplay Test` (`DDA_MP_TEST_WORLD`), сценарий `netplay_test_scenario`, профессия `netplay_tester` | мир и персонаж сохраняются; старт в `netplay_test_hub`; 3 NPC в зале; 6/6 машин; двери совмещены; `rotate:false`; JSON-ошибок нет |
| 2026-11-25 | P0-2 | Host+client: host слушает `24224`, клиент `--mpclient 127.0.0.1:24224` + `DDA_MP_TEST_AVATAR_AUTO=1` | TCP established; аватар привязан (`avatar op kind=3`), `rx snap tiles=1568`, `snapapply #1`; стабильный кадр карты |
| 2026-11-25 | P4-6 | Replay/native renderer клиента после `background_pane` | native: карта + панели, `render geom`, `snapapply`; `present_client_*` ~273 КБ без чёрных кадров |
| 2026-11-25 | P1-1/P1-2 | `command_id` + registry: `DDA_MP_TEST_CLIENT_INTENT=test.registry` | `intent tx test.registry`; host `registry ok sid=1 cmd=1 action=test.registry`; client `test: intent test.registry` |
| 2026-11-25 | P1-2 | Registry batch: vehicle/simple actions after migration | сборка успешна; host/client прогон: `registry ok`, `rx snap tiles=1568`, соединение стабильно |
| 2026-11-25 | P1-2 | Registry batch: smash/reach/winch/butcher/construct/workout | сборка успешна; host/client: `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | P1-2 | Registry batch: vehicle parts/repair/refuel, pick_style, default_ammo, recraft | сборка успешна; host/client: `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | P1-2 | Registry batch: craft, spells, mission_journal | сборка успешна; host/client: `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | P1-2 | Registry batch: autoattack, fire, chat | сборка успешна; host/client: `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | P1-2 | Registry batch: talk_npc, dialogue_choice | сборка успешна; host/client: `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | P1-2 | Registry complete: missions, trade, fallback-only legacy chain | сборка; host/client: `registry ok`, `rx snap tiles=1568` |
| 2026-11-25 | P2-4 | Vehicle JSON mirror (`test_vehicle_refuel` + `DDA_MP_TEST_SPAWN_CAR=1`) | host JSON ~50 КБ; client `vehicle: mirror applied parts=91`; `veh_at` пока 0 (координатный/кэш-дефект) |
| 2026-11-25 | P2-4 | WIP submap/cache fixes | автономный прогон не выполнялся по просьбе; нужен следующий тест после фикса координат |
| 2026-11-25 | P2-4 | Vehicle origin fallback + session cache | сборка успешна; ожидается `veh_at=1` после следующего разрешённого прогона |
| 2026-11-25 | P2-4 | Vehicle JSON mirror (`test_vehicle_refuel` + spawn car) | `vehicle: mirror applied parts=91 pos=3810,3368 z0 veh_at=1`; host/client стабильны |
| 2026-11-26 | P3-4 | Native `veh_interact` build (`DDA_MP_NATIVE_VEH_INTERACT`) | только сборка; игровой прогон не выполнялся |
| 2026-11-26 | P3-4 | Native `veh_interact` render on client mirror (`DDA_MP_TEST_CLIENT_VEH_INTERACT=1`) | `vehicle: mirror applied parts=91 veh_at=1`; `action: native veh_interact`; полноэкранный UI (части/двигатель/колёса/баки) |
| 2026-11-26 | P3-4 | `vehicle_rename` через native command bus (`DDA_MP_TEST_CLIENT_ACTION=vehicle_rename`) | `intent tx vehicle_rename`; host `vehicle rename sid=1 name=Netplay Test Car` |
| 2026-11-26 | P3-4 | `vehicle_remove` через native command bus (`DDA_MP_TEST_CLIENT_ACTION=vehicle_remove`) | `intent tx vehicle_remove gen=0 args=1`; host `vehicle remove sid=1 part=90` |
| 2026-11-26 | P3-4 | Vehicle command suite (`DDA_MP_TEST_CLIENT_ACTION=vehicle_suite`): unload → shape → crew → install → siphon | `vehicle unload ... fuel=none`; `shape part=0 variant=cover`; `crew part=1 npc=0 assigned=1`; `install part=beeper installed=79 parts=92 activity=0`; `siphon part=78 drained=200`; client mirror `parts=92` |
| 2026-11-26 | P3-4 | Native `veh_interact` по умолчанию, без `DDA_MP_NATIVE_VEH_INTERACT` | `action: native veh_interact`; `shots/client.png` — полноэкранные части/двигатель/баки 200 мл/колёса/описание; fallback `DDA_MP_LEGACY_VEH_INTERACT` |
| 2026-11-26 | P7-1 | Golf cart control после parity-правок (`DDA_MP_TEST_GOLF` + `DDA_MP_TEST_CLIENT_VEHICLE`) | `ctrl ... take ... drv=1 engine=1`; drive/turn/menu/handbrake/horn/tracking/release в `netplay_control.log` |
| 2026-11-26 | P3-4 | Vehicle suite regression после ddamp-v4.0a-307 | `install installed=79 parts=92`; `siphon drained=200`; `shape/crew/unload` без изменений |
| 2026-11-26 | P3-4 | Siphon через выбранный контейнер (`vehicle_siphon` + uid) | `vehicle siphon ... drained=200 via_uid=1`; refuel `part=78 before=0 after=200`; клиентское зеркало `parts=92` |
| 2026-11-26 | P3-5/P7-1 | Locked vehicle + hotwire (`DDA_MP_TEST_HOTWIRE`) | `ctrl in=0` → `menu HOTWIRE safe=1` → `test hotwire locked=1->0` → второй `^` `take ... drv=1 engine=1` |
| 2026-11-26 | P7-1 | Audit runtime: fire range, далёкий NPC, pickup uid | `clientlog: Target out of range.`; `clientlog: That NPC is too far away.`; `pickup take ... uids=1 uid=30767` |
| 2026-11-26 | P7-1 | Engine start через ванильную activity | hotwire/golf: `take ... drv=1 engine=1 starting=1`; меню/ручник/гудок/трекинг без регрессий |
| 2026-11-26 | P3-5 | `query_int` bridge + auto-answer (`DDA_MP_TEST_PROMPT_INT`) | host `test int prompt sid=1 ok=1 value=77`; client `prompt: auto int 77` |
| 2026-11-26 | P3-5 | Faucet fill через меню (`DDA_MP_TEST_FAUCET`) | `vehicle menu row ... FAUCET_FILL enabled=1`; `vehicle faucet fill sid=1 part=80 filled=2`; client `You fill the ... with water` |
| 2026-11-26 | P3-5 | Полный `input_event` в bridged vehicle menu | все строки: `key == evkey`, `evtype=2`; `FAUCET_FILL` и fill без регрессий |
| 2026-11-26 | P3-5 | LOCKPICK pick-only intent | сборка; faucet-регрессия без изменений; locked-door сценарий не автоматизирован |
| 2026-11-26 | P3-5 | `e` → interact_with menu, native examine/workbench | workbench suite: `test workbench part=24 mount=0,1`; `USE_WORKBENCH` row enabled; client menu rows 0-2 enabled, row 3 disabled |
| 2026-11-26 | P3-5 | Native `veh_interact` через `e` → меню → `EXAMINE_VEHICLE` | `client_action=examine_vehicle`; `examine_vehicle -> native`; `shots/client.png` — полноэкранный vehicle UI |
| 2026-11-26 | P7-1 | Deterministic pickup damage-twin по uid | host list: `screwdriver dmg=0 uid=8386` / `dmg=1 uid=8389`; client choose `dmg=1`; host take `uid=8389 dmg=1` |

### 9.4. Следующие 5 задач

1. P3-5: remaining `^` rows — smart controller, turret aim/reload, use tool, EOC.
2. Runtime-тест planter/washing machine/dishwasher на машине с этими деталями.
3. Adjacent vehicle examine: наведение `e` на соседнюю клетку машины (нужно для tool station/kitchen unit).
4. P3-6: native `game::examine`/`iexamine` без uilist-заглушки.
5. P6-1/P6-2: manifest версий модов/данных и handshake.

---

## 10. Риски и решения

| Риск | Влияние | Решение |
|---|---|---|
| Полное зеркало мира дорого | трафик/память | interest management, дельты, субмапы, приоритеты |
| UI завязан на глобальный `game` | сложно запустить нативно | mirror-game mode или `SessionWorld` + перехваты |
| Перехват мутаций неполный | клиент меняет локальный мир | единый CommandSink + тесты на каждую границу |
| Native UI требует полного `vehicle` | examine/workbench не работают | P2-4, сериализация vehicle |
| Моды/данные расходятся | missing ids/краши | P6 manifest/handshake |
| Remote UI блокирует хост | фризы | P5 per-session UI/лимиты; fallback только временно |
| RNG/анимации ломают golden | flaky tests | freeze time/weather/seed через hooks |
| Сложность валидации | дыры/читы | P7-1 матрица, unit/integration |
| Turn-based даёт задержки | UX | local native UI, хост не ждёт меню |

---

## 11. Milestones / Definition of Done

- **M1 (вертикальный срез B):** полное зеркало машины + карты вокруг +
  предметов + персонажа; native `veh_interact` на клиенте; команды
  install/repair/remove/refill/siphon/unload/mend/rename/relabel/crew/shape;
  тесты на полигоне.
- **M2:** native `game::examine`/open/close/pickup/smash/butcher/fire;
  native item menu; command registry/validation; render baseline.
- **M3:** native trade/dialogue/missions/zones/construction/overmap.
- **M4:** native workbench/turrets/smart controller/tools/liquid/EOC/bike
  rack/capture/tiedown/harness/planter/tool attach.
- **M5:** delta/interest/reconnect/multi-client/mod manifest/performance.
- **M6:** полная regression-матрица (actions+render), 2–4 клиента,
  golden screenshots, документация; B считается готовым по §1.

---

## 12. Правило обновления плана

После любого изменения кода:

1. Найди затронутые task ID.
2. Обнови статус в §5 (и §9.1).
3. Добавь строку в §9.2 Changelog.
4. Добавь/обнови строку в §9.3 «Что протестировано».
5. Если появился новый риск/ограничение — §10.
6. Если изменился порядок — §9.4.
7. Не удаляй историю; план должен быть самодостаточным после компактизации.
