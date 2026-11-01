# Match Rules — Host-Chosen Gameplay Changes

**Status**: Implemented and playtested 2026-10-29: tests 1–3 below passed, test 3 with four
patched players and with the tester both as host and as a joining player. Test 4 has been
checked as far as the lobby (the panel marks the rule unavailable with an unpatched player
present); the rest of it runs the same "nothing to post" path test 1 exercised.
**Config**: the host clicks a rule in the [lobby panel](lobby-panel.md); the choice is
remembered in `[matchrules]` in `sh2-unofficial-patch.ini` for the host's next lobby. Each
rule defaults to `0` (vanilla). Deliberately **not** in the settings panel: these are
settings of a match, not of a player.

| Rule | Kind | Ini key | Bit | Feature |
| --- | --- | --- | --- | --- |
| Disband peasant fix | fix | — | `0x2` | [disband-peasant-duplication.md](../bugs/disband-peasant-duplication.md) |
| Stuck-worker crime fix | fix | — | `0x8` | [stuck-worker-crime.md](../bugs/stuck-worker-crime.md) |
| Ranged move-order fix | fix | — | `0x10` | [ranged-move-orders.md](../bugs/ranged-move-orders.md) |
| Ballista auto-fire | option | `BallistaAutoFire` | `0x1` | [ballista-auto-fire.md](ballista-auto-fire.md) |
| No market | option | `NoMarket` | `0x4` | [no-market.md](no-market.md) |
| Unit cap 550 | option | `UnitCap550` | `0x20` | [unit-cap-raise.md](unit-cap-raise.md) |

The table is in display order. The lobby panel lists rules in the order of the `RULES`
table in `matchRules.cpp`: every fix first, then the host's options, so the two kinds read
as separate groups. Its click index is a position in that table, so new rules go in their
group there; the bits are independent of the order.

## Options and fixes

- **An option** is a gameplay change the host chooses. It is off unless the host clicks it
  on, and the choice is remembered in `[matchrules]`.
- **A fix** is a bug fix that changes the simulation, so it needs everyone to have it like an
  option does. It is never a choice: the host's proposal always includes every fix that
  everyone in the lobby supports, and the lobby panel shows it as `on (fix)` with no switch.
  With anyone lacking it, it is `unavailable` and the match keeps the stock behaviour, which
  is the same on every client and so stays in sync.
- **In single player** every fix is simply on. The test is the byte `S2Game+0x40089`
  (RVA `0xdb8259`). `S2Game::post` executes commands at once when it is set instead of
  queueing them for lockstep, so there is no other client to stay in step with. It reads `0`
  in a multiplayer match (dump). Fixes switch on when `GameScreen` attaches with that byte set
  and no Steam lobby, and off again in the menus. If the byte turned out not to be set in
  single player, the fix would just stay off there, which is safe.

A fix still commits through the rules line in multiplayer. It changes the simulation, so
switching it on is exactly as much a lockstep decision as an option. The host's game includes
it in the line without being asked. That line is now posted whenever the proposal is
non-zero, so with any fix available almost every patched match carries one.

---

## What it does

The host turns rules on before the match by clicking them in the lobby panel. Every player's
lobby panel shows what is on, and marks a rule `unavailable` when someone in the lobby cannot
run it; an unavailable rule cannot be clicked on. When the match
starts, the host's game posts one chat line that every player sees:

```text
Match rules: Ballista auto-fire [sh2up:1:1]
```

Every patched client switches those rules on as that line executes, and switches them off
again once the match is over for that client. A rule that is not in the line stays vanilla.
With nothing on, or with someone in the lobby who cannot run any of the chosen rules, no
line is posted and the match is vanilla.

---

## Why a chat line: committing through lockstep

Agreeing on rules over a side channel is the Two Generals problem: whichever side commits
first can be left alone, and a client running a rule its opponent does not is a desync. The
game already solves agreement for every player command, so the commit rides on that.

**In-game chat is a lockstep command.** Traced statically:

1. `IngameChatPanel::sendTypedMessage` (`0x64d8e0`) builds a `ChatChore` (vtable `0x9c712c`),
   stores the sender's **network peer id** (`[0x2acd8b4]` = `GameSpy::Network+0xdc`) at
   `chore+8` and the text (`std::wstring`) at `chore+0xc`, then posts it through
   `[S2Game+0x20]` (`S2Game` at `0x11781d0`). This is the same slot and object the recruit
   helper `0x4ef700` posts recruit commands through.
2. `S2Game::post` (`0x77efb0`) executes the chore immediately only when
   `byte [S2Game+0x40089]` is set (single player). Otherwise it calls `0x767050` on the
   sync-check object `0x1be6a30`, whose `+9` byte is the desync flag.
3. `0x767050` does **not** execute the chore. It looks up the chore's class in its
   registered list, drops it unless `[0x1be6a30+0x48] == 2`, pushes it onto a pending queue
   (`+0x10`, `0x766c00`), and, when the network is in session (`[0x2acd7f0] == 2`, i.e.
   `net+0x18`), serializes it and broadcasts it. The sender queues its own chore exactly as a
   receiver queues one that arrives.
4. Execution comes later, from that queue, through vtable slot 2:
   `ChatChore::execute` (`0x5e89f0`) passes `(chore+8, &chore+0xc)` to `0x699230` on the chat
   log `0xac43a8`, then tail-calls `0x69e250`. It is thiscall with no stack arguments.

So `execute` sees the same line, from the same sender, at the same point of the same tick on
every client. The patch swaps that vtable slot (VA `0x9c7134`, its only reference) and applies
the rules there, after the stock code has shown the line.

### Acceptance, the same on every client

A line switches rules on only if all of these hold. Every input is part of the lockstep
command or follows from command order, so every client reaches the same answer:

- it ends with exactly `[sh2up:<protocol>:<1-8 hex digits>]`;
- the protocol is this build's `HANDSHAKE_PROTOCOL`;
- its sender peer id equals the **host's peer id** `net+0x50`;
- no rules line has executed yet in this match;
- every bit is one this build supports. Otherwise nothing is switched on: running some of the
  rules would still leave this client out of step with whoever runs the rest.

The plan once said "only within the first few seconds". That was dropped: wall-clock time
differs between clients and would make acceptance non-deterministic. "The first rules line
from the host" is deterministic and serves the same purpose.

**Assumption confirmed by playtest, not by dump:** every client's `net+0x50` holds the host's
peer id. The dumps are all from the host's side, where `net+0x50 == net+0xdc == 1`. A client
that disagreed would reject a line the host accepted and desync the match, and a four-player
match with every client patched, played from both the host's and a joining player's seat, ran
with the rule applied and no desync. If a future change ever touches how the host is
identified, take a client-side dump to check it directly.

### Posting

The host's client posts the line from the frame tick, 2 seconds after `GameScreen` first
attaches. The rules it posts are the host's choice narrowed by `handshakeCommonRules()`, the
same value the lobby panel showed. Wall-clock timing is fine on the sending side: only the
execution has to be deterministic. Posting needs no typed line first. The chat panel is an
inline member of `GameScreen` (`[0x12c0de0] + 0xf5e68`, constructed at `0x5ec621`), and
`GameScreen` is registered at startup like the lobby screen.

### Switching back to vanilla

Rules switch off on the transition *into* the lobby screen, or when the Steam lobby is gone.
These are edges, never levels, and never on `GameScreen` detaching:

- a defeat screen or an in-game menu on one client must not turn a rule off while the other
  players are still simulating with it;
- a slow client still showing the lobby screen when its simulation runs the rules line must
  not have it switched straight back off by the next frame's level check.

Every match is entered from the lobby, so the next match always starts vanilla.

### What does *not* work as a match signal

`[0x1be6a30+0x48]` reads `2` in the main menu, the lobby and the match alike (three dumps),
so it is the queue's "accepting commands" state, not "a match is running". `GameScreen`'s
`+0x80` parent is the signal: `0` in the menu and lobby, set in the match.

### Replays

A [replay](multiplayer-replays.md) plays offline, with no Steam lobby and no network host, so
the logic above would read it as single player: options off, fixes on, and the replayed rules
line rejected for not coming from `net+0x50`. The replay sidecar therefore records the rule
state on the checkpoint frame (`matchRulesCapture`: the active bits, whether a line had
already committed, and the host's peer id). Loading a replay applies that state
(`matchRulesReplayBegin`) and suspends the frame tick until the playback ends
(`matchRulesReplayEnd`, which goes back to vanilla). A rules line after the checkpoint is a
recorded command like any other, so it is replayed on its live job and accepted against the
recorded host peer id.

---

## The lobby side

- The host publishes its narrowed choice as Steam lobby data `sh2up_rules` (hex), once a
  second when it changes. Other players read it for their lobby panel, and narrow it again
  by their own view of the lobby. This is display only: nothing is committed from lobby
  data.
- Each build advertises the rules whose sites verified on that machine, through the
  handshake's member data (`handshakeSetSupportedRules`). A rule whose install failed is
  never advertised, so it shows as `unavailable` to everyone.

---

## Multiplayer Compatibility

**Requires all clients**, and enforces that itself: a rule can only be offered when every
player in the lobby advertises it, and with any player lacking it the match is played vanilla.
A player without the patch sees the rules line as ordinary chat. They can never be in a match
where a rule is on, because their missing member data makes every rule unavailable.

---

## Tests before release

1. **Solo host, rule off:** start a match against AI; no rules line appears.
2. **Solo host, ballista on:** the line appears 2 s into the match. Deploy a field ballista
   near an enemy: it fires by itself. Back to the lobby, turn it off, start again: no line,
   and the ballista does not fire by itself.
3. **Two patched players, ballista on:** both see the line, both see the ballista fire, and
   the match runs to the end **without a desync**. Take one dump on the *client* while in
   the match: `net+0x50` must equal the host's peer id (the host's `net+0xdc`).
4. **Unpatched player present, ballista on:** the lobby panel shows `unavailable`, no line
   is posted, no desync.
5. **A second match in the same session** with the rule off: no auto-fire, which confirms the
   revert on returning to the lobby.
