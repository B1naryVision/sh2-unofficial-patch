# Patch Configuration (`sh2-unofficial-patch.ini`)

**Status:** Added in v0.5.0
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** No game-code change — a config layer (`src/core/config.cpp`) read by the individual patches at install time

---

## Motivation

Some quality-of-life features are a matter of taste: the hotkeys may collide
with a player's habits, and the faster camera zoom is too fast for some
players (requested by BinaryVision). This adds an optional `.ini` file so
players can rebind or disable those features without a custom build.

---

## File location and lifecycle

The file is `sh2-unofficial-patch.ini`, placed **next to `d3d9.dll`** in the
game directory. The path is derived from the patch DLL's own module path
(`GetModuleFileNameA(g_patchModule)`), not the working directory, so it works
regardless of how the game is launched.

The file is entirely optional:

- No file → all defaults apply.
- Missing key → that key's default applies.
- Unparseable value → that key's default applies (a typo never disables a
  feature the user tried to configure; `None` must be explicit).

Everything is read once, in `DllMain` before the game's entry point runs
(`loadConfig()` is the first call in `applyUnofficialPatches()`). Only
kernel32 calls are used (`GetModuleFileNameA`, `GetPrivateProfileStringA`),
which are safe under the loader lock like every other install step. There is
no runtime re-read; changes take effect on the next game launch.

## Settings

| Section | Key | Default | Meaning |
| --- | --- | --- | --- |
| `[hotkeys]` | `StopTroops` | `H` | Stop selected troops ([stop-troops-hotkey.md](stop-troops-hotkey.md)) |
| `[hotkeys]` | `AttackToggle` | `Mouse4` | Toggle attack-move stance ([attack-move-hotkey.md](attack-move-hotkey.md)) |
| `[hotkeys]` | `AutoMarketPanel` | `` ` `` | Toggle the auto-market editor overlay ([auto-market.md](auto-market.md)); `None` disables the whole feature |
| `[camera]` | `ZoomSpeedMultiplier` | `1.0` | Camera zoom speed factor ([zoom-speed.md](zoom-speed.md)); `1.0` leaves the game code untouched |
| `[recruitment]` | `RecruitmentShiftMultiplier` | `20` | Units queued per shift-click in barracks/mercenary post/monastery/engineers guild/siege camp ([shift-click-recruitment.md](shift-click-recruitment.md)); `0` or `1` disables (no hooks installed) |

### Hotkey value format

Parsed case-insensitively by `configHotkey()`:

- Single letter or digit (`H`, `K`, `5`) — the VK code is the ASCII uppercase
  character
- `F1`–`F24`
- Named keys: `Space`, `Tab`, `Enter`, `Backspace`, `Insert`, `Delete`,
  `Home`, `End`, `PageUp`, `PageDown`, `Backtick` (`Grave`/`Tilde`, the
  `` ` `` / `~` key)
- Extra mouse buttons: `Mouse3` (middle), `Mouse4` (`VK_XBUTTON1`), `Mouse5`
  (`VK_XBUTTON2`)
- Raw hex virtual-key code (`0x48`) for anything not named above
- `None` / `Off` / `Disabled` — returns 0 and the patch skips its install
  entirely (no frame-tick registration, no hooks)

There is deliberately **no validation against the game's own key bindings** —
the stock game reads its bindings from its own config, and the patch cannot
reliably enumerate them. The template file documents that responsibility.

### ZoomSpeedMultiplier

Accepted range `0.1`–`10.0`. Out-of-range values, `NaN`, and the default
`1.0` all leave the zoom code untouched — the feature is strictly opt-in.
See [zoom-speed.md](zoom-speed.md) for how the multiplier reaches the game
code (an `fmul` against a float in the DLL's data section).

### RecruitmentShiftMultiplier

Accepted range `2`–`100`. `0` and `1` disable the feature (vanilla single
recruit, no code patched); a missing or invalid value falls back to the
default `20`, following the "a typo never disables a configured feature"
rule above. See
[shift-click-recruitment.md](shift-click-recruitment.md).

### Auto-market editor

Auto-market keeps each good's stock inside a `[min, max]` band using real Market
buy/sell commands at the good's true market price. It has **no per-good ini
config** — thresholds are set at runtime in the in-game editor (see below) and
reset every game, because what needs buying/selling changes during a match. Its
only setting is the toggle hotkey `[hotkeys] AutoMarketPanel` (default `` ` ``);
set that to `None` to disable the whole feature (no D3D render hook, no engine).

Open the editor with the hotkey, then per good set **Min** (auto-buy below it)
and/or **Max** (auto-sell above it); `0` in a field ignores that direction.
Select a cell with the mouse or arrow keys and type a value; Backspace/Delete
edit, Esc closes. Trades use the good's real price and go through the game's own
networked command layer, so it is multiplayer-safe and behaves exactly like
clicking Buy/Sell by hand. See [auto-market.md](auto-market.md).

## Distribution

A commented template `sh2-unofficial-patch.ini` lives in the repo root and is
included in the release zip by the GitHub release workflow. The DLL never
writes the file itself.
