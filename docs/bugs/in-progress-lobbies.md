# In-Progress Lobbies Visible in Lobby Browser

**Status**: Fixed and verified in a live browser session (2026-08-27)
**Severity**: Minor UX — players see un-joinable lobbies in the list; clicking them silently fails
**Game version**: Steam build v1.5.0

---

## Symptom

The multiplayer lobby browser displays all active Steam lobbies, including games that have
already started.  Clicking an in-progress lobby does nothing at all — no error, no message,
the browser simply stays put.  On a typical evening the majority of listed lobbies may be
in-progress games.

---

## Root Cause

The feature is fully implemented in the stock game and broken by a **key-name mismatch**.
Three pieces of code are involved, and two of them agree:

| Role | Site (VA) | Lobby data key |
| --- | --- | --- |
| Host publishes the flag at game start | `0x6a2791` | **`Playing`** = `"1"` |
| Join handler refuses in-progress lobbies | `0x69d387` | **`Playing`** |
| Browser row builder computes the per-row flag | `0x7e065b` | **`Staging`** ← wrong |

Nothing in the entire executable ever writes `Staging`.  `SetLobbyData` has three users — two
direct (`HostName` at lobby creation, `Playing` at game start) and one that loads the import
into `edi` at `0x7df15b` and publishes the lobby's descriptive keys (`MapName`, `NumPlayers`,
`Country`, …).  The browser's query therefore returns nothing for every lobby, the flag is
always false, and no row is ever marked.

*(Enumerating IAT users by `ff 15 <slot>` alone misses the register-indirect callers — both of
the row builder's own `GetLobbyData` calls are `call ebx`.  Search for the slot address, not
the call encoding.)*

The two spellings live in different string blocks — `"Playing"` at `0x9e1898` sits with the
game's own UI strings, `"Staging"` at `0x9f8884` sits in the GameSpy block — which suggests
the row builder was written against a key name that was later changed on the publishing side.

The silent-click symptom is the same bug seen from the other end: the join handler reads the
*correct* key, matches it, and jumps to a bare `ret` at `0x69d46e` with no user feedback.
That the join is refused at all is proof that browsing clients do receive `Playing=1` from
remote hosts.

### Two row-add paths, only one of which checks

The browser fills its list from two ffonlinelib callbacks, and **both** reach the same row
add/update at `0x7e0340`:

| Path | Callback | In-progress check |
| --- | --- | --- |
| `0x7e03f0` | lobby **list** arrival | none — pushes a hard-coded `0` at `0x7e05bd` (`ebx`, the function's zero register) |
| `0x7e0620` | lobby **data** update | reads the key (this is the one with the wrong name) |

The list path adds a row immediately for any lobby whose data Steam has already cached; only
when `GetLobbyData(id, "HostName")` comes back empty does it call `RequestLobbyData`
(`0x7e04c9`, the executable's only call site) and let the row arrive later through the data
path.  So on a warm cache most rows are added by the path that never asks the question, which
is why correcting the key alone still left in-progress games listed.

### Where the flag ends up

The row builder at `0x7e0620` reads `HostName`, then the in-progress key, then stores the
result as a byte at **`entry+0x35`** in the browser's list model (`0x2acdae8`) via the row
add/update at `0x7e0340`.  That byte is **written once and never read anywhere in the
executable** — the field is dead, so correcting the key alone changes nothing visible.  The
patch therefore has to supply the consumer as well.

---

## Evidence (minidumps, 2026-08-27)

Three dumps taken in the browser, hosting a lobby, and after starting the hosted game.
Module base `0xdf0000` in all three.

**The host does publish the key.** The hosting-lobby and in-progress dumps are byte-identical
across the serialized Steam `SetLobbyData` request buffer at `0x68d0113` except for the
key/value pair itself:

```text
hosting lobby:  86 01 09  "Password\0"  02  "1\0"
in progress:    86 01 08  "Playing\0"   02  "1\0"
```

Both gates on the publishing site pass while hosting: `dword [0x2acd7f0]` reads `2` in the
hosting and in-progress dumps (`0` in the browser), and the predicate at `0x415e50` must have
returned true for the write to have happened at all.

**The browser's flag is always zero.** Walking the list model in the browser dump gives nine
lobbies (linked list at `model+0x8`; per node: `+0x8` lobby SteamID, `+0x18` host name
`std::string`, `+0x35` the flag):

```text
node 0x46366390  host='Sir - Egbert - Geodus'  +0x35=0
node 0x46366a50  host='JustBurky'              +0x35=0
node 0x46366350  host='Free to Join'           +0x35=0
...  (9 rows, every one 0)
```

Steam's own metadata cache for those lobbies is in-process around `0x6891400` and holds
`MaxPlayers` / `NumPlayers` / `Country` / map names for each — but no `Playing` key on any of
them, so this particular sample happened to be nine genuinely open lobbies.  The sample does
not exercise the fix; it only confirms the read path and the list layout.

---

## The Patch

`src/patches/lobbyInProgressFilter.cpp`, gated on `[multiplayer] HideInProgressLobbies`
(default `1`).  Two sites, verified all-or-nothing before either is written.

**1. Correct the key** — RVA `0x3e065b`, the `push imm32` supplying the row builder's lookup
key:

```text
before: push <base+0x5f8884>   ; "Staging"
after:  push <base+0x5e1898>   ; "Playing"
```

Both operands are **base-relocated absolute addresses**, so the site is verified by opcode
(`0x68`) plus a rebuilt operand, never by comparing against on-disk bytes.

**2. Supply the missing consumer on the data path** — trampoline at RVA `0x3e06bf`, the 5-byte
`mov ecx, <list model>` immediately before the row add:

```asm
cmp  byte ptr [ebp+0x10], 0   ; the flag the function just computed
jne  .skip
mov  ecx, <captured operand>  ; re-emit the overwritten instruction
jmp  <site+5>                 ; fall through to the row add
.skip:
add  esp, 0x18                ; the skipped callee is `ret 0x18`
jmp  <RVA 0x3e06c9>           ; pop edi/esi/ebx/ebp ; ret 0xc
```

The overwritten instruction's immediate is ASLR-relocated, so it is captured at install time
and re-emitted from a global rather than hard-coded.  The 0x18 bytes cleaned up on the skip
path are the flag, the 16-byte lobby id and the host-name pointer, already pushed for a
`ret 0x18` callee.  Only `ECX` and `EFLAGS` are touched; the replaced instruction sets `ECX`
itself and the next instruction is a `call`, so both are dead at that point.

Both branches inside the enclosing function (`0x7e0698`, `0x7e069c`) land before the hook
site, so nothing jumps into the middle of the patched bytes.

**3. Ask the question on the list path** — trampoline at RVA `0x3e05bd`, the 6 position-
independent bytes `push ebx ; sub esp,0x10 ; mov ecx,esp` that begin the row add's argument
setup.  There is no flag to correct here, so the hook performs the lookup itself: the lobby id
is the 16 bytes at `[ebp-0x5c]` and the lister is `[ebp-0x60]`.  When the game has started it
jumps to the loop's next iteration at `0x3e05e7`; nothing has been pushed at that point, so
that path needs no stack cleanup.

The shim must not be `static`: with every caller visible GCC rewrites a static function's
calling convention to a private register-based one, and the hand-written body — still reading
its arguments off the stack — then takes a stack slot as the callee address.  The first build
of this hook crashed on entering the browser with `EIP = 0` for exactly that reason.

`OnlineLobbyLister::GetLobbyData` (ffonlinelib RVA `0x88b0`) is thiscall taking the 16-byte
`OnlineID` by value plus a key pointer, and cleans its own arguments (`ret 0x14`; it reads the
id at `[ebp+8]` and the key at `[ebp+0x18]`).  The call is made through a naked shim that
reproduces the stock push order rather than declaring a C++ signature and trusting the
compiler to lay a 16-byte by-value struct out the way MSVC does.  The callback reached from
this mid-function hook does string comparison only, no floating-point work.

---

## Multiplayer Compatibility

**Safe for version mismatch.** Only the *browsing* player needs the patch — the host side is
stock behaviour, so an unpatched host still publishes `Playing=1` and its in-progress lobby
is still filtered out of a patched player's browser.  The patch reads lobby metadata and
drops a row from a local UI list; it posts nothing, touches no simulation state, and does not
change what the game does when a lobby *is* joined.

---

## Known Limits

- **The key is never cleared.** Nothing writes `Playing=0`, so a lobby that somehow outlives
  its game would stay hidden.  In practice the host's lobby does not survive a return to the
  menu, so this has not been observed.
- **Rows already listed are not retro-actively removed.** Both hooks drop a lobby as it is
  added; a lobby that starts its game while the browser is open keeps its row until the list is
  next rebuilt, because the data path can only skip the update, not erase the entry.
- The fix cannot help a player whose *own* client is unpatched, and it does nothing about the
  silent failure when such a player clicks an in-progress lobby.
- The fix cannot help a player whose *own* client is unpatched, and it does nothing about the
  silent failure when such a player clicks an in-progress lobby.

---

## Verification

Live A/B in one browser session, toggling only `HideInProgressLobbies`:

| Setting | Lobbies listed | Joinable |
| --- | --- | --- |
| `1` | 2 | 2 of 2 |
| `0` | ~12 | 2 of ~12 |

Every one of the ~10 extra rows was an in-progress game.  This also settles the question the
minidumps could not: **Steam does deliver the `Playing` key to clients that are not members of
the lobby**, so `GetLobbyData` on a browsed lobby returns it and the filter has real signal to
work with.

The dumps could never have shown this.  Lobby metadata blocks are recycled constantly —
`NumPlayers` and `MaxPlayers` are re-sent per lobby on every refresh (47 records for a single
lobby in one dump) and quickly overwrite anything set once — so the absence of a `Playing`
record from a dump was never evidence of its absence from Steam.  A two-minute A/B with the
ini switch answered what hours of heap archaeology could not.

---

## Superseded Approach

An earlier investigation looked only at the reading side, concluded no observable signal
existed, and settled on the heuristic `GetNumLobbyMembers(lobby) < atoi(GetLobbyData(lobby,
"NumPlayers"))` — players leaving the Steam lobby at game start.  Tested against seven
lobbies it caught 2 of 6 in-progress games and was abandoned as a "fundamental detection
limit".  The key-based fix catches all of them.

The limit was never in the API surface; it was in querying a key that nothing publishes.  The
lesson worth keeping: before concluding that a signal is absent, enumerate the *writers* of
the mechanism (here, all two `SetLobbyData` call sites) rather than sampling what a live
session appears to expose.  The heuristic and its `GetLobbyByIndex` IAT hook — including the
MSVC hidden-pointer return convention needed to hook it — are no longer used.

---

## References

- Steam documentation: `ISteamMatchmaking::SetLobbyData`, `GetLobbyData`
- ffonlinelib IAT: `SetLobbyData` slot `0x900a48`, `GetLobbyData` slot `0x9009ec`
- Related infrastructure: [`docs/architecture.md`](../architecture.md)
