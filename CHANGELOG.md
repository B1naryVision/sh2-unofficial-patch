# Changelog

All notable changes to this project will be documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

- **Multiplayer connect-complete crash** — game crashed with access violation at
  `base+0x3d86b8` when a `CONNECT_COMPLETE` network message arrived for a peer with no
  entry in the local peer table. The error-log path in `handleConnectCompleteMessage`
  assumed `esi` (the peer pointer) was non-null and immediately did `add esi,0x18`,
  causing a dereference of address `0x2C` when `esi` was zero. Fix: redirect the
  null-peer `je` at `base+0x3d85c6` from the shared error-log path to the function's
  return epilogue (`base+0x3d86fe`) — 6-byte patch, original `0f 84 ce 00 00 00`,
  replacement `0f 84 32 01 00 00`. The spurious message is silently ignored; the
  already-connected peer path is unchanged. Safe for version mismatch.

---

## [0.2.0]

- **Intro skip** — Firefly Studios logo video is bypassed on launch; game proceeds
  directly to the main menu. Two patches applied: counter at `base+0x4DA9F8` initialised to 2
  (was 0) so only one completion-check tick is needed; `BinkOpen` call at `base+0x27BB0D`
  replaced with a stack-balancing `add esp,0x18` to leave the Bink handle NULL, triggering
  the completion check immediately on the first `Update` tick.
- **Multiplayer AI enable** — AI opponents can now be configured in
  multiplayer lobbies. Two consecutive instructions at `base+0x2A0F69` suppressed the
  AI-enabled flag unconditionally on lobby entry; NOP'ing both (14 bytes) restores the
  host's configured value. Only the host needs the patch. Desync risk exists since AI
  is computed locally on each client.
  Offset credit: Daniel Jenssen (`gitlab.com/Daerandin/sh2_mp_ai_enabler`).

---

## [0.1.0] — 2026-05-24

### Fixed

- **Knight/catapult mount crash** — game crashed when a knight was struck by a catapult projectile at the exact moment of mounting a horse. A sub-object pointer at `[ESI+0x440]` is zeroed during partial destruction before the game's own validity check fires, causing a null dereference at `base+0x1048BB`. Fixed with a targeted null guard at the crash instruction; unaffected units continue executing the normal code path.

### Added

- DLL proxy layer forwarding all `version.dll` exports to the system library (`LoadRealVersionDll` via `GetSystemDirectoryA`)
- Hook infrastructure: `InstallHook()` for 5-byte relative JMP detours with `VirtualProtect` guard
- In-memory ring buffer logging (last 10 hook contexts); flushed to `patch_debug.txt` on exit in debug builds only
- Makefile with `all`, `debug`, `deploy`, and `clean` targets
- Source structure: `src/core/`, `src/proxy/`, `src/patches/` layout
