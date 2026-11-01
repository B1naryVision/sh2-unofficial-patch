# Ballista Auto-Fire Restore

**Status:** Added in v0.8.0 as a host-chosen [match rule](match-rules.md) (`[matchrules] BallistaAutoFire`):
switched on per match only when every player supports it. Never installed on its own.  
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

`Ballista::think` (RVA `0x177f40`) has an equivalent `[edi+0x308]` check, and it branches the
same way: non-zero *skips* the search. The flag means "a firing sequence is in progress" in
both classes. It is set on target assignment by vtable `+0x228` (RVA `0x175fd4`) and cleared by
the slot-20 firing state machine when the sequence ends. The tower searches again every 40 ticks
while the flag is clear and never looks at `[this+0x198]` in `think`. The field ballista
instead sends every engaged tick to the manual-target handler. See
[Known Bug: target lock](#known-bug-target-lock).

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

Four patches are required. Since it became a match rule, `installBallistaAutoFire` only
verifies every site against its stock bytes (all-or-nothing) and prepares the patched bytes
and the cave; `ballistaAutoFireSetEnabled` writes or restores all four together, on the
simulation thread. The call's stock bytes encode a displacement, so they are rebuilt from
the two RVAs rather than compared as a literal.

**Patch 1 — Route the deployment-flag gate through a cave (13 bytes, RVA `0x180c48`)**

The site (`cmp byte [esi+0x308], 0` / `jne`) is replaced by `jmp cave` + 8 NOPs. The 82-byte
cave (`VirtualAlloc`, allocated once at install):

- `[esi+0x198] == 0` → no target: back to `0x180c55` (auto-fire);
- the held target passes `canEngage` (vtable `+0x180`) and `S2Actor::isAlive` →
  `jmp 0x180f39` (manual-target handler, the stock path);
- otherwise → release the target, clear `[esi+0x352]` so the tick gate searches again at once,
  then back to `0x180c55`.

The full listing and the reasoning behind it are under
[Known Bug: target lock](#known-bug-target-lock). Install additionally verifies the first
13 bytes of `S2Actor::isAlive` and that `FireBallista`'s vtable slot `+0x180` (RVA `0x5c1294`)
points at `0x1778d0`. The slot is a relocated pointer, so it is compared against the rebuilt
address.

History of this site:

1. **NOP.** The first version NOPed the 6-byte `jne +0x2e4`. Ballistae that had been moved
   then stopped firing.
2. **57-byte "own cell" cave.** This version blamed movement for writing a destination cell to
   `[this+0x198]`, and it cleared the field when it matched the ballista's own position
   (`[esi+0x30]`, `[esi+0x34]`).
3. **Correction (static trace, 2026-10-31).** `[this+0x194]`/`[this+0x198]` is an 8-byte actor
   handle `{pointer, id}`, not a cell. The id's low word is the serial and its high word is the
   actor-table index, and the handle resolves through `0x369360`. Every writer reachable from a
   `FireBallista` stores either a resolved handle or zero:

   - the target setter (vtable `+0x19c`, `0x17f750`)
   - aim-at-point (`+0x228`, `0x175fc6`)
   - reset (slot 86, `0x17f7c0`)
   - the manual handler (`0x180f6b`)

   The own-cell test only matched by coincidence. The move path itself was never traced, but
   the stuck-after-moving symptom fits the target lock: a target picked before the move is
   still alive but now out of range.

**Patch 2 — NOP the player-active `je` (6 bytes, RVA `0x180c71`)**

NOP out the 6-byte `je +0x2c2` so execution always falls through to the rotation check, regardless of whether `0x415f20` returns 0 (human) or 1 (AI). The player lookup and call remain in place; only their result is discarded.

**Patch 3 — Bypass the dead `[this+0xb4]` gate (1 byte, RVA `0x180c92`)**

Change `je +0x10` (`74`) to `jmp short +0x10` (`eb`) so execution unconditionally reaches the tick gate. The preceding `xor ebx,ebx` is preserved; `bl=0` is required by `cmp byte [esi+0x352], bl` inside the tick gate.

**Patch 4 — Redirect the fire-path CALL (5 bytes, RVA `0x180ef9`)**

Redirect the CALL from `0x17f8c0` (the broken cached-target retriever) to `0x177b90` (Ballista's proven fresh-search function). Both share the same thiscall signature: ECX=this, `[ESP+4]`=result_buf pointer, returns buf pointer. Both read `[this+0x30]`/`[this+0x34]` as cell coordinates and begin with `flds 0x99df04`. The search only returns a handle. The stock code after the call checks it (`0xdc70`) and passes it to the target setter (vtable `+0x19c`, RVA `0x17f750`). The setter stores the handle in `[this+0x194]`/`[this+0x198]` and starts the firing sequence through `+0x228`. The tower does the same at RVA `0x177f8f`–`0x177fbd`.

| # | VA (ImageBase 0x400000) | RVA | Size | Before | After |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x580c48` | `base+0x180c48` | 13 bytes | `80 be 08 03 00 00 00 0f 85 e4 02 00 00` | `e9 <cave>` + `90`×8 (82-byte cave) |
| 2 | `0x580c71` | `base+0x180c71` | 6 bytes | `0f 84 c2 02 00 00` (je `+0x2c2`) | `90 90 90 90 90 90` (NOP×6) |
| 3 | `0x580c92` | `base+0x180c92` | 1 byte | `74` (je) | `eb` (jmp short) |
| 4 | `0x580ef9` | `base+0x180ef9` | 5 bytes | `e8 c2 e9 ff ff` (CALL `0x57f8c0`) | `e8 92 6c ff ff` (CALL `0x577b90`) |

Displacement arithmetic for patch 4: next IP = `0x180efe`; target = `0x177b90`; disp = `0x177b90 − 0x180efe` = `−0x936e` = `0xffff6c92` little-endian → `92 6c ff ff`.

---

## Known Bug: Target Lock

**Status:** fixed in the patch 1 cave (2026-10-31). The root cause was traced statically. The
cave bytes were checked by disassembling the built template, and every stock site and the
`canEngage` vtable slot were checked against the exe on disk. **Playtested 2026-10-31:**
ballistae switch targets quickly when an enemy leaves range, and manual targeting still works.

### Symptom

A field ballista picks a target, and then the target walks out of range or out of line of
sight while staying alive. The ballista then stops firing completely. It ignores other enemies
in range, and it often does not resume even when the original target comes back. It recovers
only when that target dies. Ballistae whose targets die quickly look fine, which is why this
was missed.

### Root cause

From the second tick on, the patch turns every auto-acquired target into a "manual" target,
and nothing ever re-checks a manual target's range or line of sight:

1. Patch 4's fire path passes the fresh-search result to the target setter (vtable `+0x19c`,
   `0x17f750`), which writes the handle to `[this+0x194]`/`[this+0x198]`.
2. On the next tick the patch 1 cave sees `[this+0x198] != 0`. Apart from the own-cell
   coincidence above, it jumps to the manual-target handler at `0x180f39`.
3. The manual handler clears the handle only when `0x369360` **fails to resolve it**: index out of
   range, empty table slot, serial mismatch or wrong class. Alive state (`S2Actor::isAlive`,
   `0x15ed0`: `[+0x98] == 0 && [+0x20] != 3`), range and line of sight are never tested. If the
   handle resolves, the handler only records the ballista as an attacker on the target
   (`0xe5b60`, which writes into a two-slot list at target `+0x274`) and returns. It does not aim
   and it does not fire.
4. The slot-20 firing state machine (`0x180fc0`) reads the handle but **never writes
   `[this+0x198]`**. When the target leaves range, its aim solver (`0xd0330` / `0xd0110`) returns
   the no-solution sentinel (−1e8, at VA `0x99df00`) and the sequence drops out. The handle
   stays set.
5. The tick gate is the only place that runs the fresh search, and it sits *after* the cave. With
   the handle stuck, the cave sends every tick to the manual handler and the ballista never
   searches again.

The tower never hits this, because its `think` holds no target. It relies on the firing state
machine clearing `[this+0x308]` and searches again every 40 ticks.

**Not proven statically:** why the ballista does not re-engage the *original* target when it
comes back into range. The likely reason is that only a call to vtable `+0x228` (made through the
setter) starts a firing sequence, and the manual handler makes no such call. So once the
sequence has dropped out, the ballista holds a handle with no sequence behind it. This needs a
runtime check.

### Fix: validate the held target in the cave

This replaces the own-cell branch of the patch 1 cave. The cave runs every `think` tick, so checking
the held target there means it is re-evaluated every tick. It uses the engine's own predicate
instead of new logic:

- **vtable `+0x180` = `canEngage(unused, Handle target)`**, `0x1778d0`, slot 96, shared by
  `Ballista` and `FireBallista`, `ret 0xc`. It resolves the handle with `0x369360`, tests range
  through vtable `+0x154` (`0x17f710` → distance check `0xcff20`) and tests line of sight with
  `0x1ced70` on `0x1242fa4`, the same terrain check the search loop uses at `0x180e25`. The first
  argument is never read. Stock callers at `0xf0aa4` and `0xf0b7b` use the same shape:
  `actor->vt[0x180](x, handle.ptr, handle.id)`.
- **`S2Actor::isAlive`** (`0x15ed0`) on top, because `canEngage` does not reject a unit that is
  dying (state 3) or already dead (`+0x98` set).

```asm
cave:
    cmp   dword [esi+0x198], 0      ; 83 be 98 01 00 00 00
    je    auto_fire                 ; 74 44            no target
    push  dword [esi+0x198]         ; ff b6 98 01 00 00   Handle.id
    push  dword [esi+0x194]         ; ff b6 94 01 00 00   Handle.ptr (callee overwrites)
    push  0                         ; 6a 00            unused by 0x1778d0
    mov   eax, [esi]                ; 8b 06
    mov   ecx, esi                  ; 8b ce
    call  [eax+0x180]               ; ff 90 80 01 00 00   canEngage, pops 0xc
    test  al, al                    ; 84 c0
    je    release                   ; 74 14
    mov   ecx, [esi+0x194]          ; 8b 8e 94 01 00 00
    call  S2Actor::isAlive          ; e8 <rel32 → base+0x15ed0>
    test  al, al                    ; 84 c0
    je    release                   ; 74 05
    jmp   manual_handler            ; e9 <rel32 → base+0x180f39>   valid: stock path
release:
    xor   eax, eax                  ; 31 c0
    mov   [esi+0x194], eax          ; 89 86 94 01 00 00
    mov   [esi+0x198], eax          ; 89 86 98 01 00 00
    mov   [esi+0x352], al           ; 88 86 52 03 00 00   take the immediate-fire path
auto_fire:
    jmp   player_check              ; e9 <rel32 → base+0x180c55>
```

The cave is 82 bytes (was 57). It holds three rel32 fields, at cave offsets `0x2c`, `0x35` and
`0x4e`, and no absolute addresses, so it is position-independent apart from those
install-time displacements. The site patch at `0x180c48` and the other three patches are
unchanged.

Why each piece is shaped this way:

- **Safe to dereference `[this+0x194]` after `canEngage` succeeds.** Every writer of `+0x198`
  stores `+0x194` from the same resolution. If the id still resolves, which `canEngage` has just
  confirmed, the pointer is the same live object. If the id no longer resolves, the cave never
  dereferences the pointer.
- **Re-scan immediately.** Clearing `[this+0x352]` sends the tick gate down its immediate path
  (`cmp byte [esi+0x352], bl / je 0x180ef3`). The fresh search therefore runs on the same tick
  the target is released, instead of up to 60 ticks later. If the search finds nothing, the fire
  path sets `+0x352` back to `1` and the normal 60-tick cadence resumes. Each release costs at
  most one extra search.
- **Leave `[this+0x308]` alone.** It belongs to the firing state machine. While a sequence is in
  progress, vtable `+0x228` does not restart it. It queues the new aim point in `+0x318`/`+0x31c`
  (`0x176000`), and the state machine adopts that point when the sequence finishes (`0x181a05`).
  That handover is how the new target gets engaged without the ballista freezing.
- **Registers and FPU.** At `0x180c55`, `eax`, `ecx` and `edx` are dead (all three are reloaded
  before use). `ebx` and `edi` are saved by the prologue and are not yet initialised, and every
  callee preserves them. The cave runs straight after `think`'s prologue and a call that returns
  `al`, so the x87 stack is empty and the float work in `canEngage` is legal.
- **Determinism.** The cave reads only simulation state: the actor table, positions and the
  terrain line-of-sight grid. The LOS helper calls only `0x18e20`, a position-to-cell
  conversion, and draws no random numbers, so every client releases the target on the same tick.

### Playtest results and remaining notes

- **Re-targeting (confirmed).** When a living target walks out of range, the ballista quickly
  switches to another enemy.
- **Manual targeting (confirmed).** Player-ordered targets still work. The fix drops *any*
  target that fails `canEngage`, so this was the main risk. If it ever regresses, for example
  with an attack order on a unit far out of range, restrict the release to the case where no
  firing sequence is running (`[this+0x308] == 0`). That mirrors the tower, which only re-decides
  when idle.
- **Per-tick cost.** One distance test and one LOS walk per engaged ballista per tick.
  This is negligible for normal ballista counts, but if needed, throttle it to the tick gate's
  `T % 60 == 30` slot.
- **Diagnosing a future report.** A stuck ballista shows `[this+0x198]` non-zero, resolving to
  a live target. Check `site[0] == 0xe9` at `0x180c48` first, to rule out "the rule never
  switched on".

### Compatibility

- **Saves and stock installs.** Unaffected. The fix only changes code bytes while the match rule
  is on, writes only fields the engine already writes (`+0x194`, `+0x198`, `+0x352`) with values
  the engine itself uses, and adds no state.
- **Multiplayer.** The old cave and the new one behave differently, so builds with each would
  desync if the rule were on. The fix still keeps rule bit `0x1`: the rule (and the handshake
  that carries it) first shipped in v0.8.0, with the new cave, so no published build has the old cave. Development builds
  from before 2026-10-31 must not share a match with this rule on. If the ballista logic changes
  again after a release, it needs a new bit, because `handshake.h` forbids reusing one.

---

## Multiplayer Compatibility

Stronghold 2 multiplayer runs a deterministic lockstep simulation: both clients execute the same game logic on the same shared state each tick. Field ballista auto-fire modifies that shared state (enemy unit health). If one client fires and the other does not, the simulation diverges and the engine detects a desync, crashing both clients.

**Requires all clients.** That is why it is a [match rule](match-rules.md): the host can only
switch it on when every player in the lobby advertises it. It is switched on for every client
on the same lockstep tick, and with anyone lacking it the match stays vanilla.

Replays record and restore it with the other rules (bit `0x1`). This rule is the case that
showed the need: without the recorded rules word, playback read as single player and field
ballistae stood idle in a replay of a match where they had fired.

---

## Investigation Notes

- Runtime logging confirmed all four defects. Key findings:
  - `[this+0x308]` starts at `0x00` on fresh deployment and is set to `0x01` by the fire path after the first shot. The original static analysis incorrectly predicted it was always nonzero for deployed units; it is only nonzero *after* firing once.
  - `[this+0x352]` (immediate-fire flag) starts at `0x00` and is set to `0x01` after the first shot. With all four patches applied, `[this+0x352]==0` triggers an immediate fire on tick 1, then `T%60==0` fires every 60 ticks.
  - `0x415f20` was confirmed to return `result=0` for human player index 4 on every tick across 100 logged calls.
- `FireBallista::think` has a float guard at `0x580c77`: `FLD [0x9a0878]=0.001f`; `FCOMP [ESI+0xe0]`. Rotation speed at `[this+0xe0]` is 0.0 for a stationary deployed ballista, so the check always passes. This guard is correct behaviour and unrelated to the bug.
- RVA `0xdc70` (VA `0x40dc70`), once noted here as the shared fire function, is a thiscall
  validity check on an 8-byte handle. It runs on the search result before the setter, and on
  `[this+0x194]` in the firing state machine (`0x1812a0`). Tower and field ballistae do share the
  setter's behaviour and vtable `+0x228`/`+0x180`/`+0x154`. Their target-holding logic in
  `think` is what differs.
- The logged "`[this+0x308]` set after the first shot" is the write in vtable `+0x228`
  (`0x175fd4`), reached from the target setter. It happens on target assignment, which is the
  same fire-path tick as the first shot.
- Ghidra addresses equal RVAs directly (Ghidra loads `Stronghold2.exe` at ImageBase `0x00000000`). x32dbg addresses at the game's preferred load address equal VAs listed above.
