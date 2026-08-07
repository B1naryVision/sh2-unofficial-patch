# End-of-Game Statistics

**Status**: Implemented. Some known edge cases under investigation (see below).

## Symptom

The victory and defeat screens show only a background image and navigation buttons.
No statistics are displayed.

## Root Cause

The game's `WinScreen` and `LoseScreen` classes both have a virtual `OnActivate`
method (vtable slot 2) that sets up the UI. No stats are ever queried or rendered.
A `StatesReportSubPanel` class exists but is used only in the in-game reports panel,
not on the end-game screens.

## What This Patch Does

Hooks `WinScreen::OnActivate` and `LoseScreen::OnActivate`. When either fires, a
translucent panel is drawn over the game showing per-player statistics:

- Player color and title
- Gold balance, honour total, popularity
- Army size (troops and siege separately)
- Per-source income (trade, castle tax, estate tax)
- Per-source honour (feasting, dancing, monastery, jousting, church, granary, statues, crime)
- Cumulative units recruited by type (tracked via unit spawn hook throughout the game)

The overlay is dismissed (and all tracking state reset) when the player returns
to the main menu, hooked via `MainMenuScreen::OnActivate`. The Win/LoseScreen
scalar destructors are also hooked as a teardown safety net, but **they do not
fire on normal screen exit** — a main-menu minidump showed both endgame screen
objects still alive at the menu, so the game creates its screens once and keeps
them for the process lifetime. Up to 8 players are shown. Numbers are
right-aligned, the panel is sized from measured text (Segoe UI, DPI-scaled) and
centred in the backbuffer, and it never grows wider than the screen.

### Drawn in the frame, not in a window

The panel used to be a `WS_EX_LAYERED` popup owned by the game window. It is now
an `OverlayPanel` drawn inside the game's own `EndScene`, like the auto-market
editor and the settings panel (see
[settings-overlay.md](settings-overlay.md)). That deleted three workarounds the
window model required, rather than merely relocating them:

- a 250 ms `WM_TIMER` polling `GetForegroundWindow` to hide and re-show the
  overlay across an alt-tab — being part of the frame, it cannot outlive it;
- `WS_EX_TOPMOST`, kept because ownership alone did not reliably keep a layered
  window above an exclusive-fullscreen device;
- the window class registration, `WM_PAINT` double-buffering, `WS_EX_TRANSPARENT`
  click-through and the process-id window search (~185 lines).

It also means the stats now appear in in-game screenshots, which a layered
window never did.

**Sizing is the reason this needed care.** The panel measures its content and
so must resize per show, and `overlayPanelSetBounds` rebuilds the quad's
vertices — float work, which is forbidden on the frame-tick and render paths
(live x87 state, see the auto-market doc). It is legal here precisely because
`showStatsOverlay` is reached from `Win/LoseScreen::OnActivate`, a **function
prologue**, where the x87 stack is empty by calling convention. The GDI bitmap
and D3D texture are *not* recreated there: they are rebuilt lazily on the render
thread when it notices the size changed, so no D3D object is touched off it.

Centring uses the backbuffer size (`d3dBackbufferSize()`, captured once per
frame in the EndScene dispatcher) rather than the game window rect, since that
is the coordinate space the quad is drawn in — correct windowed and fullscreen
alike.

`showStatsOverlay` lowers the visible flag before rebuilding its row list and
raises it only once the rows and bounds are final, so the render callback never
walks a half-built list if a second endgame screen appears without a return to
the menu in between.

## Architecture (code layout)

The feature is split into decoupled modules under `src/patches/endgameStats/`;
`src/patches/endgameStats.cpp` is wiring only (screen hooks + install):

| Module | Responsibility |
| --- | --- |
| `gameOffsets.h` | All RVAs / player-object offsets + `readRawPlayerStat()` |
| `units.h` | Dense compile-time unit table (`kUnits`, id → index map) |
| `snapshot.h` | `RawPlayerStat` (float-free), `PlayerRow`, `EndgameSnapshot` |
| `unitTracker.cpp` | Spawn hook at RVA `0x0EE3BE`, per-slot recruit counts |
| `session.cpp` | Game-thread polling, identity freezing, session lifecycle, counters |
| `collect.cpp` | The four detection passes → `EndgameSnapshot` |
| `overlay.cpp` | Row-model GDI painting into a shared `OverlayPanel`; knows nothing about game memory |
| `debugDump.cpp` | `endgame_debug.txt` writer (DEBUG builds only) |

Every path that touches **game memory** runs on the **game thread**: the session
poll rides the shared frame-tick dispatcher (`src/core/frameTick.cpp`, main-loop
trampoline at RVA `0x300c0`, shared with the stop-troops hotkey), and the
spawn/screen hooks were always on the game thread. There is no background timer
thread anymore, so no cross-thread races against the simulation and no risk of
dereferencing a player object mid-free.

Painting is the one part that runs from the render callback. It is safe by
construction: by then the snapshot has been copied into DLL-owned rows
(`std::wstring`), so the paint path reads **no game memory at all** — it cannot
race the simulation no matter which thread `EndScene` is called on. Poll/spawn paths are float-free (raw float bits only) because
those hook sites are mid-function where live x87 state is possible; conversion
happens in `collect.cpp`, called from a function-prologue hook where the x87
stack is empty by ABI.

## Player Detection Algorithm

At the moment the endgame screen fires, not all players are necessarily reachable
via the live player table — enemies may have been eliminated and allies may have
left the session. Detection uses four passes in order:

**Pass 1 — local player**: Read from the dedicated current-player global at
`base + 0x6E8C60`. Always points to the local human player regardless of castle state.

**Pass 2 — live remote players**: Scan player table slots 0–31 for entries with
`[+0x10F8] == 1` (remote player flag) and a valid color index (1–10). Entries where
both gold and honour are zero are skipped — these are persistent game-init template
slots that are always present but carry no real player data.

**Pass 3 — unit-spawn fallback**: Any slot where the unit spawn hook logged a
recruit is definitively a real player. Catches players whose castle flag was modified
at game end but whose slot is still in the table.

**Pass 4 — session cache**: Slots snapshotted by the session poll (every 10 s
on the game thread while a game is active). Catches players who left the
session before the endgame screen fired. An all-zero snapshot (gold, honour,
troops AND siege) with no recorded recruits is treated as a template/ghost
entry and skipped — a real player eliminated mid-game leaves the table
entirely, so their cached snapshot retains real values.

**Deduplication is per table slot, not per color index.** Each pass marks the
table slot it filled via `slotAdded[]`; later passes skip slots already
claimed. Color index (`[+0x4]`) is *not* a unique player identifier — see
Known Issues — so it must not be used to decide whether a player has already
been added.

## Player Name Mapping

The overlay reads each player's real name from a static per-session name
array at `base + 0xDB89B0` (see Data Offsets below).

**The array is indexed by (colour − 1).** The host assigns each joining
player a colour (1–10), and that colour *is* the player's slot in the name
array: record `[colour − 1]` holds that player's name. So the name for a
player is `nameArray[player.colour - 1]`, read directly — no positional
guessing, no local-player special case.

**Mapping rule**: `collectStats()` determines each selected player's true
colour (see "Colour corruption" below), then calls `loadNameAtIndex(base,
colour - 1, ...)` to read that record directly. The frozen colour is also
written back into the stat so the overlay's colour *label* (the fallback when
no name is available) is correct too. If the record at `colour − 1` has no
valid string pointer — e.g. a player who left, leaving a stale/freed entry —
the name stays empty and the overlay falls back to the colour label.

**Colour corruption (why the frozen colour is needed)**: the live colour
field at `[player+0x4]` is reliable mid-game but gets **overwritten near
endgame**. When a player is eliminated, their castle becomes an estate of
whoever conquered them, and the defeated player object's colour is rewritten
to the conqueror's colour. In a game where the local player (colour 4)
eliminated three opponents, all three of those slots read `colour == 4` at
endgame — which is the real origin of the long-standing "colorIdx is not
unique" bug. The fix: freeze each slot's colour at **first sighting** (the
session poll or the first unit-recruit, both well before any eliminations),
stored in `SlotCache::stableColor` and never overwritten. At endgame the
frozen colour, not the live field, is used for the name index and label.

**Stale records = players who left**: a colour index whose player has left
the session can point at freed or stale memory (a previous occupant of that
colour, or fill bytes). `loadNameAtIndex()` guards each read with
`VirtualQuery()` and treats an unreadable/committed-but-freed page as "no
name". In one 6-player FFA, record `[4]` (colour 5) decoded as garbage
because the colour-5 player had left and a later joiner had reused nearby
memory — exactly the leave/rejoin churn this guard handles.

**Evidence**: a 6-player FFA where the local player (`[AoG] Halli`, colour 4,
table slot 4) was **not** the host. A mid-game minidump showed the true,
distinct colours (slots 1/2/6/4/8/5 → colours 1/2/3/4/6/7); by endgame the
live colour field had collapsed to 4 for every slot the local player had
consumed. The name array records were: `[0]` NuNuWaVe, `[1]` West Virginia,
`[2]` Sparta, `[3]` `[AoG] Halli`, `[4]` garbage (colour-5 player left),
`[5]` stale (colour-6 player), `[6]` Apophis. Indexing by frozen colour − 1
maps slot 4 → record `[3]` → `[AoG] Halli` correctly, where the previous
positional logic mislabelled the local player as NuNuWaVe.

## Session Tracking

`session.cpp` snapshots all 32 table slots every 10 seconds **on the game
thread**, via the shared frame-tick dispatcher (main-loop trampoline at RVA
`0x300c0`). The previous implementation used a `CreateTimerQueueTimer`
background thread; that raced the simulation (a player object could be freed
between the pointer read and the field reads, and the spawn hook / endgame
reset mutated the same cache concurrently) and, worse, it kept polling in the
menus, freezing identities off the dead game's stale player table — the main
source of wrong names and stats. Both problems are structural, not patched:
everything now runs on one thread, and polling is gated by a session
lifecycle.

**Session lifecycle**:

- **Idle → Active**: the first unit recruit after a reset (`sessionOnRecruit`
  from the spawn hook). Recruitment is definitive proof that a real game is
  running; nothing is polled and no identity is frozen while Idle, so
  menu-time table remnants are never captured. (A player must recruit within
  a game for tracking to start — in practice the first barracks purchase by
  *any* player, typically within the first minute.)
- **Active → Idle (full reset)**: `MainMenuScreen::OnActivate` fires — the
  player reached the main menu, whether from the endgame screen or by
  quitting a game mid-way. All slot caches and unit counts are cleared and
  the overlay is dismissed. (The Win/LoseScreen scalar destructors are hooked
  as a teardown safety net, but screen objects persist for the process
  lifetime, so they never fire on normal screen exit.) The reset deliberately
  does not happen at overlay show — so if a screen's `OnActivate` fires
  twice, the second collection still has the full session state. The
  main-menu dump also confirmed the player table is fully nulled at the menu,
  so there is nothing stale for the next session's polls to pick up.

**Slot reuse detection**: each cache entry records the player object pointer
it was captured from. If a poll or recruit sees a different pointer at the
same slot (a new game started without the endgame screen ever firing, e.g.
quit-to-menu, or engine slot recycling), that slot's cache and unit counts
are dropped and re-captured fresh.

**Ghost/template filtering** is now a single check at collection time (see
Pass 4): all-zero snapshot + no recruits. The old fingerprint machinery
(`makeFingerprint`, `everChanged`, `unchanged`, 3-poll ghost detection,
`isGhostCacheEntry`) existed only because the tracker could not distinguish
"in a game" from "in the menus"; with the session lifecycle it was deleted
outright. The old OR-composed fingerprint could also miss real stat changes
(ORing lossy products let a troops change vanish into bits already set by
gold), which could misclassify a quiet real player as a ghost.

**Diagnostic counters**: the session keeps running counters (poll ticks,
idle-skipped ticks, snapshots, identity freezes, slot reuse detections,
recruit events, session starts/resets). A DEBUG build writes them at the top
of `endgame_debug.txt`, so "why is this player missing/mislabelled" can be
answered against how much the tracker actually observed.

## Patch Offsets

### WinScreen::OnActivate — show overlay (victory)

| Item | Value |
| --- | --- |
| Hook site RVA | `0x297fa0` |
| Hook site VA | `0x697fa0` |
| Bytes overwritten | `55 8b ec 6a ff` (`push ebp; mov ebp,esp; push -1`) |
| Return to | RVA `0x297fa5` |
| How found | RTTI type descriptor `.?AVWinScreen@Stronghold2@@` at VA `0xac4378` → COL at `0xa14c9c` → vtable at VA `0x9e1014` (RVA `0x5e1014`), slot [2] = VA `0x697fa0` |

### LoseScreen::OnActivate — show overlay (defeat)

| Item | Value |
| --- | --- |
| Hook site RVA | `0x297700` |
| Hook site VA | `0x697700` |
| Bytes overwritten | `55 8b ec 6a ff` (`push ebp; mov ebp,esp; push -1`) |
| Return to | RVA `0x297705` |
| How found | RTTI type descriptor `.?AVLoseScreen@Stronghold2@@` at VA `0xac4344` → COL at `0xa14c48` → vtable at VA `0x9e0d4c` (RVA `0x5e0d4c`), slot [2] = VA `0x697700` |

### MainMenuScreen::OnActivate — dismiss overlay, reset session

| Item | Value |
| --- | --- |
| Hook site RVA | `0x27dd30` |
| Hook site VA | `0x67dd30` |
| Bytes overwritten | `55 8b ec 6a ff` (`push ebp; mov ebp,esp; push -1`) |
| Return to | RVA `0x27dd35` |
| How found | RTTI in a main-menu minidump: `.?AVMainMenuScreen@@` (global namespace, unlike the `@Stronghold2@@` screens) → vtable RVA `0x5db97c`, slot [2] = OnActivate (same slot as the dynamically-verified Win/Lose OnActivate) |

Evidence that this is the right screen: the dump (taken at the main menu) held
exactly one instance, and it was the **only** screen object whose `Pane` flag
byte at `+0x18` read `0x1f` — WinScreen, LoseScreen, TopLevelMenu and
PlayScreen all read `0x0f`. The extra `0x10` bit marks the currently-active
pane. The same dump showed the endgame screen objects still alive, proving
screens persist for the process lifetime and their destructors never fire on
normal exit — which is why the overlay could not be dismissed before this
hook existed.

### WinScreen / LoseScreen scalar destructors — teardown safety net

These never fire on normal screen exit (see above); they are kept for real
teardown paths only.

| Screen | Hook site RVA |
| --- | --- |
| WinScreen dtor | `0x297f10` |
| LoseScreen dtor | `0x297670` |

### Unit spawn hook

| Item | Value |
| --- | --- |
| Hook site RVA | `0x0EE3BE` |
| Hook site VA | `0x4EE3BE` |
| Bytes overwritten | `80 B9 79 01 00 00 00` (`cmp byte ptr [ecx+0x179], 0`) — 7 bytes |
| Registers at hook site | `EDI` = unit type ID; `ESI` = `player_base + 0x674` |
| How found | Watchpoint on `[player+0xD8C]` (army count) during unit recruit; call-stack analysis to this CMP, which fires for both regular and siege unit code paths |

This site is in a hot path and fires far more often than just on recruit. The hook
filters immediately via the compile-time unit index (`unitIndexFromId`, `units.h`) —
only known recruitable unit type IDs are counted. Confirmed to fire for both local
and remote player recruits in multiplayer (verified via conditional breakpoint
`edi==0x13` while opponent hired an Archer).

### Session poll — shared frame-tick dispatcher

| Item | Value |
| --- | --- |
| Hook site RVA | `0x300c0` (6 bytes: `ff d7 8b 10 8b c8`) |
| Owner | `src/core/frameTick.cpp` (shared with the stop-troops hotkey) |
| Cadence | every frame; the session poll self-throttles to 10 s via `GetTickCount()` |
| How found | see docs/features/stop-troops-hotkey.md |

### Data Offsets

All offsets are relative to the player object pointer.

| Field | Offset | Type | Evidence |
| --- | --- | --- | --- |
| Colour index (1=Red…10=Gray) | `+0x4` | int | x32dbg dump. Reliable mid-game; corrupts near endgame (see Player Name Mapping). Used frozen-early as the name-array index. |
| Honour total | `+0x1C` | int | x32dbg dump |
| Regular army count | `+0xD8C` | int | x32dbg dump |
| Siege unit count | `+0xD90` | int | x32dbg dump |
| Title index (0=Freeman…9=Duke) | `+0xF58` | int | x32dbg dump |
| Castle status | `+0x10F8` | int | 2 = castle standing, 1 = castle destroyed/eliminated, 0 = unused |
| Gold (treasury balance) | `+0x1010` | float | x32dbg: `00 5C 40 46` = 12311.0f matched UI gold |
| Popularity | `+0x1028` | float | x32dbg dump |
| Trade income | `+0x1554` | int | x32dbg: value 7990 matched Trade Income panel |
| Tax: Castle | `+0x1558` | int | x32dbg: value 165 matched Tax Castle panel |
| Tax: Estates | `+0x1628` | int | x32dbg: reads 0 consistent with no estates |
| Honour: Feasting | `+0x1714` | int | x32dbg: value 150 ✓ |
| Honour: Dancing | `+0x1718` | int | x32dbg: value 1000 ✓ |
| Honour: Monastery | `+0x171C` | int | x32dbg: value 60 ✓ |
| Honour: Jousting | `+0x1720` | int | x32dbg: value 360 ✓ |
| Honour: Church | `+0x1724` | int | x32dbg: value 150 ✓ |
| Honour: Granary | `+0x1728` | int | x32dbg: value 97 ✓ |
| Honour: Statues | `+0x172C` | int | x32dbg: value 320 ✓ |
| Honour: Crime | `+0x1730` | int | x32dbg: value 95 ✓ |

**Player table**: `base + 0x6E8BD8` (RVA). Array of 4-byte pointers, 32 active
slots (indices 0–31). Slot 33 (`base + 0x6E8C5C`) is the local player's slot
index (int32). Slot 34 (`base + 0x6E8C60`) is the dedicated local player
pointer — always valid for the current human player regardless of castle state.

**Army sub-object offset**: `ESI = player_base + 0x674` at the unit spawn hook
site. Subtract `0x674` to recover `player_base`.

**Player name array**: `base + 0xDB89B0` (RVA; VA `0x11B89B0` at ImageBase
`0x400000`). `NAME_ARRAY_COUNT = 8` records, stride `0x1C` (28 bytes). Each
record is an MSVC `std::wstring`: `+0x10` is the 16-byte buffer union (heap
pointer to the UTF-16LE string for names longer than the 7-wchar SSO limit),
and `_Mysize`/`_Myres` (length and capacity) land at `+0x04`/`+0x08` of the
*next* record because the wstring straddles the stride. **The array is indexed
by (colour − 1)** — record `i` belongs to the player whose colour is `i + 1`,
not to "the i-th player to join". A colour that no current player owns (nobody
was ever assigned it, or its player left) holds a stale or freed pointer.

`loadNameAtIndex(base, colour - 1, ...)` (in `session.cpp`) reads one record
directly, **discriminating on `_Myres` first**: capacity < 8 means the
characters are inline in the union (small-string optimisation — read them in
place, nothing to dereference); capacity ≥ 8 means the union holds a heap
pointer. The heap pointer is guarded with a range check plus `VirtualQuery()`
— a pointer into freed memory (a left player's per-match pool, or a previous
session) passes the range check but fails the `MEM_COMMIT`/readable check,
and is treated as "no name" (overlay falls back to the colour label). An
earlier crash (`0xc0000005`, fault offset `0x1fdf` in `version.dll`) came
from dereferencing such a stale pointer without the VirtualQuery guard. The
copy is clamped to `_Mysize` (validated: `0 < _Mysize ≤ _Myres ≤ 4096`) and
to the committed region returned by `VirtualQuery` — a string allocated near
the end of a heap region must not pull the read across the boundary into an
uncommitted page (same crash class as above, one page later).

## Known Issues

- **Colour index at `[+0x4]` corrupts near endgame (root cause understood)**:
  the live colour field is a genuine, unique per-player colour mid-game, but
  it is overwritten when a player is eliminated — the defeated player's castle
  becomes an estate of the conqueror, and the defeated object's colour is
  rewritten to the conqueror's colour. In a game where the local player
  (colour 4) eliminated three opponents, all three slots read `colour == 4` at
  endgame. Earlier dumps that showed "all players share `colorIdx == 1`" were
  reading this post-corruption state, which led to the mistaken belief the
  field was never a real colour. Fixed by freezing each slot's colour at first
  sighting (`SlotCache::stableColor`) and using that, not the live field, for
  both the name-array index and the colour label. This also resolves the old
  cosmetic bug where every fallback label read the same colour.

  Player name, title, and stat text are still rendered in fixed white (not the
  player colour) for readability — see `CHANGELOG.md`.

- **Local player name mismatch when not host (fixed)**: the name array is
  indexed by (colour − 1), not by join order or local-player-first. The old
  code assumed `names[0]` was the local player and assigned the rest
  positionally, which mislabelled everyone whenever the local player was not
  the host. A Steam-API match was tried as an interim fix but failed — the
  Steam display name (e.g. `BinaryVision`) differs from the in-game Firefly
  Online name (e.g. `[AoG] Halli`). Fixed by reading each player's name
  directly from `nameArray[frozenColour - 1]`; verified against a non-host
  6-player FFA where the local player (slot 4, colour 4) correctly resolved to
  record `[3]` = `[AoG] Halli`.

- **A colour with no current owner has no name**: if nobody was assigned a
  colour, or that player left, the record at `colour − 1` holds a stale/freed
  pointer (or fill bytes from a prior session). `loadNameAtIndex()` rejects
  these via `VirtualQuery()` and the overlay falls back to the colour label
  for that player. Players who left mid-game therefore show a colour label
  rather than their name — unavoidable, since the game frees the string.

- **Short names rendered as CJK/"Chinese" garbage (fixed)**: names of 7
  characters or fewer are stored **inline** in the wstring's 16-byte union
  (MSVC small-string optimisation) — there is no heap pointer. The old code
  dereferenced the union unconditionally, turning the name's first two UTF-16
  characters into an "address" (second character = high word, so any letter
  there lands inside the always-mapped exe image), then read exe bytes as
  UTF-16 — and random 16-bit values overwhelmingly fall in the CJK ideograph
  block, which is why the garbage looked Chinese. Long names were unaffected
  (their union really is a heap pointer). Confirmed live: `[AoG] Halli`
  (11 chars) resolved correctly; after shortening to `Halli` (5 chars) the
  name corrupted. Fixed by discriminating on `_Myres` (capacity < 8 = inline)
  and validating `_Mysize` before copying; the size/capacity validation also
  hardens the stale-record path below.

- **Crash: stale name-array pointer caused an access violation (fixed)**: a
  record's `+0x10` pointer can be a stale reference into a per-match memory
  pool the game has since freed, even though the pointer value still looks
  plausible (passes the `[0x10000, 0x80000000)` range check). Dereferencing it
  crashed with `0xc0000005` at fault offset `0x1fdf` in `version.dll`. Fixed by
  calling `VirtualQuery()` on the pointer in `loadNameAtIndex()` and treating
  any non-`MEM_COMMIT`/non-readable page as "no name".

- **Name array behaviour outside multiplayer is unverified**: the minidumps
  used for Player Name Mapping were all multiplayer. A single-player skirmish
  or campaign win/lose screen has not been checked, so it's unknown whether
  the array at `base + 0xDB89B0` is populated, zero-filled, or holds stale
  pointers there. `loadNameAtIndex()` rejects a record whose `+0x10` pointer
  is `0`, out of range, or no longer committed, so a garbage entry should at
  worst suppress the name (colour-label fallback) rather than crash — but this
  needs a singleplayer playtest to confirm.

- **Opponent unit types missing if never purchased via barracks**: the spawn hook
  at `0x0EE3BE` covers barracks and siege workshop purchases. Units that enter play
  through other means (scripted spawns, starting units) are not counted. Known gap.

- **Pass 4 ghost guard**: an all-zero cached snapshot (gold, honour, troops
  AND siege) with no recorded recruits is treated as a template/ghost entry.
  Checking only `gold == 0 && honor == 0` (an early version) also matched real
  players eliminated before they had earned any treasury or honour. The
  fingerprint/`everChanged` machinery that once backed this heuristic was
  removed with the session lifecycle — see Session Tracking.

- **Remaining gap — eliminated before tracking starts, no recruits**: a player
  eliminated before the session's first poll (i.e. within ~10 s of the first
  recruit of the game) who never recruited a unit themselves has no cached
  snapshot, and is invisible to the overlay. Accepted: games run 30+ minutes,
  so such a player was almost certainly quitting on purpose. A DEBUG build
  writes `endgame_debug.txt` (session counters, per-slot live/cache state and
  the final selected players) to diagnose this and similar cases.

- **Quit-to-menu without an endgame screen (fixed)**: the session used to
  stay Active if a game ended without the endgame screens, risking stale
  state bleeding into the next match. The `MainMenuScreen::OnActivate` hook
  resets everything on *any* return to the main menu, and the main-menu dump
  confirmed the game nulls the entire player table there, so the next
  session starts from a genuinely clean slate. The per-slot pointer-reuse
  check remains as defence in depth for any game→game transition that skips
  the menu (e.g. a scripted restart, if one exists).

- **Endgame screens are never destroyed (why the overlay used to stick)**:
  the Win/LoseScreen scalar-destructor hooks were meant to dismiss the
  overlay on screen exit, but the main-menu minidump showed both screen
  objects still alive at the menu — the game creates its screens once and
  reuses them, so those destructors only run at real teardown. Dismissal now
  rides the main-menu activation hook; the dtor hooks remain as a safety
  net.

## Multiplayer Compatibility

**Safe for version mismatch.**

The overlay reads player object fields and draws locally into the client's own
frame. No simulation entity state is modified. The unit spawn hook increments a DLL-local
counter only. An unpatched client simply will not see the overlay; no desync.
