# Unit Cap Raise — Match Option

**Status:** Added in v0.8.0 as a host [match option](match-rules.md) (built 2026-11-01). Playtested 2026-11-01: works as expected. The advisor warning (below) was added in v0.8.1
(2026-11-02) after a player report that it still fired at the vanilla cap. Not yet
playtested.
**Config**: the host clicks *Unit cap 550* in the [lobby panel](lobby-panel.md)
(`[matchrules] UnitCap550`). Rule bit `0x20`.
**Affects:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)

---

## Description

Raises the per-player unit cap so it is always 550, regardless of player count.
In vanilla the cap scales down as more players join, dropping to 362 in 8-player
games. With the rule on, every player has the 2-player maximum of 550 units.

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

These are the only three cap checks: a scan of the whole exe for
`mov eax, 0x1f4` / `div` / `add eax, 0x12c` finds exactly these sites (re-checked
2026-11-01). That scan missed the advisor warning, which divides the same way but
adds a different constant (see below). The reliable census is the callers of the
player-count getter `0x421c60`: six in all, being these three, the two warning
sites, and an unrelated `10 + 12 / players` limit at `0x46e0eb`.

---

## Implementation

In each function the compiler emits:

```asm
MOV EAX, 0x1F4    ; eax = 500
DIV ECX           ; ecx = player count
ADD EAX, 0x12C    ; eax = 500/n + 300  ← patch target
CMP <army>, EAX
```

Replacing `ADD EAX, 0x12C` (`05 2C 01 00 00`) with `MOV EAX, 0x226`
(`B8 26 02 00 00`) discards the division result and forces the cap value to 550.
The division still executes harmlessly; only EAX is overwritten before the
comparison.

| # | Offset | Size | Original | Patched |
| --- | --- | --- | --- | --- |
| 1 | `base+0x16827` | 5 bytes | `05 2C 01 00 00` | `B8 26 02 00 00` |
| 2 | `base+0x18768` | 5 bytes | `05 2C 01 00 00` | `B8 26 02 00 00` |
| 3 | `base+0x189FB` | 5 bytes | `05 2C 01 00 00` | `B8 26 02 00 00` |

### The "army nearly full" warning

The recruit panels carry their own copy of the formula, used only to choose the
advisor's warning (speech `0x77`, played through `0x72be70` on `0xac7534`) after
a recruit click. It warns 20 units below the cap:

```asm
MOV EAX, 0x1F4    ; 500
DIV ECX           ; / players
ADD EAX, 0x118    ; + 280 = cap - 20  ← patch target
CMP EDI, EAX      ; army size
JB  skip
PUSH -1 / PUSH 0x77 / CALL 0x72be70
```

Left alone, the warning fired at the vanilla threshold (342 with eight players)
while the raised cap still let recruiting continue to 550. The same 5-byte swap,
to `MOV EAX, 0x212` (530), keeps the 20-unit margin:

| # | Offset | Handler | Original | Patched |
| --- | --- | --- | --- | --- |
| 4 | `base+0x2207fd` | `SubPanelBarracks::process` | `05 18 01 00 00` | `B8 12 02 00 00` |
| 5 | `base+0x2255f2` | `SubPanelMercPost::process` | `05 18 01 00 00` | `B8 12 02 00 00` |

These are the only two `push 0x77` advisor calls in the exe and the only two
`add eax, 0x118`. The barracks site is clear of the
[barracks crash fix](../bugs/barracks-lord-death-crash.md) at `0x2207c6`
(9 bytes). The warning is advisor speech on the local client and changes no
simulation state, so it shares rule bit `0x20` and needs no bit of its own.

`installUnitCapRaise` verifies that all three cap sites hold the stock bytes
(all-or-nothing: a cap raised on one recruitment path and not another would be
worse than none) and writes nothing. It checks the two warning sites separately:
if they don't match, the cap still installs and only the warning stays vanilla.
`unitCapRaiseSetEnabled` rewrites all five sites to the patched or the stock bytes. [Match rules](match-rules.md) calls it on
the simulation thread when the rules line executes and again when the match is
over, the same as the disband fix. None of the operands is an absolute address,
so the disk bytes are the in-memory bytes.

---

## Multiplayer Compatibility

**Requires all clients, self-enforced**, like every [match rule](match-rules.md).
Each client enforces the cap locally, so a client with the cap raised accepts
recruitment the others refuse, and the match desyncs. As a match rule it is only
offered when every player in the lobby advertises it, and switched on through the
host's rules line on the same tick on every client. With anyone lacking it, the
lobby panel shows it as unavailable and the match keeps the vanilla cap. Replays
record and restore it with the other rules (bit `0x20`).

The check is gated on `DAT_00db8454 == 0`, which is 0 only in multiplayer mode,
so single-player games have no cap to raise.

---

## Investigation Notes

Formula confirmed from four runtime data points before the code was located.
Constants 300 and 500 do not appear as instruction immediates in `.text` at the
obvious offsets — the code was found via Ghidra **Search → For Scalars** cross-
referencing both values, then filtering for functions with `MOV EAX, 0x1F4`
followed by `ADD EAX, 0x12C`. The advisor strings ("Our army is at its maximum
size") XREFed only to the advisor registration constructor, not the cap checker.
