# Disband Peasant Duplication (Rapid-Click Exploit)

**Status:** Implemented, not yet enabled — requires broad patch adoption for multiplayer compatibility
**Affects:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)
**Config:** none (excluded from the build; see [README](../../README.md) Roadmap)

---

## Symptom

Rapid-clicking **Disband** on a selection of non-siege troops returns more
peasants than the selection contained. One soldier can hand back two, three or
more peasants — as many as the player manages to click before the simulation
gets round to removing the unit. Frame drops, UI lag and multiplayer command
latency all widen the window, because the extra clicks queue up and execute in
the same simulation tick.

Siege engines are unaffected — disbanding a catapult twice returns nothing
either time.

---

## Root cause

Disband is a two-layer command like every other player input (see
[shift-click-recruitment.md](../features/shift-click-recruitment.md) for the
general pattern). Both layers are missing an idempotency check, and the
lifecycle state that would serve as one is deliberately ignored.

All addresses are VAs at the preferred image base `0x400000`
(RVA = VA − `0x400000`).

```text
click on the Disband button (troop command panel)
  └─ SubPanelTroops::process (0x62e7d0), click case at 0x62e9cb
       ├─ getLocalPlayerSlot (0x40dbd0)
       └─ S2ActorHandler::DisbandSelectedTroops(slot) (0x4f3250)  ← ALWAYS posted
            └─ allocates a 0x14-byte DisbandTroopsChore (ctor 0x5e5550) holding
               the *current selection group's* handle, copied from
               [handler+0x1244]/[handler+0x1248], and posts it to the world
               singleton's command dispatch
  simulation executor
    DisbandTroopsChore::execute (0x5e5570)
       ├─ re-resolves the selection-group handle (bails only if the group is gone)
       └─ ActorGroup::disbandAll (0x598c20)
            └─ for every clan in the group, for every unit in the clan:
                 resolve the unit handle → call vtable slot 0x15c (disband)
  Soldier::disband (0x4f6410)                                     ← NO GUARD
       ├─ if (state != 4) state = 3                 // mark for removal
       └─ resolve the player's campfire actor → 0x48fa20 → 0x51f7b0
                                                    // hand a peasant back
```

### Nothing in that chain is idempotent

1. **The UI does not gate the post.** `SubPanelTroops::process` plays the click
   sound and calls `DisbandSelectedTroops` on every click, with no check that a
   disband is already in flight. It does not clear the selection afterwards
   either — the call at `0x62e9f8` only *queries* the selection to decide
   whether to close the panel. So click N times, post N chores.

2. **The executor does not gate either.** `DisbandTroopsChore::execute` only
   checks that the selection group still resolves. The group still contains the
   units, because nothing has removed them yet.

3. **The per-unit loop's only guard is unrelated.** `ActorGroup::disbandAll`
   wraps the virtual call in a float test:

   ```asm
   598c8d:  fldz                          ; ST0 = 0.0
   598c8f:  fcom  dword [ecx+0x4c]
   598c94:  test  ah,0x1
   598c97:  jne   0x598ca7                ; [unit+0x4c] > 0  → disband
   598c99:  fld   dword [ecx+0x48]
   598c9c:  fucompp
   598ca0:  test  ah,0x44
   598ca3:  jp    0x598cb3                ; [unit+0x48] != 0 → skip
   598ca9:  call  dword [[ecx]+0x15c]     ; disband
   ```

   i.e. `[unit+0x4c] > 0.0f || [unit+0x48] == 0.0f`. Those two floats are the
   base `Actor` animation timer pair — the identical expression appears in
   `Actor::update` at `0x78678b`, where it decides whether to bump the
   animation loop counter at `[this+0x50]`, and `0x7867b0` resets `+0x4c` from
   `+0x48`. It is an animation-state test, not a liveness test, and it is true
   for essentially every unit on the field.

4. **The state field that *would* work is skipped.** `[actor+0x20]` is the base
   `Actor` lifecycle state, driven by `Actor::update` (`0x786740`):

   ```asm
   786748:  cmp   dword [esi+0x20], ecx   ; ecx = 1
   78674b:  jne   0x786754
   78674d:  mov   dword [esi+0x20], 2     ; 1 (spawned) → 2 (active)
   786754:  cmp   dword [esi+0x20], 3
   786758:  jne   0x78676e
   78675a:  call  dword [[esi]+0x1c]      ; 3 (marked for removal) → destroy
   786763:  mov   dword [esi+0x20], 4     ; → 4 (removed)
   78676a:  xor   al, al
   78676c:  ret                           ; false = drop me from the world
   ```

   So `3` means "marked for removal, not collected yet", and `0x415ed0` —
   effectively `S2Actor::isAlive()` — is `return this[+0x98] == 0 && this[+0x20] != 3`.
   The engine already has the flag the disband path needs.

   `Soldier::disband` reads it and then throws the answer away:

   ```asm
   4f6416:  cmp   dword [ecx+0x20], 4
   4f641a:  je    0x4f6423                ; already removed → don't rewrite state
   4f641c:  mov   dword [ecx+0x20], 3     ; ... but fall through regardless
   4f6423:  mov   eax, [ecx+0x58]         ; player slot
   ...                                     ; resolve the player's campfire actor
   4f6463:  call  0x48fa20                ; ← peasant refund, runs every time
   ```

   The test protects the *state write* (don't drag a state-4 actor back to 3),
   never the refund. Re-entering with state already 3 rewrites 3 over 3 and
   refunds again.

### The window

The refund is reachable for as long as the unit sits at state 3 waiting for its
own `Actor::update` tick to run the destroy virtual and flip it to 4. Every
disband command that executes inside that window scores another peasant. This is
not a data race between threads — every one of these calls happens on the
simulation thread, in order — it is a plain missing-idempotency bug, which is
why it reproduces deterministically the moment two disband commands land in the
same tick.

That also means the trigger is not limited to human click speed: any latency
that makes several commands execute in one tick (multiplayer command batching,
a frame hitch, a stalled UI queue) produces the same result.

### Why siege engines behave differently

Slot `0x15c` has three implementations, and the disband refund lives in exactly
one of them. Mapped by walking every `Stronghold2` class's primary vtable
(`vtable[0] == 0x5226a0`):

| Implementation | Classes | Behaviour |
| --- | --- | --- |
| `0x43fe50` | 83 civilian / animal classes (`Peasant`, `Lord`, `Baker`, `Cow`, …) | bare `ret` — disband does nothing |
| `0x4f6410` | 20 troop classes (`Soldier`, `Archer`, `Spearman`, `Knight`, `Monk`, `Thief`, `Engineer`, `Ladderman`, …) | mark for removal **+ refund a peasant** |
| `0x4cbb50` | 10 siege classes (`Catapult`, `Trebuchet`, `Ballista`, `BatteringRam`, `Mantlet`, `Cat`, …) | mark for removal only |

`SiegeEngine::disband` is byte-for-byte the first two instructions of
`Soldier::disband` and stops there:

```asm
4cbb50:  cmp   dword [ecx+0x20], 4
4cbb54:  je    0x4cbb5d
4cbb56:  mov   dword [ecx+0x20], 3
4cbb5d:  ret
```

A siege engine was never built from a peasant, so it has nothing to hand back;
with no refund there is nothing to duplicate, and writing `3` over `3` is
harmless. The troop version is that same function *plus* the refund tail — which
is precisely the part that needed the guard the siege version never needed.

---

## Patch

**Site:** 3 bytes at RVA `0xf6419` (VA `0x4f6419`), inside `Soldier::disband`.

| RVA | Original | Patched |
| --- | --- | --- |
| `0xf6419` | `04 74 07` | `03 73 4c` |

Before:

```asm
4f6410:  55                    push  ebp
4f6411:  8b ec                 mov   ebp, esp
4f6413:  83 ec 08              sub   esp, 8
4f6416:  83 79 20 04           cmp   dword [ecx+0x20], 4
4f641a:  74 07                 je    0x4f6423
4f641c:  c7 41 20 03 00 00 00  mov   dword [ecx+0x20], 3
4f6423:  ... peasant refund ...
4f6468:  8b e5                 mov   esp, ebp
4f646a:  5d                    pop   ebp
4f646b:  c3                    ret
```

After:

```asm
4f6410:  55                    push  ebp
4f6411:  8b ec                 mov   ebp, esp
4f6413:  83 ec 08              sub   esp, 8
4f6416:  83 79 20 03           cmp   dword [ecx+0x20], 3
4f641a:  73 4c                 jae   0x4f6468        ; state 3 or 4 → already going
4f641c:  c7 41 20 03 00 00 00  mov   dword [ecx+0x20], 3
4f6423:  ... peasant refund ...
4f6468:  8b e5                 mov   esp, ebp
4f646a:  5d                    pop   ebp
4f646b:  c3                    ret
```

Equivalent C:

```cpp
void Soldier::disband() {
    if (state >= 3) {   // added: 3 = marked for removal, 4 = removed
        return;
    }

    state = 3;
    refundPeasantToCampfire();
}
```

The unsigned compare is safe: `[actor+0x20]` only ever holds `0`–`4`, all
written as `mov dword [reg+0x20], imm`.

### Why this shape

- **The state transition still happens on the first command**, so the unit is
  removed exactly as it always was.
- **The refund is skipped for anything already leaving the world**, which also
  closes the smaller stock oddity that a state-4 (already destroyed, not yet
  freed) unit still refunded a peasant.
- A unit that reached state 3 by dying in combat no longer refunds when a
  disband command arrives in the same tick. That is the correct outcome — a dead
  soldier is not a peasant — and it is not a behaviour players could rely on.
- Nothing is left dangling. On the first (and only effective) call the unit is
  marked exactly as before, and `Actor::update` runs the stock destroy virtual
  (`vtable+0x1c` — a shared thunk chain from `0x521600` for every actor class)
  and the stock removal path unchanged. The patch adds no cleanup and skips
  none: it removes a *duplicate side effect* only, and touches no handle,
  pointer or container. Subsequent calls now return before doing anything,
  which is strictly less work than the stock path already performed twice.

### Why not the UI or the chore

Guarding `SubPanelTroops::process` would stop *fast clicking* but not the
multiplayer path (commands from a remote client never pass through the local
pane) and not AI disbands. Guarding `DisbandTroopsChore::execute` would need
per-unit bookkeeping the chore does not carry — it holds a group handle, not a
unit list. The per-unit virtual is the single point every disband route funnels
through, and the flag it needs is already in the object.

### Implementation notes

`src/patches/disbandRefundGuard.cpp` verifies the 13 stock bytes of the state
test **and** the 4 stock bytes of the epilogue before writing anything (the
rewritten branch hard-codes the distance between them), computes the `jae`
displacement from the two RVAs rather than hard-coding `0x4c`, and bails
silently if either site has moved. No trampoline, no code cave, no vtable
writes: the 21 vtable slots pointing at `0x4f6410` are all fixed by patching the
function itself.

---

## Multiplayer compatibility

**Requires all clients.** The refund mutates shared simulation state (the
player's campfire peasant pool), which the lockstep engine cross-checks. A
patched client that refunds one peasant while an unpatched client refunds three
for the same command sequence diverges immediately and desyncs both.

This is the same classification as
[ballista-auto-fire.md](../features/ballista-auto-fire.md) and
[unit-cap-raise.md](../features/unit-cap-raise.md), and for the same reason the
patch is **excluded from the build** until the patch has broad adoption. The
`.cpp`/`.h` are not in `SRCS` and `installDisbandRefundGuard()` is not called
from `registry.cpp`; enabling it is a two-line change once that changes.

It is worth noting the exploit itself is *not* a desync in stock play: every
client runs the same duplicated refund from the same command stream, so
unpatched lobbies stay in sync while quietly handing the clicker free peasants.

---

## Verification log

- [x] `DisbandTroopsChore` resolved by RTTI walk (type descriptor `0xabae04`,
      COL `0xa111ac`, vtable `0x9c6a40`, `execute` = slot 2 = `0x5e5570`),
      cross-checked against `CreateSiegeEquipmentChore`'s identical layout
- [x] Slot `0x15c` mapped across all 113 `Stronghold2` classes whose primary
      vtable starts with `0x5226a0`; three implementations, table above
- [x] `Actor::update` (`0x786740`) confirms the `1 → 2 → 3 → 4` lifecycle and
      that state 3 survives until the actor's next tick
- [x] `S2ActorHandler::DisbandSelectedTroops` = `0x4f3250`, called only from
      `SubPanelTroops::process` at `0x62e9e5`; sits directly after
      `StopSelectedTroops` (`0x4f3140`, from
      [stop-troops-hotkey.md](../features/stop-troops-hotkey.md)), same shape
- [x] No branch anywhere in `.text` targets the patched bytes (full rel8 /
      rel32 / jcc-near scan of `0x401000`–`0x900000`; the only hit is the `je`
      being replaced), and no absolute dword references them
- [x] Stock bytes at `0xf6416` and `0xf6468` confirmed against a clean Steam
      v1.5.0 `Stronghold2.exe`
- [x] Patched byte stream re-disassembled and matches the intent
- [x] Compiles clean (`-Wall -Wextra`, no warnings)
- [ ] Single-player: rapid-click disband returns exactly one peasant per unit
- [ ] Single-player: siege engine disband unchanged
- [ ] Single-player: disbanding a unit that is dying from combat in the same
      tick behaves sanely
- [ ] Multiplayer: patched ↔ patched lobby, mass disband under latency, no
      desync
