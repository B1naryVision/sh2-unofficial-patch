# Intro Skip

**Status:** Added in v0.2.0  
**Affected version:** Stronghold 2 Steam v1.5.0  
**Patch type:** In-memory write (2 sites)

---

## Symptom

On launch the game plays the Firefly Studios logo video (`firefly_logo.bik`) before reaching the main menu. Skipping requires manual input.

---

## Root Cause

The intro is managed by an `IntroBinkScreen` class. Its `OnActivate` method loads `firefly_logo.bik` via `BinkOpen`, and its `Update` method polls a completion check each frame. A counter field at `[this+0x1e4]` is initialised to 0; it is incremented each frame the Bink handle is NULL (video ended or never loaded). When the counter reaches 3 the screen transitions to the player-name screen.

Two sites control this:

| Site | VA | Bytes (original) | Role |
| --- | --- | --- | --- |
| Counter init | `0x8DA9F8` | `00 00 00 00` | Sets counter to 0 at construction |
| BinkOpen call | `0x67BB0D` | `E8 1E 2F 18 00` | Loads the intro video |

---

## Fix

**Patch 1 — `base+0x4DA9F8` (4 bytes)**

Write `02 00 00 00`, changing the counter initialisation from 0 to 2. With the counter already at 2, only one "video finished" tick is needed before the transition fires.

**Patch 2 — `base+0x27BB0D` (5 bytes)**

Replace the 5-byte `call` to `BinkOpen` with `83 C4 18 90 90` (`add esp,0x18 ; nop ; nop`). This balances the 6-argument stack frame (`6 × 4 = 24 = 0x18` bytes) without actually loading the video, leaving the Bink handle at NULL.

**Combined effect:** On the first `Update` tick the completion check finds a NULL handle, increments the counter from 2 to 3, and the screen transitions immediately — skipping the logo entirely.

---

## Disassembly Notes

`IntroBinkScreen` vtable (VA `0x9DB784`):

| Slot | VA | Role |
| --- | --- | --- |
| [0] | `0x67BD10` | destructor (deleting) |
| [1] | `0x67BC90` | destructor (scalar) |
| [2] | `0x67BA90` | `OnActivate` |
| [4] | `0x67BB30` | `Update` |

Factory function at `0x8DA970` writes the counter field at `[this+0x1e4]` (`0x8DA9F2`: `c7 86 e4 01 00 00 00 00 00 00`).

`OnActivate` calls `BinkOpen` at `0x67BB0D` with 6 pushed arguments; return value (handle) is stored at `[this+0x1C4]` (i.e. `[this+0xe0+0xe4]`).

---

## Notes

- No saved game compatibility concerns.
