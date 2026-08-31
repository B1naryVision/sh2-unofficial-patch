# Patch Configuration (`sh2-unofficial-patch.ini`)

**Status:** Added in v0.5.0; extended in v0.6.0 (key combinations, siege camp, recruitment and auto-market settings), v0.6.1 (overlay scale) and v0.7.0 (every setting editable in game)
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** No game-code change — a config layer (`src/core/config.cpp`) read by the individual patches at install time

---

## Motivation

Some quality-of-life features are a matter of taste: the hotkeys may collide
with a player's habits, and the faster camera zoom is too fast for some
players (requested by BinaryVision). This adds an optional `.ini` file so
players can rebind or disable those features without a custom build.

---

Every setting below can also be changed **in game** with `Ctrl+Shift+O`, which
applies it to the running game and writes it back into this file — see
[settings-overlay.md](settings-overlay.md). Editing the file by hand still
works and is still the only way to define auto-market `[preset:NAME]`
sections.

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
no runtime re-read: edits made to the file while the game is running take
effect on the next launch. (The settings overlay is the one writer — it changes
the live value and the file together, so it needs no re-read.)

**A setting switched on from the overlay installs its patch on the frame tick**,
not at the moment of the click: the config layer's "off means nothing is
installed" rule leaves those features with no hooks in the game, and writing
live code from the overlay's thread could tear an instruction the game thread is
executing. Switching one back off never un-patches — the hooks stay in and are
gated on a flag. See [settings-overlay.md](settings-overlay.md).

`[ui] Scale` is the one exception to the timing, not to the lifecycle: it is
read on first use rather than in `DllMain`, because nothing can be sized until
the game has created a device to read a backbuffer size from
([ui-scale.md](ui-scale.md)). It is still read once and never re-read.

## Settings

| Section | Key | Default | Meaning |
| --- | --- | --- | --- |
| `[hotkeys]` | `SettingsPanel` | `Ctrl+Shift+O` | Open the in-game settings panel, which rebinds the keys below and writes them back here ([settings-overlay.md](settings-overlay.md)); `None` removes the panel |
| `[hotkeys]` | `StopTroops` | `H` | Stop selected troops ([stop-troops-hotkey.md](stop-troops-hotkey.md)) |
| `[hotkeys]` | `AttackToggle` | `Mouse4` | Toggle attack-move stance ([attack-move-hotkey.md](attack-move-hotkey.md)) |
| `[hotkeys]` | `AutoMarketPanel` | `` ` `` | Toggle the auto-market editor overlay ([auto-market.md](auto-market.md)); `None` disables the whole feature |
| `[camera]` | `ZoomSpeedMultiplier` | `1.0` | Camera zoom speed factor ([zoom-speed.md](zoom-speed.md)); `1.0` leaves the game code untouched |
| `[camera]` | `ZoomOutLimit` | `Vanilla` | `Auto` zooms out to the furthest usable distance for the map and camera angle ([zoom-limit.md](zoom-limit.md)); `Vanilla` leaves the game code untouched |
| `[camera]` | `PanSpeedMultiplier` | `1.0` | Camera pan speed factor for the keyboard pan keys and mouse edge panning ([pan-speed.md](pan-speed.md)); `1.0` leaves the game code untouched |
| `[interface]` | `SiegeCampJumpOnSecondPress` | `1` | The siege camp hotkey (`J`) opens its panel on the first press and only moves the camera on a second press ([siege-camp-hotkey.md](siege-camp-hotkey.md)); `0` restores the stock one-press behaviour (no hooks installed) |
| `[ui]` | `Scale` | `Auto` | Size of the patch's overlay panels in percent ([ui-scale.md](ui-scale.md)); `Auto` derives it from the game's resolution, `50`–`300` sets it by hand, anything else falls back to `Auto` |
| `[multiplayer]` | `HideInProgressLobbies` | `1` | Leave games that have already started out of the multiplayer game list ([in-progress-lobbies.md](../bugs/in-progress-lobbies.md)); `0` restores the stock list (no hooks installed) |
| `[recruitment]` | `RecruitmentShiftMultiplier` | `20` | Units queued per shift-click in barracks/mercenary post/monastery/engineers guild/siege camp ([shift-click-recruitment.md](shift-click-recruitment.md)); `0` or `1` disables (no hooks installed) |

### Hotkey value format

Parsed case-insensitively by `hotkeyParse()` in `src/core/hotkey.cpp`, which
also ignores whitespace (`Ctrl + Shift + F5` = `ctrl+shift+f5`). Full list and
rules in [keybinding.md](keybinding.md):

- Optional `Ctrl` / `Shift` / `Alt` prefixes, joined with `+`
  (`Ctrl+Shift+F5`, `Alt+Mouse4`). The match is **exact**: a plain `H` does not
  fire while Ctrl is held
- Single letter or digit (`H`, `K`, `5`) — the VK code is the ASCII uppercase
  character
- `F1`–`F24`, `Numpad0`–`Numpad9`
- Named keys: `Space`, `Tab`, `Enter`, `Backspace`, `Insert`, `Delete`,
  `Home`, `End`, `PageUp`, `PageDown`, `Escape`, the arrow keys,
  `Backtick` (`Grave`/`Tilde`, the `` ` `` / `~` key), the punctuation and
  numpad-operator keys, and the lock keys
- Extra mouse buttons: `Mouse3` (middle), `Mouse4` (`VK_XBUTTON1`), `Mouse5`
  (`VK_XBUTTON2`)
- Raw hex virtual-key code (`0x48`) for anything not named above
- `None` / `Off` / `Disabled` — leaves the hotkey unbound and the patch skips
  its install entirely (no frame-tick registration, no hooks)

A value the parser does not understand falls back to the default, per the rule
above — `Ctrl+` and a bare `Ctrl` are both rejected as incomplete.

There is deliberately **no validation against the game's own key bindings** —
the stock game reads its bindings from its own config, and the patch cannot
reliably enumerate them. The template file documents that responsibility.

### ZoomSpeedMultiplier

Accepted range `0.1`–`10.0`. Out-of-range values, `NaN`, and the default
`1.0` all leave the zoom code untouched — the feature is strictly opt-in.
See [zoom-speed.md](zoom-speed.md) for how the multiplier reaches the game
code (an `fmul` against a float in the DLL's data section).

### PanSpeedMultiplier

Accepted range `0.1`–`10.0`. Out-of-range values, `NaN`, and the default
`1.0` all leave the pan code untouched — the feature is strictly opt-in. It
covers both the keyboard pan keys and pushing the mouse against a screen
edge, and scales the acceleration ramp along with the top speed. See
[pan-speed.md](pan-speed.md).

### ZoomOutLimit

Accepts `Vanilla` (default, game code untouched) or `Auto`. Any other value,
and a missing key, leave the feature off. There is no number to tune: `Auto`
derives the limit from the map's extent, the viewport aspect and the camera's
current pitch, against a calibration measured from two process dumps, and never
reduces the limit below the game's own. See [zoom-limit.md](zoom-limit.md).

### RecruitmentShiftMultiplier

Accepted range `2`–`100`. `0` and `1` disable the feature (vanilla single
recruit, no code patched); a missing or invalid value falls back to the
default `20`, following the "a typo never disables a configured feature"
rule above. See
[shift-click-recruitment.md](shift-click-recruitment.md).

### SiegeCampJumpOnSecondPress

`1` (default) splits the stock siege camp hotkey (`J`) into two steps: the first
press opens the siege camp panel on your first siege camp without moving the
camera, and a press while the panel is up travels there. `0` leaves the game
code untouched (no hooks installed). Any other value is treated as `1`, per the
"a typo never disables a configured feature" rule above. See
[siege-camp-hotkey.md](siege-camp-hotkey.md).

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

**Presets.** Named threshold sets can be defined as `[preset:NAME]` sections and
loaded from the editor's picklist (PageUp/PageDown to choose, Enter or the Apply
button to load). Each line is `Good = min:X, max:Y` (either part optional) using
the same good names the editor shows; applying a preset is **replace-all** (goods
not listed are cleared to `0`). Example:

```ini
[preset:War]
Bread = max:10
Ale   = min:5
Swords = min:5
Armour = min:5
Wood  = min:200
```

The template ships with one example preset (Fast Estate); add, edit or remove
sections freely.

## Distribution

A commented template `sh2-unofficial-patch.ini` lives in the repo root and is
included in the release zip by the GitHub release workflow. The DLL never
writes the file itself.
