# Ranged Units Ignore Move Orders in Combat

**Status:** Fixed in v0.8.0, as an automatic match fix (`src/patches/rangedWalkGuard.cpp`).
Root cause confirmed by a runtime trace; the fix playtested 2026-11-01: works as expected.
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Three `call` operands retargeted (recipe A), switched per match by
`patches/matchRules.cpp`.

---

## Symptom

Archers, crossbowmen and axe throwers in a fight often ignore a move order and
keep shooting. Some of a selected group walk off and the rest stay put, and it
takes repeated clicks to get them all moving. It happens when ordering them
towards or away from the enemy, as long as enemies stay in range, and it gets
worse the more units are ordered at once. Melee troops are not affected.

---

## Root cause

Every move order reaches every unit and is accepted. What follows is the
problem.

Archers, crossbowmen and axe throwers pick a new target every 40 ticks (each
unit on its own phase) in their `think()`, unless they are moving. The test is
`isMoving` (VA `0x485360`), which looks only at the unit's speed. A unit that
has accepted a move but is standing still counts as idle. That happens while it
waits for its path, queues behind others in a column, or turns to start. If an
enemy is in range on its 40-tick roll, the auto-pick calls `startAttack`
(`0x512a70`), whose first act is `clearActions` (`0x789f70`). The walk is
erased, the unit shoots, and only another click brings the order back.

Melee units are not affected because their auto-engagement pushes the attack
*on top of* the current order with `pushInterrupt` (`0x78a0c0`), so the walk
resumes when the fight ends. The ranged path replaces the order instead.

This explains every observation:

- **Retreating while still in range fails:** an enemy is in range for the roll.
- **More units make it worse:** more queueing and more standing time, so more
  rolls land in it.
- **Only some units stop:** each unit's 40-tick phase is different.
- **Crossbowmen seem better than archers:** their shorter range finds fewer
  enemies.

### How a move order reaches a unit (VAs)

`MoveTroopsChore::execute` (`0x5e6f10`) → `Clan::moveTo` (`0x77ba60`) →
formation pass (`0x77acb0`) → `actor->moveTo(x, y, layer)`, vtable `+0x98`,
once per unit per order. Crossbowman and AxeThrower use `Soldier::moveTo`
(`0x4f7270`). Archer (`0x513400`) overrides it only to look for a tower to
garrison, then calls the Soldier one. An accepted order clears the unit's action
stack and pushes `ScriptAction 0` plus a `GlowWorm::GotoLocation` (vtable
`0x9f4738`), which stays on top for as long as the unit is carrying out the walk.
See "Unit Behaviour" in `CLAUDE.md` for the action stack.

---

## The fix

All three classes guard the auto-pick with the same five instructions:

```text
mov  ecx, esi
call 0x485360        ; isMoving
test al, al
jne  <skip the auto-pick>
```

| Class | `think` | Call site (VA) | RVA |
| --- | --- | --- | --- |
| Archer | `0x513cf0` | `0x513eb0` | `0x113eb0` |
| Crossbowman | `0x4fe430` | `0x4fe4e7` | `0x0fe4e7` |
| AxeThrower | `0x50a2b0` | `0x50a367` | `0x10a367` |

Each site's bytes are `8b ce e8 <rel32> 84 c0 0f 85`. Install checks that
`mov ecx,esi` precedes the call, that `test al,al / jne` follows it, and that the
call resolves to `isMoving` (`RVA 0x85360`), then saves the stock `rel32`.
Enabling rewrites the three `rel32` operands to call `movingOrWalking`, which
returns `isMoving(this) || top action is a GotoLocation`. Disabling writes the
saved operands back. `isMoving` has 38 callers; only these three change.

The predicate reads only simulation state the actor holds, so every client
decides alike. It clobbers `ecx`/`edx`, which the stock function does not; both
paths after each call overwrite both before reading them (checked at all three
sites).

What is not affected:

- **Units that have arrived.** The `GotoLocation` pops when the walk ends, and
  auto-targeting resumes as before.
- **The firing state's own repositioning walk.** It runs with mask `0x81`,
  which lacks bit `0x02`, so `think` never reaches the auto-pick then.
- **Attack orders and attack-move.** Player attack orders set the target
  directly (`TroopClan::attackTarget`, `0x598330`) and go through a different
  branch of `think`. Stock ranged units already skip the auto-pick while walking
  at speed, so the fix changes only the moments a walking unit stands still.

To check in play: attack-move (Mouse4 toggle), and a unit whose path is blocked
for good. Stock code lets such a unit start shooting; with the fix it waits for
a new order while its `GotoLocation` lasts.

---

## Multiplayer Compatibility

**Requires all clients, self-enforced: an automatic match fix.** It changes when
a unit starts shooting, which is simulation state. A patched and an unpatched
client would diverge. It is rule bit `0x10` (`RULE_RANGED_WALK_FIX`). It is
offered only when every lobby member advertises it, committed by the host's
rules line on the same lockstep tick everywhere, and always on in single player.
Replays carry it with the other rules.

---

## Investigation notes

Found with a debug-only runtime trace (`src/patches/orderTrace.cpp`, built only
by `make debug`), over three playtests. Kept for the next unit-behaviour bug:

- **Refusal at the order gate is real but not this bug.** `Soldier::moveTo`
  drops an order silently when the unit's action mask lacks bit `0x80` (the `je`
  at `0x4f72c9`). In the traces this hit only units on their spawn tick (base
  state before its first step) and the `S2Actor` lockout state `0xbbd`
  (mask `0`), which melee units share. Archer `startAttack` pushes its firing
  state with flags `0x1` for one tick (`0x512b4c`); harmless in practice.
- **The first two traces misled.** They were dominated by the tester's own `H`
  presses (`StopAllTroopsChore::execute` `0x5e8520` → `0x598b60`, whole group on
  one tick) and attack clicks (`AttackActorChore::execute` `0x5e347f` →
  `0x598330`, reaching only members that pass `canEngage`, vtable `+0x180`).
  Both briefly looked like the cause. A third trace of one clean retreat order,
  with no clicks on enemies and no Stop, showed the auto-pick (`via 113ef2`)
  clearing walks one tick after acceptance and hundreds of ticks into them. Over
  that run, 30 such clears hit archers still holding a walk, all on unit ticks
  that are multiples of 40.
- **Ruled out:** a stale stored target. `moveTo` clears `+0x194/+0x198` through
  `0x4f5b00` → slot `+0x1ac` → `0x4e5850`. Also ruled out: the AI's attack
  posting (`S2Governor` → `0x5ae6f0` → `AIAttackActorChore`), which commands
  AI troops only. The right-click pick (`0x4f19f0`) is a ray test against each
  enemy's hit volume with 50 units of padding against a 1024 cell, so a click
  has to land on the enemy to become an attack.
- **Correction to an older note:** the `3` / `0x63` value chosen at `0x631363`
  from the troop panel's stance flags is a *cursor id* (stored by `0x62aba0`),
  not a field of the move order.

### Reading the trace

`make debug` writes `order_trace.txt` next to `Stronghold2.exe`: every move
order a tracked troop receives, then every clear, push and target change on that
unit for 400 ticks, plus the player's clicks and Stop-button presses.

```text
<tick> MOVE <class> <actor> gate=open|SHUT bypass=<0|1> <unit>
      before [<actions, bottom to top>] mask=<hex>
      after  [...] mask=<hex> -> ACCEPTED|UNCHANGED
<tick> CLEAR <class> <actor> by <caller RVA> via <caller's caller RVA> <unit> was [...]
<tick> PUSH|PUSHINT <class> <actor> <action> flags=<hex> by <caller RVA> onto [...]
<tick> SETTARGET|ENGAGE <class> <actor> to <handle> by <caller RVA> <unit> on [...]
<tick> CLICK mouse=<x>,<y> enemy <class> <handle> dist=<raw> | no enemy
<tick> ATTACKORDER target <class> <handle>
<tick> STOPBUTTON
```

`<unit>` is `t<unit tick> path=<0|1> ordered=<0|1> tgt=<stored target>`, plus
`shot=<target being shot>` for archers. Actions are `S<id>.<step>/<flags>`
(ScriptAction), `Int` (InterruptAction), `Goto` (GotoLocation) and `Evade`.
Caller RVAs map to VAs by adding `0x400000`. `CLICK` runs every frame for the
cursor icon, not only on clicks. `UNCHANGED` is not proof of a refusal: a unit
already walking rebuilds an identical stack. An accepted order always shows a
`CLEAR ... by f73c3` between `before` and `after`. A Stop without a
`STOPBUTTON` line was the hotkey.
