# Technical Architecture

This document covers the patch delivery mechanism, hook infrastructure, safe patching patterns, and the recommended long-term project layout.

---

## How the Patch Loads

The patch exploits Windows' DLL search order. When Stronghold 2 starts, Windows resolves `d3d9.dll` by searching the application directory before `System32`. Placing our `d3d9.dll` in the game directory causes it to load in place of the system library.

> **Note:** Earlier releases proxied `version.dll` instead. The project migrated to `d3d9.dll` because `version.dll` is sometimes ignored or rejected by Windows under stricter DLL-load policies, whereas `d3d9.dll` (which the game genuinely depends on) is not.

The patch DLL:

1. Forwards all legitimate `d3d9.dll` exports to the real system `d3d9.dll` (loaded from `System32` at runtime)
2. On `DLL_PROCESS_ATTACH`, installs detour hooks to fix bugs in the game binary

No game files are ever modified on disk.

---

## DLL Proxy Layer

The proxy is defined in `d3d9.def` (export ordinals) and implemented via naked `__declspec(naked)` functions that jump through a table of real function pointers (`g_d3d9Functions[]`).

`loadRealD3d9Dll()` must be called before any proxy exports can be used. It loads the system `d3d9.dll` by absolute path from `GetSystemDirectoryA` and resolves each export via `GetProcAddress`.

**Exports proxied:**

| Function | Ordinal |
| --- | --- |
| `Direct3DCreate9` | 1 |
| `Direct3DCreate9Ex` | 2 |
| `Direct3DShaderValidatorCreate9` | 3 |
| `PSGPError` | 4 |
| `PSGPSampleTexture` | 5 |
| `D3DPERF_BeginEvent` | 6 |
| `D3DPERF_EndEvent` | 7 |
| `D3DPERF_GetStatus` | 8 |
| `D3DPERF_QueryRepeatFrame` | 9 |
| `D3DPERF_SetMarker` | 10 |
| `D3DPERF_SetOptions` | 11 |
| `D3DPERF_SetRegion` | 12 |
| `DebugSetMute` | 13 |

The set covers every export present in all `d3d9.dll` implementations the game can run against (Windows 7+, WineD3D, DXVK). The `D3DPERF_*` family is included because injected tooling (capture tools, overlays) resolves those names against whatever module is called `d3d9.dll` — this proxy. Win10-only exports (`Direct3DCreate9On12`, `Direct3DCreate9On12Ex`) are deliberately not proxied so the proxy never exports a name the underlying DLL might lack.

If the system `d3d9.dll` cannot be loaded, `loadRealD3d9Dll()` shows an error message box and leaves the forwarding table null — the game cannot run without Direct3D 9 in any case.

---

## Hook Infrastructure

### 5-Byte Relative JMP Detour

`installHook(targetAddress, detourFunction, instructionLength)`:

1. Calls `VirtualProtect` to make the target page writable — if this fails, the hook is skipped (the site is left untouched rather than partially written)
2. Writes `0xE9` (near JMP) + 4-byte relative offset at `targetAddress`
3. NOPs any bytes between offset 5 and `instructionLength` (for instructions larger than 5 bytes)
4. Restores the original page protection
5. Calls `FlushInstructionCache` over the patched range

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
│   │   ├── d3d9Proxy.h
│   │   └── d3d9Proxy.cpp               ← loadRealD3d9Dll + all 5 naked exports
│   └── patches/
│       ├── registry.h / registry.cpp   ← applyUnofficialPatches() dispatcher
│       └── knightCatapultCrash.cpp     ← one .cpp per fix
├── docs/
│   ├── architecture.md
│   └── bugs/
│       └── *.md                        ← one file per investigated bug
├── Makefile
├── d3d9.def
├── README.md
├── CHANGELOG.md
└── LICENSE
```

**Per-patch file convention** — each `patches/*.cpp` should contain:

```cpp
/**
 * Short description
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
