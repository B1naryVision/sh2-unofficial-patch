# Stronghold 2 Unofficial Patch

An unofficial community patch for **Stronghold 2** (Firefly Studios, 2005) targeting severe crashes and memory corruption bugs while preserving original gameplay and full save compatibility.

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](https://unlicense.org)

---

## Overview

The patch is delivered as a `version.dll` that sits in the game directory. Windows loads it automatically via DLL search order before the system `version.dll`. It forwards all legitimate `version.dll` API calls to the real system library, then installs targeted detour hooks to fix specific bugs in the game binary.

No game files are modified. Removing the DLL restores the original behavior completely.

**Target**: Stronghold 2, Steam build v1.5.0
**Architecture**: 32-bit x86 Windows PE  
**Development environment**: Linux/WSL with MinGW cross-compiler

---

## Goals and Non-Goals

### Goals

- Fix crashes and memory corruption bugs in the original binary
- Preserve original gameplay behavior exactly
- Remain compatible with vanilla Steam installs and existing saves
- Remain unobtrusive — zero overhead in release builds

### Non-Goals (for now)

- Balance changes or gameplay modifications
- Multiplayer or online feature changes

---

## Current Fixes

### v0.1.0

| ID | Description | Offset | Status |
| --- | --- | --- | --- |
| [BUG-001](docs/bugs/knight-catapult-crash.md) | Knight/catapult mount crash | `base+0x1048BB` | Fixed in v0.1.0 |

---

## Prerequisites

### Linux / WSL (recommended)

```bash
sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686
```

### Windows (native MinGW)

Install [MinGW-w64](https://www.mingw-w64.org/) with i686 (32-bit) target support and ensure `i686-w64-mingw32-g++` is on your PATH.

---

## Building

```bash
make          # release build → version.dll
make debug    # debug build   → version.dll (with file logging enabled)
make clean    # remove build artifacts
```

---

## Installation

1. Build `version.dll` (see above)
2. Copy it into the Stronghold 2 game directory.
3. Launch the game normally through Steam. The patch loads automatically.

**To uninstall**: delete `version.dll` from the game directory.

---

## Debug Logging

Debug builds (`make debug`) write a `patch_debug.txt` log to the working directory when the game runs. This records the register context at each hooked call site. Release builds produce no log output and have zero logging overhead.

Log location: `<Stronghold 2 game directory>\patch_debug.txt`

---

## Reporting Bugs

If you've found a crash or other bug in Stronghold 2 that you'd like to see fixed, open a GitHub issue with the following:

**Required:**

1. Steps to reproduce — what were you doing in-game when it happened? (unit types, game mode, approximate point in the battle)
2. Windows Event Viewer log — open Event Viewer → Windows Logs → Application, find the entry for `Stronghold2.exe`, and paste the full error block. The fault offset and faulting module are the most useful fields.

**Nice to have:**

- A video of the crash occurring — even a short clip showing the sequence of events immediately before the crash is extremely helpful for identifying the exact scenario

The more reproducible the steps, the faster a fix can be found and verified.

---

## Contributing

This is an open reverse-engineering project. Contributions are welcome.

**Before submitting a fix:**

1. Document the bug in `docs/bugs/` — symptom, disassembly, root cause, game version/offset
2. Confirm the offset against a clean Steam install
3. Test the fix by reproducing the original crash scenario and verifying it no longer occurs
4. Add an entry to `CHANGELOG.md` and the fix table in this README

See [docs/architecture.md](docs/architecture.md) for technical background on the hook infrastructure, naming conventions, and safe patching patterns.

---

## License

This project is released into the public domain under the [Unlicense](LICENSE).
