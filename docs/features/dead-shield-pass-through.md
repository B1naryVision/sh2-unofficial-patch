# Dead Shields Let Shots Through — Match Option

**Status:** Built as a host [match option](match-rules.md) on 2026-11-01. Playtested
2026-11-01: behaves as expected. The two-client desync test and the replay test (tests 4 and
7) are not yet recorded.
**Config**: the host clicks *Dead shields let shots through* in the
[lobby panel](lobby-panel.md) (`[matchrules] DeadShieldPassThrough`). Rule bit `0x40`.
**Affects:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)

All addresses below are VAs at ImageBase `0x400000`; the code uses the RVAs.

---

## Symptom

A destroyed Mantlet or Cat keeps catching every arrow, bolt, axe, spear and stone sent
through it until its collapse animation has finished. Stacked shields therefore protect the
troops behind them for several seconds after the front shield is gone, which strongly favours
turtling behind a wall of Mantlets.

---

## Root cause

### The blocker list

`S2Game` (`0x11781d0`) keeps a list of the actors that catch projectiles: a
`std::vector` of 8-byte `{Actor *, handle}` entries at `+0xcd900` (begin) / `+0xcd904`
(end) / `+0xcd908` (capacity). It is saved with the game (`0x422452`) and cleared on a new
game (`0x42695a`).

| Function | VA | Notes |
| --- | --- | --- |
| `S2Game::findBlocker` | `0x421f10` | `ret 0x18`: out handle, impact point, radius, shooter's owner |
| `S2Game::addBlocker(Handle)` | `0x429e40` | push_back; called from `Player::addUnit` `0x4ee380` and its loader `0x4ed8e0` |
| `S2Game::removeBlocker(Handle)` | `0x425800` | `ret 8`, swap-remove; **one** caller, `Player::removeUnit` `0x4ee070` at `0x4ee142` |

`Player::removeUnit` removes a blocker only for unit types `0x5b` and `0x66`. Those types are
**Cat** (`0x5b`) and **Mantlet** (`0x66`), read from the unit-class list at `0xaa79ac`
(index + 1; the anchors Criminal `0x37` and WheatFarmer `0xf` reproduce). Cats get their own
catch radius in `findBlocker`.

### Every projectile asks the list first

`findBlocker` walks the list, resolves each handle (`0x769360` on `0xae8728`), skips a blocker
owned by the shooter or allied with it (diplomacy `<= 2`), and returns the first one whose
radius holds the impact point. **It never reads health or lifecycle state.** Every projectile
class calls it from its per-step sweep:

| Class | Sweep (vtable `+0x44`) | Call site |
| --- | --- | --- |
| Arrow | `0x4cf390` | `0x4cf5e0` |
| BallistaArrow | `0x4d09d0` | `0x4d0bb4` |
| RockMissile | `0x4d3830` | `0x4d3fb8` |
| Axe | `0x4d4d30` | `0x4d4e7b` |
| Spear | `0x4d5de0` | `0x4d5f2b` |
| StoneTipper | `0x4db900` (vtable `+0x24`) | `0x4dbb48` |
| ThrownRock | `0x4dc040` | `0x4dc10c` |

A non-empty result sends the projectile into the blocker's `takeDamage` (vtable `+0xa8`;
siege engines `0x575aa0` → `S2Actor::takeDamage` `0x4e8cc0`). The projectile is then
consumed whatever `takeDamage` returns. An empty result falls through to the ordinary per-tile
sweep, which already ignores corpses: at `0x4cf711` it rejects an actor with health `+0x4c`
`<= 0` unless its max health `+0x48` is 0.

### A dead shield stays in the list until its animation ends

1. The damage sink `0x786a40` drops health `+0x4c` to `<= 0`. It returns 1 both for the
   lethal blow and for any later hit on an actor it already counts as dead
   (`maxHP != 0 && HP <= 0`), so a corpse "accepts" every further hit.
2. `S2Actor::takeDamage` sets `+0x136 = 1` (death pending).
3. The death reaction (vtable `+0x28`, `0x4e6e60`) calls `S2Actor::die` `0x4e5790`, which
   sets `+0x139`, calls `clearActions` and pushes `ScriptAction(0xbcc)`, the death script.
   Mantlet and Cat hand that script range to the `SiegeEngine` handler `0x5755e0`.
4. When the script finishes, the actor goes to state 3.
5. `Actor::update` calls destroy (vtable `+0x1c`: `0x5129a0` → `0x521600` → `0x4e62f0`). That
   calls `Player::removeUnit`, which calls `removeBlocker`.

So for the whole death animation `findBlocker` keeps returning the dead shield, and every shot
through it is spent on a corpse that cannot die again.

---

## The fix

The fix adds a read-only filter in `findBlocker`: a blocker the damage sink already counts as
dead is skipped exactly as a friendly one is. The loop then goes on to the next live blocker,
so the second Mantlet in a stack catches the shot. If there is none, it returns nothing and the
ordinary sweep hits the troops behind.

**Site:** VA `0x421fdb` (RVA `0x21fdb`), right after the entry's actor has resolved and been
checked non-null (`esi` = actor, `ebx` = loop index, also at `[ebp-0x10]`).

| | Bytes | Instructions |
| --- | --- | --- |
| Before | `8b 5e 58 8b 45 1c` | `mov ebx,[esi+0x58]` / `mov eax,[ebp+0x1c]` |
| After | `e9 <rel32> 90` | `jmp cave` / `nop` |

The cave is 31 bytes, allocated with `VirtualAlloc` at install. Its test is integer-only:

```asm
test dword [esi+0x48], 0x7fffffff ; f7 46 48 ff ff ff 7f   max health ±0: indestructible, stock path
jz   alive                        ; 74 0b
cmp  dword [esi+0x4c], 0          ; 83 7e 4c 00            health bits as int32 > 0 <=> health > 0.0
jg   alive                        ; 7f 05
jmp  0x422144                     ; e9 <rel32>             dead: next entry
alive:
mov  ebx, [esi+0x58]              ; 8b 5e 58               the stock bytes
mov  eax, [ebp+0x1c]              ; 8b 45 1c
jmp  0x421fe1                     ; e9 <rel32>             the friendly test
```

- `0x422144` is where the stock friendly skip at `0x421fe3` jumps. It reloads the index and
  count from `[ebp-0x10]`/`[ebp-0xc]`, increments and loops.
- The cave's flag results are dead: `cmp eax,ebx` at `0x421fe1` rewrites them.
- The x87 stack is empty at the site, because the prologue's loads are all popped by
  `0x421f7d`. The cave uses no float instructions anyway. It reads the health as raw bits:
  `+0.0`, `-0.0` and every negative value fail `jg`, matching the sink's `!(0 < HP)`.
- No branch in the function targets `0x421fdc`–`0x421fe0`. The 6 stock bytes carry no
  relocated immediate, so install `memcmp`s them directly.
- The stock bytes are written back when the rule switches off.

Nothing is written to game state. The list, its saved form and the stock removal at state 3 are
untouched. A dead shield simply stops being handed to `takeDamage` by this path.

---

## Multiplayer Compatibility

**Requires all clients, self-enforced.** Which actor a projectile hits is simulation state: a
client with the filter would send a shot past a dead Mantlet that an unpatched client spends
on it, and the two would desync on the next hit. The rule is a host option, so it is only on
when:

- the host switches it on,
- every lobby member advertises bit `0x40`, and
- the rules chat line commits it on the same lockstep tick everywhere
  ([match-rules.md](match-rules.md)).

It is off in single player, like every option, and replays record and restore it with the
other rule bits.

---

## Investigation notes

- **The per-tile sweep was never the problem.** Each projectile's own sweep already skips an
  actor at `HP <= 0`. Only the blocker lookup ahead of it lacks the test, which is why the
  bug affects shields specifically.
- **Removing the shield from the list at death was rejected.** The list is saved, so removing
  early would change save and replay contents and the swap-remove order. `Player::removeUnit`
  and the owner-change virtual (vtable `+0x21c`, `0x5760d0`, which removes and re-adds)
  would then meet an entry that is already gone. A read-side filter changes only which actor
  a projectile hits, and reverts by restoring 6 bytes.
- **`S2Actor::isAlive` is true for the whole death animation.** It is
  `[+0x98] == 0 && [+0x20] != 3` (`0x415ed0`, two callers), and the state does not reach 3
  until the death script ends. The engine's own "dead" test is health, so that is what the
  filter uses.
- **Ranged troops do not keep aiming at a dying shield** (playtest, 2026-11-01). They move to
  a new target almost at once, so no follow-up is needed. Their target validation must test
  something other than `isAlive`, which stays true until the animation ends; it has not been
  traced.

---

## Testing

The host can run tests 1–3 alone: a Steam lobby with only the host, with
[MP AI](mp-ai-enable.md) players as opponents.

1. **Rule on, Mantlets.** Stack three Mantlets in a line with spearmen behind them, and have
   20 or more archers or crossbowmen shoot them. When the front Mantlet's health reaches
   zero, the next volley strikes the second Mantlet at once, without waiting for the
   collapse. With all three gone, arrows reach the spearmen.
2. **Rule on, Cat and catapult.** Repeat test 1 with a Cat, and with catapult stones.
3. **Rule off.** Run test 1 again. The collapsing Mantlet absorbs volleys until its animation
   ends (stock behaviour).
4. **Two patched clients, rule on.** A long fight through stacked shields, once as host and
   once joining: no "Out of sync".
5. **Unpatched player in the lobby.** The rule shows as unavailable and no rules line
   includes it.
6. **Revert.** Play a match with the rule on, return to the lobby, switch it off, and play
   again: the second match behaves as in test 3. In a dump, the byte at base+`0x21fdb` is
   `0xe9` while the rule is on and `0x8b` when it is off.
7. **Replay.** Record a match with the rule on. Play it back at 1x and at 5x: the outcome
   matches.
