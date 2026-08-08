# Overlay UI Scale (`overlayScalePercent` + measured layouts)

**Status:** Added in v0.6.1
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** No game-code change — a shared layout rule
(`src/core/overlayPanel.cpp`) followed by all three overlay panels

---

## Motivation

The three panels the patch draws over the game — the auto-market editor, the
end-of-game statistics and the settings panel — looked correct on the machine
they were authored on and wrong on plenty of others: text overflowing its input
boxes, labels running into the controls beside them, footer lines pushed past
the panel edge, and at low resolutions the bottom of a panel simply missing.

Two unrelated units had been mixed together.

**Fonts were sized against the desktop's DPI.** `overlayPanelFont` asked for a
point size and resolved it with `GetDeviceCaps(screen, LOGPIXELSY)`, so a 12pt
font came out 16px tall at 100% Windows scaling and 24px at 150%.

**Everything else was a hardcoded pixel constant.** Row pitch, field widths,
column positions and panel sizes were written down as the numbers that looked
right at 1080p on a 96-DPI display.

So the text grew with the player's Windows display scaling while the boxes
around it did not:

| Windows scaling | 12pt font | auto-market `ROW_H` was 19 |
| --- | --- | --- |
| 100% (96 DPI) | 16 px | fits |
| 125% (120 DPI) | 20 px | text taller than the row |
| 150% (144 DPI) | 24 px | badly clipped |

That is why two players at the same 1920x1080 could report completely different
results — the variable was their display scaling, not their resolution.

Independently of DPI, the fixed sizes did not suit small or large render
targets. The auto-market panel ended at y = 18 + 782 = **800px**, so at
1280x720 or 1024x768 its last rows and both help lines were off-screen; at
2560x1440 it took up half the relative area it did at 1080p.

---

## The rule

Panels are authored in **design pixels**: what the panel should measure at
1080p. Nothing is a real pixel until it has been through the scale, *including
the font*:

```c
int overlayScalePercent();                    // resolved once, per render target
int overlayScaleBy(int designPx, int percent);
HFONT overlayPanelFont(int designPoints, bool bold, int percent);
```

`overlayPanelFont` takes the point size the panel was authored at **on a 96-DPI
display** and derives the pixel height from that and the scale. The desktop DPI
is never consulted. The panel is drawn into the game's backbuffer, so the only
scale that means anything is the panel's own — a player's Windows scaling
setting no longer changes what the overlay looks like at a given resolution.

### Where the scale comes from

1. `[ui] Scale` from the ini, when it is a number in 50–300.
2. Otherwise from the backbuffer height: `renderH * 100 / 1080`, clamped to
   70–250. 1024x768 gives 71%, 1080p gives 100%, 1440p 133%, 4K 200%.

Anything else in `[ui] Scale` — absent, `Auto`, a typo — leaves it automatic,
following the ini's usual "a bad value falls back to the default" rule
([configuration.md](configuration.md)).

The value is cached against the backbuffer height and re-resolved when that
changes.

### Measured, not written down

Scaling alone still assumes every string fits the box it was drawn in. Anything
that must hold text is now measured with `overlayPanelTextWidth` /
`overlayPanelLineHeight` and sized from the result:

- **Auto-market** — the value cells fit `99999` plus padding; the name column
  fits the widest good name *and* the widest category header; the preset bar's
  buttons fit their captions; the panel is as wide as its widest line, footers
  included.
- **Settings** — the label column fits the longest setting name, and the
  keybind box fits both `Press any key...` and the longest bound key name, so
  the box can no longer be walked into by a label.
- **End-of-game statistics** — already measured its columns; its margins,
  padding and minimum column width are now scaled too.

Row heights follow the same idea from both directions:

```c
l.rowH = maxInt(overlayScaleBy(D_ROW_H, scale), line + rowGap);
```

The first term preserves the density the panel was authored at; the second
guarantees a row is never shorter than the line box of the text inside it —
which is what stops a cell clipping its digits when the font turns out taller
than the design assumed (a wide scale, or a machine that substitutes for Segoe
UI).

### Shrink to fit

A panel that still does not fit the render target measures itself again at the
scale that does, rather than being clipped:

```c
int fit = scale * available / needed;
```

Bounded to two correction passes, so the font cache sees a handful of sizes
rather than a sweep. The auto-market editor is the one that needs this — a
28-good list does not fit 800x600 even at the minimum automatic scale.

---

## Threading and the float rule

Layout is integer arithmetic and GDI measurement only, so it is safe anywhere.
`overlayPanelSetBounds` is not: it builds the panel quad, the one piece of float
work in the overlay stack, and the `EndScene` detour is a mid-function hook that
may interrupt code holding live x87 state (see
[architecture.md](../architecture.md)).

So the panels lay out on the way **up**, not at install:

| Panel | Where it lays out | Why that is safe |
| --- | --- | --- |
| Auto-market | its `WndProc`, on the toggle key | reached through `DispatchMessage`, an ordinary call boundary — the x87 stack is empty |
| Settings | its `WndProc`, in `setVisible(true)` | same |
| End-of-game statistics | `Win/LoseScreen::OnActivate` | a function-prologue hook, as before |

Install time is too early regardless: patches install from `DllMain`, long
before there is a device to read a backbuffer size from. Both panel overlays now
start at a placeholder 1x1 and are sized on their first show.

The render thread therefore **reads** a layout and never builds one, so the
panel bitmap and the panel bounds can never disagree. The trade is that a render
target which changes while a panel is open keeps the old scale until the panel
is closed and reopened.

---

## Files

- `src/core/overlayPanel.{h,cpp}` — the scale, the font cache (keyed on the
  resolved pixel height) and the measuring helpers
- `src/patches/autoMarket/overlay.cpp` — `buildLayout` / `ensureLayout` /
  `applyLayout`
- `src/patches/settingsOverlay.cpp` — `buildLayout` / `applyLayout`
- `src/patches/endgameStats/overlay.cpp` — `measureLayout` and the fit passes in
  `showStatsOverlay`
