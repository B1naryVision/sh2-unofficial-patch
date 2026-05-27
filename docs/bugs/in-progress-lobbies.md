# In-Progress Lobbies Visible in Lobby Browser

**Status**: Investigation abandoned — fundamental detection limit reached  
**Severity**: Minor UX — players see un-joinable lobbies in the list; clicking them silently fails  
**Game version**: Steam build v1.5.0

---

## Symptom

The multiplayer lobby browser displays all active Steam lobbies, including games that have already started.  Players cannot join in-progress games, but no indication is given in the UI that a lobby is un-joinable.  On a typical evening the majority of listed lobbies may be in-progress games.

---

## Root Cause (Game Side)

When a Stronghold 2 game starts, the host **never marks the lobby as in-progress**.  The Steam API provides two mechanisms for this:

- `ISteamMatchmaking::SetLobbyData(lobby, key, value)` — sets an arbitrary metadata key visible to all clients browsing lobbies
- `ISteamMatchmaking::SetLobbyJoinable(lobby, false)` — prevents new members from joining at the Steam level

SH2 calls neither.  The lobby remains fully open and indistinguishable from a waiting lobby to any external observer.

---

## Investigation

### How the game browses lobbies

`ffonlinelib.dll` (Firefly's online abstraction layer) wraps `steam_api.dll`.  `Stronghold2.exe` calls into `ffonlinelib.dll` by direct import — specifically `OnlineLobbyLister::GetLobbyByIndex` (non-virtual, mangled name `?GetLobbyByIndex@OnlineLobbyLister@Online@@QAE?AVOnlineID@2@I@Z`).  The game iterates indices 0, 1, 2 … until it receives an `OnlineID` with `isValid == 0` (a sentinel), then renders everything it received.

`GetNumMatchingLobbies` is **not** present in `Stronghold2.exe`'s IAT; it has no mechanism to know the total count before iterating.

### OnlineID struct (deduced from ffonlinelib disassembly at RVA 0x8930)

```doc
struct OnlineID_t {        // 12 bytes, MSVC layout
    uint64  steamID;       // bytes 0–7
    uint32  isValid;       // bytes 8–11  (1 = valid, 0 = sentinel)
};
```

The wrapper always writes `isValid = 1` before returning, even for out-of-range indices (where `steamID = 0`).  Stronghold2.exe detects exhaustion by `steamID == 0`, not by `isValid == 0`.

### ISteamMatchmaking vtable layout (ISteamMatchmaking009, confirmed via live disassembly)

| Slot | Offset | Function |
|------|--------|----------|
| 12 | 0x30 | `GetLobbyByIndex` |
| 17 | 0x44 | `GetNumLobbyMembers` |
| 19 | 0x4C | `GetLobbyData` |

### Available in-progress signals

Steam metadata keys present on SH2 lobbies (logged via debug vtable hook during investigation):

| Key | Typical value | Notes |
|-----|--------------|-------|
| `HostName` | player name | Cleared or absent on some in-progress lobbies |
| `NumPlayers` | integer string | Frozen at game-start player count; never updated |
| `MapName` | map name | Set at lobby creation; never changes |

There is **no** key whose value changes at game start.  The only observable runtime signal is:

> `GetNumLobbyMembers(lobby) < atoi(GetLobbyData(lobby, "NumPlayers"))`

When a game starts, players leave the Steam lobby (they no longer need it for session management).  The `NumPlayers` metadata is frozen at the pre-start count.  If the current member count falls below that frozen count, the game has started.

---

## Attempted Fix

### Architecture

Hook `OnlineLobbyLister::GetLobbyByIndex` in `Stronghold2.exe`'s IAT (not the Steam vtable — ffonlinelib is the natural seam here).  On the first call per refresh cycle (index 0), enumerate all Steam lobbies internally using saved vtable pointers, build a compacted list of passing lobby indices, then serve them in order.  For overflow, delegate to ffonlinelib with an out-of-range index so it produces its own native sentinel rather than a hand-crafted one.

### ABI note — MSVC hidden-pointer return

`CSteamID` is a class type.  MSVC uses the hidden-pointer return convention: the caller allocates stack space for the return value and pushes its address as an implicit first argument before other arguments; the callee writes the result there and returns the pointer in EAX.  Our MinGW hook must match this exactly or ffonlinelib's wrapper crashes at its first `mov ecx, [eax]` (fault address `ffonlinelib.dll+0x8961`).

The hook typedef is therefore:

```cpp
typedef void *(__attribute__((thiscall)) *FFGetLobbyByIndex_t)(
    void *lister, OnlineID_t *hiddenReturn, unsigned int index);
```

Returning `hiddenReturn` from EAX is correct; returning a scalar `uint64` is not.

### Filter predicate

```cpp
static bool isLobbyInProgress(ISteamMatchmaking *mm, CSteamID id) {
    // HostName absent → host cleared it at game start
    const char *host = getLobbyData(mm, id, "HostName");
    if (!host || !host[0]) {
        return true;
    }

    // Members < NumPlayers(meta) → players left after game started
    const char *s = getLobbyData(mm, id, "NumPlayers");
    if (!s || !s[0]) {
        return false;
    }
    int numPlayers = atoi(s);
    if (numPlayers <= 0) {
        return false;
    }
    return getNumLobbyMembers(mm, id) < numPlayers;
}
```

### Result

Tested with 7 lobbies in the browser, 6 of which were in-progress:

- **2 of 6** in-progress lobbies filtered successfully — these were games where players had left the Steam lobby after starting.
- **4 of 6** in-progress lobbies undetectable — all game players remained in the Steam lobby (stable connections, small games), making them look identical to open waiting lobbies.
- A **phantom blank lobby** (0/0 players) briefly appeared as a rendering artifact when using a hand-crafted `{steamID=0, isValid=0}` sentinel; fixed by delegating to ffonlinelib's native out-of-range path instead.

### Fundamental detection limit

The `GetNumLobbyMembers < NumPlayers` heuristic is the **only available signal** in the entire observable API surface — Steam metadata, ffonlinelib exports, and `Stronghold2.exe`'s IAT-visible imports.  It fails whenever all game participants maintain their Steam lobby membership after the game starts, which is common in small games on good connections.  No amount of additional hooking can recover signal that was never written.

---

## How Firefly Can Fix This

Any of the following changes in a future update or Stronghold 2: Definitive Edition would make the bug fully fixable — or fix it outright without any third-party patch.

### Option 1 — SetLobbyData at game start (recommended)

When the game transitions from lobby to gameplay, call:

```cpp
ISteamMatchmaking *mm = SteamMatchmaking();
mm->SetLobbyData(hLobby, "status", "in_progress");
```

This writes a key visible to all browsing clients.  The lobby browser can then filter with a server-side call before `RequestLobbyList`:

```cpp
mm->AddRequestLobbyListStringFilter(
    "status", "in_progress", k_ELobbyComparisonNotEqual);
mm->RequestLobbyList();
```

Server-side filtering means in-progress lobbies are **never downloaded** — no client-side heuristic needed.  This is the cleanest solution.  Optionally, call `SetLobbyData(hLobby, "status", "open")` when the host returns to the lobby screen (e.g. after a game ends and the host re-opens for new players).

### Option 2 — SetLobbyJoinable at game start

```cpp
SteamMatchmaking()->SetLobbyJoinable(hLobby, false);
```

This prevents `JoinLobby` from succeeding for new members, so the attempt to join an in-progress game fails cleanly at the Steam level rather than silently inside ffonlinelib.  The lobby will still appear in the browser (there is no `GetLobbyJoinable` query and no client-side filter API for joinability), but the join attempt gives an explicit rejection rather than a confusing hang.  Combined with a UI message this is a usability improvement, though not a visibility fix.

### Option 3 — Both (belt and suspenders)

Call `SetLobbyData("status", "in_progress")` **and** `SetLobbyJoinable(false)` at game start.  Option 1 hides the lobby from browsers that filter correctly; Option 2 protects players whose clients do not.

### Option 4 — NumPlayers metadata upkeep

If setting a status key is not feasible, updating `NumPlayers` to reflect the current Steam lobby member count whenever a player drops would strengthen the existing heuristic.  This is fragile (requires correct updates on every departure) and still fails for stable full lobbies, so it is strictly inferior to Options 1–3.

---

## References

- Steam documentation: `ISteamMatchmaking::SetLobbyData`, `SetLobbyJoinable`, `AddRequestLobbyListStringFilter`
- ffonlinelib export analysis: PE exports at RVA 0x8640 (`GetNumMatchingLobbies`), 0x8840 (`SetLobbyMatchListCallback`), 0x8930 (`GetLobbyByIndex`)
- Related infrastructure: [`docs/architecture.md`](../architecture.md)
