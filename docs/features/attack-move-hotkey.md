# Attack-Move Toggle Hotkey (`Mouse4` / XButton1)

**Status:** Added in v0.5.0 — confirmed in-game (firecarts attack-move on move orders)
**Affected version:** Stronghold 2 Steam v1.5.0
**Patch type:** Two hooks — a per-frame main-loop trampoline (RVA `0x300e1`) that polls
XButton1 on the sim thread, and a caching hook on the troop-panel command dispatcher
(RVA `0x22e7d0`) that captures the live panel pointer.

---

## Symptom / Motivation

The troop command panel's "Attack" toggle (crossing swords) turns the selected troops'
**attack-move** stance on/off: while it is on, a normal move order becomes an attack-move
(units engage/burn things en route). It has no keyboard/mouse shortcut. This binds the
fourth mouse button (`VK_XBUTTON1` / Mouse4) to that toggle. It is especially useful for
firecarts — enable it, then just move them around to burn things without clicking targets.

`Mouse4` is unbound by the stock game and collides with no existing binding.

The button can be changed (or the feature disabled with `None`) via
`[hotkeys] AttackToggle` in `sh2-unofficial-patch.ini` — see
[configuration.md](configuration.md). When disabled, neither of the two hooks
is installed.

---

## What "attack-move on" actually is

Attack-move is **not** a command/chore and **not** a cosmetic icon state. It is a single
byte on the active `SubPanelTroops` object:

- **`panel + 0x8be0`** — the "Attack" stance flag (`1` = on). It is one of three
  mutually-exclusive stance toggles (`0x8be0` / `0x8be1` / `0x8be2`); turning one on clears
  the others.
- The flag is **authoritative**, not a mirror. Its single reader in the binary is at RVA
  `0x231365`:

  ```asm
  mov al, [esi+0x8be0]        ; attack stance
  or  al, [esi+0x8be2]        ; OR stand-ground stance
  mov [ebp-0x24], 0x3
  je  skip                    ; if neither set, param = 0x3 (normal move)
  mov [ebp-0x24], 0x63        ; else param = 0x63 (attack-move)
  ```

  i.e. the move-order path selects parameter `0x3` (move) vs `0x63` (attack-move) based on
  this flag. Setting the flag is therefore sufficient to make subsequent moves attack-moves.

The flag is set by the stock toggle button, command id **`0x1dbd`**, whose handler
(RVA `0x22e8ac`) responds to message opcode **`0x68`** (set-state): it writes
`panel+0x8be0` from the message's on/off byte and, when enabling, clears the sibling stance
flags and refreshes their button visuals. The handler issues no chore — it only sets UI
state that the move path reads.

---

## How the hotkey works

Two hooks, both self-contained (no edits to other patches):

1. **Panel-pointer caching — dispatcher hook (RVA `0x22e7d0`).** `SubPanelTroops`'s command
   dispatcher is vtable slot 0; it is called (thiscall, `ecx` = the live panel) whenever the
   panel processes a message. The 6-byte prologue `55 8b ec 83 ec 08`
   (`push ebp ; mov ebp,esp ; sub esp,8`) is position-independent, so the trampoline stores
   `ecx` into `g_troopPanel` and re-emits it verbatim. There is **no flat global** that
   points at the active panel (verified against two process dumps — it lives in a heap
   container), so caching it from the one function guaranteed to receive it is the reliable
   route.

2. **Per-frame poll — main-loop trampoline (RVA `0x300e1`).** A few instructions past the
   Stop hotkey's hook in the same loop body. Each frame, rising-edge detection on
   `GetAsyncKeyState(VK_XBUTTON1)` (guarded by `GetForegroundWindow` belonging to the game)
   toggles the stance. See the ASLR note below.

On a press, the patch reads the current `panel+0x8be0`, then **replays the stock toggle
message** — `dispatch(panel, { opcode 0x68, subid 0x1dbd, onOff = !current })`. Using the
game's own dispatcher (rather than poking the byte directly) gives correct mutual exclusion
and button-visual refresh for free. The dispatcher's gate byte `ds:0x11b8483` is `0` during
normal play, so the message reaches the handler.

### ASLR note (frame-poll site)

The frame-poll site's 5 overwritten bytes are `b9 00 d9 ac 02` = `mov ecx, 0x2acd900`, whose
immediate is a **base-relocated module address**. The on-disk bytes are pre-relocation, so
the trampoline captures the live immediate (`*(uint32_t*)(site+1)`) at install time into
`g_attackHookEcx` and re-emits `mov ecx, g_attackHookEcx`.

---

## Resolved offsets (RVAs)

| Item | Value | Notes |
| --- | --- | --- |
| Per-frame poll hook | `0x300e1` (5 bytes) | main-loop body; sim thread; return at `+5` |
| Dispatcher / caching hook | `0x22e7d0` (6 bytes) | `SubPanelTroops::handleCommand`; `ecx` = panel; return at `+6` |
| `SubPanelTroops` vtable | `0x5cd4cc` | used to validate the cached pointer |
| Attack stance flag | `panel + 0x8be0` | `1` = attack-move on |
| Attack toggle command id | `0x1dbd` | opcode `0x68` sets the flag |
| Attack-move move param | `0x63` (vs `0x3`) | chosen at RVA `0x231365` from the flag |

---

## How the offsets were found (and a correction)

Recovered from two full user-mode minidumps (see the "Minidump + RTTI analysis" section of
`CLAUDE.md`).

An **initial identification was wrong**: mapping the panel's 15 buttons to a 4×4 grid by
their stored screen coordinates pointed at the top-right cell (command id `0x1dc9`), a
*momentary* button whose handler submits a chore via `S2ActorHandler::sub_0xf35d0`. Wiring
Mouse4 to it fired cleanly (confirmed by debug logging) but did nothing visible.

A second dump — **taken with a firecart selected and attack-move already enabled via the
stock button** — settled it: the active `SubPanelTroops` had `+0x8be0 = 1`. That flag is
set by command id `0x1dbd` (a stance `ToggleButton`), not `0x1dc9`. Tracing the flag's
single reader (`0x231365`) confirmed it drives the move-vs-attack-move parameter. Lesson:
identify a button by the state a live "feature-enabled" dump shows, not by static grid
coordinates alone.

---

## Multiplayer Compatibility

**Safe for version mismatch — the other player does not need the patch.**

Toggling `panel+0x8be0` is **client-local UI state**, identical to the local player clicking
the stock Attack toggle. It issues no networked message itself. It only changes how the
*local* player's *subsequent* move orders are generated (move vs attack-move); that move
order is a normal player command, queued/broadcast in lockstep and executed identically on
every client. Because the stock toggle is a shipped MP feature that does not desync, neither
does this hotkey.

### Threading

The toggle is applied from the main-loop body hook (RVA `0x300e1`), i.e. on the sim thread
that owns the panel — the same thread a real click is processed on. No cross-thread race.

---

## Notes

- No saved-game compatibility concerns.
- Target button is `Mouse4` (`VK_XBUTTON1` = `0x05`). Confirm it is unbound in the player's
  settings before release.
- If the panel has not been captured yet (no message has flowed through the dispatcher since
  selection), the first press is a no-op; interacting with the troop panel once populates
  `g_troopPanel`. In practice selecting troops triggers panel messages that cache it.
- Coexists with the stop-troops hotkey: both poll on the same per-frame main-loop body via
  independent trampolines (`0x300c0` for Stop, `0x300e1` for Attack).
