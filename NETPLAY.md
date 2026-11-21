# DDA NetPlay — co-op multiplayer for Cataclysm: Dark Days Ahead

A fork of Cataclysm: DDA (experimental) that adds a working co-op mode: one
player opens the world they are already playing, another joins over TCP and
plays their own character in that same world. No separate server build, no
second copy of the save, no manual world sync.

```text
Host:    load or create a world -> ESC -> "Network: open world for co-op" (port 25565)
Client:  main menu -> "Join Network World"  (or: cataclysm-tiles.exe --mpclient <host[:port]>)
```

* **Status:** playable host + one client on LAN/localhost; the networking layer
  is already session-keyed, so adding more clients does not need a redesign.
* **Platform:** Windows x64 (tiles), MSVC + vcpkg static build. The socket layer
  has POSIX branches, but only Windows is tested.
* **Base:** Cataclysm: DDA experimental, current `master` as of the fork.
* **Version:** `ddamp-v4.0a-223`.

---

## What it is

Cataclysm is a turn-based game where the world advances in turns. DDA NetPlay
keeps that model instead of turning the game into real time:

* the **host** is the authority — it runs the real game, the map, the NPCs,
  monsters, weather, saving and the world clock;
* a **client** is a thin, fully playable front end — it renders the host's view
  around its own character, sends keypresses and UI choices, and receives
  character state, inventory, minimap and message-log lines;
* a remote player exists in the host world as a normal NPC **avatar** owned by
  that session, so monsters, NPCs, vehicles and map code treat them as a
  creature that is really there.

Turns are **strict**: the world advances only after every player has given an
order or is legitimately busy (sleeping, doing an activity, helping someone,
dead and respawning, or in long-wait/AFK mode). A player who is simply idle
holds the world, exactly like an idle single-player character holds its own
turn. There is no hidden auto-turn that plays for a connected player.

---

## What works

### World and characters

* Joining, creating a character on the client, resuming an existing avatar by
  nickname, and keeping the session through death.
* Shared world state: time, weather, map, monsters, NPCs, vehicles, items.
* Split regions: a player who walks far from the host gets their own map bubble
  instead of dragging everyone into one screen.
* Own character sheet, sidebar, inventory, overmap minimap, message log, chat.
* Death: the normal single-player death screen, then the normal character
  creation flow — the session stays connected, no reconnect needed.
* Reconnect after a drop resumes the same living avatar.
* Silent-session heartbeat: an idle client is pinged; a client that stops
  answering is announced in chat and removed from the turn gate instead of
  freezing the world.

### Actions

* Movement, terrain interaction, doors, stairs, vehicles (board, drive, leave).
* Melee, auto-attack, aiming, firing, throwing.
* Inventory: pick up (including choosing a tile), wear, wield, take off, drop,
  give, reload, unload, mend, apply, read, eat, pocket settings, favorites,
  letter reassignment.
* Crafting and recrafting, construction, butchering, hauling and autohaul,
  sleep.
* NPC dialogue, trade, mission offers and mission turn-in, including rewards
  paid to the session character.
* Vehicle status, refuel, damaged-parts list and repair as a real timed
  activity.
* Spellbook with the host's targeting validation; bionics and mutations as
  read-only lists; zones as a read-only list.
* Helping another player with their activity, long-wait/AFK mode, suicide.

### Language

Each player reads in their own language. The wire protocol carries message ids
and typed arguments (item, monster, terrain, furniture, mission, …) and the
client resolves them from its own catalogs, so a Russian host and an English
client both see their own text. Fork-specific UI strings use the same
mechanism. The only deliberate gap is dynamically generated NPC speech, which
the engine produces as already-translated host text.

---

## What is intentionally different from single player

These are design choices, documented rather than bugs:

* **Saving and loading stay on the host.** The world and the save belong to the
  host; a client never owns a save.
* **Debug tools stay on the host.** The debug menu can rewrite the shared world.
* **Global options stay on the host.** Safe mode, auto features, auto travel and
  similar switches are world options, not per-character state; letting a client
  toggle them would change the host's game. Per-character thief mode is
  supported.
* **One client for now.** The protocol, state tables and turn gate are keyed by
  session id (`sid`), so a second client is an extension, not a rewrite; the
  second live window is not supported yet.
* **Diary is session-local.** Diary data lives on `avatar`, and remote players
  are NPCs, so the diary is not persisted after leaving.
* **Disassembly and zone editing are not exposed to clients yet.**

---

## How it works

### Authority and view

The host builds a self-contained frame for each session around that session's
avatar and streams it, along with the rendered sidebar, character sheet,
inventory rows, minimap and message-log events. Framing uses a compact binary
protocol with compressed payloads. The client renders frames locally and sends
input events, UI choices and typed requests upstream. The host never trusts the
client: profiles are validated, targets are re-checked against the real map, and
world state always changes on the host only.

### Turns

The host tracks a move pool per session. A world turn ends when the host and
every live, non-busy session have spent their order; busy sessions count as
done because their activity or sleep advances on world turns. The gate is
strict on purpose — it is what makes the shared world feel like a tabletop
session rather than two games running in parallel.

### Sessions

Everything per player — avatar binding, move pool, accepted missions, trade
state, UI state, death notice, activity — is keyed by `sid`. A session ends with
its socket; a reconnect with the same nickname binds a new `sid` to the same
avatar. This is why extra clients do not require new code paths.

### Diagnostics

Netplay ships with its own diagnostics folder (`%TEMP%\opencode\ddadiag` by
default) rather than writing into the game directory. `DDA_MP_DIAG=0` disables
it, `DDA_MP_DIAG=<dir>` redirects it.

---

## What changed in the base game

The netplay layer is deliberately kept in `src/netplay`, but a turn-based game
does not expose remote characters or remote menus by itself, so the fork also
touches the engine:

* **New module:** `src/netplay/` (~19k lines) — transport, protocol, host world
  glue, client UI, region/bubble handling, message capture and diagnostics.
* **Rendering and input:** screen frames, sidebar and panel widgets, tiles
  drawing, mouse/keyboard routing, chat and message-log relaying.
* **Turns:** `do_turn.cpp` / `game.cpp` hooks for the round gate, idle pumping
  and per-session input.
* **Characters:** session avatars as NPCs, status mirrors, activity ticks on the
  avatar, mission effects applied as the avatar instead of the host.
* **World:** bubble/split-region map views, lighting and creature tracking per
  view, vehicle and terrain interaction paths that accept a session character.
* **UI flows:** inventory, pickup, crafting, construction, butchery, trade,
  missions, NPC dialogue, aiming, medical and bodygraph views routed through
  session-aware handlers.
* **Messages and localization:** msgid-based relaying, typed arguments, fork
  string catalog, per-client language resolution.
* **Safety:** prompt suppression for untrusted remote data, so a modal debug
  prompt triggered by a malformed profile cannot freeze every session.

The diff against upstream `master` is about **221 commits, 76 files and +20k
lines**; the bulk is the new netplay module.

---

## Development approach

1. **Single player stays sacred.** Every feature is added behind netplay checks;
   a normal game must behave exactly as before. Changes are built and the
   single-player paths are kept free of new mandatory state.
2. **Parity first, shortcuts later.** A client action either goes through the
   real engine path on the host or is not offered. Test hooks that shorten
   timers or grant items exist only to make automation possible, never to make a
   feature "work" in a different way.
3. **Authority on the host.** The client sends intent; the host validates and
   executes. That keeps saves, rules and cheat resistance in one place.
4. **Typed protocol, versioned by feature.** Frames carry explicit payloads and
   new capabilities get new opcodes, so old and new code paths stay readable and
   regressions are easy to bisect.
5. **Localize at the edge.** Send ids and arguments, not sentences, so the
   receiving client decides the language.
6. **Automate everything that can be automated.** A headless host + headless
   client harness runs scripted scenarios, checks a diagnostics log and asserts
   on concrete lines (`round complete`, `vehicle repair done`, `avatar op …
   kind=2`, …). There are test-only environment hooks for avatar creation,
   movement, menus, missions and timers.
7. **Test the uncomfortable link.** Long runs go through a local TCP relay that
   adds latency and can silently swallow one direction, so turn pacing,
   reconnect and the heartbeat are exercised under conditions a localhost test
   would never produce.
8. **Document the edges.** Known gaps and deliberate omissions live in
   `doc/NETPLAY_LIMITATIONS.md` and `doc/NETPLAY_LOCALIZATION.md` instead of
   being rediscovered later.

Current automated coverage includes movement and the round gate, combat and
auto-attack, inventory actions, vehicles, crafting, construction, trade,
missions and turn-in, NPC dialogue, death/respawn, reconnect, latency runs and
the silent-session heartbeat.

## Project workflow

- `master` is the approved line: what players and the launcher use.
- `experimental` is the working branch: every change lands there first.
- After review, `experimental` is merged into `master` (fast-forward when
  possible).
- GitHub Actions inherited from upstream are removed; builds and automated
  playtests run locally, so no CI minutes or third-party runners are involved.

---

## Building

Windows x64, Visual Studio 2022 Build Tools and vcpkg:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=D:\vcpkg
msbuild msvc-full-features\Cataclysm-vcpkg-static.sln /t:Cataclysm-vcpkg-static /p:Configuration=Release /m:8
```

The result is `cataclysm-tiles.exe` in the repository root; copy it into a game
folder that has `data/`, `gfx/`, `sound/`, `font/` and `lang/`.

---

## Known limitations

* Dynamic NPC dialogue lines are spoken in the host's language.
* A client cannot save or load; the world save is the host's.
* Only one client is supported at a time for now.
* Debug tools are host-only.
* Disassembly and zone editing are not available to clients.
* Some dynamic engine values inside otherwise-localized sentences may still be
  host-language (see `doc/NETPLAY_LOCALIZATION.md`).

---

## Credits and license

Built on [Cataclysm: Dark Days Ahead](https://github.com/CleverRaven/Cataclysm-DDA)
by CleverRaven and contributors; the fork follows the upstream license
(CC-BY-SA 3.0 for content, code under the upstream terms — see `LICENSE.txt`).
Netplay code and documentation are part of this fork.
