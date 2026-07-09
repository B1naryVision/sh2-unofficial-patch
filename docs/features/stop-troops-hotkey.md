# Stop Selected Troops Hotkey (`H`)

**Status:** Implemented (offsets confirmed from a process dump; MP-safe game-thread version)
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Main-loop trampoline (RVA `0x300c0`) that polls `H` on the sim thread and calls the game's own stop routine via `thiscall`

---

## Symptom / Motivation

The in-game "Stop" command for a selected group of troops is only reachable by
clicking the Stop button on the command panel. There is no keyboard shortcut.
This feature binds `H` to that same command.

---

## How input is captured

`Stronghold2.exe` has **no `WndProc`/message-based key handling** — it imports
only `GetAsyncKeyState` (used at 4 sites, all for Shift/Alt modifiers). Window
creation and the message pump live in `dragonfly.dll`. There is no shared
in-exe hotkey table to hook a new key into.

Rather than a background thread (which would race the command queue — see
Multiplayer Compatibility), the key is polled **on the game thread** via a
trampoline hooked into the main-loop body:

- **Hook site: RVA `0x300c0`** — the top of the main loop's per-frame body (the
  target of the `jne` loop-back after `EventMgr::processEvents`), 6 bytes:
  `ff d7 8b 10 8b c8` = `call edi ; mov edx,[eax] ; mov ecx,eax`. These are
  position-independent, so the trampoline re-emits them verbatim after running
  the tick. This function is the sim thread — the same thread that services the
  command queue at `base + 0xd781d0` — so a command submitted here is on the
  correct thread at a safe point, right after events are processed.
- The trampoline at this site is now owned by the **shared frame-tick
  dispatcher** (`src/core/frameTick.cpp`), which runs any number of registered
  per-frame callbacks; this hotkey registers its tick via `registerFrameTick()`
  and the endgame-stats session poll shares the same site. Dispatcher callbacks
  must be float-free (the site is mid-function, so live x87 state is possible).
- Each frame the tick does rising-edge detection on `GetAsyncKeyState('H')` (one
  press → one command), guards on `GetForegroundWindow()` belonging to the game
  process, and calls the game's own stop routine.

`H` is safe: the exe never polls it, and it is unbound by default.

---

## The stop routine (resolved offsets)

All RVAs; the game's internal Stop button issues command id **`0x66`** through
the command dispatcher at RVA `0x22e7d0`, which runs exactly this:

| Item | Value | Notes |
| --- | --- | --- |
| Per-frame hook site | RVA `0x300c0` (6 bytes) | main-loop body; sim thread; return at `+6` |
| Stop function | RVA `0xf3140` | `void __thiscall S2ActorHandler::StopSelectedTroops(int playerSlot)` |
| `this` | `base + 0xdb8cb8` | the `S2ActorHandler` **static global object** (use the address itself, not a deref) |
| `playerSlot` arg | `*(int*)(base + 0x6e8c5c)` | local player's table slot (same global used by endgame-stats) |

The game's own call site (RVA `0x22e9bb`):

```asm
mov ecx, 0x11781d0     ; scheduler global
call 0x40dbd0          ; -> returns *(int*)0xae8c5c  (local player slot)
push eax               ; playerSlot
mov ecx, 0x11b8cb8     ; this = S2ActorHandler global   (RVA 0xdb8cb8)
call 0x4f3140          ; StopSelectedTroops             (RVA 0xf3140)
```

`StopSelectedTroops` does everything internally: it builds the selected-troop
group into a scratch buffer at `[this+0x1240]` (a `std::vector`, empty at rest),
`new`s a 20-byte `StopAllTroopsChore` (RVA `0xf3140` calls the ctor at RVA
`0x1e8500`), copies the group into it, and submits it to the global scheduler at
`base + 0xd781d0`. So the patch does **not** need to touch the selection or
construct a chore itself — one `thiscall` reproduces the button exactly.

### Command architecture (for reference)

Unit commands are `*Chore@Stronghold2` objects submitted to a scheduler.
Confirmed vtable RVAs (from RTTI in the dump):

| Chore | vtable RVA |
| --- | --- |
| `StopAllTroopsChore` | `0x5c6ffc` (+ `0x5c7008` secondary base) |
| `SelectTroopsChore` | `0x5c6fa8` |
| `SetTroopStanceChore` | `0x5c6bb0` |
| `MoveTroopsChore` | `0x5c6e60` |
| `SubPanelTroops` (troop command panel) | `0x5cd4cc` |

---

## How the offsets were found

Static search failed (the command path is only exercised at runtime), so the
offsets were recovered from a full user-mode minidump (`Stronghold2.DMP`) taken
with troops selected. See the "Minidump + RTTI analysis" section of
`CLAUDE.md`. Summary:

1. Parsed the minidump; the module loaded at base `0x008e0000` (ASLR), confirmed
   against the known player table at RVA `0x6e8bd8`.
2. MSVC RTTI is intact — resolved class names from vtables. Cataloged all 774
   `*@Stronghold2@@` classes; found `StopAllTroopsChore`, `S2ActorHandler`,
   `SubPanelTroops`, etc.
3. Found the single caller of the `StopAllTroopsChore` constructor → the
   `StopSelectedTroops` handler → its only caller (the command dispatcher),
   which revealed the `S2ActorHandler` global and the `playerSlot` argument.

---

## Multiplayer Compatibility

**Safe for version mismatch — the other player does not need the patch.**

This patch does not add any behaviour to the simulation. It invokes the *same*
function, with the *same* arguments, as the in-game Stop button (command id
`0x66` → `StopSelectedTroops(handler, localSlot)`); the only change is that a
key press triggers it instead of a mouse click. A "stop" is a **player command**
(an input), not autonomous per-tick logic. In SH2's deterministic lockstep,
player commands are queued, tagged with an execution tick, and broadcast so
every client executes them identically — and `StopSelectedTroops` serialises the
concrete unit set into the chore, so the remote client reproduces it without
needing the local player's selection. Because the stock Stop button is a shipped
MP feature that does not desync, neither does this hotkey.

This is unlike ballista auto-fire / unit-cap raise, which change autonomous
simulation logic each client computes independently (patched vs unpatched
diverge). Triggering an existing networked command does not.

### Threading

Resolved. The command is submitted from the main-loop body hook (RVA `0x300c0`),
i.e. on the sim thread that services the command queue — not from a background
thread. There is no cross-thread race with the queue; the call is equivalent to
the game itself processing a Stop click on that frame. No remaining MP caveat.

---

## Notes

- No saved-game compatibility concerns.
- Target key is `H` (`VK` = `0x48`). Confirm it is unbound in the player's
  keybindings before release.
