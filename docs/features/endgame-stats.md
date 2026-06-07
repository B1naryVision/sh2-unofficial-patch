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
transparent Win32 overlay window appears over the game showing per-player statistics:

- Player color and title
- Gold balance, honour total
- Army size (troops and siege separately)
- Per-source income (trade, castle tax, estate tax)
- Per-source honour (feasting, dancing, monastery, jousting, church, granary, statues, crime)
- Cumulative units recruited by type (tracked via unit spawn hook throughout the game)

The overlay is dismissed when the player exits the victory/defeat screen, hooked via
each screen's scalar destructor. Up to 8 players are shown.

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

**Pass 4 — periodic cache**: Slots snapshotted by the 60-second polling timer.
Catches players who left the session before the endgame screen fired.

## Periodic Player Cache

A `CreateTimerQueueTimer` callback fires every 60 seconds. It iterates all 32 table
slots and snapshots any slot with a valid pointer and color index (1–10).

**Ghost detection**: a slot that shows no stat change across 3 or more consecutive
polls and where no unit recruit was ever seen is marked invalid. This avoids pulling
in stale entries from previous game sessions that were never cleared from the table.

**Override on first recruit**: when the unit spawn hook fires for a slot that has
not yet been polled, an initial snapshot is taken immediately and the slot is
permanently marked valid. Unit recruitment is definitive proof of a real player and
cannot be overridden by the ghost heuristic.

## Patch Offsets

### WinScreen::OnActivate — show overlay (victory)

| Item | Value |
|------|-------|
| Hook site RVA | `0x297fa0` |
| Hook site VA | `0x697fa0` |
| Bytes overwritten | `55 8b ec 6a ff` (`push ebp; mov ebp,esp; push -1`) |
| Return to | RVA `0x297fa5` |
| How found | RTTI type descriptor `.?AVWinScreen@Stronghold2@@` at VA `0xac4378` → COL at `0xa14c9c` → vtable at VA `0x9e1014` (RVA `0x5e1014`), slot [2] = VA `0x697fa0` |

### LoseScreen::OnActivate — show overlay (defeat)

| Item | Value |
|------|-------|
| Hook site RVA | `0x297700` |
| Hook site VA | `0x697700` |
| Bytes overwritten | `55 8b ec 6a ff` (`push ebp; mov ebp,esp; push -1`) |
| Return to | RVA `0x297705` |
| How found | RTTI type descriptor `.?AVLoseScreen@Stronghold2@@` at VA `0xac4344` → COL at `0xa14c48` → vtable at VA `0x9e0d4c` (RVA `0x5e0d4c`), slot [2] = VA `0x697700` |

### WinScreen / LoseScreen scalar destructors — close overlay

| Screen          | Hook site RVA |
|-----------------|---------------|
| WinScreen dtor  | `0x297f10`    |
| LoseScreen dtor | `0x297670`    |

### Unit spawn hook

| Item | Value |
|------|-------|
| Hook site RVA | `0x0EE3BE` |
| Hook site VA | `0x4EE3BE` |
| Bytes overwritten | `80 B9 79 01 00 00 00` (`cmp byte ptr [ecx+0x179], 0`) — 7 bytes |
| Registers at hook site | `EDI` = unit type ID; `ESI` = `player_base + 0x674` |
| How found | Watchpoint on `[player+0xD8C]` (army count) during unit recruit; call-stack analysis to this CMP, which fires for both regular and siege unit code paths |

This site is in a hot path and fires far more often than just on recruit. The hook
filters immediately by checking `s_unitNames[EDI]` — only known recruitable unit
type IDs are counted. Confirmed to fire for both local and remote player recruits
in multiplayer (verified via conditional breakpoint `edi==0x13` while opponent
hired an Archer).

### Data Offsets

All offsets are relative to the player object pointer.

| Field | Offset | Type | Evidence |
| ----- | ------ | ---- | -------- |
| Color index (1=Red…10=Gray) | `+0x4` | int | x32dbg dump |
| Honour total | `+0x1C` | int | x32dbg dump |
| Regular army count | `+0xD8C` | int | x32dbg dump |
| Siege unit count | `+0xD90` | int | x32dbg dump |
| Title index (0=Freeman…9=Duke) | `+0xF58` | int | x32dbg dump |
| Active player flag | `+0x10F8` | int | 2 = local player, 1 = remote, 0 = unused |
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
slots (indices 0–31). Slot 34 (`base + 0x6E8C60`) is the dedicated local player
pointer — always valid for the current human player regardless of castle state.

**Army sub-object offset**: `ESI = player_base + 0x674` at the unit spawn hook
site. Subtract `0x674` to recover `player_base`.

## Known Issues

- **Color label vs. visual castle color**: the color index at `[+0x4]` may not
  always match the player's visual castle color as displayed in the game UI. The
  relationship between internal color index and assigned castle colour is not yet
  fully understood.

- **Opponent unit types missing if never purchased via barracks**: the spawn hook
  at `0x0EE3BE` covers barracks and siege workshop purchases. Units that enter play
  through other means (scripted spawns, starting units) are not counted. Known gap.

## Multiplayer Compatibility

**Safe for version mismatch.**

The overlay reads player object fields and renders to a local Win32 window. No
simulation entity state is modified. The unit spawn hook increments a DLL-local
counter only. An unpatched client simply will not see the overlay; no desync.
