# Technical Architecture

This document covers the patch delivery mechanism, hook infrastructure, safe patching patterns, and the recommended long-term project layout.

---

## How the Patch Loads

The patch exploits Windows' DLL search order. When Stronghold 2 starts, Windows resolves `version.dll` by searching the application directory before `System32`. Placing our `version.dll` in the game directory causes it to load in place of the system library.

The patch DLL:

1. Forwards all legitimate `version.dll` exports to the real system `version.dll` (loaded from `System32` at runtime)
2. On `DLL_PROCESS_ATTACH`, installs detour hooks to fix bugs in the game binary

No game files are ever modified on disk.

---

## DLL Proxy Layer

The proxy is defined in `version.def` (export ordinals) and implemented via naked `__declspec(naked)` functions that jump through a table of real function pointers (`g_realFunctions[]`).

`loadRealVersionDll()` must be called before any proxy exports can be used. It loads the system `version.dll` by absolute path from `GetSystemDirectoryA` and resolves each export via `GetProcAddress`.

**Exports proxied:**

| Function | Ordinal |
| --- | --- |
| `GetFileVersionInfoA` | 1 |
| `GetFileVersionInfoW` | 2 |
| `GetFileVersionInfoSizeA` | 3 |
| `GetFileVersionInfoSizeW` | 4 |
| `VerQueryValueA` | 5 |
| `VerQueryValueW` | 6 |

---

## Hook Infrastructure

### 5-Byte Relative JMP Detour

`installHook(targetAddress, detourFunction, instructionLength)`:

1. Calls `VirtualProtect` to make the target page writable
2. Writes `0xE9` (near JMP) + 4-byte relative offset at `targetAddress`
3. NOPs any bytes between offset 5 and `instructionLength` (for instructions larger than 5 bytes)
4. Restores the original page protection

The detour function must be a `__declspec(naked)` assembly function that:

- Saves all registers before doing anything (`pushal`)
- Performs the fix logic
- Restores registers (`popal`)
- Re-executes the original instruction (if still needed)
- Jumps to `s_returnAddress` (set to `targetAddress + instructionLength`)

### Calling Conventions in 32-bit MinGW

Naked functions use `__asm__ volatile(...)` with GNU inline assembly syntax. Global variables referenced from inline assembly require a leading underscore (`_varName`) to match MinGW's 32-bit name mangling.

### Return Address

Each hook sets a global return address before installing the hook:

```cpp
s_returnAddress = targetInstruction + instructionLength;
installHook((void*)targetInstruction, detourFn, instructionLength);
```

The naked function jumps to `*_s_returnAddress` at the end. Each hook needs its own return address global, and that global must **not** be `static` — inline assembly cannot resolve LOCAL symbols at link time.

---

## Debug Logging

`logPushContext(value)` in `src/core/log.cpp` writes to a 10-entry in-memory ring buffer with zero disk I/O during gameplay. The buffer is flushed to `patch_debug.txt` only on `DLL_PROCESS_DETACH` (game exit or crash), and only in debug builds.

`make debug` passes `-DDEBUG`; `make` (release) does not, making all logging a compile-time no-op.

**Alternatives for live inspection:**

- `OutputDebugStringA()` — readable in [x32dbg](https://x32dbg.com/) or [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) without any file overhead

---

## Debugging Workflow (WSL to Windows)

| Stage | Tool | Notes |
| --- | --- | --- |
| Build | `make debug` in WSL | Produces debug DLL with logging |
| Deploy | `make deploy` | Copies to game directory via WSL mount |
| Runtime log | `patch_debug.txt` | Written to game dir; open in any text editor |
| Crash investigation | [x32dbg](https://x32dbg.com/) | Attach to `Stronghold2.exe`; set BP at offsets |
| Memory inspection | [Cheat Engine](https://cheatengine.org/) | Good for initial offset discovery |
| Symbol export | `make debug` + MAP file | Add `-Wl,-Map=patch.map` to CXXFLAGS for symbol addresses |

x32dbg is the primary recommended debugger. It handles 32-bit processes natively, has good symbol support, and can display `OutputDebugStringA` output.

---

## Source Layout

```text
sh2-unofficial-patch/
├── src/
│   ├── dllmain.cpp                     ← DllMain only; calls proxy init + apply patches
│   ├── core/
│   │   ├── hook.h / hook.cpp           ← installHook
│   │   └── log.h / log.cpp             ← ring-buffer logging (no-op in release)
│   ├── proxy/
│   │   ├── versionProxy.h
│   │   └── versionProxy.cpp            ← loadRealVersionDll + all 6 naked exports
│   └── patches/
│       ├── registry.h / registry.cpp   ← applyUnofficialPatches() dispatcher
│       └── knightCatapultCrash.cpp     ← one .cpp per fix
├── docs/
│   ├── architecture.md
│   └── bugs/
│       └── *.md                        ← one file per investigated bug
├── Makefile
├── version.def
├── README.md
├── CHANGELOG.md
└── LICENSE
```

**Per-patch file convention** — each `patches/*.cpp` should contain:

```cpp
/**
 * [BUG-ID] Short description
 *
 * Hook — base+0xXXXXXX: <original instruction> (N bytes)
 *
 * Symptom: ...
 * Cause:   ...
 * Fix:     ...
 */

// Return-address globals must NOT be static — inline assembly can't resolve LOCAL symbols.
uintptr_t s_returnAddress = 0;

__declspec(naked) static void bugIdHook() {
    __asm__ volatile(
        // ... re-execute overwritten instruction(s), then:
        "jmp *_s_returnAddress\n\t");
}

void installBugIdFix() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t target = base + 0xXXXXXX;
    s_returnAddress = target + INSTRUCTION_LENGTH;
    installHook((void *)target, reinterpret_cast<void *>(bugIdHook), INSTRUCTION_LENGTH);
}
```

And `registry.cpp` calls each `install*Fix()` in sequence.

---

## Compile-Time Feature Flags

For optional fixes or quality-of-life changes that some players may not want, use compile-time flags rather than runtime toggles:

```makefile
# In Makefile, add per-feature flags:
CXXFLAGS += -DPATCH_KNIGHT_CRASH
# CXXFLAGS += -DFIX_AI_PATHFINDING   # uncomment to enable
```

This keeps the release binary minimal and avoids runtime branching overhead.

---

## Safe Patching Checklist

Before shipping any new hook:

- [ ] Confirmed the target offset is correct for the tested game version
- [ ] Confirmed instruction length at the target (disassemble — do not guess)
- [ ] Return address set to `target + actual_instruction_length` before hook install
- [ ] Naked function saves all registers before any logic, restores before jumping back
- [ ] Debug build tested: hook fires, log output is sane
- [ ] Release build tested: game runs normally without regression
- [ ] Bug documented in `docs/bugs/`
