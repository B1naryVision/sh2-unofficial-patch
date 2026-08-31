# In-game Settings Overlay

**Status:** Added in v0.6.0; extended to every ini option in v0.7.0
**Patch type:** No game-code change — a Direct3D-drawn panel
(`src/patches/settingsOverlay.cpp`) plus the shared panel plumbing
(`src/core/overlayPanel.cpp`) and two controls
(`src/core/keybindWidget.cpp`, `src/core/optionWidget.cpp`)

---

## What it does

Opens over the game with **`Ctrl+Shift+O`** and exposes everything
`sh2-unofficial-patch.ini` holds — every patch hotkey and every patch option —
as controls, instead of the player editing a text file between launches.
Changes apply to the running game as they are made and are written back to the
ini, so they survive a restart.

Two lists in one panel:

- **Shortcuts** — a label, the current binding as a button, and an `[x]` that
  clears it. Click the button, press the new key (with any of Ctrl/Shift/Alt),
  and it is bound; `Esc` cancels the capture, `Esc` again closes the panel. The
  capture control itself is documented in [keybinding.md](keybinding.md).
- **Options** — a slider for a number, a switch for a choice. Both apply live,
  which is the point of the section: values like a camera speed are chosen by
  feel, and feel cannot be read off a config file.

| Shortcut row | Ini key | Feature |
| --- | --- | --- |
| Stop selected troops | `StopTroops` | [stop-troops-hotkey.md](stop-troops-hotkey.md) |
| Attack-move toggle | `AttackToggle` | [attack-move-hotkey.md](attack-move-hotkey.md) |
| Auto-market panel | `AutoMarketPanel` | [auto-market.md](auto-market.md) |
| This settings panel | `SettingsPanel` | this doc |

| Option row | Ini key | Control | Feature |
| --- | --- | --- | --- |
| Camera zoom speed | `[camera] ZoomSpeedMultiplier` | slider, 0.1x–10.0x | [zoom-speed.md](zoom-speed.md) |
| Camera pan speed | `[camera] PanSpeedMultiplier` | slider, 0.1x–10.0x | [pan-speed.md](pan-speed.md) |
| Zoom out limit | `[camera] ZoomOutLimit` | Vanilla / Auto | [zoom-limit.md](zoom-limit.md) |
| Siege camp shortcut | `[interface] SiegeCampJumpOnSecondPress` | Jump at once / Open first | [siege-camp-hotkey.md](siege-camp-hotkey.md) |
| Shift-click recruits | `[recruitment] RecruitmentShiftMultiplier` | slider, Off–100 | [shift-click-recruitment.md](shift-click-recruitment.md) |
| Started games in the list | `[multiplayer] HideInProgressLobbies` | Show all / Hide started | [in-progress-lobbies.md](../bugs/in-progress-lobbies.md) |
| Panel size | `[ui] Scale` | slider, Auto–300% | [ui-scale.md](ui-scale.md) |

The `[preset:NAME]` sections are deliberately not here: they are auto-market
data rather than settings, and the auto-market editor already has a preset
picklist of its own.

Adding a shortcut row is a table entry plus three accessors on the feature
(`xxxBinding` / `xxxSetBinding` / `xxxInstalled`); adding an option row is a
table entry plus `get` / `set` / `failed` and, for a slider, the two formatters
that render its value for the screen and for the file. See `HotkeyRow` and
`OptionRow` in `settingsOverlay.cpp`.

## Values are integers end to end

Every option is carried as an `int`, including the ones the ini stores as
decimals — a multiplier of 1.5 is 15 tenths. That is not tidiness: the panel is
drawn from inside the `EndScene` detour, where float arithmetic is illegal (see
below), and a slider that computed positions from a `float` would put it there.
The conversion to a real `float` happens in the feature's setter, on the
`WndProc` path, where the x87 stack is empty.

The panel-size slider carries `Auto` as one extra position below its 50%
minimum, so a single control covers both "pick it for me" and a hand-set
percentage without a second widget.

## Applying an option live

The config layer's rule is that a feature switched off installs **nothing**, so
most option rows start with no hooks in the game at all. Turning one on from the
panel therefore has to install them, and that runs into a race: rewriting live
code from the `WndProc` thread can tear an instruction the game thread is
executing.

The fix is uniform across all six features: the setter records what the player
asked for and raises a pending flag, and the byte writing happens in a
**frame-tick callback** — on the game thread, in the main loop, where the game
is by definition not inside the function being patched. Each feature keeps the
same shape:

```text
xxxSetValue()   WndProc thread: store the value, raise the pending flag
xxxTick()       game thread:    write the bytes once, record success
```

**The callback is registered at install time even when the feature is off**, and
that is load-bearing rather than lazy-by-omission: `registerFrameTick` writes
the main-loop hook on its *first* registration, so a registration arriving from
the overlay's thread would rewrite six bytes of the main loop while the game
thread ran them — the very race the deferral exists to avoid. The cost of
registering unconditionally is one call per frame per feature that returns
immediately. `MAX_TICKS` in `frameTick.cpp` has to cover every tick-using patch
in the build for the same reason: an overflowed registration is dropped
silently, and the feature behind it would simply never switch on.

Turning a feature back **off never un-patches anything.** The hooks stay in and
are gated on a flag; the two multiplier patches instead get handed the stock
constant, so the patched instruction yields the value the stock code produced.
Un-patching would mean a second live rewrite for no benefit, and the gated path
costs a predictable branch per call.

Three states the panel has to be honest about:

- **`xxxInstalled()` is false on a shortcut row** when the hotkey was `None` in
  the ini at startup. Nothing re-runs a hotkey install at runtime, so the panel
  writes the new binding to the ini, marks the row with a `*`, and says it
  applies at the next launch. Keeping that rule intact is deliberate: it is what
  makes "off" mean zero footprint.
- **`xxxFailed()` is true on an option row** when a deferred install was
  rejected because the game's bytes did not match. The row is marked with a `!`
  and the footer says this copy of the game could not be patched — deliberately
  *not* "restart to apply", because a restart would hit the same byte check.
- **A write can fail** (game folder not writable). The footer then says the ini
  could not be written rather than silently pretending it saved. The live value
  still changed, so the setting works for this session.

## Feeling a camera change with the panel open

The panel swallows all keyboard input while it is up, so the game's keyboard
pan keys do nothing behind it. **The mouse still reaches the game**: the wheel
zooms (`WM_MOUSEWHEEL` is never swallowed) and pushing the cursor to a screen
edge pans, because edge panning is polled from the cursor position rather than
delivered as a message ([pan-speed.md](pan-speed.md)). Between them both camera
sliders can be judged without closing the panel, which is the whole point of
making them live.

Mouse movement *is* swallowed while a slider is being dragged, so the camera
does not edge-pan away underneath the drag.

## Sliders and the button that never comes up

A drag started in the panel is tracked with the raw coordinate mapping
(`overlayPanelMapPointRaw`), not the bounds-checked one, so a drag that leaves
the control still steers it — what a slider is expected to do.

The panel takes no mouse capture, so a button released outside the game window
never produces a `WM_LBUTTONUP` here, and the slider would otherwise stay glued
to the cursor. `handleDragMove` therefore checks `GetKeyState(VK_LBUTTON)` and
ends the drag itself when the button is no longer down.

Slider positions are pixel-exact at any normal panel size: over the 170
design-pixel track, all 100 positions of a 0.1x–10.0x multiplier and all 52
detents of the panel-size slider round-trip value → handle x → value unchanged
(verified at 100%, 70% and 250% scale). Only at the 40% shrink-to-fit floor,
where the track is 68 pixels and there are fewer pixels than values, can a click
land one step off the pixel it was aimed at — worst case 0.1x, and the value
text always shows what was actually set.

A slider is persisted to the ini when the drag **ends**, not on every step of
it; a switch is persisted on the click. The live value is applied on every step
either way, so the feedback is immediate while the file is written once.
Rebuilding the layout — which the panel-size row triggers — is deferred to the
same point, because a rebuild re-creates the widgets and would drop a drag the
player is still holding.

## Clearing a shortcut

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
involved, and they are computed **off the render thread**: once in
`overlayPanelInit` at install, then again by `overlayPanelSetBounds` each time
the panel is opened, which is where it is sized to the current resolution (see
[ui-scale.md](ui-scale.md)). The draw path copies the prebuilt array and never
does arithmetic on it. That is why neither call may be made from a render
callback — the panel's `WndProc`, reached through `DispatchMessage`, is the
safe place for both.

The auto-market editor was migrated onto the same plumbing once it had been
confirmed in-game, so there is one implementation rather than two: it kept
every layout rule, colour and hit region, and only lost its private copy of
the bitmap/texture/quad code (about 150 lines).

## Multiplayer compatibility

**Safe for version mismatch.** The panel draws, reads local input and writes a
local ini file; it touches no game entity and posts no command. Rebinding
changes which key issues an existing command, never what the command does.

The options it can switch on are a different matter: the panel inherits
whatever classification the feature it is toggling already has, and it does not
weaken any of them. Every option listed above is `solo` (see each feature's
doc) — turning `Shift`-click recruitment on mid-match, for instance, is exactly
as safe as having started with it on, because it still posts stock commands
through the networked command layer. A feature classified "requires all
clients" must not be given a row here without deciding what a mid-match toggle
means for the other players first.
