# In-game Settings Overlay

**Status:** Added in v0.6.0
**Patch type:** No game-code change — a Direct3D-drawn panel
(`src/patches/settingsOverlay.cpp`) plus the shared panel plumbing
(`src/core/overlayPanel.cpp`)

---

## What it does

Opens over the game with **`Ctrl+Shift+O`** and lets the player rebind every
patch hotkey with the mouse and keyboard, instead of editing
`sh2-unofficial-patch.ini` by hand. Changes apply immediately and are written
back to the ini, so they survive a restart.

One row per hotkey: a label, the current binding as a button, and an `[x]` that
clears it. Click the button, press the new key (with any of Ctrl/Shift/Alt),
and it is bound; `Esc` cancels the capture, `Esc` again closes the panel. The
capture control itself is documented in [keybinding.md](keybinding.md) — this
doc covers the panel that hosts it.

| Row | Ini key | Feature |
| --- | --- | --- |
| Stop selected troops | `StopTroops` | [stop-troops-hotkey.md](stop-troops-hotkey.md) |
| Attack-move toggle | `AttackToggle` | [attack-move-hotkey.md](attack-move-hotkey.md) |
| Auto-market panel | `AutoMarketPanel` | [auto-market.md](auto-market.md) |
| This settings panel | `SettingsPanel` | this doc |

Adding a row is a table entry plus three accessors on the feature
(`xxxBinding` / `xxxSetBinding` / `xxxInstalled`) — see `SettingRow` in
`settingsOverlay.cpp`.

## Applying a rebind live

Each feature exposes its binding as a live variable, so a rebind takes effect on
the next frame without a restart. Two states the panel has to be honest about:

- **`xxxInstalled()` is false** when the hotkey was `None` in the ini at
  startup. The config layer's rule is that a disabled feature installs *nothing*
  (no frame-tick registration, no code hooks), and nothing re-runs an install at
  runtime — so the panel writes the new binding to the ini, marks the row with a
  `*`, and says it applies at the next launch. Keeping that rule intact is
  deliberate: it is what makes "off" mean zero footprint.
- **`hotkeySave` can fail** (game folder not writable). The footer then says the
  ini could not be written rather than silently pretending it saved. The live
  binding still changed, so the rebind works for this session.

Clearing a row to `None` disables its hotkey immediately. For the auto-market
row that only disables the *panel toggle* this session — the trade engine keeps
running with the thresholds it has, because the feature's full off-switch is an
install-time decision. Restarting applies the ini value properly.

**The panel's own row cannot be cleared** (`allowUnbind = false`): unbinding the
only way back into the panel would lock the player out with no in-game recovery.

## Conflicts

Two rows bound to the same key are drawn in red. Nothing prevents it — the
panel just tells the truth about it, because which one wins is decided by
window-subclass order, not by anything the player can see. There is
deliberately no check against the *game's* own bindings: the stock hotkeys are
hardcoded in a switch (CLAUDE.md, "Finding the Game's Own Keyboard Shortcuts")
and cannot be enumerated reliably.

## Two overlays, one window

The auto-market editor and this panel both subclass the game window and both
swallow input while they are up, so the order matters:

- Both call `SetWindowLongPtrW` from their **first render callback**, and
  callbacks run in registration order. `installSettingsOverlay()` is registered
  **last** in `registry.cpp`, so its procedure wraps the auto-market editor's
  and sees input first.
- The settings toggle is ignored while the auto-market editor is visible
  (`autoMarketOverlayVisible()`), so the two are never on screen together.
  While the settings panel is up it swallows everything, so the editor's toggle
  cannot fire underneath it.

`installSettingsOverlay()` must also run **after** every feature it lists,
because it seeds its widgets from their loaded bindings.

## Drawing (`src/core/overlayPanel.cpp`)

The panel plumbing was extracted so a second D3D-drawn panel did not mean a
second copy of it: a GDI bitmap the caller paints with ordinary GDI calls,
uploaded to a `D3DPOOL_MANAGED` texture (survives device reset) and drawn as one
alpha-blended pre-transformed quad from inside the `EndScene` hook, repainted
only when dirty. It also owns a small font cache and the client→panel
coordinate mapping that makes clicks land correctly windowed.

**The render path is float-free**, as the `EndScene` detour requires (it is a
mid-function hook that may have live x87 state — see
[auto-market.md](auto-market.md)). The quad's vertices are the only floats
involved and they are computed **once, in `overlayPanelInit`, at install time**;
the draw path copies the prebuilt array and never does arithmetic on it. That
is why `overlayPanelInit` must not be called from a render callback.

The auto-market editor was migrated onto the same plumbing once it had been
confirmed in-game, so there is one implementation rather than two: it kept
every layout constant, colour and hit region, and only lost its private copy of
the bitmap/texture/quad code (about 150 lines).

## Multiplayer compatibility

**Safe for version mismatch.** The panel draws, reads local input and writes a
local ini file; it touches no game entity and posts no command. Rebinding
changes which key issues an existing command, never what the command does — the
bound features keep their own classifications (see their docs).
