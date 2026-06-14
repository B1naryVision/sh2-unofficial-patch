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

**Deduplication is per table slot, not per color index.** Each pass marks the
table slot it filled via `slotAdded[]`; later passes skip slots already
claimed. Color index (`[+0x4]`) is *not* a unique player identifier — see
Known Issues — so it must not be used to decide whether a player has already
been added.

## Player Name Mapping

Since `colorIdx` cannot identify a player (see Known Issues), the overlay
instead reads each player's real name from a static per-session name array at
`base + 0xDB89B0` (see Data Offsets below).

**Mapping rule**: `loadPlayerNames()` returns the array entries in order
(`names[0]`, `names[1]`, ...). `name[0]` is always the local player's real
name. `collectStats()` finds whichever `s_stats[]` entry is the local player
— by checking `[player+0x10F8] == 2` on that entry's table slot — and pairs
it with `names[0]` directly, regardless of where Pass 1-4 placed it in
`s_stats[]`. The remaining `names[1..]` are paired with the remaining
`s_stats[]` entries in order. If an `s_stats[]` entry has no corresponding
name entry left — e.g. a vs-AI opponent that never appears in the array —
`name[0]` stays `0` and the overlay falls back to the `colorIdx`-based colour
label as before.

**Evidence**: confirmed via two Task Manager minidumps plus a `#ifdef DEBUG`
`endgame_debug.txt` from a live 3-player FFA. The minidumps (a lobby-screen
capture with 3 players and a defeat-screen capture from a later 1v1) showed
the array at the same RVA, stride, and field layout. In the 1v1 defeat dump,
`names[0] = "NMaestro"` (the local player, `[+0x10F8] == 2`) and
`names[1] = "[AoG] Soup"` (the remote, `[+0x10F8] == 1`) — here `s_stats[0]`
*was* the local player, so the original positional pairing happened to work.

**3+ players (confirmed via `endgame_debug.txt`)**: in a 3-player FFA,
`names[0] = "NMaestro"` again, but Pass 1 failed to find the local player
(`base+0x6E8C60`'s color check or table match didn't hit), so the local
player (table slot 8, `castle == 2`) was only added later by Pass 3 and
landed at `s_stats[1]`, while a remote (slot 2, `castle == 1`) landed at
`s_stats[0]`. With the old positional pairing, `names[0]="NMaestro"` (the
local player's real name) was shown on the *remote's* column, and the local
player's own column showed `names[1]`, which was a garbage/unreadable
UTF-16 string (the "Chinese characters" reported after a multiplayer game).
The fix above (matching on `castle == 2` rather than position) makes the
local player's column always show `names[0]` correctly.

**Still unverified for 3+ players**: `names[1..]` for *remote* players is
still paired with the remaining `s_stats[]` entries positionally. In the FFA
dump, the second name slot (`names[1]`, after skipping the local player's
`names[0]`) was the garbage entry described above — so one remote's column
would still show garbage instead of a real name. Whether `names[1..]`'s
order matches the remaining `s_stats[]` order for remotes, and why one entry
was garbage in that game, remains open — see Known Issues.

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

**Pass 4 ghost-snapshot filter (`isGhostCacheEntry`)**: a slot's cached `valid`
flag can be tentatively `true` even for a true ghost/template entry, if the
endgame screen fires before the 3-poll ghost detection above has run (within
~180s of game start). Pass 4 therefore re-checks each cached snapshot with
`isGhostCacheEntry()`: a slot is treated as a ghost only if its last snapshot
has **zero gold, honour, troops, AND siege**, the fingerprint never changed
across polls (`!everChanged`), and the unit-spawn hook never recorded a
recruit for that slot. Earlier versions checked only `gold == 0 && honor ==
0`, which also matched real players eliminated early — before they had
earned any treasury or honour — and incorrectly dropped them from the
overlay (see Known Issues).

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

**Player name array**: `base + 0xDB89B0` (RVA; VA `0x11B89B0` at ImageBase
`0x400000`). Up to `NAME_ARRAY_COUNT = 8` records, stride `0x1C` (28 bytes).
`+0x10` is a pointer to a heap-allocated, null-terminated UTF-16LE name
string; the other fields in each record are unexplored. Unused trailing
records are all-zero (`+0x10 == 0`), and the array is packed — the first
zero pointer marks the end of the list. Evidence: identical layout found in a
lobby-screen minidump (3 players, names `4H|TheSettler`, `NMaestro`,
`[AoG] ¶Vengeance†`) and a later defeat-screen minidump from a 1v1 (names
`NMaestro`, `[AoG] Soup`), the latter cross-referenced against the player
table's `[+0x10F8]` flag — see Player Name Mapping above.

**Record string pointers can dangle across matches**: on a second match in
the same session, a `+0x10` record pointer can pass the `[0x10000,
0x80000000)` range check yet point into memory the game has since freed
(observed as a hard `0xc0000005` access violation reading the very first
`wchar_t` of the string — crash address inside `loadPlayerNames()`, fault
offset `0x1fdf` in `version.dll`). The array itself is apparently not
cleared/repopulated for every record between matches, so a stale pointer
from a previous match's per-match memory pool can survive into the next.
`loadPlayerNames()` now calls `VirtualQuery()` on each `+0x10` pointer and
stops (treats it as end-of-list) unless the page is `MEM_COMMIT` and
readable (not `PAGE_NOACCESS`/`PAGE_GUARD`).

## Known Issues

- **Color index at `[+0x4]` is not a unique player identifier (confirmed)**: a
  `#ifdef DEBUG` dump (`endgame_debug.txt`) from a 6-player FFA showed all 6
  distinct players — 6 different table slots, each with its own gold/honour/army
  data and unit-recruit history — reporting the *same* `colorIdx == 1`. The field
  is therefore not "this player's colour" in the 1:1 sense the rest of this doc
  assumed; its real meaning (team? civilisation? something else?) is still
  unknown. It also does not reliably match the player's visual castle colour in
  the game UI.

  **Consequence for this patch**: the overlay's per-player colour label and text
  colour (`COLOR_NAMES[colorIdx]` / `COLOR_VALS[colorIdx]`) can show the *same*
  colour/name for multiple distinct players when they share a `colorIdx`. This is
  cosmetic only — each player's data column is still correct — but the headers
  may all read e.g. "Red". Finding the real per-player colour field is a separate
  investigation.

  **Consequence that was a real bug (fixed)**: detection previously deduplicated
  players via a `colorSeen[colorIdx]` array — once one player with a given
  `colorIdx` was added, every other player sharing that `colorIdx` was skipped as
  "already seen". Since `colorIdx` is not unique, this silently dropped real
  players. In the 6-player FFA above (all `colorIdx == 1`), this reduced 6 real
  players down to 1. In an earlier report with more varied `colorIdx` values
  across players, it reduced 6 down to 3. Fixed by deduplicating on table slot
  index (`slotAdded[]`) instead, which the debug dump confirms is unique per
  player.

- **Remote name ordering for 3+ players is unverified, and one entry can be
  garbage**: `names[0]` ↔ local player is now matched explicitly via
  `castle == 2` (see Player Name Mapping above) and confirmed correct in a
  3-player FFA dump. The remaining `names[1..]` are still paired with the
  remaining `s_stats[]` entries positionally. In that same FFA dump,
  `names[1]` (after skipping the local player's `names[0]`) decoded as
  unreadable UTF-16 — likely a stale or use-after-free pointer in that name
  array record — so the remote player who would have received that slot got
  garbage text instead of their real name. Whether `names[1..]`'s order
  matches the remaining `s_stats[]` order, and why that record's pointer was
  bad, remains open. `endgame_debug.txt` now logs `castle=` for each selected
  player alongside `name=`, so the next FFA playtest can confirm the local
  player's column is correct and help narrow down the remaining garbage-entry
  case.

- **Crash: stale name-array pointer caused an access violation on a second
  match (fixed)**: `loadPlayerNames()`'s `[0x10000, 0x80000000)` range check
  alone was not sufficient — a record's `+0x10` pointer can be a stale
  reference into a per-match memory pool the game has since freed by the time
  a *second* match's endgame screen runs, even though the pointer value
  itself still looks plausible. Dereferencing it crashed with `0xc0000005` at
  fault offset `0x1fdf` in `version.dll` (the first `wchar_t` read in
  `loadPlayerNames()`). Fixed by calling `VirtualQuery()` on each pointer and
  stopping (as if it were the end of the packed array) unless the page is
  `MEM_COMMIT` and readable. See Data Offsets above.

- **Name array behaviour outside multiplayer is unverified**: both minidumps
  used for Player Name Mapping were captured from multiplayer sessions. A
  single-player skirmish or campaign win/lose screen has not yet been checked,
  so it's unknown whether the array at `base + 0xDB89B0` is populated,
  zero-filled, or holds stale/garbage pointers in that case.
  `loadPlayerNames()` guards against the latter by stopping as soon as a
  record's `+0x10` pointer is `0`, outside `[0x10000, 0x80000000)`, or no
  longer backed by committed memory (see the crash fix above), so a garbage
  entry should at worst suppress name lookup (falling back to the colour
  label) rather than crash — but this fallback path itself needs a
  singleplayer playtest to confirm.

- **Opponent unit types missing if never purchased via barracks**: the spawn hook
  at `0x0EE3BE` covers barracks and siege workshop purchases. Units that enter play
  through other means (scripted spawns, starting units) are not counted. Known gap.

- **Pass 4 ghost guard refined**: Pass 4's old `gold == 0 && honor == 0` ghost
  guard also matched real players eliminated before they had earned any treasury
  or honour — indistinguishable from a true ghost/template slot using only those
  two fields. Replaced with `isGhostCacheEntry()` (see Periodic Player Cache
  above), which additionally checks troops, siege, `everChanged`, and recorded
  recruits.

- **Remaining gap — eliminated before first poll, no recruits**: a player
  eliminated within the first 60s of the game who never recruited a unit has
  `cache.seen == false`, so `cache.valid` stays `false` and Pass 4 has no
  snapshot for them at all. Such a player is still invisible to the overlay.
  A `#ifdef DEBUG` build writes `endgame_debug.txt` (per-slot live/cache state
  and the final selected players) to help diagnose this and similar cases.

## Multiplayer Compatibility

**Safe for version mismatch.**

The overlay reads player object fields and renders to a local Win32 window. No
simulation entity state is modified. The unit spawn hook increments a DLL-local
counter only. An unpatched client simply will not see the overlay; no desync.
