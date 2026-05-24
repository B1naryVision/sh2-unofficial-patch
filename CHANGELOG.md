# Changelog

All notable changes to this project will be documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

---

## [0.1.0] — 2026-05-24

### Fixed

- **BUG-001: Knight/catapult mount crash** — game crashed when a knight was struck by a catapult projectile at the exact moment of mounting a horse. A sub-object pointer at `[ESI+0x440]` is zeroed during partial destruction before the game's own validity check fires, causing a null dereference at `base+0x1048BB`. Fixed with a targeted null guard at the crash instruction; unaffected units continue executing the normal code path.

### Added

- DLL proxy layer forwarding all `version.dll` exports to the system library (`LoadRealVersionDll` via `GetSystemDirectoryA`)
- Hook infrastructure: `InstallHook()` for 5-byte relative JMP detours with `VirtualProtect` guard
- In-memory ring buffer logging (last 10 hook contexts); flushed to `patch_debug.txt` on exit in debug builds only
- Makefile with `all`, `debug`, `deploy`, and `clean` targets
- Source structure: `src/core/`, `src/proxy/`, `src/patches/` layout
