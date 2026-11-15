# Netplay feature limits and notes

Last updated: v4.0a-207 (2026-11-15).

## Mission rewards

- Turn-in mirrors `talk_function::mission_reward`: the NPC's
  `op_of_u.owed` grows by the mission value.
- Generic mission reward is also added to the session barter credit; buying
  spends both the credit and the NPC's `owed`.
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

## Vehicle servicing

- Status, refuel, damaged-parts list and repair activity are implemented.
- Repair completion depends on the avatar's tools/materials and was not part of
  the automated test.
