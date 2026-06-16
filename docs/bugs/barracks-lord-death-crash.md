# Barracks UI Crash on Lord Death

## Symptom

Instant crash to desktop (`0xc0000005` access violation) when the player's Lord
dies while actively recruiting units. Simply having the barracks window open is
not sufficient to trigger the crash; a recruitment must be in progress. The fault
occurs at module offset `0x2207d0`.

## Root Cause

The barracks UI event handler at RVA `0x2206d0` processes recruitment events
including a message of type `0x66` (a periodic queue-length check). Inside that
handler, it calls `0x411360` to retrieve the active barracks object. When the
Lord has just died the manager returns NULL, so ESI is set to zero and execution
jumps to the merge point at RVA `0x2207c6`.

Firefly had already identified this code path as dangerous and added a guard:

```asm
; RVA 0x2207c6
83 3d 54 84 1b 01 00   cmp  DWORD PTR [0x61b8454], 0   ; "game over?" flag
75 46                  jne  0x620815                    ; skip if set
; fall-through:
57                     push edi
8b be 90 0d 00 00      mov  edi, [esi+0xd90]            ; ← crash (esi == 0)
```

The flag at VA `0x61b8454` (RVA `0x21b8454`) is supposed to be non-zero during
a game-over state so the null-ESI block is skipped. However the flag is set
**after** the UI event fires — the barracks object is nulled out first, the
event arrives, the handler runs with ESI=0, and the flag read still returns 0.
The guard does not fire in time.

## Patch

**Site:** RVA `0x2207c6` (VA `0x6207c6`), 9 bytes

Replace the stale-flag guard with a direct pointer null test:

| | Bytes | Disassembly |
|---|---|---|
| Before | `83 3d 54 84 1b 01 00 75 46` | `cmp ds:0x11b8454, 0` / `jne 0x620815` |
| After  | `85 f6 0f 84 47 00 00 00 90` | `test esi, esi` / `je near 0x620815` / `nop` |

The `je near` displacement `0x47` brings execution to `0x620815` (the existing
clean-exit path — the same target the original `jne` used). When ESI is
non-null the test falls through and the queue-overflow check executes normally.

**Why this is better than the original flag:** The original relied on an
external flag being set by the time this handler executes. The direct null test
has no such ordering dependency; it reads the actual invalid condition.

## Investigation Dead-Ends

- The flag at `0x61b8454` is not the game-over state directly; it appears to
  gate a different subsystem. Attempting to force-set it earlier would require
  finding its write site and risks side-effects.
- The call to `0x411360` (the barracks object getter) does not have a
  caller-side null check in this path — adding one there would require
  relocating 6+ bytes and is unnecessary given the merge-point fix.

## Multiplayer Compatibility

**Safe for version mismatch.** This patch affects only the recruitment queue UI
path. The defeat screen in multiplayer is controlled by separate simulation
logic that checks team-death state independently; the barracks message handler
does not interact with it. The exit path at `0x620815` writes a state marker on
the barracks window object itself, acknowledges the incoming message, and
updates the UI event queue — all of which are per-message bookkeeping operations
that also run on the non-null ESI path. No unit state, health, or simulation
variables are changed. A patched client and an unpatched client will produce
identical simulation outcomes.
