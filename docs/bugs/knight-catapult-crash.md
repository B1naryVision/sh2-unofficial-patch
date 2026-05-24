# BUG-001: Knight/Catapult Mount Crash

**Status**: Fixed in v0.1.0
**Severity**: Critical — deterministic crash, no workaround for the player
**Game version**: Steam build v1.5.0 (PE timestamp `0x5EA311C0`)

---

## Symptom

The game crashes when a Knight unit is struck by a catapult projectile at the exact moment they are in the process of mounting a horse.

Crash type: access violation (`0xC0000005`). Fault offset: `0x001048BB`.

---

## Root Cause

Inside the function at `base+0xD6488B`, the game:

1. Calls a validity check at `CD5520` — which returns **true** (the knight still appears alive)
2. Loads a sub-object pointer from `[ESI+0x440]` into ECX
3. Writes `1` to `[ECX+0x300]`

The catapult hit zeroes the sub-object pointer at `[ESI+0x440]` partway through the knight's destruction sequence — after the validity check passes, but before the write executes. ECX is null at step 3, causing the access violation.

```asm
00D6488B  mov ecx, esi
00D6488D  call CD5520           ; validity check — returns true (too early to catch partial destruction)
00D64892  test al, al
00D64894  je D64C58             ; would exit here if invalid — but it doesn't
...
00D648AE  mov ecx, [esi+440]   ; loads sub-object ptr — already NULL at this point
00D648B4  mov [esi+448], 1
00D648BB  mov byte ptr [ecx+300], 1   ; CRASH: ecx is null
```

This is a TOCTOU window between the game's own validity check and the sub-object dereference.

---

## Fix

Two detour hooks are installed.

**Hook 1** — `base+0x39031F` (5 bytes: `CALL EDX` + `MOV [ESI+10],EAX`)

Diagnostic hook installed earlier in the call chain. Logs the EDX register context to an in-memory ring buffer on every call. No effect on crash behavior; retained for future investigation use.

**Hook 2** — `base+0x1048BB` (7 bytes: `MOV BYTE PTR [ECX+300],1`)

Null-guards ECX immediately before the faulting write:

- If ECX is null: jumps to `base+0x104C58`, the function's own safe exit path (the same destination as its null-check epilogue at `D64C58`)
- If ECX is valid: executes the original write and returns normally

This targets exactly the crash instruction, leaving all other code paths — including legitimate AI units with a null sub-object — completely unaffected.

---

## Offset Reference

| Field | Value |
| --- | --- |
| Module | `Stronghold2.exe` |
| Hook 1 offset | `base+0x39031F` |
| Hook 1 length | 5 bytes (`FF D2 89 46 10`) |
| Hook 2 offset | `base+0x1048BB` |
| Hook 2 length | 7 bytes (`C6 81 00 03 00 00 01`) |
| Skip target | `base+0x104C58` |
| Return (normal) | `base+0x1048C2` |

---

## References

- Implementation: [src/patches/knightCatapultCrash.cpp](../../src/patches/knightCatapultCrash.cpp)
- Hook infrastructure: [docs/architecture.md](../architecture.md)
