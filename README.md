# Stronghold 2 Unofficial Patch

An unofficial community patch for **Stronghold 2** (Firefly Studios, 2005) fixing crashes, restoring missing functionality, and improving quality of life — all while remaining fully compatible with vanilla saves and stock installs.

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](https://unlicense.org)

---

## Overview

The patch is delivered as a `d3d9.dll` that sits in the game directory. Windows loads it automatically via DLL search order before the system `d3d9.dll`. It forwards all legitimate `d3d9.dll` API calls to the real system library, then installs targeted detour hooks to fix specific bugs in the game binary.

No game files are modified. Removing the DLL restores the original behavior completely.

- Target: Stronghold 2, Steam build v1.5.0
- Architecture: 32-bit x86 Windows PE  
- Development environment: Linux/WSL with MinGW cross-compiler

---

## Goals and Non-Goals

### Goals

- Fix crashes and memory corruption bugs
- Restore suppressed or broken functionality to its intended state
- Improve quality of life
- Remain compatible with vanilla Steam installs and existing saves
- Remain unobtrusive — zero overhead in release builds
- Balance changes or rebalancing of existing mechanics

### Non-Goals (for now)

- Content additions (new units, buildings, missions)

---

## Current Fixes and Features

### v0.4.0

| Description | Offset | Status |
| --- | --- | --- |
| [Knight/catapult mount crash](docs/bugs/knight-catapult-crash.md) | `base+0x1048BB` | Fixed in v0.1.0 |
| [Barracks UI crash on Lord death](docs/bugs/barracks-lord-death-crash.md) | `base+0x2207c6` | Fixed in v0.4.0 |
| [AI opponents in multiplayer lobbies](docs/features/mp-ai-enable.md) | `base+0x2A0F69` | Added in v0.2.0 |
| [Skip Firefly logo intro on launch](docs/features/intro-skip.md) | `base+0x4DA9F8`, `base+0x27BB0D` | Added in v0.2.0 |
| [MP connect-complete crash](docs/bugs/mp-connect-complete-crash.md) | `base+0x3d85c6` | Added in v0.3.0 |
| [End-of-game statistics overlay](docs/features/endgame-stats.md) | `base+0x297fa0`, `base+0x297700` | Added in v0.3.0 |
| [Stop selected troops hotkey (`H`)](docs/features/stop-troops-hotkey.md) | `base+0xf3140` | Unreleased |
| [Attack-move toggle hotkey (`Mouse4`)](docs/features/attack-move-hotkey.md) | `base+0x22e7d0`, `base+0x300e1` | Added in v0.4.0 |
| [Faster camera zoom (3×)](docs/features/zoom-speed.md) | `base+0x3623D8` | Unreleased |
| [Out-of-bounds troop formation crash](docs/bugs/map-edge-formation-crash.md) | `base+0x37dd6b` | Unreleased |

---

## Roadmap

Items under active investigation or queued for a future release.

The following are implemented but excluded from the build until the patch has broader adoption — both require **all players to run the same patch version** to avoid multiplayer desyncs:

| Description | Notes |
| --- | --- |
| [Field ballista auto-fire restore](docs/features/ballista-auto-fire.md) | Modifies unit health state each tick |
| [Unit cap raise (550 in all lobby sizes)](docs/features/unit-cap-raise.md) | Per-player cap enforced locally; patched vs unpatched clients diverge |

| Priority | Type | Description | Requested by |
| --- | --- | --- | --- |
| Medium | Bug fix | Wheat Farmers and Candle Makers should not be eligible to become criminals — they get permanently stuck inside buildings | TheSettler |
| Medium | Feature | Patch configuration, possibly via an `.ini` file in the game directory — toggle individual fixes/features on or off and rebind hotkeys | BinaryVision |
| Low | Feature | Checkbox to disable market placement in No Market multiplayer games | Ignite |

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
make          # release build → d3d9.dll
make debug    # debug build   → d3d9.dll (with file logging enabled)
make clean    # remove build artifacts
```

---

## Installation

1. Build `d3d9.dll` (see above)
2. Copy it into the Stronghold 2 game directory.
3. Launch the game normally through Steam. The patch loads automatically.

**To uninstall**: delete `d3d9.dll` from the game directory.

---

## Debug Logging

Debug builds (`make debug`) write a `patch_debug.txt` log to the working directory when the game runs. This records the register context at each hooked call site. Release builds produce no log output and have zero logging overhead.

Log location: `<Stronghold 2 game directory>\patch_debug.txt`

---

## Reporting Bugs

If you've found a crash or other bug, please report it so it can be investigated and fixed.

### How to open a report

**Option 1 — GitHub** (preferred, keeps everything public and trackable):

1. Go to the [Issues page](https://github.com/B1naryVision/sh2-unofficial-patch/issues) on GitHub. (GitHub is a free website — you'll need to create an account if you don't have one.)
2. Click the green **New issue** button.
3. Give your issue a short title, e.g. *"Game crashes when catapult fires at a wall"*.
4. Fill in the description using the information below, then click **Submit new issue**.

**Option 2 — Discord** (if you'd rather not sign up for GitHub):

Send a direct message to BinaryVision (`196165954042986496`) on Discord with the same details listed below.

### What to include

**Required — describe what happened:**

- What were you doing when the crash or bug occurred? For example: Multiplayer, Kingmaker or a mission? Which units were involved, and roughly how far into the game it happened.
- What steps would someone else need to take to make it happen again? The more specific the better — "it crashed randomly" is hard to investigate; "it always crashes when I fire the catapult at knights in the process of mounting" can be fixed.

**Required — attach the Windows error log:**

Windows automatically records crash details. Here's how to find them:

1. Press **Win + R**, type `eventvwr`, and press Enter. This opens Event Viewer.
2. In the left panel, expand **Windows Logs** and click **Application**.
3. Look through the list for a red **Error** entry that appeared around the time of the crash. The source will usually be `Application Error`.
4. Double-click that entry to open it.
5. Copy the full text from the **General** tab and paste it into your report. The lines that start with *Faulting application name* and *Fault offset* are the most important.

If there is also a `patch_debug.txt` file in your Stronghold 2 game folder, please attach that too. (See [Debug Logging](#debug-logging) above for where to find it.)

**Optional but very helpful:**

- A short video or screen recording of the crash happening — even 10–15 seconds showing what you were doing just before the crash can make a big difference.

The more detail you can provide, the faster a fix can be found and verified.

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

## Acknowledgements

**TheSettler** — Primary multiplayer co-tester, invaluable in verifying that each release stays fully compatible with vanilla Stronghold 2 installs. Also a key source of ideas for improvements to the game.

**Ignite** — Ongoing assistance debugging patch loading issues on Windows 10, and a steady stream of ideas for features and fixes that have shaped the project's direction.

**SD7804** — Instrumental in finding the relevant memory locations for the end-of-game statistics feature.

---

## License

This project is released into the public domain under the [Unlicense](LICENSE).
