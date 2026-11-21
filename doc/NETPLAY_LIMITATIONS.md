# Netplay feature limits and notes

Last updated: v4.0a-207 (2026-11-15).

## Mission rewards

- Turn-in mirrors `talk_function::mission_reward`: the NPC's
  `op_of_u.owed` grows by the mission value.
- Generic mission reward is also added to the session barter credit; buying
  spends that session-wide credit. The NPC's `owed` is opinion/reward state
  and is not reduced when the credit is spent at another trader.
- The engine's `mission::reward` (`npc_favor`) field is not used by current
  upstream code; there is no separate item reward to transfer. If a fork/mod
  starts using it, a dedicated reward protocol is needed.
- Full turn-in completion was not automated; it needs an NPC whose mission
  objective the test avatar can satisfy.

## Zones

- Read-only list: host streams the player faction's zones (name, zone type id,
  center); the client resolves the type name in its own language.
- Editing zones, zone options and zone-based activities remain host-side.

## Diary

- The diary is stored only on `avatar` (`a_diary`). Session characters are
  NPCs and have no diary object, so the diary action stays hidden for clients.
  A separate per-session diary would be a new feature, not engine mirroring.

## Auto-modes

- Safe mode, auto travel, auto features, auto pulp/mining/foraging and similar
  switches are global game options, not per-character state. Exposing them to a
  client would change the host player's options, so they stay host-side.
- Per-character thief mode is supported separately (`THIEF_MODE`).

## Grab / dragging

- Furniture dragging works for session characters through the engine activity.
- Vehicle dragging still uses the host-avatar-only vehicle path and is rejected
  for remote sessions.

## Action menu parity

- Added for clients: movement-mode picker, directional drop, inventory
  organize (letter changes via `=` in the inventory), armor sorting by uid
  order, spellbook with the netplay targeting cursor, session-local diary,
  suicide, and the host-global auto/safe-mode toggles.
- Auto features, safe mode and auto travel are host-global settings; toggling
  them from a client changes the setting for the shared world and is announced.
- Save/load remain host-side by design; debug tools stay host-only.
- Diary is session-local: it is not persisted after leaving the session.
- Spells require a client target; the host validates it against
  `spell::targetable_locations` before starting the spellcasting activity.
- `a` on the wielded item and mend (wielded item) go through the host's
  inventory command path, so they use the real item, tools and materials.
  Unlike the stock action, mend has no client-side darkness pre-check.
- Wielding (`w`), `a` (apply) and mend all address items by their mirror uid,
  so they act on carried items inside containers too, not only on gear.
- Autoattack runs on the host: it picks the target with the stock rating
  (NPC weapon value, monster difficulty), melees an adjacent enemy or uses a
  reach attack. It deliberately avoids `avatar_action::autoattack`, which
  only accepts an `avatar`.

## Playtest 2026-11-15

Automated host+client scenarios (60 total, all completed without a crash or
timeout): movement and round gate, cycle-move, drop, haul, workout, thief,
grab/drag, vehicle status/refuel/parts/repair, zones, bionics, mutations, map,
sky, factions, bodygraph, medical, style, NPC dialogue, mission offers/accept,
trade buy/sell, language mismatch, long wait, auto-turn, help, diary, spellbook,
armor sorting, auto features/safe mode, suicide, use-wielded, mend, wield,
autoattack, death/respawn, mission turn-in, repair completion and reconnect.

Mission turn-in was verified end to end with `MISSION_GET_INHALER`:
the item lands in the session avatar's backpack (`has_inhaler=1`,
`complete=1`), the list is built, the pick is sent, the end effects run as
the avatar, and the reward/journal update (`Reward credit` event,
`journal open entries=1`).

The stock test world has no spellbook, so spell casting was verified with a
test-only hook that teaches `bio_nl_antigravity_jump_spell` to the session
character: `test spell cast ... ok=1`. The normal client path (spellbook list,
targeting cursor, host-side `spell::targetable_locations` validation) uses the
same host handler; a world with real spells was not part of the automation.

Use-wielded, mend, autoattack and wield use test-only host hooks too: the
session character gets a weapon (auto-equipped when no pocket fits it) and a
harmless hallucination is spawned next to it. Observed results:
- `action: use_wielded uid key=97` and `action: mend uid key=109` on the client,
  then the host's inventory command path ran;
- `autoattack sid=1 targets=1`: the host selected the adjacent enemy and
  applied the melee/hallucination path;
- `wield cmd sid=1 item=knife_combat changed=1 armed=1`: with a backpack worn
  and the knife carried, the client `w` command equipped it.

Note for future tests: `Character::i_add()` auto-wields an item when no pocket
fits it and returns a location pointing at the weapon slot. Calling `wield()`
on that location therefore *unwields* it (it takes the engine's "already
wielding" branch). That is expected engine behaviour, not a netplay bug; the
earlier "wield does nothing" observation was this toggle.

Observed warnings (not caused by the netplay features):
- the tileset reports no `unknown` tile;
- the client avatar profile has no scenario and logs the generic-scenario
  fallback, and the mirror deserialize logs a second `setID` warning.

Not covered automatically: two clients at once, the visual death screen (it
waits for input; the code path is the single-player one), repair with real
tools/materials (the test only shortens the timer), and dynamic NPC speech
language.

## Multi-client

- Status: experimental, paused. The supported configuration is **host + one
  client**; two clients can join and move, but a second live window on one
  machine is not supported yet (the second client can stall in modal menus
  such as the player-interaction bump menu).
- Several sessions are supported by the networking layer; two clients were
  tested together (one resuming an existing avatar, one creating a new one).
  Both joined, saw each other as characters and moved.
- A client profile is untrusted data: avatar creation and status pushes run
  with debug prompts suppressed (`mp_quiet_remote_data`), because a modal
  debugmsg on the host stops every pump and freezes all sessions. A profile
  without a body is repaired only where possible; test profiles created
  headlessly must call `set_body()` before serializing.
- A newly bound session gets its first status (panel layout + character
  mirror) immediately after the bind, not on the next host action.
- Turn parity is strict by design: the world advances only when every player
  gave an order or is busy (sleep, activity, helping, AFK/long wait). A
  connected player who has not acted keeps the world waiting — that is the
  intended behaviour, not a freeze.
- For automated runs with an idle host, the host must declare long wait
  (`DDA_MP_TEST_HOST_LONGWAIT=1`), which counts as busy.

## Death and respawn

- The host announces a session avatar's death the moment `npc::die` runs
  (`host_notify_avatar_death`). Waiting for a world turn does not work: with a
  dead client the idle host stops turning.
- The client shows the single-player death screen (message log + scores), then
  resets its avatar and runs the normal character-creation flow for the same
  nickname. The session stays connected; no reconnect is needed.
- The avatar registry keeps one row per nickname: a respawn replaces the dead
  row, and the death-notified flag is cleared on every bind.
- Headless test runs (`DDA_MP_TEST_AVATAR_AUTO`) skip the two screens because
  they wait for input; creation still runs and the new avatar is verified.
- Dead avatars are skipped by the round gate, so the remaining players are not
  blocked.

## Missions

- Offer, accept, journal, turn-in list and turn-in run on the host; the client
  only picks from ids/names it is sent.
- Turn-in candidates merge the missions accepted in this session with the
  NPC's assigned list, so a resumed character can still turn missions in.
- Mission start/end effects are built around `get_avatar()` in single player;
  netplay temporarily points them at the session avatar
  (`set_mission_effects_player`), so items and rewards touch the right
  character instead of the host player.
- The turn-in path wraps the engine's end effects with debug-prompt
  suppression: a missing turn-in item must not open a modal on the host.
- Test hooks: `DDA_MP_TEST_MISSION_DONE` grants the known mission items,
  `DDA_MP_TEST_MISSION_AUTO` accepts the first offer,
  `DDA_MP_TEST_TURNIN_AUTO` picks the first turn-in entry, and the client
  test action `npc_real_turnin` asks for the list.

## Architecture notes

- Every remote player is a session id (`sid`). Avatar bindings, move pools,
  accepted missions, trade state, death notices and UI state are keyed by
  `sid`; adding another client does not need a new code path.
- Host code that would act "for the player" takes the session avatar
  explicitly (mission effects via `set_mission_effects_player`, status
  pushes, turn passes).

## Reconnect

- A session ends with its socket: the reader thread removes it from the
  host's session table, so an idle host no longer waits for a ghost player.
- A client that reconnects with the same nickname resumes its living avatar
  through the normal avatar list. Verified: client A killed, host logged
  `reader exit sid=1`; client B connected as sid=2 and got
  `avatar op ... kind=2` (resume) for the same avatar, mirror and panels up.

## Vehicle servicing

- Status, refuel, damaged-parts list and repair activity are implemented.
- A session avatar's activities now advance on every world turn: the bubble
  is anchored on the avatar, so map-bound activities (repair, construction,
  disassembly) are safe. A busy or sleeping session counts as active for the
  host's long-wait auto-pass, otherwise the world would never turn while the
  client is busy and the activity could never finish.
- Repair completion was verified with a shortened test activity
  (`DDA_MP_TEST_REPAIR_FAST`): `vehicle repair start ... time=5`, world turns,
  `vehicle repair done sid=1`.
- Repair duration still depends on the avatar's tools/materials and skills,
  exactly like single player; the automated test only shortens the timer.
