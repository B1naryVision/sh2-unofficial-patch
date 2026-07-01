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
60 s poll or the first unit-recruit, both well before any eliminations),
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

`loadNameAtIndex(base, colour - 1, ...)` reads one record directly and guards
the `+0x10` pointer with a range check plus `VirtualQuery()` — a pointer into
freed memory (a left player's per-match pool, or a previous session) passes
the range check but fails the `MEM_COMMIT`/readable check, and is treated as
"no name" (overlay falls back to the colour label). An earlier crash
(`0xc0000005`, fault offset `0x1fdf` in `version.dll`) came from dereferencing
such a stale pointer without the VirtualQuery guard.

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
