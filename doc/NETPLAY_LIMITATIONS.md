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
- Autoattack runs on the host: it picks the target with the stock rating
  (NPC weapon value, monster difficulty), melees an adjacent enemy or uses a
  reach attack. It deliberately avoids `avatar_action::autoattack`, which
  only accepts an `avatar`.

## Playtest 2026-11-15

Automated host+client scenarios (57 total, all completed without a crash or
timeout): movement and round gate, cycle-move, drop, haul, workout, thief,
grab/drag, vehicle status/refuel/parts/repair, zones, bionics, mutations, map,
sky, factions, bodygraph, medical, style, NPC dialogue, mission offers/accept,
trade buy/sell, language mismatch, long wait, auto-turn, help, diary, spellbook,
armor sorting, auto features/safe mode, suicide, use-wielded, mend and
autoattack.

The stock test world has no spellbook, so spell casting was verified with a
test-only hook that teaches `bio_nl_antigravity_jump_spell` to the session
character: `test spell cast ... ok=1`. The normal client path (spellbook list,
targeting cursor, host-side `spell::targetable_locations` validation) uses the
same host handler; a world with real spells was not part of the automation.

Use-wielded, mend and autoattack use test-only host hooks too: the session
character gets a directly assigned weapon, and a harmless hallucination is
spawned next to it. Observed results:
- `action: use_wielded uid key=97` and `action: mend uid key=109` on the client,
  then the host's inventory command path ran;
- `autoattack sid=1 targets=1`: the host selected the adjacent enemy and
  applied the melee/hallucination path.

Note for future tests: `npc::wield()` on a session avatar returned true but left
the weapon slot empty in this build, so the hook assigns the weapon through
`set_wielded_item()`. The client `w` (wield) path was not re-verified here.

Observed warnings (not caused by the netplay features):
- the tileset reports no `unknown` tile;
- the client avatar profile has no scenario and logs the generic-scenario
  fallback, and the mirror deserialize logs a second `setID` warning.

Not covered automatically: two clients at once, successful mission turn-in
with a completable objective, and repair completion with real tools/materials.

## Vehicle servicing

- Status, refuel, damaged-parts list and repair activity are implemented.
- Repair completion depends on the avatar's tools/materials and was not part of
  the automated test.
