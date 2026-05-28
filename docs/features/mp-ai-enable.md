# Multiplayer AI Enable

**Status:** Added in v0.2.0  
**Affects:** Stronghold 2 Steam v1.5.0 (32-bit, `Stronghold2.exe`)

---

## Description

Restores the ability to add AI opponents when hosting a multiplayer lobby. The option
exists in the game's UI but is suppressed at runtime whenever a multiplayer session
is entered, making AI-vs-human multiplayer matches impossible without this patch.

---

## Background

Two consecutive 7-byte instructions at `base+0x2A0F69` and `base+0x2A0F70` force-reset
the AI-enabled flag to 0 on lobby entry. The game sets the variable correctly during
normal single-player flow, but these instructions override it unconditionally in the
multiplayer code path, hiding the AI option from the host's lobby UI.

---

## Implementation

Both instructions (14 bytes total) are overwritten with NOPs (`0x90`) at process
startup. With neither instruction executing, the flag retains the value the host
configured, and the AI opponent button remains available throughout lobby setup.

| # | Offset | Size | Original | Patched |
|---|--------|------|----------|---------|
| 1 | `base+0x2A0F69` | 7 bytes | unknown | `90 90 90 90 90 90 90` |
| 2 | `base+0x2A0F70` | 7 bytes | unknown | `90 90 90 90 90 90 90` |

The original instruction bytes were not captured during investigation; the offsets
were derived from the community tool by Daniel Jenssen
(`gitlab.com/Daerandin/sh2_mp_ai_enabler`, v0.3.1), which independently identified
the same offsets against the same Steam build.

---

## Multiplayer Compatibility

Only the **host** needs this patch to configure AI opponents in the lobby. Clients
joining a host-configured AI game do not require the patch.

**Desync risk:** AI decisions are computed locally on each machine. If clients
diverge in tick timing or RNG state, game state may split. This was likely the
original reason Firefly disabled the feature. Stability in practice is untested;
use with caution in competitive play.

---

## Credits

Offset discovery: Daniel Jenssen (`gitlab.com/Daerandin/sh2_mp_ai_enabler`)
