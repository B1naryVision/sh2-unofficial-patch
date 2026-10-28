# Patch Handshake — Who Has Which Patch Version

**Status**: Implemented; playtested 2026-10-28 with patched and unpatched players. Advertises **no match rules** yet
(`RULES_SUPPORTED = 0`); it exists so that rules can be added safely later.
**Config**: none — always active.

---

## What it does

Every patched client announces its patch version to the Steam lobby it is in, and reads
everyone else's. Typing `!ver` in multiplayer chat broadcasts the result:

```text
Patch: Alice 0.8.0 (host), Bob 0.7 or older, Carol 0.8.0
```

It is the foundation for host-chosen **match rules** (non-vanilla gameplay such as
[ballista auto-fire](ballista-auto-fire.md)) that may only be switched on when every player
supports them. See [Match rules: the plan](#match-rules-the-plan) below.

---

## Transport: Steam lobby member data, not P2P messages

The first design used request/reply maintenance messages (`FF 42` / `FF 43`, like
[`!ping`](ping-command.md)). Steam lobby **member data** is better for this in every way:

- Each member may write key/value pairs on itself; Steam replicates them to every member,
  **including players who join later**. No request, no reply, no timeout, no retry.
- It exists precisely where the negotiation has to happen — in the lobby, before the
  simulation. The P2P peer table's lobby behaviour is unverified; `!ping` has only been
  exercised in game.
- Nothing to parse from a raw packet. The value is still written by another player, so it is
  still untrusted, but Steam bounds and delivers it.
- An unpatched client ignores a key it never reads, by construction.

Each patched client writes key `sh2up` = `<protocol>;<version>;<rules hex>`, e.g.
`1;0.8.0;0`. A member with no key is **unpatched or on 0.7.0 or older** (0.7.0 predates the
handshake), and every rule is unavailable. A key that does not parse exactly (`unknown`) is
treated the same.

`handshakeCommonRules()` is the AND of every member's rule bits, and is `0` the moment any
member is missing, unreadable or on a different protocol — **every failure mode narrows to
nothing**.

---

## The game's lobby object

The exe keeps a single `Online::OnlineLobby` (implemented in `ffonlinelib.dll`) as a static
global at **VA `0x2acd918`** — `GameSpy::Network` (`0x2acd7d8`) `+0x140`. Found by its
`ecx` at the exe's eight `GetLobbyOnlineID` call sites (IAT `0x900a50`); the lobby screen's
`SetLobbyData("Playing", "1")` at `0x6a2791` uses it the same way.

| Field | Offset | Evidence |
| --- | --- | --- |
| Lobby SteamID (`OnlineID`) | `+0x10` (8) | `GetLobbyOnlineID` = `lea eax, [ecx+0x10]` |
| `OnlineID` valid flag | `+0x18` | `GetLobbyOwnerOnlineID` checks `[esi+0x18] == 1` |
| Online type (`1` = Steam) | `+0x20` | every Steam-path wrapper checks `[ecx+0x20] == 1` |
| Local player owns lobby | `+0x28` (byte) | `IsLocalPlayerOwner` = `mov al, [ecx+0x28]` |

`Online::OnlineID` passed **by value** is 16 bytes (`GetFriendPersonaName` ends `ret 0x10`).

### ISteamMatchmaking calls

The patch calls `steam_api.dll`'s `SteamMatchmaking()` itself. Slots are SDK interface
**v009**, each one cross-checked against the ffonlinelib wrapper that uses it:

| Slot | Method | ffonlinelib wrapper (RVA) |
| --- | --- | --- |
| `+0x44` | `GetNumLobbyMembers` | `OnlineLobby::GetOnlineNumMembers` `0x8260` |
| `+0x48` | `GetLobbyMemberByIndex` | (v009 order; neighbours verified) |
| `+0x4c` | `GetLobbyData` | `OnlineLobby::GetLobbyData` `0x8430` |
| `+0x50` | `SetLobbyData` | `OnlineLobby::SetLobbyData` `0x8470` |
| `+0x60` | `GetLobbyMemberData` | `OnlineLobby::GetLobbyMemberData` `0x7830` |
| `+0x64` | `SetLobbyMemberData` | `OnlineLobby::SetLobbyMemberData` `0x7880` |
| `+0x8c` | `GetLobbyOwner` | `OnlineLobby::GetLobbyOwnerOnlineID` `0x7740` |

`CSteamID` has constructors, so MSVC returns it through a **hidden pointer passed as the
first stack argument** (`GetLobbyOwnerOnlineID` pushes `lea ecx, [esp+0x18]` last). Our own
SteamID and display names come from ffonlinelib's exported
`OnlineInterface::GetLocalOnlineID64` and `GetFriendPersonaName`, resolved by name.

### Who is the host

Two independent signals, both static-analysis only so far:

- **Steam's lobby owner** — `OnlineLobby+0x28` locally, `GetLobbyOwner` for everyone else.
  This is what the patch uses: it is what Steam itself enforces (`SetLobbyData` only takes
  effect for the owner), and every member agrees on it.
- **The engine's own host predicate** `0x415e50` (thiscall on `GameSpy::Network`):
  `net[+0x18] != 1 && net[+0x50] == net[+0xdc]`, i.e. "the host's peer id is mine". The lobby
  screen gates its `SetLobbyData("Playing", "1")` on it. Not the same thing as the byte at
  `0xdb8483`, which pattern-matched "host" and read `0` for a whole MP match.

---

## Threading and float safety

Everything runs on the frame tick (game thread), refreshed once a second and once more right
before `!ver` reports. The frame tick is a mid-function hook and steam_api is free to use the
FPU, so each refresh runs inside `cleanFpu()` (`src/core/cleanFpu.h`: aligned `fxsave` /
`fninit` / `fxrstor`).

---

## Runtime evidence (solo host, 2026-10-28)

Three full minidumps of one session, taken in sequence (main menu, own lobby as host, match in
progress against AI). The base was `0x830000` in all three, and all three show `e9` at the
frame-tick, maintenance and chat-send sites, so every hook was installed. `!ver` in the match
answered `0.8.0 (host)` and `!ping` answered `no other players`, as expected.

| Field | Main menu | Lobby (host) | In match |
| --- | --- | --- | --- |
| `OnlineLobby+0x10` lobby SteamID | `0` | `0x0186000068f7c2be` | same |
| `+0x18` OnlineID valid | `1` | `1` | `1` |
| `+0x20` online type | `1` | `1` | `1` |
| `+0x28` local owner | `0` | `1` | `1` |
| `net+0x18` / `+0x50` / `+0xdc` | `0` / `-1` / `-1` | `2` / `1` / `1` | same |

What this establishes:

- **`+0x18` and `+0x20` do not mean "in a lobby"**: both read `1` in the main menu with no
  lobby. The lobby SteamID being non-zero is the real test. `lobbyActive()` already requires
  it; it is a valid Steam *chat*-type ID (account-type nibble `8`, universe `1`).
- **The Steam lobby survives into the match**, owner flag included, so `!ver` works in game and
  the lobby data is readable for the whole session.
- **The engine predicate `0x415e50` is meaningless outside a session.** In the menu its two
  peer ids are both `-1`, so `+0x50 == +0xdc` holds and only the `+0x18 != 1` test keeps the
  answer meaningful. In the lobby it agrees with Steam's flag. One more reason to trust
  Steam's owner.
- **The published value reached Steam's lobby store**: `1;0.8.0;0` sits in steam client
  memory next to the lobby's own `MaxPlayers` / `NumPlayers` keys. It also appears in the menu
  dump, left over from the lobby tested just before it.

## Playtest (multiple players, 2026-10-28)

`!ver` in matches with both patched and unpatched players worked as expected: member data
replicates to the other players, the joining side names the right host, and unpatched players
read as `0.7 or older` with no desync.

---

## Multiplayer Compatibility

**Safe for version mismatch.** Lobby member data is a Steam-side key/value store that the
stock game never reads, `!ver` is an ordinary chat message, and no simulation state is
touched. The ruleset it computes is `0` (vanilla) whenever any player lacks the patch.

---

## Match rules: the plan

Agreed design (2026-10-25); none of this is built yet.

1. **Host picks rules in a lobby overlay panel** (chat does not exist in the lobby — it is a
   lockstep command). One row per rule, one column per player showing their version. A rule
   is selectable only if `handshakeCommonRules()` has its bit. The host publishes the
   selection as lobby data (`sh2up_rules`) so every player's panel shows it read-only.
2. **The commit goes through the lockstep layer.** Lobby data cannot commit the rules: when
   it arrives is the Two Generals problem again. Instead, at match start the host posts the
   ruleset as an in-game chat message, e.g. `Match rules: ballista auto-fire [r:0001]`. In-game
   chat is a `ChatChore` (vtable `0x9c712c`), and lockstep runs every command on the same tick
   on every client, so every patched client sees the message on the same tick and switches
   the rules on there. Unpatched clients just see a readable line. Needs: `ChatChore`'s
   execute virtual (RTTI walk).
3. **Acceptance rule, the same on every client:** only the first rules message, only from the
   lobby owner's player slot, only within the first few seconds of the match; anything later
   is ignored.
4. **Every rule patch becomes reversible** (`enable` / `disable`, byte-verified both ways), is
   switched off again when the session ends, and is advertised in `RULES_SUPPORTED` only once
   that works. Ballista auto-fire is first.
