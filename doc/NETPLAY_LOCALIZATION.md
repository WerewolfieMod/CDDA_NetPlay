# Netplay localisation status

Last updated: v4.0a-202 (2026-11-15).

The client must render text in the language selected on the **client**, even when
the host runs a different language. The current implementation sends msgids,
mission type ids and item type ids over the wire and lets the client resolve
them from its own catalogs and game data.

## Implemented

- **Fork-only UI strings** (`fork_text`): the host records `msgid -> host text`
  pairs, turns a plain sentence back into `msgid + arguments`, and sends the
  `\x01` event protocol. The client translates the msgid with its own
  `fork_text` catalog (Russian table in `netplay.cpp`).
- **Engine messages** relayed with `add_msg`/`add_msg_if_player`: the existing
  `\x03` msgid + instance protocol is used. `capture_msgid_for` also recognises
  fork templates, and `mp_rebuild_message` falls back to the client's
  `fork_text` when a msgid is not in the game catalog.
- **Typed event arguments** (`\x02<T|F|I|M|N|D>id`): terrain, furniture, item,
  monster, mission name and mission description.
- **Missions**: offers/journal/turn-in names carry the mission type id (`\x07`);
  descriptions use `\x08`; statuses are sent as English msgids; accepted and
  completed messages use `\x02N<id>`.
- **Trade**: entry names carry the item type id (`\x06`); buy/sell messages use
  `\x02I<id>` for the item argument. The window header and prices are
  formatted on the client.
- **Dialogue options**: response labels carry the raw response translation
  msgid (`\x05`) when the engine exposes it; the fallback "Close" option is a
  msgid as well.

## Known gaps

1. **Dynamic NPC dialogue lines** (`dialogue::dynamic_line`) are still
   host-language. `dynamic_line_t` evaluates conditions and returns an already
   translated `std::string`; the selected raw msgid is not retained. Options
   and service messages are localised, but the spoken line itself is not.
2. **Dynamic engine text with host-rendered arguments** can still contain
   host-language names (for example some bionic/mutation activation messages,
   gun firing-mode names, or text built by activity actors). The sentence
   template is localised; the embedded dynamic value may not be.
3. **Trade item names** are resolved from `typeId`, so they show the client's
   base item name. Quality/condition modifiers that `item::tname()` would add
   are not reconstructed on the client.
4. **Translations with a JSON context** may not resolve through
   `to_translation(msgid)` because the wire format does not carry the context.
   Most mission names and dialogue options are context-free.
5. **Generated names** (NPC names, player nicknames, some map notes) are
   runtime strings, not translatable catalog entries.

## Where to fix the remaining gaps

- Store the selected raw `translation`/msgid alongside the `dynamic_line_t`
  result (core change in `dialogue.h` / `npctalk.cpp`), or run the dialogue
  line generator on the client mirror and stream only the topic/condition
  state.
- Extend the typed-argument protocol for more dynamic values (bionics, martial
  styles, vehicle parts, activity names).
- For trade modifiers, send a compact list of quality/condition data and
  rebuild the name on the client, or keep base names as the deliberate
  simplification.

## How to test

The client language is the normal `USE_LANG` option in its own `config/options.json`.
For an automated check, run the host in Russian and the client with
`USE_LANG = en`, then inspect `diag/netplay_inventory.log` for the
`clientlog:` lines and the `dialogue option:` / `trade entry:` / `missions:`
lines. Test scripts under `W:\Users\Sanyok\AppData\Local\Temp\opencode\mptest`
(`run_lang*.ps1`) switch the client language, run a scenario and restore the
config.
