# Unit Cap Raise

**Status:** Not added, incompatible in multiplayer for players without patch.
**Affects:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)

---

## Description

Raises the per-player unit cap so it is always 550, regardless of player count.
In vanilla the cap scales down as more players join, dropping to 362 in 8-player
games. With this patch every player always has the 2-player maximum of 550 units.

---

## Background

Three functions (`FUN_00016680` at VA `0x416680`, `FUN_00018640` at `0x418640`,
`FUN_00018940` at `0x418940`) each implement the same per-player cap check. They
cover different unit categories or recruitment contexts but share identical logic:

```c
// Simplified from Ghidra decompiler output
if (DAT_00db8454 == 0
    && army_size > 299                              // early-exit below 300
    && (n = getPlayerCount()) != 0                  // non-zero player count
    && (int)(500 / (ulonglong)n) + 300 <= army_size // cap reached
   ) {
    return 0;   // block recruitment
}
```

The cap formula `300 + floor(500 / player_count)` produces:

| Players | Cap |
|---------|-----|
| 2       | 550 |
| 4       | 425 |
| 6       | 383 |
| 8       | 362 |

---

## Implementation

In each function the compiler emits:

```asm
MOV EAX, 0x1F4    ; eax = 500
<IDIV player_count>
ADD EAX, 0x12C    ; eax = 500/n + 300  ← patch target
CMP EAX, army_size
```

Replacing `ADD EAX, 0x12C` (`05 2C 01 00 00`) with `MOV EAX, 0x226`
(`B8 26 02 00 00`) discards the division result and forces the cap value to 550.
The division still executes harmlessly; only EAX is overwritten before the
comparison.

| # | Offset | Size | Original | Patched |
|---|--------|------|----------|---------|
| 1 | `base+0x16827` | 5 bytes | `05 2C 01 00 00` | `B8 26 02 00 00` |
| 2 | `base+0x18768` | 5 bytes | `05 2C 01 00 00` | `B8 26 02 00 00` |
| 3 | `base+0x189FB` | 5 bytes | `05 2C 01 00 00` | `B8 26 02 00 00` |

---

## Multiplayer Compatibility

**This patch is not compatible with vanilla multiplayer.** Each client enforces
the cap locally, so a patched player can field 550 units while an unpatched
player is capped at the vanilla value. This creates an unfair asymmetry and
potential confusion. All players in a lobby must run the same patch version.

For this reason this patch is currently excluded from the build.

The check is gated on `DAT_00db8454 == 0`, which is 0 only in multiplayer mode,
so single-player games are unaffected.

Probably for the best, as it might give an advantage to players with the patch vs those without.

---

## Investigation Notes

Formula confirmed from four runtime data points before the code was located.
Constants 300 and 500 do not appear as instruction immediates in `.text` at the
obvious offsets — the code was found via Ghidra **Search → For Scalars** cross-
referencing both values, then filtering for functions with `MOV EAX, 0x1F4`
followed by `ADD EAX, 0x12C`. The advisor strings ("Our army is at its maximum
size") XREFed only to the advisor registration constructor, not the cap checker.
