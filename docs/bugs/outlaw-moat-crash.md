# Outlaws Crash the Game While Filling a Moat

**Status:** Fixed in v0.9.0, as an automatic match fix (`src/patches/moatTileGuard.cpp`).
Root cause established by static analysis of the WER report's fault offset. Playtested
before the v0.9.0 release (2026-11-22), including the replay test (test 6 in
[match-rules.md](../features/match-rules.md)): passed.
**Affected version:** Stronghold 2 Steam v1.5.0 (exe timestamp `0x5ea311c0`)
**Patch type:** 6-byte detour into a 26-byte cave, switched per match by
`patches/matchRules.cpp`.

---

## Symptom

Against an AI that attacks with outlaws (The Hammer sends many), the game closes to the
desktop while outlaws throw javelins at defenders on the castle walls. Reported on
Windows 10 x64; the reporter found it did not happen in Windows 7 compatibility mode, and
it could not be reproduced on another Windows machine. Event log:

```
Faulting application: Stronghold2.exe 1.5.0.0, timestamp 5ea311c0
Exception code: c0000005   Fault offset: 0x000fa257
```

---

## Root cause

Fault offset `0xfa257` is VA `0x4fa257`, in `Soldier::handleMessage` (`0x4f8860`), step 1
of script `0x7d7`. That script is a soldier's **fill-in-moat** work: `FillInMoatChore`
(player) and `AIFillInMoatChore` (AI) execute through `0x59bf00`, which hands each soldier a
moat tile and calls `0x4f78e0`. That stores the tile in `soldier+0x3b8` (8 bytes: packed
tile, then x and y words) and stacks the script with a walk to the tile on top.
`Outlaw::handleMessage` (`0x50f660`) forwards ids `0x7d1..0x7de` to the Soldier handler, so
outlaws run the same code.

Step 1 reads the tile's flags with no check:

```
4fa229: lea  ebx, [esi+0x3b8]
...
4fa252: mov  ecx, [ebx]
4fa254: shr  ecx, 8                          ; tile index = y * 256 + x
4fa257: mov  edx, [ecx*4 + 0x20c152c]        ; per-tile flags, 65536 dwords   <-- fault
```

`Outlaw::startAttack` (`0x50f1f0`, reached from `0x5102f1` and `0x510335`) does this:

1. It calls the Soldier work-state reset `0x4f5b00`, which writes the "no tile" constant
   from `0x10794d0` into `+0x3b8`: packed tile `0xfffeff00`, x and y `-1`.
2. It calls `clearActions` only when the outlaw is standing still (`[+0xe0] < 0.001`, the
   constant at `0x9a0878`) and `+0xb4` is zero.
3. It then always pushes the attack with `pushInterrupt`, on top of whatever is there.

An outlaw that spots a target **while walking to its moat tile** therefore keeps the
fill-in-moat script underneath its attack, now holding "no tile". After the attack it
finishes the walk, the script runs step 0 (turns towards world position `-512,-512`) and
then step 1, which indexes the table with `0xfffeff`. That is `0x3fffbfc` bytes past the
table, at exe base + `0x5cc1128`, about 52 MB past the end of the 40 MB image.

The Archer's attack starter (`0x512a70`) calls `clearActions` whenever `+0xb4` is zero, so
an archer just drops the moat job. Of the classes checked, only the Outlaw keeps it.

### Why it looked OS-specific

The stale tile is there on every machine. Whether the read faults depends on whether
anything happens to be mapped at that address. That changes with ASLR (the exe moves every
launch), compatibility shims (Windows 7 mode loads `AcLayers` and changes allocation
patterns), injected overlays and drivers. Where the read does not fault, it returns
whatever is there, and bit 12 of that garbage decides whether the outlaw keeps waiting or
"fills" tile `(-1, -1)` through `0x5db1d0`. That is also a desync source in multiplayer,
since two clients read different garbage.

To see the bug on a machine where it does not crash, set an x32dbg breakpoint at
`base+0xfa257` with the condition `ecx >= 0x10000`.

---

## The fix

At the start of step 1 (`0x4fa229`, right after the step's own early-exit test, `0x4e3490` at
`0x4fa219`),
a script whose tile is not on the map goes down the stock "tile already filled" path at
`0x4fa315`. That path resets the tile, looks for another moat tile to fill near the unit
(`0x775cc0`, radius argument `0x14`), and otherwise walks the unit back to where it started (step 10). An
outlaw distracted on its way to the moat therefore picks a nearby tile and carries on.

A packed tile is `((y * 256 + x) << 8) | layer` (built that way at `0x4fa3dd`), so it is
on the map exactly when it is below `1 << 24`. The guard is placed before both table
reads (`0x4fa257` and `0x4fa26f`) and before the stock code adds the tile to the moat
list at `0x5bb600`, so the bad tile is never put into that list either.

| Address (VA) | RVA | Before | After |
| --- | --- | --- | --- |
| `0x4fa229` | `0xfa229` | `8d 9e b8 03 00 00` (`lea ebx,[esi+0x3b8]`) | `e9 <cave> 90` |

The cave (`VirtualAlloc`, 26 bytes):

```
lea  ebx, [esi+0x3b8]       ; the instruction the jump replaced
mov  ecx, [ebx]
cmp  ecx, 0x1000000
jb   on_map
jmp  base+0xfa315           ; tile already filled: reset, look for another
on_map:
jmp  base+0xfa22f           ; stock: push ebx / add to the moat list
```

`ecx` and the flags are dead at the site: the next stock instructions are `push ebx` and
`mov ecx, <moat list>`. The path at `0x4fa315` uses `esi` (the soldier) and `edi` (the
script action), and writes every other register it reads. Install verifies the 6 site
bytes, and the move-on target's opcodes (`8b 15` then `89 96 b8 03 00 00`), skipping the
relocated `abs32` operand, before anything is written. One stock branch lands on the site
(`je` at `0x4fa220`); none land inside it.

The root cause in `Outlaw::startAttack` is left alone: changing which work state an attack
resets touches every outlaw attack, while this guard only changes what happens to a script
that would otherwise read memory off the map.

---

## Multiplayer Compatibility

**Requires all clients, self-enforced: an automatic match fix.** In the stock game a
non-crashing run steers the outlaw by garbage memory, and the fix replaces that with the
"move on" path, which is a change to simulation state. A patched and an unpatched client
would diverge. It is rule bit `0x80` (`RULE_MOAT_TILE_FIX`). It is offered only when every
lobby member advertises it, committed by the host's rules line on the same lockstep tick
everywhere, and always on in single player. In a lobby where someone lacks it, the match
keeps the stock behaviour and the crash. Replays record and restore it with the other
rules.

It is not unconditional, because the stock game does not always crash here, so a
recording without the fix can exist.

---

## Investigation notes

- The table at `0x20c152c` is 256 KB: `0x2101788` (another object, used at `0x4fa4cd`)
  begins `0x4025c` bytes later, and every valid index is below `0x10000`. The
  endgame-stats kill tracker already treated `packedTile >> 8` as a tile index under the
  same bound.
- `0x4f5b00` has 20 callers. Twelve call `clearActions` straight after it, so the stale
  tile has no script left to resume. The other eight are ranged-attack starters that set
  the target and then decide whether to clear. The Outlaw's is the one confirmed to keep
  the script.
- Step 0 with "no tile" is harmless: it only turns the unit towards `-512,-512` before
  step 1.
