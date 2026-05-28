# Ballista Auto-Fire Restore

**Status:** Implemented, not yet enabled — requires broad patch adoption for multiplayer compatibility  
**Affects:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)

---

## Symptom

Field ballistae do not automatically target or fire at enemies. Tower-mounted ballistae fire normally. The behaviour diverged in patch 1.4.0, which Firefly described as a balance change.

However, this has actually made knights exceptionally strong: 4 times out of 5, the player with the most knights wins. Knights can be countered with catapults, but their low range means they can be swarmed quickly. Burning large army segments helps, but a well-managed knight rush wins most games against players who cannot keep up economically.

---

## Root Cause

The game has two ballista classes:

- `Ballista` — tower-mounted; think function at RVA `0x177f40`
- `FireBallista` — field-deployed; think function at RVA `0x180c30`

`Ballista::think` calls `0x177b90` to perform a fresh enemy search and fire on every eligible tick. This works correctly.

`FireBallista::think` is blocked by **four independent defects**, all confirmed by runtime logging:

### Defect A — Deployment-flag gate at `[this+0x308]` (RVA `0x180c4f`)

```asm
; RVA 0x180c48
cmp  byte [esi+0x308], 0
jne  0x180f39              ; non-zero → manual-target handler
```

`[this+0x308]` is zero on first deployment. The fire path itself sets it to `0x01` after the first shot. From tick 2 onward the `jne` is always taken, routing all subsequent think calls to the manual-target handler at `0x180f39` (which exits immediately when `[this+0x198]` is zero). Auto-fire fires once, then is permanently suppressed.

`Ballista::think` has an equivalent `[edi+0x308]` check, but uses `jne` in the opposite sense (non-zero means *proceed to fire*), reflecting the different semantics of the flag in each class.

### Defect B — Human-player gate at `call 0x415f20` (RVA `0x180c71`)

After the deployment-flag check, `FireBallista::think` reads the player index from `[this+0x58]`, bounds-checks it against 32, looks it up in the player table at `0xae8bd8`, and calls `0x415f20` with the player object:

```asm
; RVA 0x180c55
mov  eax, [esi+0x58]          ; player index
cmp  eax, 0x20                ; bounds-check < 32
jae  null_player
mov  eax, [eax*4+0x00ae8bd8]  ; player object pointer
null_player:
mov  ecx, eax                 ; ECX = player_obj (or NULL)
call 0x415f20                 ; returns 0 for human, 1 for AI
test al, al
je   0x180f39                 ; 0 → manual-target handler
```

`0x415f20` returns 0 for human-controlled players and 1 for AI. For a human-owned field ballista the `je` is taken on every tick, routing to the manual-target handler before the tick gate is ever reached. Auto-fire is permanently blocked for human players.

`Ballista::think` has **no equivalent check** — tower ballistae auto-fire for any player type.

### Defect C — Dead gate at `[this+0xb4]` (RVA `0x180c92`)

```asm
; RVA 0x180c8a
xor  ebx, ebx
cmp  dword [esi+0xb4], ebx   ; is float field == 0.0?
je   tick_gate               ; only proceed if zero
; fall-through: reset [esi+0x352]=0, return al=1
```

`[this+0xb4]` is a float field initialised to `1536.0` (`00 00 c0 44`) in the constructor. It is never set to `0.0`. The `je` (zero-flag branch) is therefore never taken; execution falls to an epilogue that resets `[this+0x352]` to 0 and returns — bypassing the tick gate entirely. Auto-fire is permanently blocked.

`Ballista::think` has no equivalent check.

### Defect D — Broken cached-target retriever (RVA `0x180ef9`)

With the gates above removed, `FireBallista::think` reaches a tick-based fire structure:

- `[this+0x352] == 0` → immediate fire path (first deployment)
- `T % 60 == 0` → fire path (every 60 ticks thereafter)
- `T % 60 == 30` → cache-search path

The fire path at `0x180ef3` calls `0x17f8c0` to retrieve a cached target and fire. `0x17f8c0` is a cached-target retriever whose cache is never populated (the cache-search path at `T%60==30` uses filters that reject all valid targets). It returns an empty result buffer, so the fire call does nothing.

---

## Fix

Four patches are required.

**Patch 1 — NOP the deployment-flag `jne` (6 bytes, RVA `0x180c4f`)**

NOP out the 6-byte `jne +0x2e4` so execution always falls through to the player check. This prevents the post-first-shot suppression.

**Patch 2 — NOP the player-active `je` (6 bytes, RVA `0x180c71`)**

NOP out the 6-byte `je +0x2c2` so execution always falls through to the rotation check, regardless of whether `0x415f20` returns 0 (human) or 1 (AI). The player lookup and call remain in place; only their result is discarded.

**Patch 3 — Bypass the dead `[this+0xb4]` gate (1 byte, RVA `0x180c92`)**

Change `je +0x10` (`74`) to `jmp short +0x10` (`eb`) so execution unconditionally reaches the tick gate. The preceding `xor ebx,ebx` is preserved; `bl=0` is required by `cmp byte [esi+0x352], bl` inside the tick gate.

**Patch 4 — Redirect the fire-path CALL (5 bytes, RVA `0x180ef9`)**

Redirect the CALL from `0x17f8c0` (the broken cached-target retriever) to `0x177b90` (Ballista's proven fresh-search-and-fire function). Both share the same thiscall signature: ECX=this, `[ESP+4]`=result_buf pointer, returns buf pointer. Both read `[this+0x30]`/`[this+0x34]` as cell coordinates and begin with `flds 0x99df04`.

| # | VA (ImageBase 0x400000) | RVA | Size | Before | After |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x580c4f` | `base+0x180c4f` | 6 bytes | `0f 85 e4 02 00 00` (jne `+0x2e4`) | `90 90 90 90 90 90` (NOP×6) |
| 2 | `0x580c71` | `base+0x180c71` | 6 bytes | `0f 84 c2 02 00 00` (je `+0x2c2`) | `90 90 90 90 90 90` (NOP×6) |
| 3 | `0x580c92` | `base+0x180c92` | 1 byte | `74` (je) | `eb` (jmp short) |
| 4 | `0x580ef9` | `base+0x180ef9` | 5 bytes | `e8 c2 e9 ff ff` (CALL `0x57f8c0`) | `e8 92 6c ff ff` (CALL `0x577b90`) |

Displacement arithmetic for patch 4: next IP = `0x180efe`; target = `0x177b90`; disp = `0x177b90 − 0x180efe` = `−0x936e` = `0xffff6c92` little-endian → `92 6c ff ff`.

---

## Multiplayer Compatibility

Stronghold 2 multiplayer runs a deterministic lockstep simulation: both clients execute the same game logic on the same shared state each tick. Field ballista auto-fire modifies that shared state (enemy unit health). If one client fires and the other does not, the simulation diverges and the engine detects a desync, crashing both clients.

**Both players must have the patch installed.** Playing against an unpatched client will cause a desync crash.

Players without the patch will also be at a mechanical disadvantage: their field ballistae will not auto-fire.

---

## Investigation Notes

- Runtime logging confirmed all four defects. Key findings:
  - `[this+0x308]` starts at `0x00` on fresh deployment and is set to `0x01` by the fire path after the first shot. The original static analysis incorrectly predicted it was always nonzero for deployed units; it is only nonzero *after* firing once.
  - `[this+0x352]` (immediate-fire flag) starts at `0x00` and is set to `0x01` after the first shot. With all four patches applied, `[this+0x352]==0` triggers an immediate fire on tick 1, then `T%60==0` fires every 60 ticks.
  - `0x415f20` was confirmed to return `result=0` for human player index 4 on every tick across 100 logged calls.
- `FireBallista::think` has a float guard at `0x580c77`: `FLD [0x9a0878]=0.001f`; `FCOMP [ESI+0xe0]`. Rotation speed at `[this+0xe0]` is 0.0 for a stationary deployed ballista, so the check always passes. This guard is correct behaviour and unrelated to the bug.
- Tower and field ballistae share the fire function at RVA `0xdc70` (VA `0x40dc70`). Only the target-selection step differs.
- Ghidra addresses equal RVAs directly (Ghidra loads `Stronghold2.exe` at ImageBase `0x00000000`). x32dbg addresses at the game's preferred load address equal VAs listed above.
