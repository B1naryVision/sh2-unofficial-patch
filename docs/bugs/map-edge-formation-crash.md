# Out-of-Bounds Troop Formation Crash

**Status**: Fixed in [Unreleased]
**Severity**: High — reproducible crash to desktop, player-triggerable
**Game version**: Steam build v1.5.0 (PE timestamp `0x5EA311C0`)

---

## Symptom

The game crashes to desktop when troops (or their formation preview) are pushed
**off the edge of the map** — most reliably by sending a selected group's
formation line into a corner then switching to circle formation.

Crash type: access violation (`0xC0000005`). Fault offset: `0x0037DD6B`
(reported by two separate players, plus an unpatched player — this is a stock
game bug, independent of the patch).

---

## Root Cause

The faulting instruction is at RVA `0x37dd6b` (VA `0x77dd6b`):

```asm
0077dd6b  test byte ptr [ecx+0x1d6124c], 0x3f   ; read a per-tile flag  <-- CRASH
0077dd72  je   0x77dd8d                          ; flag clear -> "off map" return
```

`0x1d6124c` is the base of the **map-flag array** — a flat `256 x 256` byte grid
(1 flag byte per tile). This particular accessor is method **slot 6 of
`GlowWorm::KnotFormation`** (function at RVA `0x37db80`), the routine that walks
the sample points of a formation/path spline and checks each one against the map.

For each sample point it converts two floating-point world coordinates to
integers (`_ftol` at `0x80fdc0`), divides by 1024 (`sar eax,0xa`) to get tile
X/Y, **sign-extends** them, and folds them into a flat index:

```asm
0077dd4c  sar eax,0xa            ; tileY = coordY / 1024
0077dd4f  mov di,ax
0077dd52  call 0x80fdc0          ; _ftol
0077dd57  sar eax,0xa            ; tileX = coordX / 1024
0077dd5a  movsx ecx,ax           ; SIGN-extend tileX
0077dd5d  shl ecx,0x8
0077dd60  movsx edx,di           ; SIGN-extend tileY
0077dd63  add ecx,edx            ; ecx = tileX*256 + tileY
0077dd65  shl ecx,0x8
0077dd68  shr ecx,0x8            ; mask to 24 bits
0077dd6b  test byte [ecx+0x1d6124c], 0x3f
```

There is **no bounds check**. When a sample point lands off the map, its world
coordinate goes negative; `coord/1024` becomes `-1`, `-2`, …; sign extension
turns that into a huge index (e.g. the corner case computes `0xFFFEFF`), so the
read lands ~16 MB past the 64 KB array in unmapped memory and faults.

The game's *other* map-flag accessors do guard this. For example the accessor at
RVA `0x1852a8` first does:

```asm
mov  eax, 0x100                  ; 256 = grid dimension
cmp  word ptr [esi+0x14], ax     ; tileX >= 256 ?
jae  .off_map                    ; unsigned compare also rejects negatives
cmp  word ptr [esi+0x16], cx     ; tileY >= 256 ?
jae  .off_map
```

The `KnotFormation` sample-loop accessor simply omits that guard.

### Investigation dead-ends worth knowing

- Initially suspected the 3x zoom-speed patch (camera projection producing a
  degenerate coordinate). Ruled out: an **unpatched** player reproduced the same
  fault offset, and the crash reproduces purely from dragging troops off-map. A
  `NaN`/`Inf` from `_ftol` is *not* required — a plain negative tile coordinate
  is enough.
- There is at least one sibling unguarded accessor in the same `KnotFormation`
  cluster (RVA `0x37d120`, same `_ftol` -> sign-extend -> read pattern). It was
  **not** the site in any observed crash (all reports are `0x37dd6b`), so it is
  left unpatched for now; guard it identically if it ever surfaces.

---

## Fix

A trampoline hook at the crash site (`src/patches/mapEdgeCrash.cpp`). The 9
original bytes (`test byte ptr [ecx+mapflags],0x3f` + `je`) are replaced with a
jump to a guard that validates the already-computed flat index before the read:

```asm
cmp  ecx, 0x10000        ; valid cells are [0, 256*256)
jae  off_map             ; off-map / negative -> divert
mov  eax, [g_mapFlagBase]
test byte [eax+ecx], 0x3f ; original read, using the ASLR-relocated array base
jz   off_map             ; original je: flag clear -> off-map/false
jmp  continue            ; flag set -> resume the loop at RVA 0x37dd74
off_map:
jmp  0x37dd8d            ; the routine's own "off map" (returns false) path
```

Diverting to `0x37dd8d` returns the routine's existing "not on a valid/passable
tile" answer — the correct result for an off-map sample point.

### Implementation notes

- **Coordinate-agnostic guard.** The check is on the final combined index
  (`ecx >= 0x10000`), so it covers every edge and both axes — corner, top, left,
  right, bottom — not just the corner where it was first reproduced. Any off-map
  index is `>= 0x10000`; the valid 64 KB array is entirely below that, so no OOB
  read is possible after the guard.
- **ASLR-relocated immediate.** The array base in the original instruction
  (`0x1d6124c`) is base-relocated by the loader, so the on-disk bytes are stale.
  The install code captures the live value from `*(site+2)` into `g_mapFlagBase`
  and the trampoline re-emits the read against it, rather than hardcoding the
  disk immediate. The stock-bytes safety check compares only the non-relocated
  bytes (opcode, modrm, imm8, and the following `je`).
- **Register/flag safety.** `EAX` is dead at the hook site and neither resume
  target reads `EAX` or the incoming flags, so the guard is free to clobber both.
  Verified no branch inside the function lands within the 9-byte hook window.

---

## Multiplayer Compatibility

**Safe for version mismatch.** This is a crash fix, analogous to the
knight/catapult crash: the triggering condition (troops off the map edge)
crashes an *unpatched* client anyway, so a patched client surviving with a
deterministic "off map" answer creates no new divergence against an unpatched
peer (the peer is faulting on the same input). `KnotFormation` is a
GlowWorm-engine geometry routine driven by the formation UI; the guard only
substitutes the game's own "off map" result for an out-of-bounds read.
