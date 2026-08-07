# Keybinding (`Hotkey` + in-game rebind widget)

**Status:** Added in v0.6.0
**Patch type:** No game-code change — a shared component (`src/core/hotkey.cpp`,
`src/core/keybindWidget.cpp`) used by the hotkey patches and the overlays

---

## Motivation

Rebinding a hotkey meant closing the game, finding `sh2-unofficial-patch.ini`
and typing a key name — unintuitive for players who never edit config files.
This is the groundwork for an in-game settings overlay: a binding type that
survives a round trip through the ini in readable form, and a rebind control
the overlays can drop into a panel.

Two pieces, deliberately separate:

- **`src/core/hotkey.{h,cpp}`** — the `Hotkey` value type (virtual-key code +
  Ctrl/Shift/Alt bits), its string form, its ini load/save, and the two
  "is it pressed" predicates. No UI, no Direct3D.
- **`src/core/keybindWidget.{h,cpp}`** — the click-then-press control: capture
  state machine, input swallowing, and GDI drawing into a host panel's bitmap.
  Owns no window and no surface.

## The `Hotkey` type

```cpp
struct Hotkey {
    int vk;   // virtual-key code; 0 = unbound
    int mods; // HK_CTRL | HK_SHIFT | HK_ALT
};
```

Two predicates, one per input path, because the two paths read modifier state
differently:

| Function | Use from | Reads state with |
| --- | --- | --- |
| `hotkeyHeld(hk)` | frame-tick polling (stop-troops, attack toggle) | `GetAsyncKeyState` |
| `hotkeyMatchesKeyDown(hk, vk)` | a `WM_KEYDOWN` in a window procedure (auto-market toggle) | `GetKeyState` |

**The modifier match is exact.** `H` does not fire while Ctrl is held; that is
what makes `H` and `Ctrl+H` bindable to different things. It is also a small
behaviour change for the pre-existing hotkeys, which previously ignored
modifier state entirely. A binding *on* a bare modifier key excludes its own
bit from the comparison, so `0xA2` (left Ctrl) is not self-defeating.

## String format

`hotkeyParse` is a superset of the old `configHotkey` parser — every value the
shipped ini template could contain still parses — plus modifiers and a wider
key-name table. It is case-insensitive and ignores whitespace, so
`Ctrl + Shift + F5` and `ctrl+shift+f5` are the same value.

```text
[modifier +]... <key>     Ctrl+Shift+F5, Alt+Mouse4, Ctrl+Alt+G
None | Off | Disabled     unbound
```

Key names: a letter or digit; `F1`–`F24`; `Numpad0`–`Numpad9`; `Mouse3`/
`Mouse4`/`Mouse5`; `Space`, `Tab`, `Enter`, `Backspace`, `Insert`, `Delete`,
`Home`, `End`, `PageUp`, `PageDown`, `Escape`, `Up`/`Down`/`Left`/`Right`,
`Backtick`, `Semicolon`, `Plus`, `Comma`, `Minus`, `Period`, `Slash`,
`LBracket`, `Backslash`, `RBracket`, `Quote`, `CapsLock`, `ScrollLock`,
`NumLock`, `Pause`, `PrintScreen`, `NumpadPlus`/`Minus`/`Multiply`/`Divide`/
`Decimal`; or a raw hex code (`0x48`). Aliases (`Grave`, `Tilde`, `Esc`,
`Return`, `Control`, `Equals`) parse but are never written back.

Malformed values return `false` and leave the caller's default untouched,
keeping the config layer's rule that a typo never silently disables a feature —
`None` must be explicit. `Ctrl+`, `Ctrl` alone, `G+Ctrl` and `F25` are all
rejected.

### Two writers, one parser

`hotkeyToString` produces the **canonical ini form** (`Ctrl+Shift+F5`) and is
guaranteed to round-trip: every name it can emit, `hotkeyParse` accepts. Keys
with no canonical name fall back to `0x%02X` rather than to a prettier name
that would not parse back.

`hotkeyDisplayName` produces the **UI form** (`Ctrl + Shift + F5`) and may fall
back to `GetKeyNameTextA` for unnamed keys, which is localized and
layout-specific. That name is for drawing only — **never write it to the ini.**

The capture path refuses virtual-key codes outside `1..254` (the driver reports
`0xFF` for keys it cannot identify), so the widget cannot produce a binding
that `hotkeyToString` would write and `hotkeyParse` would then reject.

## Ini storage

`hotkeyLoad(section, key, default)` and `hotkeySave(section, key, hk)` read and
write `sh2-unofficial-patch.ini` through the config layer's resolved path
(`configIniPath()`), so they follow the DLL's own directory like every other
setting. Saving uses `WritePrivateProfileStringA` and then flushes the ini
write cache, so the file is correct on disk even if the game is killed rather
than closed.

Two consequences worth knowing:

- **`hotkeySave` returns `false` when the file cannot be written** — a game
  directory under `Program Files` without write rights, or no resolved ini
  path. A settings UI must surface that rather than assume it saved.
- **Windows rewrites the whole key line**, so a trailing comment on that one
  line is lost. Comments elsewhere in the file survive untouched.

## The widget

The host overlay owns the panel; the widget is a value it draws and feeds
messages to. `src/patches/settingsOverlay.cpp` is the real host — see
[settings-overlay.md](settings-overlay.md). Wiring it into a panel that already
has a GDI bitmap and a subclassed `WndProc` is three calls:

```cpp
static KeybindWidget s_stopKey;

// install: panel-local box
keybindWidgetInit(s_stopKey, hotkeyLoad("hotkeys", "StopTroops", def), 196, 90, 330, 109);

// repaint (inside the panel's bitmap render, host font already selected)
keybindWidgetDraw(s_stopKey, memDC, "Stop troops:");

// WndProc, before the panel's own handling
if (keybindWidgetOnMessage(s_stopKey, msg, wparam, lparam)) {
    return 0; // swallowed
}

if (msg == WM_LBUTTONDOWN && keybindWidgetHit(s_stopKey, lx, ly)) {
    keybindWidgetBeginCapture(s_stopKey);
    return 0;
}

// after handling, persist an accepted rebind
if (keybindWidgetTakeChanged(s_stopKey)) {
    hotkeySave("hotkeys", "StopTroops", s_stopKey.binding);
}
```

`keybindWidgetOnMessage` returns `false` when the widget is not listening, so
the host can call it unconditionally.

### Capture rules

| Input while listening | Result |
| --- | --- |
| Any ordinary key | bound, with the modifiers held at that moment |
| Bare Ctrl / Shift / Alt | swallowed, keeps listening (it is half a combination) |
| `Esc` | cancels; the binding is unchanged |
| Middle / Mouse4 / Mouse5 | bound (with modifiers) |
| Left / right click | cancels — those stay usable for the rest of the panel |
| Losing window focus | cancels, and the message still reaches the game |

Only one widget listens at a time; starting a capture cancels any other.

### Input swallowing

While listening, **every** keyboard and mouse message returns `true` and the
host must return `0` — key-ups, `WM_CHAR`/`WM_SYSCHAR` echoes (the latter would
otherwise beep on an Alt combination), button-ups, double-clicks, wheel and
motion. The game's own shortcuts arrive as UI events through the window
procedure (see CLAUDE.md, "Finding the Game's Own Keyboard Shortcuts"), so
swallowing there is enough to keep them from firing behind the prompt.

**The patch's own frame-tick hotkeys are the exception, and it is not
optional.** Stop-troops and the attack toggle poll `GetAsyncKeyState` on the
sim thread and never see a window message, so swallowing cannot reach them —
pressing `H` to bind it would also stop your troops. Both check
`keybindCaptureActive()` before acting. Any future polling hotkey must do the
same; that is the whole reason the flag is exported.

Focus loss cancels capture for the same reason: a stranded prompt would keep
those hotkeys muted with no visible cause.

## Drawing

`keybindWidgetDraw` renders into the host's DC with the host's selected font,
in the auto-market editor's palette so mixed panels look like one control set:
a filled, framed box with the binding name centred, the caption right-aligned
just left of it, and a yellow border plus "Press any key..." while listening.

It does no float arithmetic. The overlays draw from inside the `EndScene`
detour, which is a mid-function hook that may have live x87 state — see
[auto-market.md](auto-market.md) for that constraint.

## Multiplayer compatibility

**Safe for version mismatch.** Client-local UI and config: the widget draws,
reads input and writes an ini file, and touches no game entity. Rebinding
changes *which* key issues an existing command, never what the command does —
the bound features keep their own classifications
([stop-troops-hotkey.md](stop-troops-hotkey.md),
[attack-move-hotkey.md](attack-move-hotkey.md),
[auto-market.md](auto-market.md)).
