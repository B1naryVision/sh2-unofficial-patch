# MP Connect Complete Crash

## Symptom

The game crashes with exception code `0xC0000005` (access violation) at fault offset
`0x003d86b8` during a multiplayer session. The crash occurs when a `CONNECT_COMPLETE`
network message arrives for a peer that has no entry in the local peer table.

Windows Event Log entry:
```
Exception code: 0xc0000005
Fault offset:   0x003d86b8
Module:         Stronghold2.exe v1.5.0
```

## Root Cause

The function `GameSpy::Network::handleConnectCompleteMessage` (starts near VA `0x7d85a0`)
processes incoming connection-complete handshakes. Its structure:

1. Call `findPeer(id)` → result in `esi`.
2. If `esi == 0` (peer not found): jump to error-log path at `0x7d869a`.
3. If `[esi+0x3d] != 0` (peer already connected): jump to same path at `0x7d869a`.
4. Otherwise: process the connection normally.

The error-log path at `0x7d869a` is intended to log:
> "CONNECT_COMPLETE: Peer %S (%s) has completed connection but does not have an active peer entry!!"

To build this message it formats the peer's numeric ID (from the argument in `edi`) via
`sprintf`, then retrieves the peer's name string from `esi+0x18` (an embedded `std::string`
inside the peer object).

The bug: the path at `0x7d869a` is **shared** by both the null-peer case and the
already-connected case. In the null-peer case `esi` is `0`. After:

```asm
7d86b2:  add esi, 0x18      ; esi = 0 → esi = 0x18
7d86b5:  add esp, 0x14
7d86b8:  cmp DWORD PTR [esi+0x14], 0x8   ; reads from 0x2C — null page → AV
```

the dereference hits address `0x2C`, which is in the null page → access violation.

The already-connected case (second jump source, `0x7d85d0`) is not affected because `esi`
is a valid pointer there; `esi+0x18` is a valid string.

### When Does This Trigger?

A `CONNECT_COMPLETE` arrives for a peer with no table entry. Possible causes:
- Duplicate `CONNECT_COMPLETE` messages (peer already removed after first)
- Race condition in GameSpy peer-table management
- Stale messages delivered out of order

## Patch

**Site**: `0x7d85c6` (RVA `0x3d85c6`)

```asm
; Before: null-peer path jumps to error-log handler (which then crashes)
0f 84 ce 00 00 00   je 0x7d869a

; After: null-peer path jumps directly to function return epilogue
0f 84 32 01 00 00   je 0x7d86fe
```

**Return epilogue at `0x7d86fe`:**
```asm
5f          pop edi
5e          pop esi
8b e5       mov esp, ebp
5d          pop ebp
c2 0c 00    ret 0xc
```

Only the null-peer case is redirected. The already-connected case (`jne 0x7d869a` at
`0x7d85d0`) is unchanged and continues to reach the error-log path correctly, where `esi`
is a valid pointer.

The spurious `CONNECT_COMPLETE` for an unknown peer is silently ignored; the function
returns cleanly with no side effects.

## Bytes Summary

| Address (VA) | RVA | File offset | Before | After |
| --- | --- | --- | --- | --- |
| `0x7d85c6` | `0x3d85c6` | `0x3d79c6` | `0f 84 ce 00 00 00` | `0f 84 32 01 00 00` |

## Multiplayer Compatibility

**Safe for version mismatch.** The patch makes the local client silently ignore an invalid
network message instead of crashing. No simulation state is read or written; no unit
fires, moves, or takes damage as a result of this fix. An unpatched client receiving the
same spurious message will crash and end the session for both players, but the patched
client does not introduce any state divergence.
