# Lobby Patch Panel

**Status**: Added in v0.8.0; playtested 2026-10-28 solo and with an unpatched player (shows in the
lobby, hides in the match, toggle works, unpatched row reads `0.7 or older`), and 2026-10-29
with four patched players, including the host's rule switches and the read-only view as a
joining player.
**Config**: `[hotkeys] LobbyPanel` (default `Ctrl+Shift+L`) shows or hides the panel; it
appears by itself whenever the lobby screen is up, multiplayer or Kingmaker.

---

## What it does

A small read-only panel in the top-right corner of the multiplayer lobby screen lists every
player, their patch version and who is hosting, with one status line underneath:

```text
 Unofficial Patch
 Halli        0.8.0          host
 TheSettler   0.7 or older
 Vanilla rules only - not everyone has it
```

It answers "can we use patch-only rules in this match?" before anyone presses Start, which
`!ver` cannot: in-game chat does not exist in the lobby (see
[patch-handshake.md](patch-handshake.md)). The data is the handshake's lobby snapshot; the
panel adds no network traffic of its own.

The toggle key is polled on the frame tick, the same way as the stop-troops hotkey. Hiding
the panel lasts until the game is restarted.

Clicks go through a window subclass (`lobbyPanelWndProc`), installed from the first render
callback like every panel's. Any click inside the panel is swallowed, so it never reaches
lobby controls underneath, and keys are never touched. The settings panel is installed later,
so its subclass sits outside this one and still takes all input while it is open.

Below the players it lists every [match rule](match-rules.md): `on` / `off` for the next
match, or `unavailable` when someone in the lobby cannot run it. The automatic fixes come
first and the host's options last, so the switchable rows sit together at the bottom. **The host clicks a rule to
switch it**; available rules get a button outline on the host's screen only. Everyone else's
panel is read-only, and `matchRuleToggle` refuses a click from anyone but the lobby owner, or
on a rule someone cannot run. The host's change shows at once and reaches the other players
through lobby data within about a second.

### Single player (Kingmaker)

The Kingmaker setup screen is the same `ScenarioLobbyScreen`, so the panel shows there too.
With no Steam lobby it used to read `Waiting for the Steam lobby...` with every rule
`unavailable`, because the common rule set is empty outside a lobby. It now uses the same
test the match rules use to switch every fix on (`matchRulesSinglePlayer`: no Steam lobby and
no other peer in the network session's peer table), so what the panel says is what the
match will do:

```text
 Unofficial Patch
 Single player - every fix is on
 Match rules
 Disband peasant fix          on (fix)
 ...
 Ballista auto-fire           multiplayer only
```

A fix whose sites did not verify on this machine still reads `unavailable`. The host options
read `multiplayer only` because single player runs fixes only. Playtested 2026-11-14 on the
Kingmaker setup screen.

Not yet confirmed: whether a host's multiplayer lobby screen can appear before its Steam
lobby exists. If it can, the host sees `Single player` for that moment instead of `Waiting`.
That is cosmetic, because the decision that matters is made when the match screen comes up.

---

## Finding the lobby screen

`ScenarioLobbyScreen` is the only `*Lobby*` class in the RTTI catalogue. Its single instance is
created at startup by a static initializer (VA `0x8dd154`):

```text
[0x12c5f28] = Dragonfly::Screen::instance().registerWindow("scenario lobby screen", ...)
```

and read back by the game's own accessor `0x69df50` (`mov eax, [0x12c5f28] / ret`).
`registerWindow` returns the `Pane*`, which is the object itself: the `Pane` vtable `0x9e2c54`
is at subobject offset `0`.

**"On screen" is `Pane::parent` (`+0x80`) being non-null**, the rule CLAUDE.md records for
every screen. Verified across three dumps of one session:

| Dump | `[0x12c5f28]` | `screen+0x80` |
| --- | --- | --- |
| Main menu | `0x5e10718` | `0` |
| Lobby (host) | `0x5e10718` | `0x5b8db70` |
| Match in progress | `0x5e10718` | `0` |

The Steam lobby itself stays alive during the match (see the handshake doc), so
`lobbyActive()` alone would leave the panel up over the game. The screen's parent pointer
is what restricts it to the lobby.

---

## Where the layout runs: a `draw` vtable swap

Sizing the panel to its content rebuilds the quad, which is float work. That is illegal on
the frame tick and in the render callback (both mid-function), so it needs a function
boundary that runs while the lobby is up:

- `Pane` vtable slot 4 is `draw`. Established by lining up `GameScreen`'s `Pane` vtable
  (`0x9c76ec`): its slot 4 is `0x5f4860`, which contains the known desync poll at `0x5f48b5`
  in `GameScreen::draw`.
- `ScenarioLobbyScreen` slot 4 (VA `0x9e2c64`) = `0x69f650`: thiscall, one 4-byte stack
  argument (`ret 0x4`), standard prologue. That vtable slot is its **only** reference: no
  direct `call` targets it, so a slot swap sees every call.

The patch replaces the slot with a `thiscall` C++ function that calls the stock `draw` and
then rebuilds the layout **only when the content changed**. The content is the player
snapshot, the status, the UI scale and the backbuffer size, compared with `memcmp`. The slot
holds a relocated absolute address, so the install compares it against
`base + 0x29f650`, never the on-disk bytes. It writes one dword and touches no code bytes.

Painting and the D3D texture stay on the render callback, which draws only while
`screen+0x80` is non-null. Visibility is lowered while a new layout is built, as every
panel here does.

| Item | RVA |
| --- | --- |
| Lobby screen pointer | `0xec5f28` |
| `draw` vtable slot | `0x5e2c64` (stock value `base + 0x29f650`) |

---

## Multiplayer Compatibility

**Safe for version mismatch.** The panel draws on the local screen only, reads data the
handshake already published, and touches no simulation or lobby state. Players without the
patch are unaffected.
