# Аудит клиентских действий netplay (DDA MP)

Дата: 25.11.2026, ветка `experimental`, коммит `92dec6e` + правки examine-меню.
Метод: разбор кода `src/netplay/*`, `src/vehicle_use.cpp`, `src/veh_interact.cpp`,
`src/handle_action.cpp`; сверка клиентских `client_send_intent` / `ACTION_*` /
`KEYS` с хостовыми обработчиками. Аудит **по коду**, не полный рантайм-прогон.

Обозначения: **High** — может сломать игру/дать неверный результат, **Medium** —
заметное расхождение с хостом, **Low** — косметика/редкий случай.

## 1. Меню автомобиля (`^`)

### 1.1. «Осмотреть автомобиль» (Examine vehicle)

Раньше `EXAMINE_VEHICLE` на клиенте подменялся на список повреждённых деталей
и `vehicle_repair`; это не было меню осмотра и списывало ход. Теперь клиент
отправляет `vehicle_menu_select`, а на хосте вызывается штатный callback
`g->exam_vehicle()`.

Для удалённого игрока `veh_interact::run()` уходит в `netplay_loop()`:
хостовые проверки/операции те же, но выбор идёт через обычные `uilist` /
`query_yn` (они уже транслируются на клиент через prompt bridge).

Реализовано: install, repair, refill, remove, siphon, unload, mend, rename,
shape, assign crew, relabel, перемещение курсора по 8 направлениям.

Отличия от штатного экрана (осознанные упрощения, требуют проверки в игре):

- **High/Medium:** нет графической сетки автомобиля; это последовательность
  uilist-окон. Список деталей в тексте ограничен 20 строками.
- **Medium:** `siphon` сейчас сливает жидкость на пол под аватаром, а не
  предлагает контейнер/частичный перенос, как `liquid_handler`.
- **Medium:** `refill` показывает только предметы в инвентаре аватара; штатный
  `g->inv_map_splice` также позволяет брать предметы с соседних клеток.
- **Medium:** `install` — плоский список кандидатов текущей клетки, без
  вкладок/фильтра/прокрутки штатного экрана.
- **Medium:** `shape` только поворачивает отображаемую деталь; штатный
  `veh_shape` умеет менять multi-tile форму.
- **Low:** `crew` показывает союзников хоста (`g->allies()`), а не отдельный
  список союзников клиента.
- **Low:** `select_part()` в remote-режиме показывает все подходящие детали
  автомобиля, а не только доступные в текущем «окне обзора».
- **Medium (архитектура):** пока клиент находится в examine-меню, главный поток
  хоста блокируется на prompt bridge (каждый prompt ждёт до 60 с). Это
  ожидаемо для модального экрана, но хост в это время не тикает.

### 1.2. Верхнеуровневые строки меню `^`

Теперь выполняются на хосте (через callback + prompt bridge), а не отключены:

- `EXAMINE_VEHICLE` (см. выше)
- `CONTROL_AUTOPILOT` (но «Patrol…» при отсутствии зоны откроет на хосте
  `zone_manager_ui` — это не транслируется; **Medium**)
- `TURRET_TARGET_MODE`, `TURRET_FIRE_MODE` (uilist-циклы; карта-подсказка
  `pointmenu_cb` на клиенте не рисуется — **Low**)
- `UNLOAD_TURRET` (`Character::unload`: uilist/activity)
- `FOLD_VEHICLE` (исправлено на `get_player_character()`, иначе складывался
  хостовый аватар)
- `FAUCET_DRINK`, `PURIFY_WATER` (после remote-версии `veh_interact::select_part`)

Остаются отключёнными (открывают хостовый custom UI, не uilist):

- `CONTROL_SMART_ENGINE` — `smart_controller_ui`.
- `TURRET_MANUAL_AIM`, `TURRET_MANUAL_OVERRIDE`, `TURRET_SINGLE_FIRE` — turret aim.
- `RELOAD_TURRET` — `select_ammo`.
- «Use <tool>» — `use_vehicle_tool` → `invoke_item` (UI инструмента).
- `FAUCET_FILL`, `SIPHON` (верхнеуровневый) — `liquid_handler`.
- `CURTAIN_PEEK` — `g->peek`.
- `USE_CAPTURE_MONSTER_VEH`, `USE_TIEDOWN_FURNITURE_VEH`, `USE_ANIMAL_CTRL`.
- `USE_WORKBENCH` — crafting UI.
- `USE_PLANTER` — `query_seed` + `query_int` (int-промпт ещё не транслируется).
- Attach/detach tool у toolstation — inventory UI.
- Bike rack: строки `hotkey_auto()` без `_hotkey_action`, поэтому
  `mp_vehicle_menu_safe()` их не пропускает, хотя callbacks безопасны.
- EOC activation — dialogue UI.
- `LOCKPICK` — uilist + `iuse::crowbar`; не включён без проверки.

### 1.3. Старые vehicle-intent'ы

`vehicle_release`, `vehicle_handbrake`, `vehicle_toggle_tracking`,
`vehicle_horn`, `vehicle_parts`, `vehicle_repair`, `vehicle_refuel` остались
для тестов/старых путей. Расхождения:

- **Medium:** `vehicle_release` не вызывает `g->setremoteveh(nullptr)`, в
  отличие от штатного `RELEASE_CONTROLS`.
- **Medium:** `vehicle_toggle_tracking`/`vehicle_horn` не проверяют
  `controlling_vehicle`/наличие `CONTROLS`; верхнеуровневые callbacks это
  проверяют через `build_interact_menu`.
- **Medium:** `vehicle_repair` стартует `vehicle_activity_actor` без проверок
  `cant_do()`/`format_reqs()` (инструменты, подъём, мораль, свет). Штатный
  examine отказывает до старта activity.
- **Medium:** `vehicle_refuel` сам выбирает первую подходящую деталь и первую
  канистру из инвентаря; клиент не выбирает ни деталь, ни топливо/жидкость.
  Штатный `do_refill()` позволяет выбрать и то, и другое (включая предметы на
  земле).
- **Low:** `vehicle_info` — read-only; поля скорости (`velocity / 100`)
  совпадают с тем, что клиент показывает.
- **Low:** `vehicle_engine_toggle` не перепроверяет `part.is_available()` после
  generation-проверки; при поломке детали между открытием и кликом возможен
  toggle сломанной детали.

## 2. Управление автомобилем (`^`) и вождение

- **High:** `host_pump_client_actions` / `ACTION_CONTROL_VEHICLE`
  (`netplay_world.cpp`) сам сажает аватара за controls и вызывает
  `start_engines_direct()`. В отличие от штатного `game::control_vehicle()`:
  - не проверяется `veh->is_locked` (штатный путь открывает `interact_with`
    для hotwire);
  - не вызывается `handle_potential_theft()` (можно уехать на чужой машине);
  - не проверяются двуручное оружие/`RESTRICT_HANDS` и предложение убрать его;
  - не поддерживаются `CONTROL_ANIMAL`/вожжи;
  - обходится время/требования штатного запуска двигателя.
- **Medium:** `mp_client_vehicle_control()` на клиенте **оптимистично**
  ставит `get_avatar().controlling_vehicle = true` до подтверждения хостом.
  Если хост ответил «No vehicle controls found», клиент до следующего
  snapshot считает, что уже ведёт; второе `^` уходит как `vehicle_menu`, хост
  отвечает «You are not driving». Это и наблюдалось в логе 287.
- **Medium:** `vehicle_menu` у хоста требует `av->controlling_vehicle`.
  Штатный `interact_with()` открывается и для запертой/чужой машины (hotwire),
  но клиент туда не попадает: `ACTION_CONTROL_VEHICLE` уже «угнал» машину.
- **Low:** `mp_session_handbrake()` дублирует `handbrake()`; для верхнего меню
  теперь используется штатный callback `handbrake()` с scoped-персонажем.

## 3. Движение, ходы, клавиши

- **Medium:** `movement_mode` (intent из picker'а) вызывает
  `av->set_movement_mode(mode)` без списания `move_mode_switch_cost()`. В
  `ACTION_CYCLE_MOVE` стоимость списывается. То есть смена режима через
  picker бесплатна.
- **Low/Medium:** `ACTION_SLEEP` сразу назначает `try_sleep_activity_actor`
  на 24 часа, без штатного выбора/проверки комфорта и прерываний.
- **Low:** `ACTION_MOVE_UP/DOWN` проверяет только `GOES_UP/GOES_DOWN` и
  `passable`; штатный `game::vertical_move` дополнительно обрабатывает
  рампы/лестницы/транспорт и т.п.
- **Low:** `KEYS`-линия поддерживает только фиксированные коды
  (`1–4`, `12–15`, `20`); `CONFIRM`/`pause` клиент не отправляет. Это
  соответствует текущему UI, но не полному набору клавиш хоста.
- **Medium:** `ACTION_CYCLE_MOVE`/`TOGGLE_RUN/CROUCH/PRONE` списывают
  стоимость из `av_move_pool`, но не проверяют, хватает ли ходов; при
  отрицательном остатке клампится в 0. Штатный хост может отказать в смене
  режима при нехватке ходов.

## 4. Инвентарь, pickup, прицеливание

- **Medium:** `pickup.action` хранит строки как `{tripoint_abs_ms, itype_id}`,
  а при применении заново ищет **первый** предмет этого типа на клетке. Если
  на клетке два одинаковых типа с разным damage/charges/содержимым, хост
  возьмёт не тот предмет, который видел клиент. Нужен stable uid.
- **Low:** `pickup.tiles` принимает произвольные `dx/dy`; generation
  проверяется, но диапазон не ограничен.
- **Good:** inventory numeric rows проверяют `av_inv_gen`; uid-действия ищут
  предмет по `mp_uid` и сообщают «That item moved», если он исчез.
- **Good:** `inv.settings`/`inv.settings.uid` проверяют generation/node и
  ловят исключения при разборе JSON.
- **Low:** `aim.info` считает панель по произвольным `dx/dy` без ограничения
  дальности; это read-only, но лишняя нагрузка/информация.
- **Medium:** `fire` принимает произвольные `dx/dy` и `shots`, не проверяет
  дальность/линию видимости; `fire_gun()` может отказать, но ход всё равно
  помечается (`av_mark_acted`). Штатный `avatar_action::fire` сначала
  валидирует цель/патроны.
- **Medium:** `reach_attack` принимает произвольные `dx/dy`; проверка
  дальности остаётся на `npc::reach_attack()`, ход помечается всегда.

## 5. Бой и прочие действия

- **Medium:** `autoattack` — отдельная реimplementation выбора цели
  (`get_targetable_creatures`, `evaluate_weapon`/`unarmed_value`). Она не
  повторяет все ветки `avatar_action::autoattack` (safe mode, force-unarmed,
  special reach rules). В логах уже были расхождения с хостовым TAB.
- **Low:** `suicide` вызывает `av->die(&here, nullptr)`; штатный путь
  `avatar_action`/`game` может дополнительно обрабатывать death screen,
  достижения и т.п.
- **Low:** `sort_armor` переупорядочивает `worn` по списку uid без проверки,
  что список содержит ровно текущие надетые предметы; устаревший/частичный
  список может дать неверный порядок.
- **Low:** `default_ammo` берёт любой предмет с данным `mp_uid` и ставит его
  в `ammo_location`; нет проверки, что это боеприпас/валидный default-ammo.
- **Good:** `cast_spell` проверяет `knows_spell` и `targetable_locations`.
- **Good:** `construct` проверяет группу и соседнюю клетку.
- **Good:** `smash` проверяет соседнюю клетку и наличие цели.
- **Low:** `use_winch` использует жёсткий whitelist terrain id; модовые
  winch-террейны не поддерживаются.
- **Medium:** `butcher` принимает произвольные `dx/dy`; `host_butcher_at()`
  не проверяет, что труп рядом (только `here.inbounds`).
- **Good:** `workout` клампит intensity/minutes.
- **Good:** `recraft` использует `lastrecipe`/`last_batch` аватара.
- **Good:** `pick_style` проверяет, что стиль валиден и известен аватару.

## 6. NPC, миссии, торговля, диалоги

- **Medium (security/parity):** `talk_npc`, `mission_offers`, `mission_accept`,
  `mission_turnin_list`, `mission_turnin`, `trade_list`, `trade_buy`,
  `trade_sell` принимают `dx/dy` от клиента и находят NPC по этой точке без
  проверки дистанции/соседства. Штатный UI работает только с NPC рядом.
- **Good:** mission accept/turn-in проверяют, что миссия принадлежит NPC и
  выдана этому аватару; turn-in проверяет готовность.
- **Good:** trade использует `mp_trade_npc_id` и кэш предметов; покупка
  пересчитывает цену для charge-items, проверяет деньги.
- **Low:** `trade_sell` делает `other->i_add(gift)` без проверки, что предмет
  поместился; при переполнении инвентаря NPC возможна потеря/выпадение.
- **Good:** dialogue_choice проверяет индекс ответа, ролл и применяет эффекты
  на хосте.

## 7. Хоткеи автомобильного меню

- **High (исходный баг `e`):** клиент получает только `int` первого input'а
  (`mp_vehicle_menu_key` → `get_first_input()`) и строит `uilist_entry` через
  `hotkey_from_char()`. Это не эквивалентно хостовому `veh_keybind()`:
  - для `keyboard_code` с модификатором теряется модификатор;
  - для заглавной `keyboard_char` `hotkey_from_char` делает Shift+lowercase
    keycode, а хост сравнивает исходный unicode;
  - при отсутствии бинда хост отдаёт пустой `input_event()` (хоткей
    отключён), а клиент получает `MENU_AUTOASSIGN` и назначает случайную
    клавишу.
  Как минимум для `EXAMINE_VEHICLE`/`TOGGLE_ENGINE` это нужно проверить после
  пересборки; надёжное решение — передавать полный `input_event`
  (тип + sequence + модификаторы) и восстанавливать его на клиенте, либо
  выставлять `uilist_entry.hotkey` напрямую, минуя `hotkey_from_char`.

## 8. Что проверено и расхождений не найдено

- `vehicle_menu` generation/hash проверяется до выполнения callback.
- `vehicle_engine_toggle` проверяет generation/hash.
- `inventory` numeric/uid, `pickup`, `aim` — generation/uid проверки есть.
- `cast_spell`, `construct`, `smash`, `workout`, `pick_style` — валидация есть.
- `dialogue_choice` — индекс/ролл на хосте.
- `mission_accept`/`mission_turnin` — принадлежность миссии проверяется.
- `help`/`haul`/`grab` — используют сессионного NPC; vehicle dragging явно
  не поддерживается и сообщает об этом.

## 9. Приоритеты исправлений (не исправлялось в этом проходе)

1. `ACTION_CONTROL_VEHICLE`: locked/theft/two-handed/reins/engine-start parity.
2. `pickup.action`: stable uid вместо `type`-поиска.
3. Полный `input_event` для хоткеев автомобильного меню.
4. `movement_mode`: списание `move_mode_switch_cost`.
5. `fire`/`reach_attack`/NPC-действия: проверка дистанции/валидности цели.
6. Examine: siphon/refill/install/shape довести до штатного поведения.
