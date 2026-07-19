# Shift-Click Batch Recruitment

**Status:** Implemented, awaiting playtest (single-player and multiplayer)
**Affects:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)
**Config:** `[recruitment] RecruitmentShiftMultiplier` in `sh2-unofficial-patch.ini` (default `20`, `0`/`1` disables)

---

## Description

Holding **Shift** while clicking a unit icon in the **barracks**, **mercenary
post**, **monastery**, **engineers guild** (engineer button; see the known
limitation for its ladderman button) or **siege camp** (all nine engine
buttons plus the engineer button) queues up to `RecruitmentShiftMultiplier`
units in one click instead of one. The batch stops early when gold, weapons, peasants, honor or
the unit cap run out — exactly as if the player had clicked the icon that
many times by hand.

---

## How the engine recruits a unit (reverse-engineering trail)

The recruitment pipeline is split between the UI layer and the simulation
layer. All addresses are VAs at the preferred image base `0x400000`
(RVA = VA − `0x400000`).

```text
click on unit icon
  └─ SubPanelBarracks::process (0x6206d0)        [same shape in MercPost/Monastery]
       ├─ canRecruit(player, unitId)  (0x418640) ← advisor feedback only
       │    └─ on failure: play advisor line for reason at [player+0x10e4]
       │    └─ on success: play click sound (Dragonfly::SoundUtil::playSound)
       └─ postRecruitCommand(ctx, unitId) (0x4ef700)   ← ALWAYS posted
            └─ allocates a small command event {byte unitId @+8, byte ctx @+9}
               and hands it to the game/world singleton (0x11781d0) → command
               dispatch (0x5e4dd0/0x5e4e40) → simulation executor
  simulation executor (0x41cae0)
       ├─ canRecruit(player, unitId) (0x418640)  ← RE-VALIDATED here
       │    └─ fails → command silently dropped
       └─ takes a peasant from the campfire ([player+0x674], 0x4eda20),
          deducts cost, creates the unit object (spawn hook 0x4ee3be fires)
```

Key facts established from disassembly:

- `0x418640` (`canRecruit`) is **validation only**: it maps the unit id
  through the type table at `0xa8d750`, then checks gold (float at
  `[player+0x1010]` vs cost table `0xa8d840`), honor (`[player+0x1c]` vs
  `0xa8d890`), required weapons (tables `0xa8d8f8`/`0xa8d8fc` vs inventory at
  `[player+0xf5c]`), available peasants (campfire at `[player+0x674]`), and
  the army cap. It writes nothing except a failure-reason code at
  `[player+0x10e4]` (2 = gold, 3 = weapons, 4 = peasants, 6 = honor,
  0xa = unit cap, …). This is the same function family already documented in
  [unit-cap-raise.md](unit-cap-raise.md).
- `0x4ef700` (`postRecruitCommand`) is the **single shared post helper** for
  recruit commands. It has exactly five call sites in the executable, all in
  UI pane handlers. Convention: two stack dwords `(ctx, unitTypeId)`, callee
  cleans the stack (`ret 8`). ECX is set to the `S2ActorHandler` global
  (`0x11b8cb8`, known from the stop-troops patch) by every caller but is
  **dead on entry** — the function overwrites it before any read.
- The `ctx` byte comes from a no-arg virtual getter (`vtbl+0x78`) on the
  `GameInterface` singleton (`[0x12c0d6c]`, getter `0x5ec430`) and is captured
  by the hook from the original call's argument, so reposted commands are
  bit-identical to the original one.
- The UI posts the command **even when its own validation failed** — safe
  because the simulation executor at `0x41cae0` re-validates every command
  against live resources before executing it and silently drops failures.
  This engine-side re-validation is what makes batch posting inherently safe.

### Pane handler identification

Recovered via MSVC RTTI (type descriptor → complete object locator → vtable):

| Class | vtable | `process(Event)` | recruit post site(s) |
| --- | --- | --- | --- |
| `SubPanelBarracks` | `0x9cbe0c` | `0x6206d0` | `0x62083a` |
| `SubPanelEngineersGuild` | `0x9cc40c` | `0x623be0` | `0x623ca9`, `0x623dc9` |
| `SubPanelMercPost` | `0x9cc5f4` | `0x625490` | `0x625622` |
| `SubPanelMonastery` | `0x9cca94` | `0x6284d0` | `0x628539` |

`SubPanelBarracks::process = 0x6206d0` cross-checks against the handler
documented in [barracks-lord-death-crash.md](../bugs/barracks-lord-death-crash.md).

That accounts for all five call sites of the shared post helper. The
Engineers Guild pair was initially misattributed to
`SubPanelBarracksAssemblyPoints` (its `process` at `0x623be0` sits between
that class's virtuals in the address space) and left unhooked; re-running the
RTTI mapping for `SubPanelEngineersGuild` placed both sites inside its
`process`. The two hooked buttons post hard-coded unit ids: `0x30` at
`0x623ca9` and `0x1d` (engineer — the same id the siege camp's engineer
button posts) at `0x623dc9`. Both are unit types the executor pairs with a
companion object (`0x1d` at `0x41cca2`, `0x30` at `0x41cdf6`), and both pass
through the standard `canRecruit` gate at `0x41cc4d` before anything is
created, so they batch exactly like the other infantry posts.

**Known limitation — guild laddermen do not batch.** The guild handler's
third click case (`0x623de3`) builds a 0x28-byte `CreateSiegeEquipmentChore`
inline — the same event class as the siege camp's engine buttons (ctor
`0x5e4a00`), with the equipment type from the button payload at `+0x8` and
the *guild* building handle at `+0x10`/`+0x14`. Playtest confirmed this is
the path the guild's ladderman button takes (the guild's engineer button
batches; its ladderman button does not), which also disproves the earlier
guess that the `0x30` post site was the ladderman button — `0x30` is some
other guild-recruitable unit. The limitation is accepted: laddermen batch
normally through the siege camp pane, whose `process` is wrapped. If guild
batching is ever wanted, the fix is the same vtable-wrapper recipe applied
to `SubPanelEngineersGuild` (vtable `0x9cc40c`, slot 0 = `0x623be0`).

### Siege camp (different command path)

`SubPanelSiegeCamp::process` (`0x628c50`, vtable `0x9ccba4`) does **not** use
the shared post helper — it builds its command events inline, which is why
the original three-site hook did not affect siege equipment:

- Buttons `0xfff`–`0x1007` (the nine engine types): allocates a 0x28-byte
  **`CreateSiegeEquipmentChore`** event (class name embedded after its vtable
  at `0x9c68fc`, ctor `0x5e4a00`) with the engine type id at `+0x8` (copied
  from the click message payload), the local player slot at `+0xc` (from the
  `GameInterface` `vtbl+0x78` getter — the slot indexes the player table at
  `0xae8bd8`), and the siege camp object pointer/handle id at `+0x10`/`+0x14`
  (from `[pane+0x4c40]`/`[pane+0x4c44]`).
- Button `0x1008` (engineer): builds the same 0x1c-byte event class the
  infantry panes use, with hard-coded unit id `0x1d` and the camp handle in
  `+0x14`/`+0x18` (the executor links the new engineer to that camp).

Both are posted straight to the world singleton's dispatch (`vtbl+0x20` on
`0x11781d0`). Execution is re-validated exactly like infantry:
`CreateSiegeEquipmentChore::execute` (`0x5e4a50`) → creation worker
(`0x41ab50`), which first re-resolves the siege camp handle (bails if the
camp is gone) and then calls `canRecruitSiege` (`0x418940`, the third
function of the validation family in [unit-cap-raise.md](unit-cap-raise.md))
**before creating anything** — a failed command is silently dropped.

Because the event construction is inline, there is no 5-byte call to
retarget. Instead the patch swaps `SubPanelSiegeCamp`'s `process` vtable slot
(one dword in `.rdata` at `0x9ccba4`, RVA `0x5ccba4`) for a wrapper that
calls the stock handler, then — for a Shift-held click on subids
`0xfff`–`0x1008` — re-invokes the stock handler with the same message up to
`multiplier − 1` more times, gated on `canRecruitSiege` (engines) or
`canRecruit` with unit `0x1d` (engineer button). Each re-invocation rebuilds
a fresh, correctly refcounted event through the game's own code, so no
engine-internal allocation or Handle discipline is replicated in the DLL.

One cosmetic difference: the click sound plays inside the siege handler, so
a batch layers the click sound once per queued engine (the infantry hooks
sit below the sound call and play it once).

### Market shift-click reference

The executable calls `GetAsyncKeyState` (IAT slot `0x900408`) at exactly four
sites, all modifier checks — confirming the note in
[stop-troops-hotkey.md](stop-troops-hotkey.md):

| Site | Key | Purpose |
| --- | --- | --- |
| `0x5fc6bf` | Alt (`0x12`) | alternate action on a UI click |
| `0x5ff132` | Shift | camera scroll speed boost |
| `0x6314d1` | Shift | troop-command modifier flag (1 → 2) |
| `0x6af3a5` | Shift | goods/trade panel: suppress auto-close while Shift held |

There is **no client-side quantity multiplier** in the market path — the
engine's market shift behavior keeps the trade panel open so the player can
repeat the transaction, i.e. Shift means "repeat the standard command". The
batch-recruit hook follows the same idiom: it checks `VK_SHIFT` the same way
the engine does and repeats the standard, individually-validated recruit
command rather than inventing a new bulk command.

---

## Patch

**Sites:** the five 5-byte `call 0x4ef700` instructions listed above.
Original bytes:

| RVA | Original | Meaning |
| --- | --- | --- |
| `0x22083a` | `e8 c1 ee ec ff` | `call 0x4ef700` |
| `0x223ca9` | `e8 52 ba ec ff` | `call 0x4ef700` |
| `0x223dc9` | `e8 32 b9 ec ff` | `call 0x4ef700` |
| `0x225622` | `e8 d9 a0 ec ff` | `call 0x4ef700` |
| `0x228539` | `e8 c2 71 ec ff` | `call 0x4ef700` |

plus one data patch: the `SubPanelSiegeCamp` vtable slot at RVA `0x5ccba4`
(stock value: image base + `0x228c50`) is repointed at the siege wrapper.

Each call is retargeted (`E8` + new rel32) to a stdcall stub in the DLL with
the same `(ctx, unitTypeId)` / `ret 8` convention, so no register or stack
fixups and no naked-assembly trampoline are needed. Install verifies the
stock `E8` rel32 bytes at every site first; a site that does not match is
left untouched.

The stub:

1. Performs the original post once (vanilla behavior).
2. If `GetAsyncKeyState(VK_SHIFT)` does not report the key held → done.
3. Otherwise loops up to `multiplier − 1` more times: fetch the player
   object (`0x411360` on the world singleton `0x11781d0`; returns NULL after
   Lord death → stop), call `canRecruit` (`0x418640`), stop at the first
   failure, else post the identical command again.

### Why resource exhaustion cannot underflow

Two independent layers stop the batch:

- **Local (best-effort):** `canRecruit` between posts stops the loop as soon
  as validation fails. If command execution is deferred (multiplayer command
  queue), this check may keep passing — which is fine, because of the second
  layer.
- **Authoritative:** the simulation executor re-validates every single
  command with the same `canRecruit` before deducting anything, and drops
  commands that fail. This is the stock engine path that already protects
  against rapid manual clicking; the patch adds no new state transitions.

The advisor "not enough gold" voice line is only triggered from the pane
handler's own single validation, so a partially-filled batch does not spam
advisor speech.

---

## Multiplayer compatibility

**Safe for version mismatch.** The patch issues N standard recruit commands
through the stock command layer — indistinguishable from N fast manual
clicks. Unpatched clients process the received commands with the identical
executor and validation, so patched and unpatched clients stay in lockstep.
The batch is bounded (≤ 100 commands per click) and each command is a few
bytes, so command-queue pressure is negligible.

---

## Configuration

| Value | Behavior |
| --- | --- |
| *(missing key / no ini)* | default `20` |
| `0` or `1` | feature disabled — no sites are patched, zero footprint |
| `2` … `100` | units queued per shift-click |
| anything else (unparseable, negative, > 100) | default `20` |

---

## Verification log

- [x] Release build compiles clean (`make`, no warnings)
- [x] Stock bytes at all three call sites and the siege vtable slot confirmed
      against a clean Steam v1.5.0 `Stronghold2.exe` (byte-verified from the
      game directory copy)
- [x] Single-player: barracks, mercenary post and monastery batches confirmed
      working in-game (initial playtest)
- [x] Single-player: siege camp — engine and engineer buttons confirmed
      working in-game (playtest)
- [x] Single-player: engineers guild — engineer button confirmed working
      (playtest). The guild's **ladderman button does not batch** — it posts a
      `CreateSiegeEquipmentChore` inline (`0x623de3`, see the known-limitation
      note above), which the call-site hooks do not cover. But who buys ladder men in the Engineer's Guild anyway.
- [x] Single-player: batch stops at resource exhaustion as expected
      (playtest)
- [x] Config matrix behaves as documented (playtest)
- [x] Siege camp: camp-destroyed race while shift-clicking
- [x] Multiplayer: patched ↔ unpatched lobby, batch recruit, no desync;
      remote client sees the same units
